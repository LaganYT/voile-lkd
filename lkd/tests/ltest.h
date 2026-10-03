/* ltest.h: tiny assertion framework for LKD tests (header only).
 *
 * Usage:
 *   LT_TEST(my_case) { LT_EQ(1 + 1, 2); LT_ERRNO(call(), LX_ENOENT); }
 *   int main(void) { LT_RUN(my_case); return lt_finish(); }
 *
 * Output: 'PASS name' or 'FAIL name: file:line: detail' per test case, then one
 * 'SUMMARY <assertions> assertions, <cases> cases, <failed> failed' line.
 * A test program with zero assertions fails (it proves nothing).
 * Assertions keep running after a failure so one run shows every defect.
 */
#ifndef LKD_LTEST_H
#define LKD_LTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int lt_checks, lt_cases, lt_failed_cases;
static int lt_case_fail, lt_case_checks;
static const char *lt_case_name = "";
static char lt_case_detail[512];

static inline void lt_fail_(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static inline void lt_fail_(const char *file, int line, const char *fmt, ...)
{
    __builtin_va_list ap;
    char msg[320];
    __builtin_va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    __builtin_va_end(ap);
    if (!lt_case_fail)
        snprintf(lt_case_detail, sizeof lt_case_detail, "%s:%d: %s", file, line, msg);
    else
        fprintf(stderr, "  also failed: %s:%d: %s\n", file, line, msg);
    lt_case_fail = 1;
}

#define LT_PASS_() (lt_checks++, lt_case_checks++)

#define LT_CHECK(cond) do { LT_PASS_(); \
    if (!(cond)) lt_fail_(__FILE__, __LINE__, "check failed: %s", #cond); } while (0)

#define LT_MSG(cond, ...) do { LT_PASS_(); \
    if (!(cond)) lt_fail_(__FILE__, __LINE__, __VA_ARGS__); } while (0)

/* Integer equality; both sides are printed as signed and hexadecimal. */
#define LT_EQ(a, b) do { long long lt_a_ = (long long)(a), lt_b_ = (long long)(b); LT_PASS_(); \
    if (lt_a_ != lt_b_) lt_fail_(__FILE__, __LINE__, "%s == %s: got %lld (0x%llx), want %lld (0x%llx)", \
        #a, #b, lt_a_, (unsigned long long)lt_a_, lt_b_, (unsigned long long)lt_b_); } while (0)

#define LT_NE(a, b) do { long long lt_a_ = (long long)(a), lt_b_ = (long long)(b); LT_PASS_(); \
    if (lt_a_ == lt_b_) lt_fail_(__FILE__, __LINE__, "%s != %s: both %lld", #a, #b, lt_a_); } while (0)

/* A syscall-style result must be the negative Linux errno (pass the positive LX_E* name). */
#define LT_ERRNO(call, lx_errno) do { long long lt_r_ = (long long)(call); LT_PASS_(); \
    if (lt_r_ != -(long long)(lx_errno)) lt_fail_(__FILE__, __LINE__, "%s: got %lld, want -%d (%s)", \
        #call, lt_r_, (int)(lx_errno), #lx_errno); } while (0)

#define LT_STREQ(a, b) do { const char *lt_a_ = (a), *lt_b_ = (b); LT_PASS_(); \
    if (!lt_a_ || !lt_b_ || strcmp(lt_a_, lt_b_)) lt_fail_(__FILE__, __LINE__, "%s: got \"%s\", want \"%s\"", \
        #a, lt_a_ ? lt_a_ : "(null)", lt_b_ ? lt_b_ : "(null)"); } while (0)

#define LT_MEMEQ(a, b, n) do { LT_PASS_(); \
    if (memcmp((a), (b), (n))) lt_fail_(__FILE__, __LINE__, "memcmp(%s, %s, %s) differs", #a, #b, #n); } while (0)

#define LT_TEST(name) static void name(void)

#define LT_RUN(name) do { lt_case_name = #name; lt_case_fail = 0; lt_case_checks = 0; lt_case_detail[0] = 0; \
    name(); lt_cases++; \
    if (lt_case_fail) { lt_failed_cases++; printf("FAIL %s: %s\n", #name, lt_case_detail); } \
    else if (lt_case_checks == 0) { lt_failed_cases++; printf("FAIL %s: no assertion ran\n", #name); } \
    else printf("PASS %s\n", #name); fflush(stdout); } while (0)

static inline int lt_finish(void)
{
    printf("SUMMARY %d assertions, %d cases, %d failed\n", lt_checks, lt_cases, lt_failed_cases);
    fflush(stdout);
    return (lt_failed_cases || lt_cases == 0) ? 1 : 0;
}

/* Run fn in a forked child with a deadline; returns its exit status, 128+sig on a signal, -1 on a
 * timeout. fn must not return: it calls _exit(). Keep the child count small (machine safety rule). */
#include <sys/wait.h>
#include <signal.h>
static inline int lt_run_child(void (*fn)(void *), void *arg, int timeout_s)
{
    fflush(NULL);
    pid_t p = fork();
    if (p < 0) return -2;
    if (p == 0) { alarm((unsigned)timeout_s); fn(arg); _exit(0); }
    for (int i = 0; i < timeout_s * 100; i++) {
        int st;
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        usleep(10000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    return -1;
}

#endif /* LKD_LTEST_H */
