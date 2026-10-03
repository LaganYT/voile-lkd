// SPDX-License-Identifier: MIT
// engine-top: CPU of every thread of the engine's native processes (wsd, the render server) and of every
// fexdarwin-run process, plus GPU utilization, every INTERVAL seconds. It reads kernel counters (proc_pidinfo,
// IOAccelerator statistics) and never suspends a thread, unlike sample(1), which halves the game's frame rate.
//
// Build: clang -O2 -o engine-top engine-top.c -framework IOKit -framework CoreFoundation
// Usage: engine-top [INTERVAL] [COUNT]   (default 5 s, until stopped)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 4096

struct thread_row {
    int pid;
    uint64_t id;
    uint64_t cpu_ns;
    char name[64];
};

static struct thread_row prev[MAX_THREADS], cur[MAX_THREADS];
static int n_prev, n_cur;

static int watched(const char *name) {
    return !strcmp(name, "wsd") || !strncmp(name, "virgl_", 6) || !strcmp(name, "fexdarwin-run");
}

static void sample_process(int pid, const char *pname) {
    uint64_t ids[1024];
    int n = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, ids, sizeof ids);
    if (n <= 0) return;
    n /= (int)sizeof(uint64_t);
    for (int i = 0; i < n && n_cur < MAX_THREADS; i++) {
        struct proc_threadinfo ti;
        if (proc_pidinfo(pid, PROC_PIDTHREADINFO, ids[i], &ti, sizeof ti) != sizeof ti) continue;
        struct thread_row *r = &cur[n_cur++];
        r->pid = pid;
        r->id = ids[i];
        r->cpu_ns = ti.pth_user_time + ti.pth_system_time;
        snprintf(r->name, sizeof r->name, "%s/%s", pname, ti.pth_name[0] ? ti.pth_name : "-");
    }
}

static double gpu_utilization(void) {
    double u = -1;
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return u;
    io_object_t s;
    while ((s = IOIteratorNext(it))) {
        CFDictionaryRef stats = IORegistryEntryCreateCFProperty(s, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (stats) {
            CFNumberRef v = CFDictionaryGetValue(stats, CFSTR("Device Utilization %"));
            int64_t x;
            if (v && CFNumberGetValue(v, kCFNumberSInt64Type, &x)) u = (double)x;
            CFRelease(stats);
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return u;
}

static int by_cpu(const void *a, const void *b) {
    const double *x = a, *y = b;
    return x[0] < y[0] ? 1 : x[0] > y[0] ? -1 : 0;
}

int main(int argc, char **argv) {
    const double interval = argc > 1 ? atof(argv[1]) : 5;
    const int count = argc > 2 ? atoi(argv[2]) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int round = 0; !count || round <= count; round++) {
        n_cur = 0;
        int pids[4096];
        int np = proc_listallpids(pids, sizeof pids);
        for (int i = 0; i < np; i++) {
            char name[2 * MAXCOMLEN + 1] = "";
            if (proc_name(pids[i], name, sizeof name) <= 0 || !watched(name)) continue;
            char label[80];
            snprintf(label, sizeof label, "%s[%d]", name, pids[i]);
            sample_process(pids[i], label);
        }
        const double gpu = gpu_utilization();
        if (round) {
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            // rows: cpu ms, index into cur
            static double rows[MAX_THREADS][2];
            int nr = 0;
            double total = 0;
            for (int i = 0; i < n_cur; i++) {
                uint64_t was = 0;
                for (int j = 0; j < n_prev; j++)
                    if (prev[j].pid == cur[i].pid && prev[j].id == cur[i].id) { was = prev[j].cpu_ns; break; }
                if (was > cur[i].cpu_ns) was = 0; /* the handle now names a new thread */
                const double ms = (cur[i].cpu_ns - was) / 1e6;
                total += ms;
                if (ms >= 0.5) { rows[nr][0] = ms; rows[nr][1] = i; nr++; }
            }
            qsort(rows, (size_t)nr, sizeof rows[0], by_cpu);
            printf("engine-top %02d:%02d:%02d interval %.1f s: GPU %.0f %%, engine threads %.1f ms CPU (%.2f cores)\n",
                   lt.tm_hour, lt.tm_min, lt.tm_sec, interval, gpu, total, total / (interval * 1000));
            for (int k = 0; k < nr && k < 25; k++)
                printf("engine-top   %7.1f ms %5.1f %%  %s\n", rows[k][0], rows[k][0] / (interval * 10),
                       cur[(int)rows[k][1]].name);
        }
        memcpy(prev, cur, sizeof(cur[0]) * (size_t)n_cur);
        n_prev = n_cur;
        if (!count || round < count) usleep((useconds_t)(interval * 1e6));
    }
    return 0;
}
