// Witness (p): instructions Wine's 64-bit code uses that touch segment state or the processor id: rdtscp and rdpid
// (TSC_AUX = (node << 12) | cpu, here a small cpu index), reload of %ds and %es with user selectors, and iretq back to
// the same 64-bit code segment (Wine's __wine_syscall_dispatcher returns that way).
#include "rt.h"
#define SYS_getcpu_ 309

static int run(void) {
  puts_("w16_segtsc: rdtscp, rdpid, segment reloads, iretq\n");
  u32 lo, hi, aux;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  u32 lo2, hi2, aux2;
  __asm__ volatile("rdtscp" : "=a"(lo2), "=d"(hi2), "=c"(aux2));
  u64 t1 = ((u64)hi << 32) | lo, t2 = ((u64)hi2 << 32) | lo2;
  checkb("rdtscp: the counter advances", t2 > t1);
  checkb("rdtscp: TSC_AUX is a cpu index below 4096 (node 0)", aux < 4096 && aux2 < 4096);
  u32 cpu = 0, node = 0;
  check64("getcpu works", (u64)sys3(SYS_getcpu_, (i64)&cpu, (i64)&node, 0), 0);
  checkb("getcpu cpu below 4096", cpu < 4096);
  u64 pid_aux;
  __asm__ volatile("rdpid %0" : "=r"(pid_aux));
  checkb("rdpid: cpu index below 4096", pid_aux < 4096);

  u16 ds, es, cs, ss;
  __asm__ volatile("movw %%ds, %0\n\tmovw %%es, %1\n\tmovw %%cs, %2\n\tmovw %%ss, %3" : "=r"(ds), "=r"(es), "=r"(cs), "=r"(ss));
  check64("cs is the 64-bit user selector", cs, 0x33);
  // reload %ds and %es with the stack selector and then the null selector, as Wine's leave_handler does
  __asm__ volatile("movw %0, %%ds\n\tmovw %0, %%es" : : "r"((u16)0x2b));
  u16 ds2; __asm__ volatile("movw %%ds, %0" : "=r"(ds2));
  check64("ds reloaded with 0x2b", ds2, 0x2b);
  __asm__ volatile("movw %0, %%ds\n\tmovw %0, %%es" : : "r"((u16)0));
  __asm__ volatile("movw %%ds, %0" : "=r"(ds2));
  check64("ds reloaded with the null selector", ds2, 0);

  // iretq to a label in the same code segment with the same stack
  u64 marker = 0;
  __asm__ volatile(
    "movq %%rsp, %%rax\n\t"
    "pushq %q[ss]\n\t"
    "pushq %%rax\n\t"
    "pushfq\n\t"
    "pushq %q[cs]\n\t"
    "leaq 1f(%%rip), %%rax\n\t"
    "pushq %%rax\n\t"
    "iretq\n\t"
    "1: movq $0x1234, %[m]\n\t"
    : [m] "=r"(marker) : [cs] "r"((u64)cs), [ss] "r"((u64)ss) : "rax", "memory", "cc");
  check64("iretq returned to the label", marker, 0x1234);
  return finish("w16_segtsc");
}
WITNESS_MAIN(run)
