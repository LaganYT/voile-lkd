// Witness (f): self-modifying code. Generated code is rewritten between executions and must be re-translated.
// The expected results are the values the freshly written instructions return on real hardware.
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20 };
#define K16 16384UL
typedef i64 (*fn_t)(void);

static u8* map_rwx(u64 len) { i64 r = sys6(SYS_mmap, 0, (i64)len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return (r < 0 && r > -4096) ? 0 : (u8*)r; }
static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
// mov eax, imm32 ; ret
static void emit_ret_imm(u8* p, u32 v) { p[0] = 0xb8; put32(p + 1, v); p[5] = 0xc3; }

static int run(void) {
  puts_("w06_smc: self-modifying code and translation invalidation\n");
  u8* p = map_rwx(2 * K16);
  checkb("RWX mmap works", p != 0);
  if (!p) return finish("w06_smc");

  // 1. rewrite the same code between calls
  emit_ret_imm(p, 42);
  check64("first execution", (u64)((fn_t)p)(), 42);
  emit_ret_imm(p, 99);
  check64("after rewriting the immediate", (u64)((fn_t)p)(), 99);
  { int ok = 1; for (u32 i = 0; i < 200; i++) { emit_ret_imm(p, 1000 + i); if ((u32)((fn_t)p)() != 1000 + i) ok = 0; } checkb("200 rewrite/execute rounds all return the new value", ok); }

  // 2. a different instruction at the same address
  { u8 c[] = {0x48, 0xc7, 0xc0, 0x07, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc0, 0x05, 0xc3}; for (u32 i = 0; i < sizeof c; i++) p[i] = c[i]; /* mov rax,7 ; add rax,5 ; ret */
    check64("different code at the same address", (u64)((fn_t)p)(), 12); }

  // 3. patch an instruction of the block that is running right now (store and patched instruction in one basic block)
  { u8* q = p + 256;
    // movabs rax, <q+14> ; mov byte [rax], 0x2b ; mov eax, 0x11 ; ret
    q[0] = 0x48; q[1] = 0xb8; u64 target = (u64)(q + 14); for (int i = 0; i < 8; i++) q[2 + i] = (u8)(target >> (8 * i));
    q[10] = 0xc6; q[11] = 0x00; q[12] = 0x2b;
    q[13] = 0xb8; q[14] = 0x11; q[15] = 0; q[16] = 0; q[17] = 0; q[18] = 0xc3;
    check64("store into the very next instruction of the running block", (u64)((fn_t)q)(), 0x2b);
    // run it again after resetting the immediate: patched at run time again
    q[14] = 0x11; check64("same self-patching code, second run", (u64)((fn_t)q)(), 0x2b); }

  // 4. data written in the same 16 KiB host page as translated code but in a different 4 KiB guest page
  { u8* code = p + K16; emit_ret_imm(code, 5);
    check64("code in the second host page runs", (u64)((fn_t)code)(), 5);
    volatile u64* data = (volatile u64*)(code + 0x2000); *data = 0x1234; check64("data write next to translated code succeeds", *data, 0x1234);
    check64("the code still runs after the neighbouring data write", (u64)((fn_t)code)(), 5);
    emit_ret_imm(code, 6); check64("and is re-translated after a rewrite", (u64)((fn_t)code)(), 6); }

  // 5. mprotect cycle: RW -> write -> R+X -> execute, repeated with new code
  { u8* m = map_rwx(K16); sys3(SYS_mprotect, (i64)m, K16, PROT_READ | PROT_WRITE);
    int ok = 1; for (u32 i = 0; i < 20; i++) {
      sys3(SYS_mprotect, (i64)m, K16, PROT_READ | PROT_WRITE); emit_ret_imm(m, 500 + i);
      sys3(SYS_mprotect, (i64)m, K16, PROT_READ | PROT_EXEC);
      if ((u32)((fn_t)m)() != 500 + i) ok = 0; }
    checkb("20 mprotect RW/RX cycles each execute the newest code", ok); }

  // 6. munmap then map different code at the same address
  { u8* m = map_rwx(K16); emit_ret_imm(m, 111); check64("code before munmap", (u64)((fn_t)m)(), 111);
    sys2(SYS_munmap, (i64)m, K16);
    i64 r = sys6(SYS_mmap, (i64)m, K16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    check64("mmap MAP_FIXED at the unmapped address", (u64)r, (u64)m);
    emit_ret_imm(m, 222); check64("new code after remapping the same address", (u64)((fn_t)m)(), 222); }
  // 7. MAP_FIXED over live translated code (no munmap in between)
  { u8* m = map_rwx(K16); emit_ret_imm(m, 31); check64("code before the MAP_FIXED overwrite", (u64)((fn_t)m)(), 31);
    sys6(SYS_mmap, (i64)m, K16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    emit_ret_imm(m, 32); check64("new code after MAP_FIXED over translated code", (u64)((fn_t)m)(), 32); }
  // 8. patching a call target inside a loop that is already hot
  { u8* m = map_rwx(K16);
    // loop: mov ecx, 50 ; xor eax,eax ; L: call fn ; add rax? -> keep simple: sum of 50 calls to a function returning the patched value
    u8* fn = m + 128; emit_ret_imm(fn, 1);
    u8 *c = m; u32 n = 0;
    c[n++] = 0x48; c[n++] = 0x31; c[n++] = 0xdb;                               /* xor rbx, rbx */
    c[n++] = 0xb9; put32(c + n, 50); n += 4;                                  /* mov ecx, 50 */
    u32 loop = n;
    c[n++] = 0x51;                                                              /* push rcx */
    c[n++] = 0xe8; put32(c + n, (u32)((i32)((fn - m) - (n + 4)))); n += 4;     /* call fn */
    c[n++] = 0x59;                                                              /* pop rcx */
    c[n++] = 0x48; c[n++] = 0x01; c[n++] = 0xc3;                               /* add rbx, rax */
    c[n++] = 0xe2; c[n] = (u8)(i8)(loop - (n + 1)); n++;                        /* loop L */
    c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0xd8;                               /* mov rax, rbx */
    c[n++] = 0xc3;
    check64("hot loop calling a function returning 1, 50 times", (u64)((fn_t)m)(), 50);
    emit_ret_imm(fn, 3); check64("callee rewritten between runs, 50 calls", (u64)((fn_t)m)(), 150);
    emit_ret_imm(fn, 7); check64("callee rewritten again", (u64)((fn_t)m)(), 350); }
  return finish("w06_smc");
}
WITNESS_MAIN(run)
