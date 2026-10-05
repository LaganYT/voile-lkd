/*
 * wsd.m - Obelisk window server spike: native arm64 Cocoa process that owns NSWindows,
 * presents client frames from shared memory through Metal, and forwards input events.
 * Wire format: wsd_protocol.h, PROTOCOL.md.
 *
 * Threading: everything runs on the main queue (AppKit + socket I/O through GCD sources);
 * Metal completion/presented handlers hop back to the main queue.
 * Present path: shm fd -> mmap(MAP_SHARED) -> newBufferWithBytesNoCopy -> linear MTLTexture
 * (no CPU pixel copy) -> one render pass per display-link tick into the CAMetalLayer drawable.
 *
 * Usage: wsd --dir <private-dir> [--exit-on-disconnect]
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <Carbon/Carbon.h> /* kVK_* */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include "wsd_protocol.h"

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define LOG(...) do { fprintf(stderr, "wsd: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_pipe;
static BOOL g_exit_on_disconnect;
static BOOL g_injecting;       /* set while DEBUG_INJECT feeds synthetic events */
static BOOL g_relative_global; /* CGAssociateMouseAndMouseCursorPosition(false) in effect */
static int g_cursor_hidden;
static NSMutableSet *g_clients;

static void restore_input_state(void) {
  if (g_relative_global) {
    CGAssociateMouseAndMouseCursorPosition(true);
    g_relative_global = NO;
  }
  while (g_cursor_hidden > 0) {
    [NSCursor unhide];
    g_cursor_hidden--;
  }
}

static NSString *const kShader =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VO { float4 pos [[position]]; float2 uv; };\n"
     "vertex VO vmain(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
     "fragment float4 fmain(VO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  return t.sample(s, in.uv); }\n";

@class WsdClient;

@interface WsdBuffer : NSObject
@property uint32_t bufferId, width, height, stride, format;
@property BOOL detached; /* BUFFER_DETACH received: never redisplayed */
@property uint8_t *cpu;   /* CPU view of the shared pixels (first pixel), for the latency probe */
@property(strong) id<MTLTexture> texture;
@end
@implementation WsdBuffer
@end

/* Input latency probe (test only, WSD_LATENCY_PROBE=1 or the first DEBUG_INPUT): a DEBUG_INPUT arms it; every overlay frame received after that
 * is compared on a coarse luminance grid with the frame received just before the input. The first frame whose
 * difference rises clearly above the frame-to-frame noise is the game's visible reaction. Times use now_ns(), which is
 * also the guest CLOCK_MONOTONIC under LKD, so submit times from nptunix are on the same clock. */
#define PROBE_GX 48
#define PROBE_GY 27
static BOOL g_probe_enabled;
static uint64_t g_probe_t0;
static int g_probe_frames, g_probe_reacted;
static float g_probe_base[PROBE_GX * PROBE_GY], g_probe_last[PROBE_GX * PROBE_GY];
static BOOL g_probe_have_last;
static double g_probe_noise;

static void probe_signature(WsdBuffer *b, float *sig) {
  for (int gy = 0; gy < PROBE_GY; gy++)
    for (int gx = 0; gx < PROBE_GX; gx++) {
      uint32_t x0 = (uint32_t)((gx + 0.5) * b.width / PROBE_GX), y0 = (uint32_t)((gy + 0.5) * b.height / PROBE_GY);
      float acc = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 4; dx++) {
          uint32_t x = MIN(x0 + dx * 2, b.width - 1), y = MIN(y0 + dy * 2, b.height - 1);
          const uint8_t *px = b.cpu + (size_t)y * b.stride + (size_t)x * 4;
          acc += px[0] + px[1] + px[2];
        }
      sig[gy * PROBE_GX + gx] = acc / 48.0f;
    }
}

static double probe_diff(const float *a, const float *b) {
  double s = 0;
  for (int i = 0; i < PROBE_GX * PROBE_GY; i++) s += fabs(a[i] - b[i]);
  return s / (PROBE_GX * PROBE_GY);
}

static void probe_arm(const char *what) {
  if (!g_probe_enabled) return;
  g_probe_t0 = now_ns();
  g_probe_frames = 0;
  g_probe_reacted = 0;
  memcpy(g_probe_base, g_probe_last, sizeof(g_probe_base));
  fprintf(stderr, "wsd: probe: armed by %s at %llu (noise %.2f)\n", what, (unsigned long long)g_probe_t0, g_probe_noise);
}

static void probe_frame(WsdBuffer *b, uint64_t client_ns, uint64_t rx_ns) {
  if (!g_probe_enabled || !b.cpu) return;
  float sig[PROBE_GX * PROBE_GY];
  probe_signature(b, sig);
  double dprev = g_probe_have_last ? probe_diff(sig, g_probe_last) : 0;
  if (g_probe_t0 && g_probe_frames < 40) {
    double dbase = probe_diff(sig, g_probe_base);
    double thr = MAX(4 * g_probe_noise, 1.5);
    g_probe_frames++;
    BOOL react = !g_probe_reacted && dbase > thr;
    if (react) g_probe_reacted = 1;
    fprintf(stderr, "wsd: probe: frame %d submit %+.1f ms received %+.1f ms diff %.2f (prev %.2f, thr %.2f)%s\n",
            g_probe_frames, ((double)client_ns - (double)g_probe_t0) / 1e6, ((double)rx_ns - (double)g_probe_t0) / 1e6,
            dbase, dprev, thr, react ? " REACT" : "");
    if (g_probe_frames == 40) g_probe_t0 = 0;
  } else {
    g_probe_noise = g_probe_have_last ? 0.9 * g_probe_noise + 0.1 * dprev : 0;
  }
  memcpy(g_probe_last, sig, sizeof(sig));
  g_probe_have_last = YES;
}

@interface WsdView : NSView
@property(weak) id owner; /* WsdWindow */
@end

/* Wine draws the frame of most game windows itself, so they are created borderless; a borderless NSWindow refuses
 * key and main status by default and then never receives keyboard events (2026-10-03: Rocket League got no keys
 * from the real keyboard, key=0 in every window state, while injected events sent to the window directly worked). */
@interface WsdNSWindow : NSWindow
@end
@implementation WsdNSWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@interface WsdWindow : NSObject <NSWindowDelegate>
@property uint32_t windowId;
@property(weak) WsdClient *client;
@property(strong) NSWindow *window;
@property(strong) WsdView *view;
@property(strong) CAMetalLayer *layer;
@property(strong) CADisplayLink *link;
/* pending (mailbox) frame */
@property(strong) WsdBuffer *pendingBuffer;
/* last encoded buffer, shown again (without events) when its frame was never displayed, e.g. the first frame
 * after WINDOW_SHOW of a window that is not yet on screen, while the client has nothing newer to present */
@property(strong) WsdBuffer *lastBuffer;
@property BOOL redisplay;
@property int redisplayTries;
@property uint64_t pendingSerial, pendingClientNs, pendingReceivedNs;
@property uint32_t replaced, nPresented, nZero;
/* present interval statistics (logged every 120 shown frames per plane) */
@property uint64_t statLastNs, statSumNs, statMaxNs, statMinNs;
@property uint32_t statCount;
@property BOOL relative, clip, fullscreen, transitioning;
@property NSRect clipPx;
/* overlay plane (PRESENT_REGION, e.g. a D3D swapchain in the client area): a WsdWindow without an NSWindow of its
 * own, whose layer is a sublayer of the view's layer */
@property(strong) WsdWindow *overlay;
@property BOOL isOverlay;
@property NSRect overlayPx;
@property NSRect savedFrame;
@property NSWindowStyleMask savedStyle;
@property NSWindowLevel savedLevel;
- (void)sendEvent:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra;
@end

/* Audio output stream (WSD_AUDIO_*): one default-output AudioUnit per stream, whose render callback reads the
 * client's shared ring directly (no copy in between, no message per buffer). CoreAudio converts the client format
 * and rate to the device and mixes the streams. */
struct wsd_audio_rt {
  struct wsd_audio_ring *ring;
  uint8_t *data;
  uint32_t ring_frames, frame_bytes;
  uint8_t silence;
  _Atomic int running;
};

static OSStatus audio_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                             UInt32 frames, AudioBufferList *io) {
  struct wsd_audio_rt *rt = ref;
  uint8_t *out = io->mBuffers[0].mData;
  uint32_t want = frames, got = 0;
  (void)ts; (void)bus;
  if (atomic_load_explicit(&rt->running, memory_order_acquire)) {
    _Atomic uint64_t *wp = (_Atomic uint64_t *)&rt->ring->write_frames, *rp = (_Atomic uint64_t *)&rt->ring->read_frames;
    uint64_t w = atomic_load_explicit(wp, memory_order_acquire), r = atomic_load_explicit(rp, memory_order_relaxed);
    uint64_t avail = w > r ? w - r : 0;
    if (avail > rt->ring_frames) avail = rt->ring_frames; /* a misbehaving client cannot make us read out of range */
    got = avail < want ? (uint32_t)avail : want;
    for (uint32_t done = 0; done < got;) {
      uint32_t pos = (uint32_t)((r + done) % rt->ring_frames), n = rt->ring_frames - pos;
      if (n > got - done) n = got - done;
      memcpy(out + (size_t)done * rt->frame_bytes, rt->data + (size_t)pos * rt->frame_bytes, (size_t)n * rt->frame_bytes);
      done += n;
    }
    atomic_store_explicit(rp, r + got, memory_order_release);
    if (got < want) rt->ring->underrun_frames += want - got;
  }
  if (got < want) memset(out + (size_t)got * rt->frame_bytes, rt->silence, (size_t)(want - got) * rt->frame_bytes);
  if (got == 0 && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
  return noErr;
}

@interface WsdAudio : NSObject
@property uint32_t streamId;
@property AudioComponentInstance unit;
@property void *map;
@property size_t mapLen;
@property struct wsd_audio_rt *rt;
@end
@implementation WsdAudio
- (void)dispose {
  if (self.unit) {
    AudioOutputUnitStop(self.unit); /* synchronous: the render callback is not running afterwards */
    AudioUnitUninitialize(self.unit);
    AudioComponentInstanceDispose(self.unit);
    self.unit = NULL;
  }
  if (self.map) { munmap(self.map, self.mapLen); self.map = NULL; }
  free(self.rt);
  self.rt = NULL;
}
@end

@interface WsdClient : NSObject
@property int fd;
@property pid_t pid;
@property(strong) dispatch_source_t readSrc, writeSrc;
@property(strong) NSMutableData *inbuf, *outbuf;
@property(strong) NSMutableArray<NSNumber *> *fdQueue;
@property(strong) NSMutableDictionary<NSNumber *, WsdWindow *> *windows;
@property(strong) NSMutableDictionary<NSNumber *, WsdBuffer *> *buffers;
@property(strong) NSMutableDictionary<NSNumber *, WsdAudio *> *audio;
@property BOOL dead;
- (void)send:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra;
- (void)close;
@end

/* The screen with the menu bar: the client's display, and the origin of the pixel coordinates wsd exchanges with it.
 * NSScreen.mainScreen is the screen of the key window, which moves (2026-10-04: with Obelisk on the MacBook's screen,
 * Rocket League got 3024x1964 while its window was on a 2560x1440 one, and its picture overflowed). */
static NSScreen *primary_screen(void) {
  return NSScreen.screens.firstObject ?: NSScreen.mainScreen;
}

static double backing_scale(NSWindow *w) {
  double s = w ? w.backingScaleFactor : primary_screen().backingScaleFactor;
  return s > 0 ? s : 1.0;
}

/* WSD_STYLE_* to an AppKit style mask: a titled window gets the macOS title bar and buttons; borderless otherwise
 * (the client draws any frame itself). */
static NSWindowStyleMask style_mask(uint32_t style) {
  NSWindowStyleMask mask = 0;
  if (style & WSD_STYLE_BORDERLESS) return NSWindowStyleMaskBorderless;
  if (style & WSD_STYLE_TITLED) mask |= NSWindowStyleMaskTitled;
  if (style & WSD_STYLE_CLOSABLE) mask |= NSWindowStyleMaskClosable;
  if (style & WSD_STYLE_RESIZABLE) mask |= NSWindowStyleMaskResizable;
  if (style & WSD_STYLE_MINIATURIZABLE) mask |= NSWindowStyleMaskMiniaturizable;
  return mask;
}

/* ---------------------------------------------------------------- view and input */

static CAMetalLayer *make_metal_layer(void) {
  CAMetalLayer *l = [CAMetalLayer layer];
  l.device = g_dev;
  l.pixelFormat = MTLPixelFormatBGRA8Unorm;
  /* Clients write sRGB-encoded pixels (Win32 GDI, D3D swapchains in UNORM): tag the layer as sRGB so the window
   * server color-matches it to the display instead of showing the raw values in the display's own space. */
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  l.colorspace = srgb;
  CGColorSpaceRelease(srgb);
  /* Opaque and black: Win32 GDI never writes alpha (its pixels have alpha 0), and a window shown before its first
   * frame must not let the window's grey background through (2026-10-03: Rocket League's startup window). */
  l.opaque = YES;
  l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  l.framebufferOnly = YES;
  l.maximumDrawableCount = 3;
  l.displaySyncEnabled = YES;
  l.allowsNextDrawableTimeout = YES;
  l.contentsGravity = kCAGravityResize;
  return l;
}



@implementation WsdView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
  return make_metal_layer();
}
- (WsdWindow *)ww { return (WsdWindow *)self.owner; }
- (uint32_t)synth { return g_injecting ? WSD_KEY_SYNTHETIC : 0; }
- (uint64_t)evNs:(NSEvent *)e { return (uint64_t)(e.timestamp * 1e9); }
- (void)pixelPoint:(NSEvent *)e x:(float *)x y:(float *)y {
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  double s = backing_scale(self.window);
  *x = (float)(p.x * s);
  *y = (float)((self.bounds.size.height - p.y) * s);
}
- (void)key:(NSEvent *)e down:(BOOL)down {
  struct wsd_ev_key k = {self.ww.windowId, (down ? WSD_KEY_DOWN : 0) | (e.isARepeat ? WSD_KEY_REPEAT : 0) | [self synth],
                         e.keyCode, (uint32_t)(e.modifierFlags >> 16), [self evNs:e]};
  NSData *chars = [e.characters dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
  [self.ww sendEvent:WSD_EV_KEY payload:&k len:sizeof(k) extra:chars];
}
- (void)keyDown:(NSEvent *)e { [self key:e down:YES]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:NO]; }
- (void)flagsChanged:(NSEvent *)e {
  NSEventModifierFlags bit = 0;
  switch (e.keyCode) {
  case kVK_Shift: case kVK_RightShift: bit = NSEventModifierFlagShift; break;
  case kVK_Control: case kVK_RightControl: bit = NSEventModifierFlagControl; break;
  case kVK_Option: case kVK_RightOption: bit = NSEventModifierFlagOption; break;
  case kVK_Command: case kVK_RightCommand: bit = NSEventModifierFlagCommand; break;
  case kVK_CapsLock: bit = NSEventModifierFlagCapsLock; break;
  case kVK_Function: bit = NSEventModifierFlagFunction; break;
  default: return;
  }
  struct wsd_ev_key k = {self.ww.windowId, ((e.modifierFlags & bit) ? WSD_KEY_DOWN : 0) | [self synth], e.keyCode,
                         (uint32_t)(e.modifierFlags >> 16), [self evNs:e]};
  [self.ww sendEvent:WSD_EV_KEY payload:&k len:sizeof(k) extra:nil];
}
- (void)button:(NSEvent *)e which:(uint32_t)b down:(BOOL)down {
  struct wsd_ev_button m = {self.ww.windowId, b, down, (uint32_t)(e.modifierFlags >> 16), 0, 0, [self evNs:e]};
  [self pixelPoint:e x:&m.x y:&m.y];
  [self.ww sendEvent:WSD_EV_BUTTON payload:&m len:sizeof(m) extra:nil];
}
- (void)mouseDown:(NSEvent *)e { [self button:e which:0 down:YES]; }
- (void)mouseUp:(NSEvent *)e { [self button:e which:0 down:NO]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:e which:1 down:YES]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:e which:1 down:NO]; }
- (void)otherMouseDown:(NSEvent *)e { [self button:e which:(uint32_t)e.buttonNumber down:YES]; }
- (void)otherMouseUp:(NSEvent *)e { [self button:e which:(uint32_t)e.buttonNumber down:NO]; }
- (void)motion:(NSEvent *)e {
  WsdWindow *w = self.ww;
  if (w.relative) {
    struct wsd_ev_relative r = {w.windowId, [self synth], (float)e.deltaX, (float)e.deltaY, [self evNs:e]};
    [w sendEvent:WSD_EV_RELATIVE payload:&r len:sizeof(r) extra:nil];
    return;
  }
  struct wsd_ev_motion m = {w.windowId, [self synth], 0, 0, [self evNs:e]};
  [self pixelPoint:e x:&m.x y:&m.y];
  if (w.clip) {
    NSRect c = w.clipPx;
    float cx = fminf(fmaxf(m.x, c.origin.x), c.origin.x + c.size.width - 1);
    float cy = fminf(fmaxf(m.y, c.origin.y), c.origin.y + c.size.height - 1);
    if (cx != m.x || cy != m.y) {
      m.flags |= 1; /* clamped */
      if (!g_injecting) {
        /* Real pointer left the clip rect: warp it back (global, top-left display coordinates). */
        double s = backing_scale(self.window);
        NSPoint vp = NSMakePoint(cx / s, self.bounds.size.height - cy / s);
        NSPoint sp = [self.window convertPointToScreen:[self convertPoint:vp toView:nil]];
        CGFloat top = primary_screen().frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(sp.x, top - sp.y));
        CGAssociateMouseAndMouseCursorPosition(true);
      }
      m.x = cx;
      m.y = cy;
    }
  }
  [w sendEvent:WSD_EV_MOTION payload:&m len:sizeof(m) extra:nil];
}
- (void)mouseMoved:(NSEvent *)e { [self motion:e]; }
- (void)mouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)scrollWheel:(NSEvent *)e {
  struct wsd_ev_wheel w = {self.ww.windowId, e.hasPreciseScrollingDeltas | [self synth], (float)e.scrollingDeltaX,
                           (float)e.scrollingDeltaY, [self evNs:e]};
  [self.ww sendEvent:WSD_EV_WHEEL payload:&w len:sizeof(w) extra:nil];
}
@end

