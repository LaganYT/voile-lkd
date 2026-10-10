/* w32_bigshared: a 4 GiB MAP_SHARED mapping in a process that already has thousands of mappings, as a D3D12 game's
 * committed UPLOAD buffer (Neptune maps a host shm object of the buffer's size into the guest).
 *
 * 1. FILLER mappings (argument 2, default 4000): one anonymous region whose every other 4 KiB page is made read-only.
 * 2. A memfd of SIZE GiB (argument 1, default 4), mapped MAP_SHARED read-write. The mmap must be quick, and the object
 *    must read as zeros (first page, middle, last byte) without the witness touching the rest.
 * 3. A second read-write view of the same object: a store through one view is seen through the other, near 0 and
 *    past 4 GiB.
 * 4. Code aliases, which the per-view work of a new shared mapping is for: a small RX view of a page deep inside a
 *    second object runs code; then a read-write view of the WHOLE object is mapped, and code rewritten through it must
 *    run from the RX view (the new view starts write-protected over the translated page). Then the same with a
 *    whole-object view mapped BEFORE the code runs.
 *
 * Prints the time of each mmap. Fails when the 4 GiB mmap takes more than LIMIT ms (argument 3, default 200).
 * Before the LKD fix the mmap walked every mapping of the process once per 16 KiB host page of the new view: about
 * 1.3 s per 4 GiB with a Wine process's mappings, seconds in a game.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

typedef int (*fn_t)(void);
static void emit_ret_imm(volatile uint8_t *p, uint32_t v)   /* mov eax, imm32 ; ret */
{
    p[0] = 0xb8; p[1] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)(v >> 16); p[4] = (uint8_t)(v >> 24);
    p[5] = 0xc3;
}

static void code_alias(int map_rw_first)
{
    const size_t osz = 64 << 20, off = 32 << 20;   /* the code page sits 32 MiB into a 64 MiB object */
    int fd = memfd_create("w32code", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)osz) == 0, "code memfd");
    uint8_t *pre = NULL;
    if (map_rw_first) {
        pre = mmap(NULL, osz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(pre != MAP_FAILED, "whole RW view before the code runs");
        emit_ret_imm(pre + off, 1);
    } else {
        uint8_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
        CHECK(w != MAP_FAILED, "small RW view");
        emit_ret_imm(w, 1);
        munmap(w, 4096);
    }
    uint8_t *x = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, (off_t)off);
    CHECK(x != MAP_FAILED, "RX view");
    fn_t f = (fn_t)(void *)x;
    int r1 = f();
    CHECK(r1 == 1, "first run gives %d, not 1", r1);
    uint8_t *rw = pre;
    double t0 = ms();
    if (!rw) rw = mmap(NULL, osz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    double t1 = ms();
    CHECK(rw != MAP_FAILED, "whole RW view after the code ran");
    int ok = 1;
    for (uint32_t i = 2; i < 102; i++) {
        emit_ret_imm(rw + off, i);
        if ((uint32_t)f() != i) ok = 0;
    }
    CHECK(ok, "100 rewrites through the %s view, run from the RX view", map_rw_first ? "earlier" : "later");
    printf("code alias (%s whole view): %s%s\n", map_rw_first ? "earlier" : "later", ok && r1 == 1 ? "ok" : "WRONG",
           map_rw_first ? "" : "");
    if (!map_rw_first) printf("  64 MiB view over translated code: mmap %.3f ms\n", t1 - t0);
    munmap(x, 4096);
    munmap(rw, osz);
    close(fd);
}

int main(int argc, char **argv)
{
    uint64_t gib = argc > 1 ? strtoull(argv[1], NULL, 0) : 4;
    int filler = argc > 2 ? atoi(argv[2]) : 4000;
    double limit = argc > 3 ? atof(argv[3]) : 200;
    const uint64_t size = gib << 30;

    if (filler > 0) {
        size_t len = (size_t)filler * 2 * 4096;
        uint8_t *f = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(f != MAP_FAILED, "filler");
        for (int i = 0; i < filler; i++) mprotect(f + (size_t)i * 8192, 4096, PROT_READ);
    }
    int maps = 0;
    FILE *m = fopen("/proc/self/maps", "r");
    for (int c; m && (c = fgetc(m)) != EOF;) maps += c == '\n';
    if (m) fclose(m);
    printf("w32_bigshared: %llu GiB object, %d mappings in the process\n", (unsigned long long)gib, maps);

    int fd = memfd_create("w32big", MFD_CLOEXEC);
    CHECK(fd >= 0, "memfd_create");
    double t0 = ms();
    CHECK(ftruncate(fd, (off_t)size) == 0, "ftruncate");
    double t1 = ms();
    volatile uint8_t *a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    double t2 = ms();
    CHECK(a != MAP_FAILED, "mmap of the object");
    if (a == MAP_FAILED) return 1;
    printf("ftruncate %.3f ms, mmap %.3f ms\n", t1 - t0, t2 - t1);
    CHECK(t2 - t1 <= limit, "mmap of %llu GiB took %.1f ms (limit %.0f)", (unsigned long long)gib, t2 - t1, limit);

    int zero = 1;
    for (int i = 0; i < 4096; i++) if (a[i]) zero = 0;
    if (a[size / 2 + 123] || a[size - 1]) zero = 0;
    CHECK(zero, "a new object does not read as zeros");

    double t3 = ms();
    volatile uint8_t *b = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    double t4 = ms();
    CHECK(b != MAP_FAILED, "second view");
    printf("second view: mmap %.3f ms\n", t4 - t3);
    if (b != MAP_FAILED) {
        uint64_t far = size > (4ull << 30) + 4096 ? (4ull << 30) + 1000 : size - 8;
        a[0] = 0x5a; a[far] = 0xa5; b[4096 * 3 + 1] = 0x33;
        CHECK(b[0] == 0x5a && b[far] == 0xa5 && a[4096 * 3 + 1] == 0x33, "the two views do not show the same bytes");
        double t5 = ms();
        munmap((void *)b, size);
        printf("munmap of the second view %.3f ms\n", ms() - t5);
    }
    double t6 = ms();
    munmap((void *)a, size);
    printf("munmap %.3f ms\n", ms() - t6);
    close(fd);

    code_alias(0);
    code_alias(1);

    printf(fails ? "W32 FAIL (%d)\n" : "W32 PASS\n", fails);
    return fails ? 1 : 0;
}
