#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static double now(clockid_t k){struct timespec t;clock_gettime(k,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *waiter(void *a){
  for(int i=0;i<10;i++){
    struct timespec d; clock_gettime(CLOCK_REALTIME,&d);
    d.tv_nsec += 100*1000000; if(d.tv_nsec>=1000000000){d.tv_sec++;d.tv_nsec-=1000000000;}
    pthread_mutex_lock(&m); double t0=now(CLOCK_MONOTONIC);
    int rc=pthread_cond_timedwait(&c,&m,&d);
    pthread_mutex_unlock(&m);
    printf("thread %ld wait %d: rc=%d (%s) %.1f ms\n",(long)a,i,rc,rc==ETIMEDOUT?"ETIMEDOUT":"other",(now(CLOCK_MONOTONIC)-t0)*1e3);
    fflush(stdout);
  }
  return 0;
}
int main(void){
  printf("realtime %.3f monotonic %.3f\n",now(CLOCK_REALTIME),now(CLOCK_MONOTONIC));
  pthread_t t[3]; for(long i=0;i<3;i++) pthread_create(&t[i],0,waiter,(void*)i);
  for(int i=0;i<3;i++) pthread_join(t[i],0);
  return 0;
}