/* ---------------------------------------------------------------- windows and present */

@implementation WsdWindow
- (void)sendEvent:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra {
  [self.client send:type payload:p len:n extra:extra];
}
- (void)sendResize {
  NSSize sz = self.view.bounds.size;
  double s = backing_scale(self.window);
  struct wsd_ev_resize r = {self.windowId, (uint32_t)lround(sz.width * s), (uint32_t)lround(sz.height * s),
                            (uint32_t)lround(s * 1000)};
  [self sendEvent:WSD_EV_RESIZE payload:&r len:sizeof(r) extra:nil];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
  struct wsd_window w = {self.windowId};
  [self sendEvent:WSD_EV_CLOSE payload:&w len:sizeof(w) extra:nil];
  return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n {
  /* back in the window: a relative-mode window takes the pointer again */
  if (self.relative && !g_relative_global) {
    CGError e = CGAssociateMouseAndMouseCursorPosition(false);
    g_relative_global = (e == kCGErrorSuccess);
    LOG("window %u: focus back, relative mouse resumed (%d)", self.windowId, e);
  }
  struct wsd_window_flag f = {self.windowId, 1};
  [self sendEvent:WSD_EV_FOCUS payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidResignKey:(NSNotification *)n {
  /* The pointer disassociation is global: leaving the window (Cmd+Tab, another app, a notification) must give the
   * user their cursor back at once, whatever the game does with the focus event (2026-10-03 session: the cursor
   * stayed captured and the user had to kill wsd). */
  if (self.relative && g_relative_global) {
    CGAssociateMouseAndMouseCursorPosition(true);
    g_relative_global = NO;
    LOG("window %u: focus lost, relative mouse suspended", self.windowId);
  }
  struct wsd_window_flag f = {self.windowId, 0};
  [self sendEvent:WSD_EV_FOCUS payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidResize:(NSNotification *)n {
  if (self.transitioning) return; /* one RESIZE per fullscreen transition */
  [self sendResize];
  [self sendFrame];
}
/* The user (or AppKit, e.g. moving it off a removed screen) moved or resized the window: the client follows. */
- (void)sendFrame {
  if (self.transitioning || self.fullscreen || self.window.miniaturized) return;
  NSWindow *w = self.window;
  double s = backing_scale(w);
  NSRect content = [w contentRectForFrameRect:w.frame];
  double top = NSMaxY(primary_screen().frame);
  struct wsd_window_frame f = {self.windowId, WSD_FRAME_MOVE | WSD_FRAME_SIZE, (int32_t)lround(content.origin.x * s),
                               (int32_t)lround((top - NSMaxY(content)) * s), (uint32_t)lround(content.size.width * s),
                               (uint32_t)lround(content.size.height * s)};
  [self sendEvent:WSD_EV_FRAME payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidMove:(NSNotification *)n {
  [self sendFrame];
}
/* Also sent after WINDOW_MINIMIZE (the animation ends later): the client ignores a state it already has. */
- (void)windowDidMiniaturize:(NSNotification *)n {
  struct wsd_window_flag f = {self.windowId, 1};
  [self sendEvent:WSD_EV_MINIMIZE payload:&f len:sizeof(f) extra:nil];
}
- (void)windowDidDeminiaturize:(NSNotification *)n {
  struct wsd_window_flag f = {self.windowId, 0};
  [self sendEvent:WSD_EV_MINIMIZE payload:&f len:sizeof(f) extra:nil];
}
/* The client maximizes and restores the window itself (Win32 SC_MAXIMIZE) and then sets its frame. */
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)frame {
  struct wsd_window w = {self.windowId};
  [self sendEvent:WSD_EV_ZOOM payload:&w len:sizeof(w) extra:nil];
  return NO;
}
- (void)setStyle:(uint32_t)style {
  NSWindowStyleMask mask = style_mask(style);
  NSWindow *w = self.window;
  if (self.fullscreen) { self.savedStyle = mask; return; } /* applied when fullscreen ends */
  if (w.styleMask == mask) return;
  NSRect content = [w contentRectForFrameRect:w.frame];
  self.transitioning = YES;
  w.styleMask = mask;
  [w setFrame:[w frameRectForContentRect:content] display:YES];
  [w makeFirstResponder:self.view];
  self.transitioning = NO;
  LOG("window %u style 0x%x", self.windowId, style);
}
- (void)minimize:(BOOL)on {
  NSWindow *w = self.window;
  if (on && !w.miniaturized) {
    if (w.styleMask & NSWindowStyleMaskMiniaturizable) [w miniaturize:nil];
    else [w orderOut:nil];
  } else if (!on && w.miniaturized) {
    [w deminiaturize:nil];
  }
}
- (void)logState:(const char *)when {
  NSWindow *w = self.window;
  NSRect f = w.frame;
  LOG("window %u [%s]: visible=%d occlusion_visible=%d on_active_space=%d key=%d app_active=%d screen='%s' "
      "frame=(%.0f,%.0f %.0fx%.0f) level=%ld presented=%u presented_zero=%u overlay_presented=%u overlay_zero=%u",
      self.windowId, when, w.visible, (w.occlusionState & NSWindowOcclusionStateVisible) != 0, w.onActiveSpace,
      w.keyWindow, NSApp.active, w.screen.localizedName.UTF8String ?: "(none)", f.origin.x, f.origin.y,
      f.size.width, f.size.height, (long)w.level, self.nPresented, self.nZero, self.overlay.nPresented, self.overlay.nZero);
}

- (void)releaseBuffer:(WsdBuffer *)b serial:(uint64_t)serial shown:(BOOL)shown {
  struct wsd_ev_release r = {b.bufferId, shown, serial};
  [self.client send:WSD_EV_RELEASE payload:&r len:sizeof(r) extra:nil];
}

/* Display-link tick: encode the latest pending frame, if any. */
- (void)tick:(CADisplayLink *)link {
  [self tickPlane];
  if (self.overlay) [self.overlay tickPlane];
}

- (void)tickPlane {
  WsdBuffer *b = self.pendingBuffer;
  BOOL redisplay = NO;
  if (!b && self.redisplay && self.lastBuffer && !self.lastBuffer.detached) { b = self.lastBuffer; redisplay = YES; }
  if (!b || !self.window.visible) return;
  uint64_t tick_ns = now_ns();
  id<CAMetalDrawable> d = [self.layer nextDrawable];
  if (!d) return; /* keep it pending */
  if (redisplay) self.redisplay = NO;
  else { self.pendingBuffer = nil; self.lastBuffer = b; self.redisplayTries = 0; }
  CGSize want = CGSizeMake(b.width, b.height);
  if (!CGSizeEqualToSize(self.layer.drawableSize, want)) self.layer.drawableSize = want;

  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLCommandBuffer> cb = [g_queue commandBuffer];
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:g_pipe];
  [enc setFragmentTexture:b.texture atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];

  struct wsd_ev_presented ev = {self.windowId, b.bufferId, self.pendingSerial, self.pendingClientNs,
                                self.pendingReceivedNs, tick_ns, 0, self.replaced, 0};
  self.replaced = 0;
  uint64_t serial = self.pendingSerial;
  __weak WsdWindow *weakSelf = self;
  [d addPresentedHandler:^(id<MTLDrawable> dr) {
    struct wsd_ev_presented e2 = ev;
    e2.presented_ns = (uint64_t)(dr.presentedTime * 1e9);
    dispatch_async(dispatch_get_main_queue(), ^{
      WsdWindow *sw = weakSelf;
      if (e2.presented_ns) sw.nPresented++; else sw.nZero++;
      if (e2.presented_ns && !redisplay) {
        if (sw.statLastNs && e2.presented_ns > sw.statLastNs) {
          uint64_t d = e2.presented_ns - sw.statLastNs;
          if (d > 50000000) { /* a hitch: wall-clock time, to match against profiles of the game */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct tm lt;
            localtime_r(&tv.tv_sec, &lt);
            LOG("window %u %s: frame held %.1f ms, shown at %02d:%02d:%02d.%03d", sw.windowId,
                sw.isOverlay ? "overlay" : "base", d / 1e6, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)(tv.tv_usec / 1000));
          }
          sw.statSumNs += d;
          if (d > sw.statMaxNs) sw.statMaxNs = d;
          if (!sw.statMinNs || d < sw.statMinNs) sw.statMinNs = d;
          if (++sw.statCount == 120) {
            LOG("window %u %s: 120 frames shown, present interval avg %.3f min %.3f max %.3f ms, latency submit->shown %.3f ms",
                sw.windowId, sw.isOverlay ? "overlay" : "base", sw.statSumNs / 120 / 1e6, sw.statMinNs / 1e6,
                sw.statMaxNs / 1e6, (e2.presented_ns > e2.client_time_ns ? e2.presented_ns - e2.client_time_ns : 0) / 1e6);
            sw.statCount = 0; sw.statSumNs = sw.statMaxNs = sw.statMinNs = 0;
          }
        }
        sw.statLastNs = e2.presented_ns;
      }
      /* never displayed and nothing newer queued: show the same buffer again on a later tick (bounded) */
      if (!e2.presented_ns && !sw.pendingBuffer && sw.lastBuffer == b && sw.redisplayTries < 240) {
        sw.redisplayTries++;
        sw.redisplay = YES;
      }
      if (!redisplay) [weakSelf sendEvent:WSD_EV_PRESENTED payload:&e2 len:sizeof(e2) extra:nil];
    });
  }];
  [cb presentDrawable:d];
  if (!redisplay)
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
      dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf releaseBuffer:b serial:serial shown:YES]; });
    }];
  [cb commit];
}

