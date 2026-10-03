#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    LARGE_INTEGER f, a, b; WCHAR path[260]; int n = atoi(argv[2]), i;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < n; i++) { swprintf(path, 260, L"%hs\\missing%d.txt", argv[1], i); GetFileAttributesW(path); }
    QueryPerformanceCounter(&b);
    printf("missbench %s: %d misses, %.1f us each\n", argv[1], n, (double)(b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / n);
    QueryPerformanceCounter(&a);
    for (i = 0; i < n; i++) { swprintf(path, 260, L"%hs\\F%d.TXT", argv[1], i % 10); GetFileAttributesW(path); }
    QueryPerformanceCounter(&b);
    printf("missbench %s: %d hits with other case, %.1f us each\n", argv[1], n, (double)(b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / n);
    return 0;
}
