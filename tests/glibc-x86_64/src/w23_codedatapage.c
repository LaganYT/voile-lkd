/* w23_codedatapage: a glibc mutex in a 4 KiB rw- guest page that shares its 16 KiB host page with an r-x guest page
 * whose code runs all the time (nptunix.so layout: .text tail + .bss in one host page). Under fexdarwin "mtrack" the
 * host page is write-protected after each translation, so every store to the mutex goes through the SMC write fault.
 * Checks: no deadlock (watchdog), exact counter, owner field 0 at the end. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define NW 4
static pthread_mutex_t *m;
static volatile long *counter;
static volatile int stop;
static volatile long progress[NW + 2];
typedef long (*fn_t)(long);
static fn_t code;
/* x86-64: lea rax,[rdi+1]; ret */
static const unsigned char body[] = { 0x48, 0x8d, 0x47, 0x01, 0xc3 };
static void *runner(void *a){ long i=(long)a, x=0; while(!stop){ x = code(x); if(!(x & 1023)) progress[i] = x; } return 0; }
static void *worker(void *a){
  long i=(long)a;
  for(long n=0; !stop; n++){ pthread_mutex_lock(m); (*counter)++; pthread_mutex_unlock(m); progress[i]=n; }
  return 0;
}
int main(int argc,char**argv){
  int secs = argc>1?atoi(argv[1]):10;
  unsigned char *p = mmap(0, 4 * 16384, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  unsigned char *b = (unsigned char *)(((unsigned long)p + 16383) & ~16383ul); /* 16 KiB aligned */
  memcpy(b + 0x1000 - 64, body, sizeof body);        /* code at the end of guest page 0 */
  code = (fn_t)(b + 0x1000 - 64);
  printf("step map %p\n", b); fflush(stdout);
  mprotect(b, 0x1000, PROT_READ|PROT_EXEC);           /* guest page 0: r-x (code), pages 2-3: rw- (data) */
  m = (pthread_mutex_t *)(b + 0x2000 + 0x5a0);
  counter = (volatile long *)(b + 0x3000 + 0x10);
  pthread_mutex_init(m, 0);
  printf("step init, code(41)=%ld\n", code(41)); fflush(stdout);
  pthread_t t[NW + 2];
  for(long i=0;i<2;i++) pthread_create(&t[i],0,runner,(void*)i);
  for(long i=2;i<NW+2;i++) pthread_create(&t[i],0,worker,(void*)i);
  long last[NW+2]={0}; int stuck=0;
  printf("step threads\n"); fflush(stdout);
  for(int k=0;k<secs*4;k++){
    usleep(250000);
    int dead=0; for(int i=2;i<NW+2;i++){ if(progress[i]==last[i]) dead++; last[i]=progress[i]; }
    if(dead==NW){ if(++stuck>=8){ printf("FAIL: workers stuck: lock word %d owner %d counter %ld\n", ((int*)m)[0], ((int*)m)[2], *counter); fflush(stdout); _exit(1);} } else stuck=0;
  }
  stop=1;
  for(int i=0;i<NW+2;i++) pthread_join(t[i],0);
  long sum=0; for(int i=2;i<NW+2;i++) sum+=progress[i]+1;
  int ok = *counter==sum && ((int*)m)[0]==0 && ((int*)m)[2]==0;
  printf("%s: counter %ld (expected %ld) lock word %d owner %d\n", ok?"PASS":"FAIL", *counter, sum, ((int*)m)[0], ((int*)m)[2]);
  return !ok;
}