/* Moves and/or resizes the content area. Pixels are converted with the backing scale; y counts down from the top of
 * the main screen (Cocoa counts up from its bottom). One EV_RESIZE follows a size change. */
- (void)setFramePx:(const struct wsd_window_frame *)f {
  NSWindow *w = self.window;
  double s = backing_scale(w);
  NSRect content = [w contentRectForFrameRect:w.frame];
  if (f->flags & WSD_FRAME_SIZE) content.size = NSMakeSize(f->width / s, f->height / s);
  if (f->flags & WSD_FRAME_MOVE) {
    double top = NSMaxY(primary_screen().frame);
    content.origin = NSMakePoint(f->x / s, top - f->y / s - content.size.height);
  } else {
    content.origin.y += [w contentRectForFrameRect:w.frame].size.height - content.size.height; /* keep the top edge */
  }
  BOOL resized = (f->flags & WSD_FRAME_SIZE) != 0;
  self.transitioning = YES;
  [w setFrame:[w frameRectForContentRect:content] display:YES];
  self.transitioning = NO;
  LOG("window %u frame px (%d,%d %ux%u) flags 0x%x", self.windowId, f->x, f->y, f->width, f->height, f->flags);
  if (resized) [self sendResize];
  /* AppKit keeps a title bar below the menu bar and inside the screen (a Win32 caption is shorter than the macOS
   * title bar, e.g. a maximized window): the client takes the frame the window really got. */
  NSRect got = [w contentRectForFrameRect:w.frame];
  if (fabs(got.origin.x - content.origin.x) > 0.5 || fabs(got.origin.y - content.origin.y) > 0.5 ||
      fabs(got.size.width - content.size.width) > 0.5 || fabs(got.size.height - content.size.height) > 0.5)
    [self sendFrame];
}

