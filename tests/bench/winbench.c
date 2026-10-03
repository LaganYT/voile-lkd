/* winbench: cost of the crossings a Windows game makes through the Linux engine, per call.
 * Build: x86_64-w64-mingw32-clang -O2 -o winbench.exe winbench.c
 * Each line: test name, calls, nanoseconds per call (QueryPerformanceCounter clock). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER freq;

static double now_ns(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)freq.QuadPart;
}

static void report(const char *name, long n, double t0)
{
    double ns = (now_ns() - t0) / n;
    printf("winbench %-28s %8ld calls %10.1f ns/call\n", name, n, ns);
    fflush(stdout);
}

static HANDLE ping, pong;
static volatile LONG stop;

static DWORD WINAPI echo(void *arg)
{
    (void)arg;
    for (;;)
    {
        WaitForSingleObject(ping, INFINITE);
        if (stop) return 0;
        SetEvent(pong);
    }
}

static volatile unsigned sink;
static const char *only;

/* winbench N [TEST]: TEST runs one test alone (its first word, e.g. "SetEvent"), so a profile sees only its calls */
static int want(const char *name)
{
    return !only || !strncmp(name, only, strlen(only));
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 20000;
    only = argc > 2 ? argv[2] : NULL;
    double t0;
    long i;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    QueryPerformanceFrequency(&freq);

    if (want("QueryPerformanceCounter")) {
    t0 = now_ns();
    for (i = 0; i < n * 10; i++) { LARGE_INTEGER t; QueryPerformanceCounter(&t); sink += t.LowPart; }
    report("QueryPerformanceCounter", n * 10, t0);
    }

    if (want("GetTickCount")) {
    t0 = now_ns();
    for (i = 0; i < n * 10; i++) sink += GetTickCount();
    report("GetTickCount", n * 10, t0);
    }

    if (want("SwitchToThread")) {
    t0 = now_ns();
    for (i = 0; i < n; i++) SwitchToThread();
    report("SwitchToThread (sched_yield)", n, t0);
    }

    if (want("WaitForSingleObject")) {
    t0 = now_ns();
    for (i = 0; i < n; i++) sink += WaitForSingleObject(ev, 0);
    report("WaitForSingleObject(0)", n, t0);
    }

    if (want("SetEvent")) {
    t0 = now_ns();
    for (i = 0; i < n; i++) { SetEvent(ev); }
    report("SetEvent", n, t0);
    }

    if (want("CreateEvent")) {
    t0 = now_ns();
    for (i = 0; i < n; i++) { HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL); CloseHandle(h); }
    report("CreateEvent+CloseHandle", n, t0);
    }

    if (want("EnterLeaveCriticalSection")) {
        CRITICAL_SECTION cs;
        InitializeCriticalSection(&cs);
        t0 = now_ns();
        for (i = 0; i < n * 10; i++) { EnterCriticalSection(&cs); LeaveCriticalSection(&cs); }
        report("EnterLeaveCriticalSection", n * 10, t0);
    }

    if (want("SetFilePointer")) {
        HANDLE f = CreateFileW(L"winbench.tmp", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        char buf[4096];
        DWORD done;
        memset(buf, 1, sizeof buf);
        WriteFile(f, buf, sizeof buf, &done, NULL);
        t0 = now_ns();
        for (i = 0; i < n; i++) { SetFilePointer(f, 0, NULL, FILE_BEGIN); ReadFile(f, buf, sizeof buf, &done, NULL); }
        report("SetFilePointer+ReadFile 4K", n, t0);
        CloseHandle(f);
    }

    if (want("event")) {
        ping = CreateEventW(NULL, FALSE, FALSE, NULL);
        pong = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE th = CreateThread(NULL, 0, echo, NULL, 0, NULL);
        long m = n / 4;
        t0 = now_ns();
        for (i = 0; i < m; i++) { SetEvent(ping); WaitForSingleObject(pong, INFINITE); }
        report("event ping-pong round trip", m, t0);
        stop = 1;
        SetEvent(ping);
        WaitForSingleObject(th, INFINITE);
    }

    if (want("compute")) {
        /* plain computation: integer hash and float loop, for the translation's own speed */
        unsigned h = 2166136261u;
        double x = 1.0;
        t0 = now_ns();
        for (i = 0; i < n * 1000; i++) { h = (h ^ (unsigned)i) * 16777619u; x = x * 1.0000001 + 1e-9; }
        sink += h + (unsigned)x;
        report("compute loop (per iteration)", n * 1000, t0);
    }

    if (want("memcpy")) {
        static char a[1 << 20], b[1 << 20];
        long m = n / 20;
        t0 = now_ns();
        for (i = 0; i < m; i++) { memcpy(b, a, sizeof a); a[i & 0xfffff] = (char)i; }
        sink += b[7];
        report("memcpy 1 MiB", m, t0);
    }
    return 0;
}
