/* w22_mutexsig: glibc mutexes and condvars under a barrage of asynchronous signals (the fexdarwin kick path).
 * 4 workers lock/unlock one mutex (and timed-wait a condvar now and then); a signaller tgkills SIGUSR1 at them.
 * Checks: no self-deadlock (watchdog), counter exact, owner field consistent. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#define NW 4
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static volatile long counter, sigs;
static volatile int stop;
static volatile pid_t tids[NW];
static volatile long progress[NW];
static void h(int s){ (void)s; __atomic_add_fetch(&sigs,1,__ATOMIC_RELAXED); }
static void *worker(void *a){
  long i=(long)a; tids[i]=syscall(SYS_gettid);
  for(long n=0; !stop; n++){
    pthread_mutex_lock(&m);
    counter++;
    if((n & 255)==0){ struct timespec d; clock_gettime(CLOCK_REALTIME,&d); d.tv_nsec+=200000; if(d.tv_nsec>=1000000000){d.tv_sec++;d.tv_nsec-=1000000000;}
      pthread_cond_timedwait(&c,&m,&d); }
    if((n & 63)==0) pthread_cond_broadcast(&c);
    pthread_mutex_unlock(&m);
    progress[i]=n;
  }
  return 0;
}
int main(int argc,char**argv){
  int secs = argc>1?atoi(argv[1]):10;
  struct sigaction sa={0}; sa.sa_handler=h; sa.sa_flags=SA_RESTART; sigaction(SIGUSR1,&sa,0);
  pthread_t t[NW]; for(long i=0;i<NW;i++) pthread_create(&t[i],0,worker,(void*)i);
  usleep(100000);
  pid_t pid=getpid(); long last[NW]={0}; int stuck=0;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
  for(int k=0;;k++){
    for(int i=0;i<NW;i++) syscall(SYS_tgkill,pid,tids[i],SIGUSR1);
    usleep(200);
    if(k%2500==0){ /* every ~0.5 s: progress check */
      int dead=0; for(int i=0;i<NW;i++){ if(progress[i]==last[i]) dead++; last[i]=progress[i]; }
      if(dead==NW){ if(++stuck>=4){ printf("FAIL: all workers stuck; lock word %d owner %d counter %ld sigs %ld\n", *(int*)&m, ((int*)&m)[2], counter, sigs); fflush(stdout); _exit(1);} } else stuck=0;
      struct timespec t1; clock_gettime(CLOCK_MONOTONIC,&t1);
      if(t1.tv_sec-t0.tv_sec>=secs) break;
    }
  }
  stop=1; pthread_mutex_lock(&m); pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
  for(int i=0;i<NW;i++) pthread_join(t[i],0);
  long sum=0; for(int i=0;i<NW;i++) sum+=progress[i]+1;
  printf("%s: counter %ld (expected %ld) signals %ld\n", counter==sum?"PASS":"FAIL", counter, sum, sigs);
  return counter==sum?0:1;
}