- (void)setFullscreenMode:(BOOL)on {
  if (on == self.fullscreen) return;
  NSWindow *w = self.window;
  self.transitioning = YES;
  if (on) {
    self.savedFrame = w.frame;
    self.savedStyle = w.styleMask;
    self.savedLevel = w.level;
    w.styleMask = NSWindowStyleMaskBorderless;
    w.level = NSMainMenuWindowLevel + 1;
    [w setFrame:w.screen ? w.screen.frame : primary_screen().frame display:YES];
    @try {
      NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    } @catch (NSException *ex) {
      LOG("presentationOptions refused: %s", ex.reason.UTF8String);
    }
  } else {
    w.styleMask = self.savedStyle;
    w.level = self.savedLevel;
    [w setFrame:self.savedFrame display:YES];
    NSApp.presentationOptions = NSApplicationPresentationDefault;
  }
  [w makeFirstResponder:self.view];
  self.transitioning = NO;
  self.fullscreen = on;
  [self logState:on ? "fullscreen-on" : "fullscreen-off"];
  [self sendResize];
}

- (void)removeOverlay {
  WsdWindow *o = self.overlay;
  if (!o) return;
  if (o.pendingBuffer) [o releaseBuffer:o.pendingBuffer serial:o.pendingSerial shown:NO];
  o.pendingBuffer = nil;
  o.lastBuffer = nil;
  [o.layer removeFromSuperlayer];
  self.overlay = nil;
}

/* Places the overlay plane at a rectangle of the content view given in pixels (top-left origin). */
- (WsdWindow *)overlayAt:(NSRect)px {
  WsdWindow *o = self.overlay;
  double s = backing_scale(self.window);
  if (!o) {
    o = [WsdWindow new];
    o.isOverlay = YES;
    o.windowId = self.windowId;
    o.client = self.client;
    o.window = self.window;
    o.layer = make_metal_layer();
    o.layer.contentsScale = s;
    [self.view.layer addSublayer:o.layer];
    self.overlay = o;
    LOG("window %u: overlay plane created", self.windowId);
  }
  if (!NSEqualRects(px, self.overlayPx) || !o.layer.superlayer) {
    NSSize vs = self.view.bounds.size;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    o.layer.frame = CGRectMake(px.origin.x / s, vs.height - (px.origin.y + px.size.height) / s, px.size.width / s,
                               px.size.height / s);
    [CATransaction commit];
    self.overlayPx = px;
    LOG("window %u: overlay at px (%.0f,%.0f %.0fx%.0f)", self.windowId, px.origin.x, px.origin.y, px.size.width, px.size.height);
  }
  return o;
}

- (void)destroy {
  [self logState:"destroy"];
  [self removeOverlay];
  if (self.pendingBuffer) [self releaseBuffer:self.pendingBuffer serial:self.pendingSerial shown:NO];
  self.pendingBuffer = nil;
  [self.link invalidate];
  self.link = nil;
  if (self.relative) {
    restore_input_state();
    self.relative = NO;
  }
  if (self.fullscreen) NSApp.presentationOptions = NSApplicationPresentationDefault;
  self.window.delegate = nil;
  [self.window orderOut:nil];
  [self.window close];
  self.window = nil;
}
@end

/* ---------------------------------------------------------------- client connection */

@implementation WsdClient
- (void)send:(uint32_t)type payload:(const void *)p len:(uint32_t)n extra:(NSData *)extra {
  if (self.dead) return;
  uint32_t total = n + (uint32_t)extra.length;
  if (total > WSD_MAX_PAYLOAD) return;
  struct wsd_hdr h = {type, total};
  [self.outbuf appendBytes:&h length:sizeof(h)];
  if (n) [self.outbuf appendBytes:p length:n];
  if (extra.length) [self.outbuf appendData:extra];
  if (self.outbuf.length > (8u << 20)) { /* client stopped reading */
    LOG("client %d output backlog over 8 MiB, dropping it", self.pid);
    [self close];
    return;
  }
  [self flush];
}
- (void)flush {
  while (self.outbuf.length) {
    ssize_t r = write(self.fd, self.outbuf.bytes, self.outbuf.length);
    if (r > 0) {
      [self.outbuf replaceBytesInRange:NSMakeRange(0, (NSUInteger)r) withBytes:NULL length:0];
      continue;
    }
    if (r < 0 && errno == EAGAIN) {
      if (!self.writeSrc) {
        self.writeSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, self.fd, 0, dispatch_get_main_queue());
        __weak WsdClient *ws = self;
        dispatch_source_set_event_handler(self.writeSrc, ^{
          WsdClient *s = ws;
          [s flush];
          if (s && !s.outbuf.length && s.writeSrc) { dispatch_source_cancel(s.writeSrc); s.writeSrc = nil; }
        });
        dispatch_resume(self.writeSrc);
      }
      return;
    }
    [self close];
    return;
  }
}
- (void)error:(uint32_t)req code:(int32_t)code {
  struct wsd_ev_error e = {req, code};
  LOG("client %d: request %u failed with %d", self.pid, req, code);
  [self send:WSD_EV_ERROR payload:&e len:sizeof(e) extra:nil];
}
- (void)close {
  if (self.dead) return;
  self.dead = YES;
  for (WsdWindow *w in self.windows.allValues) [w destroy];
  [self.windows removeAllObjects];
  [self.buffers removeAllObjects]; /* textures die after in-flight command buffers release them */
  for (WsdAudio *a in self.audio.allValues) [a dispose];
  [self.audio removeAllObjects];
  for (NSNumber *f in self.fdQueue) close(f.intValue);
  [self.fdQueue removeAllObjects];
  if (self.readSrc) dispatch_source_cancel(self.readSrc);
  if (self.writeSrc) dispatch_source_cancel(self.writeSrc);
  close(self.fd);
  LOG("client %d disconnected", self.pid);
  [g_clients removeObject:self];
  if (g_exit_on_disconnect && g_clients.count == 0) {
    restore_input_state();
    exit(0);
  }
}

