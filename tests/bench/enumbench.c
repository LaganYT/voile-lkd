/* enumbench DIR N: cost per entry of FindFirstFileW/FindNextFileW over DIR\*, N passes (a game builds its package
 * list this way). Build: x86_64-w64-mingw32-clang -O2 -o enumbench.exe enumbench.c */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    LARGE_INTEGER f, a, b;
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    long entries = 0;
    int passes = argc > 2 ? atoi(argv[2]) : 3;
    swprintf(pattern, MAX_PATH, L"%hs\\*", argv[1]);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int p = 0; p < passes; p++)
    {
        HANDLE h = FindFirstFileW(pattern, &data);
        if (h == INVALID_HANDLE_VALUE) { printf("enumbench: FindFirstFile failed %lu\n", GetLastError()); return 1; }
        do entries++; while (FindNextFileW(h, &data));
        FindClose(h);
    }
    QueryPerformanceCounter(&b);
    printf("enumbench %s: %ld entries in %d passes, %.2f us each\n", argv[1], entries, passes,
           (double)(b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / entries);
    return 0;
}
