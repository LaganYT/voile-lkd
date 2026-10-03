// Witness (request wine-7): code in a memfd seen through two views of the same bytes, as the EAC module of the
// game does. View B: PROT_READ|PROT_EXEC, MAP_SHARED|MAP_FIXED_NOREPLACE at an address 0x3000 into a 16 KiB
// page, offset 0 (EINVAL on LKD before the memfd phase). View A: PROT_READ|PROT_WRITE, MAP_SHARED, address
// chosen by the kernel. Code written through A is executed through B; rewritten through A, B must run the new
// code (translations of B dropped on the store through A). A pwrite through the descriptor counts too.
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_SHARED = 1, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20,
       MAP_FIXED_NOREPLACE = 0x100000, SYS_memfd_create = 319, SYS_ftruncate = 77, SYS_pwrite64 = 18, SYS_close = 3 };
typedef i64 (*fn_t)(void);

static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void emit_ret_imm(u8* p, u32 v) { p[0] = 0xb8; put32(p + 1, v); p[5] = 0xc3; }   // mov eax, imm32 ; ret

static int run(void) {
  puts_("w19_memfd_alias: memfd code through a RW view and an RX view\n");
  i64 fd = sys3(SYS_memfd_create, (i64)"w19", 1 /* MFD_CLOEXEC */, 0);
  checkb("memfd_create", fd >= 0);
  check64("ftruncate 0x1000", (u64)sys3(SYS_ftruncate, fd, 0x1000, 0), 0);
  // a free 64 KiB region, then the RX view 0x7000 into it (0x3000 into its 16 KiB page)
  i64 r = sys6(SYS_mmap, 0, 0x10000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  sys3(SYS_munmap, r, 0x10000, 0);
  u64 bx = (u64)r + 0x7000;
  i64 b = sys6(SYS_mmap, (i64)bx, 0x1000, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
  check64("RX view at 0x3000 into its host page, offset 0", (u64)b, bx);
  i64 a = sys6(SYS_mmap, 0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  checkb("RW view", a > 0 || a < -4096);
  if (b != (i64)bx || (a < 0 && a > -4096)) { sys1(SYS_close, fd); return finish("w19_memfd_alias"); }
  u8* A = (u8*)a; fn_t B = (fn_t)b;
  emit_ret_imm(A, 1);
  check64("written through A, executed through B", (u64)B(), 1);
  emit_ret_imm(A, 2);
  check64("rewritten through A, B runs the new code", (u64)B(), 2);
  { int ok = 1; for (u32 i = 0; i < 100; i++) { emit_ret_imm(A, 100 + i); if ((u32)B() != 100 + i) ok = 0; } checkb("100 rewrite/execute rounds", ok); }
  { u8 code[6]; emit_ret_imm(code, 7); sys6(SYS_pwrite64, fd, (i64)code, 6, 0, 0, 0); }
  check64("pwrite through the descriptor, B runs the new code", (u64)B(), 7);
  sys1(SYS_close, fd);
  return finish("w19_memfd_alias");
}
WITNESS_MAIN(run)