- (WsdWindow *)win:(uint32_t)wid req:(uint32_t)req {
  WsdWindow *w = self.windows[@(wid)];
  if (!w) [self error:req code:-ENOENT];
  return w;
}

- (void)createWindow:(const struct wsd_window_create *)c title:(NSString *)title {
  if (!c->window_id || self.windows[@(c->window_id)] || self.windows.count >= WSD_MAX_WINDOWS ||
      !c->width_px || !c->height_px || c->width_px > 16384 || c->height_px > 16384) {
    [self error:WSD_WINDOW_CREATE code:-EINVAL];
    return;
  }
  NSWindowStyleMask style = style_mask(c->style);
  double s = backing_scale(nil);
  NSRect r = NSMakeRect(120, 120, c->width_px / s, c->height_px / s);
  WsdWindow *w = [WsdWindow new];
  w.windowId = c->window_id;
  w.client = self;
  w.window = [[WsdNSWindow alloc] initWithContentRect:r styleMask:style backing:NSBackingStoreBuffered defer:NO];
  w.window.releasedWhenClosed = NO;
  w.window.backgroundColor = NSColor.blackColor;
  /* the zoom button maximizes through the client (WSD_EV_ZOOM); wsd's own fullscreen is WINDOW_FULLSCREEN */
  w.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
  w.window.tabbingMode = NSWindowTabbingModeDisallowed;
  w.window.title = title;
  w.window.acceptsMouseMovedEvents = YES;
  if (c->style & WSD_STYLE_TOPMOST) w.window.level = NSFloatingWindowLevel;
  w.window.delegate = w;
  w.view = [[WsdView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
  w.view.owner = w;
  w.view.wantsLayer = YES;
  w.layer = (CAMetalLayer *)w.view.layer;
  w.window.contentView = w.view;
  [w.window makeFirstResponder:w.view];
  w.link = [w.view displayLinkWithTarget:w selector:@selector(tick:)];
  [w.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  self.windows[@(c->window_id)] = w;
  LOG("client %d: window %u %ux%u px style 0x%x", self.pid, c->window_id, c->width_px, c->height_px, c->style);
}

- (void)audioOpen:(const struct wsd_audio_open *)o {
  if (!self.fdQueue.count) { [self error:WSD_AUDIO_OPEN code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  uint32_t sample = o->format == WSD_AUDIO_S16 ? 2 : o->format == WSD_AUDIO_S24 ? 3 : o->format == WSD_AUDIO_U8 ? 1
                    : (o->format == WSD_AUDIO_S32 || o->format == WSD_AUDIO_F32) ? 4 : 0;
  struct stat st;
  size_t page = (size_t)getpagesize();
  uint64_t floor_off = o->offset & ~(uint64_t)(page - 1), delta = o->offset - floor_off;
  uint64_t maplen = (delta + o->size + page - 1) & ~(uint64_t)(page - 1);
  if (!o->stream_id || self.audio[@(o->stream_id)] || self.audio.count >= 32 || !sample || !o->channels ||
      o->channels > 8 || o->frame_bytes != sample * o->channels || o->rate < 4000 || o->rate > 384000 ||
      !o->ring_frames || o->ring_frames > (1u << 22) || o->size < sizeof(struct wsd_audio_ring) +
      (uint64_t)o->ring_frames * o->frame_bytes || (delta % 8) || fstat(fd, &st) ||
      (uint64_t)st.st_size < o->offset + o->size) { /* the mapping may end in a partial page past EOF: never read */
    close(fd);
    [self error:WSD_AUDIO_OPEN code:-EINVAL];
    return;
  }
  void *p = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)floor_off);
  close(fd);
  if (p == MAP_FAILED) { [self error:WSD_AUDIO_OPEN code:-errno]; return; }
  WsdAudio *a = [WsdAudio new];
  a.streamId = o->stream_id;
  a.map = p;
  a.mapLen = (size_t)maplen;
  struct wsd_audio_rt *rt = calloc(1, sizeof(*rt));
  rt->ring = (struct wsd_audio_ring *)((uint8_t *)p + delta);
  rt->data = (uint8_t *)rt->ring + sizeof(struct wsd_audio_ring);
  rt->ring_frames = o->ring_frames;
  rt->frame_bytes = o->frame_bytes;
  rt->silence = o->format == WSD_AUDIO_U8 ? 0x80 : 0;
  a.rt = rt;

  AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0};
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  AudioComponentInstance unit = NULL;
  OSStatus e = comp ? AudioComponentInstanceNew(comp, &unit) : -1;
  if (e == noErr) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = o->rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsPacked |
                     (o->format == WSD_AUDIO_F32 ? kAudioFormatFlagIsFloat
                      : o->format == WSD_AUDIO_U8 ? 0 : kAudioFormatFlagIsSignedInteger);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = o->channels;
    f.mBitsPerChannel = sample * 8;
    f.mBytesPerFrame = f.mBytesPerPacket = o->frame_bytes;
    e = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f));
  }
  if (e == noErr) {
    AURenderCallbackStruct cb = {audio_render, rt};
    e = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
  }
  if (e == noErr) e = AudioUnitInitialize(unit);
  a.unit = unit;
  if (e != noErr) {
    LOG("client %d: audio stream %u: AudioUnit setup failed (%d)", self.pid, o->stream_id, (int)e);
    [a dispose];
    [self error:WSD_AUDIO_OPEN code:-EIO];
    return;
  }
  self.audio[@(o->stream_id)] = a;
  LOG("client %d: audio stream %u: format %u, %u Hz, %u channels, ring %u frames", self.pid, o->stream_id, o->format,
      o->rate, o->channels, o->ring_frames);
}
/* The Dock and Cmd-Tab icon of the whole server (one game per wsd): the client's program icon. */
- (void)appIcon:(const struct wsd_app_icon *)a {
  if (!self.fdQueue.count) { [self error:WSD_APP_ICON code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  struct stat st;
  if (!a->width || !a->height || a->width > 1024 || a->height > 1024 || a->stride < a->width * 4 ||
      a->size < (uint64_t)a->stride * a->height || fstat(fd, &st) || (uint64_t)st.st_size < a->offset + a->size) {
    close(fd);
    [self error:WSD_APP_ICON code:-EINVAL];
    return;
  }
  NSMutableData *pixels = [NSMutableData dataWithLength:(NSUInteger)a->size];
  ssize_t got = pread(fd, pixels.mutableBytes, (size_t)a->size, (off_t)a->offset);
  close(fd);
  if (got != (ssize_t)a->size) { [self error:WSD_APP_ICON code:-EIO]; return; }
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(a->width, a->height, 8, 32, a->stride, srgb,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaFirst, provider, NULL, false,
                                   kCGRenderingIntentDefault);
  CGColorSpaceRelease(srgb);
  CGDataProviderRelease(provider);
  if (!image) { [self error:WSD_APP_ICON code:-EINVAL]; return; }
  NSApp.applicationIconImage = [[NSImage alloc] initWithCGImage:image size:NSZeroSize];
  CGImageRelease(image);
  LOG("client %d: application icon %ux%u", self.pid, a->width, a->height);
}

- (void)attachBuffer:(const struct wsd_buffer_attach *)a {
  if (!self.fdQueue.count) { [self error:WSD_BUFFER_ATTACH code:-EBADF]; return; }
  int fd = self.fdQueue.firstObject.intValue;
  [self.fdQueue removeObjectAtIndex:0];
  MTLPixelFormat pf = a->format == WSD_FMT_BGRA8 ? MTLPixelFormatBGRA8Unorm
                      : a->format == WSD_FMT_RGBA8 ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatInvalid;
  NSUInteger align = pf != MTLPixelFormatInvalid ? [g_dev minimumLinearTextureAlignmentForPixelFormat:pf] : 1;
  struct stat st;
  size_t page = (size_t)getpagesize();
  uint64_t floor_off = a->offset & ~(uint64_t)(page - 1), delta = a->offset - floor_off;
  uint64_t maplen = (delta + a->size + page - 1) & ~(uint64_t)(page - 1);
  if (!a->buffer_id || self.buffers[@(a->buffer_id)] || self.buffers.count >= WSD_MAX_BUFFERS ||
      pf == MTLPixelFormatInvalid || !a->width || !a->height || a->width > 16384 || a->height > 16384 ||
      a->stride < (uint64_t)a->width * 4 || a->size < (uint64_t)a->stride * a->height || (a->stride % align) ||
      (delta % align) || fstat(fd, &st) || (uint64_t)st.st_size < floor_off + maplen) {
    close(fd);
    [self error:WSD_BUFFER_ATTACH code:-EINVAL];
    return;
  }
  void *p = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)floor_off);
  close(fd); /* the mapping keeps the object alive */
  if (p == MAP_FAILED) { [self error:WSD_BUFFER_ATTACH code:-errno]; return; }
  size_t len = (size_t)maplen;
  id<MTLBuffer> mb = [g_dev newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared
                                         deallocator:^(void *ptr, NSUInteger l) { munmap(ptr, l); }];
  if (!mb) { munmap(p, len); [self error:WSD_BUFFER_ATTACH code:-ENOMEM]; return; }
  MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:a->width
                                                                               height:a->height mipmapped:NO];
  td.usage = MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModeShared;
  id<MTLTexture> tex = [mb newTextureWithDescriptor:td offset:(NSUInteger)delta bytesPerRow:a->stride];
  if (!tex) { [self error:WSD_BUFFER_ATTACH code:-EINVAL]; return; }
  WsdBuffer *b = [WsdBuffer new];
  b.bufferId = a->buffer_id; b.width = a->width; b.height = a->height; b.stride = a->stride; b.format = a->format;
  b.texture = tex;
  b.cpu = (uint8_t *)p + delta;
  self.buffers[@(a->buffer_id)] = b;
  LOG("client %d: buffer %u %ux%u stride %u fmt %u mapped %zu bytes (no copy)", self.pid, a->buffer_id, a->width,
      a->height, a->stride, a->format, len);
}

- (void)debugInject:(WsdWindow *)w scenario:(uint32_t)sc {
  NSWindow *nw = w.window;
  double s = backing_scale(nw);
  NSInteger wn = nw.windowNumber;
  NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
  NSPoint p = NSMakePoint(10 / s, w.view.bounds.size.height - 20 / s); /* pixel (10,20) */
  g_injecting = YES;
  if (sc == 1) {
    NSEvent *kd = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                                  isARepeat:NO keyCode:kVK_ANSI_A];
    NSEvent *ku = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                                  isARepeat:NO keyCode:kVK_ANSI_A];
    NSEvent *sd = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint
                              modifierFlags:NSEventModifierFlagShift timestamp:t windowNumber:wn context:nil
                                 characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:kVK_Shift];
    NSEvent *su = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:0 timestamp:t
                               windowNumber:wn context:nil characters:@"" charactersIgnoringModifiers:@""
                                  isARepeat:NO keyCode:kVK_Shift];
    NSEvent *md = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:1 clickCount:1 pressure:1];
    NSEvent *mu = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:2 clickCount:1 pressure:0];
    NSEvent *rd = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:3 clickCount:1 pressure:1];
    NSEvent *ru = [NSEvent mouseEventWithType:NSEventTypeRightMouseUp location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:4 clickCount:1 pressure:0];
    NSEvent *mm = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:p modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:5 clickCount:0 pressure:0];
    for (NSEvent *e in @[ kd, ku, sd, su, md, mu, rd, ru, mm ]) [nw sendEvent:e];
    CGEventRef sw = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, -3, 1);
    [w.view scrollWheel:[NSEvent eventWithCGEvent:sw]];
    CFRelease(sw);
  } else if (sc == 2) {
    /* Raw delta path: CGEvent mouse-moved carrying deltas, handed to the view. */
    CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(100, 100), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(mv, kCGMouseEventDeltaX, 5);
    CGEventSetIntegerValueField(mv, kCGMouseEventDeltaY, -7);
    [w.view mouseMoved:[NSEvent eventWithCGEvent:mv]];
    CFRelease(mv);
  } else if (sc == 3) {
    /* Motion outside the clip rect: expect clamped coordinates (flag bit 0). */
    NSPoint far = NSMakePoint(w.view.bounds.size.width - 1, 1);
    NSEvent *mm = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:far modifierFlags:0 timestamp:t
                                 windowNumber:wn context:nil eventNumber:6 clickCount:0 pressure:0];
    [nw sendEvent:mm];
  }
  g_injecting = NO;
}

- (void)handle:(uint32_t)type data:(const uint8_t *)d len:(uint32_t)n {
#define NEED(T) if (n < sizeof(struct T)) { [self error:type code:-EMSGSIZE]; return; } const struct T *m = (const void *)d
  switch (type) {
  case WSD_HELLO: {
    NEED(wsd_hello);
    NSScreen *sc = primary_screen();
    double s = backing_scale(nil);
    NSRect vf = sc.visibleFrame; /* without the menu bar and the Dock */
    struct wsd_hello_reply r = {WSD_PROTOCOL_VERSION, getpid(), (uint32_t)lround(sc.frame.size.width * s),
                                (uint32_t)lround(sc.frame.size.height * s), (uint32_t)sc.maximumFramesPerSecond,
                                (uint32_t)lround(s * 1000), (int32_t)lround((vf.origin.x - sc.frame.origin.x) * s),
                                (int32_t)lround((NSMaxY(sc.frame) - NSMaxY(vf)) * s),
                                (uint32_t)lround(vf.size.width * s), (uint32_t)lround(vf.size.height * s)};
    LOG("client %d hello version %u", self.pid, m->version);
    [self send:WSD_EV_HELLO payload:&r len:sizeof(r) extra:nil];
    break;
  }
  case WSD_WINDOW_CREATE: {
    NEED(wsd_window_create);
    NSString *title = [[NSString alloc] initWithBytes:d + sizeof(*m) length:n - sizeof(*m)
                                             encoding:NSUTF8StringEncoding] ?: @"";
    [self createWindow:m title:title];
    break;
  }
  case WSD_WINDOW_DESTROY: {
    NEED(wsd_window);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) { [w destroy]; [self.windows removeObjectForKey:@(m->window_id)]; }
    break;
  }
  case WSD_WINDOW_SHOW: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    if (m->value) {
      [NSApp activate];
      [w.window makeKeyAndOrderFront:nil];
      [w.window orderFrontRegardless]; /* cooperative activation may leave the app inactive */
      [w sendResize];
      [w logState:"show"];
      __weak WsdWindow *ww = w;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1000000000), dispatch_get_main_queue(), ^{ [ww logState:"show+1s"]; });
    } else {
      [w.window orderOut:nil];
    }
    break;
  }
  case WSD_WINDOW_FULLSCREEN: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w setFullscreenMode:m->value != 0];
    break;
  }
  case WSD_CURSOR_VISIBLE: {
    NEED(wsd_window_flag);
    if (!m->value && g_cursor_hidden == 0) { [NSCursor hide]; g_cursor_hidden = 1; }
    if (m->value && g_cursor_hidden) { [NSCursor unhide]; g_cursor_hidden = 0; }
    break;
  }
  case WSD_CURSOR_CLIP: {
    NEED(wsd_cursor_clip);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    w.clip = m->enable && m->width && m->height;
    w.clipPx = NSMakeRect(m->x, m->y, m->width, m->height);
    break;
  }
  case WSD_RELATIVE_MOUSE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    BOOL on = m->value != 0;
    if (on != w.relative) {
      CGError e = CGAssociateMouseAndMouseCursorPosition(!on);
      LOG("client %d: relative mouse %s -> CGAssociateMouseAndMouseCursorPosition(%d) = %d", self.pid,
          on ? "on" : "off", !on, e);
      if (e != kCGErrorSuccess) { [self error:type code:e]; break; }
      w.relative = on;
      g_relative_global = on;
    }
    break;
  }
  case WSD_BUFFER_ATTACH: {
    NEED(wsd_buffer_attach);
    [self attachBuffer:m];
    break;
  }
  case WSD_AUDIO_OPEN: {
    NEED(wsd_audio_open);
    [self audioOpen:m];
    break;
  }
  case WSD_AUDIO_START:
  case WSD_AUDIO_STOP:
  case WSD_AUDIO_RESET:
  case WSD_AUDIO_CLOSE: {
    NEED(wsd_audio_stream);
    WsdAudio *a = self.audio[@(m->stream_id)];
    if (!a) { [self error:type code:-ENOENT]; break; }
    if (type == WSD_AUDIO_START) {
      atomic_store_explicit(&a.rt->running, 1, memory_order_release);
      OSStatus e = AudioOutputUnitStart(a.unit);
      if (e != noErr) { LOG("client %d: audio stream %u: start failed (%d)", self.pid, m->stream_id, (int)e);
                        [self error:type code:-EIO]; }
    } else if (type == WSD_AUDIO_STOP) {
      atomic_store_explicit(&a.rt->running, 0, memory_order_release);
      AudioOutputUnitStop(a.unit);
    } else if (type == WSD_AUDIO_RESET) {
      _Atomic uint64_t *wp = (_Atomic uint64_t *)&a.rt->ring->write_frames;
      atomic_store_explicit((_Atomic uint64_t *)&a.rt->ring->read_frames,
                            atomic_load_explicit(wp, memory_order_acquire), memory_order_release);
    } else {
      [a dispose];
      [self.audio removeObjectForKey:@(m->stream_id)];
    }
    break;
  }
  case WSD_BUFFER_DETACH: {
    NEED(wsd_buffer);
    ((WsdBuffer *)self.buffers[@(m->buffer_id)]).detached = YES;
    [self.buffers removeObjectForKey:@(m->buffer_id)];
    break;
  }
  case WSD_PRESENT: {
    NEED(wsd_present);
    uint64_t rx = now_ns();
    WsdWindow *w = [self win:m->window_id req:type];
    WsdBuffer *b = self.buffers[@(m->buffer_id)];
    if (!w) break;
    if (!b) { [self error:type code:-ENOENT]; break; }
    if (w.pendingBuffer) { /* mailbox: the newer frame replaces the unshown one */
      [w releaseBuffer:w.pendingBuffer serial:w.pendingSerial shown:NO];
      w.replaced++;
    }
    w.pendingBuffer = b;
    w.pendingSerial = m->serial;
    w.pendingClientNs = m->client_time_ns;
    w.pendingReceivedNs = rx;
    break;
  }
  case WSD_DEBUG_INPUT: {
    NEED(wsd_debug_input);
    /* test only (driving a game from a host-side script): the target is the largest visible window of any client */
    WsdWindow *best = nil;
    double area = 0;
    for (WsdClient *c in g_clients)
      for (WsdWindow *w in c.windows.allValues) {
        NSSize sz = w.window.frame.size;
        if ((w.window.visible || (m->kind == 4 && w.window.miniaturized)) && sz.width * sz.height > area) {
          area = sz.width * sz.height;
          best = w;
        }
      }
    if (!best) { [self error:type code:-ENOENT]; break; }
    NSWindow *nw = best.window;
    if (m->kind == 4) {
      /* the AppKit calls behind the title bar, its buttons and the Dock: the delegate sees what a user does */
      double top = NSMaxY(primary_screen().frame);
      NSRect f = nw.frame;
      switch (m->a) {
      case WSD_DEBUG_WINDOW_MOVE: [nw setFrameTopLeftPoint:NSMakePoint(m->b, top - m->reserved)]; break;
      case WSD_DEBUG_WINDOW_SIZE: {
        NSRect c = [nw contentRectForFrameRect:f];
        c.origin.y += c.size.height - m->reserved;
        c.size = NSMakeSize(m->b, m->reserved);
        [nw setFrame:[nw frameRectForContentRect:c] display:YES];
        break;
      }
      case WSD_DEBUG_WINDOW_MINIMIZE: [nw performMiniaturize:nil]; break;
      case WSD_DEBUG_WINDOW_RESTORE: [nw deminiaturize:nil]; break;
      case WSD_DEBUG_WINDOW_ZOOM: [nw performZoom:nil]; break;
      case WSD_DEBUG_WINDOW_CLOSE: [nw performClose:nil]; break;
      case WSD_DEBUG_APP_QUIT: [NSApp terminate:nil]; break;
      }
      LOG("debug window action %u (%u, %u) -> window %u", m->a, m->b, m->reserved, best.windowId);
      break;
    }
    NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
    /* the first test input turns the probe on when WSD_LATENCY_PROBE was not set (the launcher passes a fixed
     * environment); it arms nothing itself: the probe needs earlier frames for its base and noise */
    if (!g_probe_enabled) {
      g_probe_enabled = YES;
      LOG("latency probe on (first test input)");
    } else if (m->kind != 3 && (m->kind == 2 || m->b)) probe_arm(m->kind == 1 ? "key" : "click");
    g_injecting = YES;
    if (m->kind == 3) {
      /* raw mouse motion (relative mode test): delta x = a - 10000, delta y = b - 10000, through the view as a real
       * mouse-moved event carrying deltas */
      CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(100, 100), kCGMouseButtonLeft);
      CGEventSetIntegerValueField(mv, kCGMouseEventDeltaX, (int64_t)m->a - 10000);
      CGEventSetIntegerValueField(mv, kCGMouseEventDeltaY, (int64_t)m->b - 10000);
      [best.view mouseMoved:[NSEvent eventWithCGEvent:mv]];
      CFRelease(mv);
      g_injecting = NO;
      break;
    }
    if (m->kind == 1) {
      NSEvent *e = [NSEvent keyEventWithType:m->b ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
                               modifierFlags:0 timestamp:t windowNumber:nw.windowNumber context:nil characters:@""
                 charactersIgnoringModifiers:@"" isARepeat:NO keyCode:(unsigned short)m->a];
      [nw sendEvent:e];
    } else if (m->kind == 2) {
      double sc = backing_scale(nw);
      NSPoint p = NSMakePoint(m->a / sc, best.view.bounds.size.height - m->b / sc);
      for (int i = 0; i < 2; i++) {
        NSEvent *e = [NSEvent mouseEventWithType:i ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown location:p
                                   modifierFlags:0 timestamp:t windowNumber:nw.windowNumber context:nil
                                     eventNumber:100 + i clickCount:1 pressure:i ? 0 : 1];
        [nw sendEvent:e];
      }
    }
    g_injecting = NO;
    LOG("debug input kind %u a %u b %u -> window %u", m->kind, m->a, m->b, best.windowId);
    break;
  }
  case WSD_PRESENT_REGION: {
    NEED(wsd_present_region);
    uint64_t rx = now_ns();
    WsdWindow *w = [self win:m->present.window_id req:type];
    if (!w) break;
    if (!m->width || !m->height) { [w removeOverlay]; break; }
    WsdBuffer *b = self.buffers[@(m->present.buffer_id)];
    if (!b) { [self error:type code:-ENOENT]; break; }
    WsdWindow *o = [w overlayAt:NSMakeRect(m->x, m->y, m->width, m->height)];
    if (o.pendingBuffer) {
      [o releaseBuffer:o.pendingBuffer serial:o.pendingSerial shown:NO];
      o.replaced++;
    }
    probe_frame(b, m->present.client_time_ns, rx);
    o.pendingBuffer = b;
    o.pendingSerial = m->present.serial;
    o.pendingClientNs = m->present.client_time_ns;
    o.pendingReceivedNs = rx;
    break;
  }
  case WSD_SET_TITLE: {
    NEED(wsd_window);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w)
      w.window.title = [[NSString alloc] initWithBytes:d + sizeof(*m) length:n - sizeof(*m)
                                              encoding:NSUTF8StringEncoding] ?: @"";
    break;
  }
  case WSD_SET_FRAME_RATE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    float hz = (float)m->value;
    w.link.preferredFrameRateRange = hz > 0 ? CAFrameRateRangeMake(hz, hz, hz) : CAFrameRateRangeDefault;
    LOG("client %d: window %u preferred frame rate %u Hz", self.pid, m->window_id, m->value);
    break;
  }
  case WSD_PING: {
    NEED(wsd_ping);
    struct wsd_ping r = *m;
    r.time_ns = now_ns();
    [self send:WSD_EV_PONG payload:&r len:sizeof(r) extra:nil];
    break;
  }
  case WSD_DEBUG_INJECT: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [self debugInject:w scenario:m->value];
    break;
  }
  case WSD_WINDOW_SET_FRAME: {
    NEED(wsd_window_frame);
    WsdWindow *w = [self win:m->window_id req:type];
    if (!w) break;
    if (((m->flags & WSD_FRAME_SIZE) && (!m->width || !m->height || m->width > 16384 || m->height > 16384)) ||
        w.fullscreen) {
      [self error:type code:-EINVAL];
      break;
    }
    [w setFramePx:m];
    break;
  }
  case WSD_WINDOW_SET_STYLE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w setStyle:m->value];
    break;
  }
  case WSD_WINDOW_MINIMIZE: {
    NEED(wsd_window_flag);
    WsdWindow *w = [self win:m->window_id req:type];
    if (w) [w minimize:m->value != 0];
    break;
  }
  case WSD_APP_ICON: {
    NEED(wsd_app_icon);
    [self appIcon:m];
    break;
  }
  case WSD_QUIT:
    LOG("client %d asked the server to quit", self.pid);
    restore_input_state();
    [self flush];
    exit(0);
  default:
    [self error:type code:-ENOSYS];
  }
#undef NEED
}

- (void)readable {
  uint8_t buf[16384];
  char ctl[CMSG_SPACE(sizeof(int) * 4)];
  struct iovec iov = {buf, sizeof(buf)};
  struct msghdr mh = {0};
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = ctl; mh.msg_controllen = sizeof(ctl);
  ssize_t r = recvmsg(self.fd, &mh, 0);
  if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { [self close]; return; }
  if (r < 0) return;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < nfd; i++) {
        int f;
        memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        if (self.fdQueue.count >= 8) close(f); else [self.fdQueue addObject:@(f)];
      }
    }
  [self.inbuf appendBytes:buf length:(NSUInteger)r];
  NSUInteger off = 0;
  while (!self.dead && self.inbuf.length - off >= sizeof(struct wsd_hdr)) {
    struct wsd_hdr h;
    memcpy(&h, (const uint8_t *)self.inbuf.bytes + off, sizeof(h));
    if (h.len > WSD_MAX_PAYLOAD) { LOG("client %d: oversized message", self.pid); [self close]; return; }
    if (self.inbuf.length - off < sizeof(h) + h.len) break;
    uint8_t payload[WSD_MAX_PAYLOAD];
    memcpy(payload, (const uint8_t *)self.inbuf.bytes + off + sizeof(h), h.len);
    off += sizeof(h) + h.len;
    [self handle:h.type data:payload len:h.len];
  }
  if (!self.dead) [self.inbuf replaceBytesInRange:NSMakeRange(0, off) withBytes:NULL length:0];
}
@end

/* ---------------------------------------------------------------- main */

/* Quit (Cmd-Q, the Dock) asks the game to close its windows, as the close button does, instead of ending the server
 * under a running game (which then lost its windows and input but kept running); whoever started the game ends the
 * server after it. Without any window, Quit ends the server. */
@interface WsdAppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation WsdAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
  NSUInteger asked = 0;
  for (WsdClient *c in g_clients)
    for (WsdWindow *w in c.windows.allValues) {
      struct wsd_window m = {w.windowId};
      [w sendEvent:WSD_EV_CLOSE payload:&m len:sizeof(m) extra:nil];
      asked++;
    }
  if (!asked) return NSTerminateNow;
  LOG("quit requested: asked %lu windows to close", (unsigned long)asked);
  return NSTerminateCancel;
}
@end

/* The application menu: the bundle's name (Obelisk starts wsd from a bundle named after the game), Hide and Quit. */
static void make_menu(void) {
  NSString *name = NSBundle.mainBundle.infoDictionary[@"CFBundleName"] ?: NSProcessInfo.processInfo.processName;
  BOOL fr = [NSLocale.preferredLanguages.firstObject hasPrefix:@"fr"];
  NSMenu *bar = [NSMenu new], *menu = [[NSMenu alloc] initWithTitle:name];
  NSMenuItem *item = [NSMenuItem new];
  [menu addItemWithTitle:[(fr ? @"Masquer " : @"Hide ") stringByAppendingString:name] action:@selector(hide:)
           keyEquivalent:@"h"];
  [menu addItem:NSMenuItem.separatorItem];
  [menu addItemWithTitle:[(fr ? @"Quitter " : @"Quit ") stringByAppendingString:name] action:@selector(terminate:)
           keyEquivalent:@"q"];
  item.submenu = menu;
  [bar addItem:item];
  NSApp.mainMenu = bar;
}

static int make_listener(const char *dir, char *path_out, size_t path_cap) {
  if (mkdir(dir, 0700) && errno != EEXIST) { LOG("mkdir %s: %s", dir, strerror(errno)); return -1; }
  struct stat st;
  if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
    LOG("refusing %s: must be a directory owned by uid %d with mode 0700", dir, getuid());
    return -1;
  }
  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  int n = snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/wsd.sock", dir);
  if (n < 0 || (size_t)n >= sizeof(sa.sun_path)) { LOG("socket path too long (limit %zu)", sizeof(sa.sun_path) - 1); return -1; }
  unlink(sa.sun_path);
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) || chmod(sa.sun_path, 0600) || listen(s, 8)) {
    LOG("listen %s: %s", sa.sun_path, strerror(errno));
    return -1;
  }
  fcntl(s, F_SETFL, O_NONBLOCK);
  snprintf(path_out, path_cap, "%s", sa.sun_path);
  return s;
}

static void accept_one(int ls) {
  int fd = accept(ls, NULL, NULL);
  if (fd < 0) return;
  uid_t uid; gid_t gid;
  pid_t pid = -1;
  socklen_t pl = sizeof(pid);
  getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl);
  if (getpeereid(fd, &uid, &gid) || uid != getuid()) {
    LOG("rejecting peer pid %d uid %d (server uid %d)", pid, (int)uid, (int)getuid());
    close(fd);
    return;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  fcntl(fd, F_SETFL, O_NONBLOCK);
  WsdClient *c = [WsdClient new];
  c.fd = fd; c.pid = pid;
  c.inbuf = [NSMutableData data]; c.outbuf = [NSMutableData data];
  c.fdQueue = [NSMutableArray array];
  c.windows = [NSMutableDictionary dictionary]; c.buffers = [NSMutableDictionary dictionary];
  c.audio = [NSMutableDictionary dictionary];
  c.readSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, dispatch_get_main_queue());
  __weak WsdClient *wc = c;
  dispatch_source_set_event_handler(c.readSrc, ^{ [wc readable]; });
  dispatch_resume(c.readSrc);
  [g_clients addObject:c];
  LOG("client pid %d uid %d accepted", pid, (int)uid);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
      else if (!strcmp(argv[i], "--exit-on-disconnect")) g_exit_on_disconnect = YES;
    }
    if (!dir) { fprintf(stderr, "usage: wsd --dir <private-dir> [--exit-on-disconnect]\n"); return 2; }
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_probe_enabled = getenv("WSD_LATENCY_PROBE") != NULL;

    g_dev = MTLCreateSystemDefaultDevice();
    g_queue = [g_dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShader options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_pipe = lib ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    if (!g_pipe) { LOG("Metal setup failed: %s", err.localizedDescription.UTF8String); return 1; }

    static char path[128];
    int ls = make_listener(dir, path, sizeof(path));
    if (ls < 0) return 1;
    g_clients = [NSMutableSet set];

    /* Termination (timeout wrapper sends SIGALRM): restore pointer state, then exit. */
    static dispatch_source_t sigsrc[4];
    int sigs[4] = {SIGTERM, SIGINT, SIGALRM, SIGHUP};
    for (int i = 0; i < 4; i++) {
      signal(sigs[i], SIG_IGN);
      sigsrc[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, dispatch_get_main_queue());
      int sn = sigs[i];
      dispatch_source_set_event_handler(sigsrc[i], ^{
        LOG("signal %d, restoring input state and exiting", sn);
        restore_input_state();
        unlink(path);
        exit(0);
      });
      dispatch_resume(sigsrc[i]);
    }
    atexit(restore_input_state);
    /* a Quit from the Dock or the app menu ends the process through -terminate: say so in the log */
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationWillTerminateNotification object:nil
                                                       queue:nil usingBlock:^(NSNotification *note) {
      LOG("application quit requested (Dock or menu), restoring input state");
      restore_input_state();
    }];

    [NSApplication sharedApplication];
    static WsdAppDelegate *delegate;
    delegate = [WsdAppDelegate new];
    NSApp.delegate = delegate;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    make_menu();
    [NSApp finishLaunching];

    static dispatch_source_t acc;
    acc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)ls, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(acc, ^{ accept_one(ls); });
    dispatch_resume(acc);

    LOG("listening on %s (pid %d, device %s, max %ld Hz)", path, getpid(), g_dev.name.UTF8String,
        (long)primary_screen().maximumFramesPerSecond);
    [NSApp run];
  }
  return 0;
}
