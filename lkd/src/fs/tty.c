/* tty.c: terminal ioctls on host terminals (TCGETS/TCSETS*, TIOCGWINSZ/TIOCSWINSZ, TIOCGPGRP...).
 *
 * The termios flag translation is imported from HL syscall_io.c (helpers linux_*flag_to_mac and
 * mac_*flag_to_linux, c_cc index tables). The flag values used there are those of
 * asm-generic/termbits.h, which x86-64 uses as well. Adapted: the x86-64 TCGETS structure is the
 * 36-byte kernel termios without speed fields, so the speed travels in the CBAUD bits of c_cflag.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "fs_priv.h"

/* imported from HL syscall_io.c: termios flag translation */
#define LINUX_IFLAG_LOW_MASK 0x1ff
#define LINUX_IXON 0x0400
#define LINUX_IXOFF 0x1000
#define LINUX_IMAXBEL 0x2000
#define LINUX_IUTF8 0x4000
#define LINUX_OPOST 0x001
#define LINUX_ONLCR 0x004
#define LINUX_OCRNL 0x008
#define LINUX_ONOCR 0x010
#define LINUX_ONLRET 0x020
#define LINUX_OFILL 0x040
#define LINUX_OFDEL 0x080
#define LINUX_CSIZE 0x0030
#define LINUX_CS5 0x0000
#define LINUX_CS6 0x0010
#define LINUX_CS7 0x0020
#define LINUX_CS8 0x0030
#define LINUX_CSTOPB 0x0040
#define LINUX_CREAD 0x0080
#define LINUX_PARENB 0x0100
#define LINUX_PARODD 0x0200
#define LINUX_HUPCL 0x0400
#define LINUX_CLOCAL 0x0800
#define LINUX_CBAUD 0x100f
#define LINUX_ISIG 0x00001
#define LINUX_ICANON 0x00002
#define LINUX_ECHO 0x00008
#define LINUX_ECHOE 0x00010
#define LINUX_ECHOK 0x00020
#define LINUX_ECHONL 0x00040
#define LINUX_NOFLSH 0x00080
#define LINUX_TOSTOP 0x00100
#define LINUX_ECHOCTL 0x00200
#define LINUX_ECHOPRT 0x00400
#define LINUX_ECHOKE 0x00800
#define LINUX_FLUSHO 0x01000
#define LINUX_PENDIN 0x04000
#define LINUX_IEXTEN 0x08000
#define LINUX_EXTPROC 0x10000

static tcflag_t linux_iflag_to_mac(uint32_t lf)
{
    tcflag_t mf = lf & LINUX_IFLAG_LOW_MASK;
    if (lf & 0x800) mf |= IXANY;
    if (lf & LINUX_IXON) mf |= IXON;
    if (lf & LINUX_IXOFF) mf |= IXOFF;
    if (lf & LINUX_IMAXBEL) mf |= IMAXBEL;
    if (lf & LINUX_IUTF8) mf |= IUTF8;
    return mf;
}
static uint32_t mac_iflag_to_linux(tcflag_t mf)
{
    uint32_t lf = (uint32_t)(mf & LINUX_IFLAG_LOW_MASK);
    if (mf & IXANY) lf |= 0x800;
    if (mf & IXON) lf |= LINUX_IXON;
    if (mf & IXOFF) lf |= LINUX_IXOFF;
    if (mf & IMAXBEL) lf |= LINUX_IMAXBEL;
    if (mf & IUTF8) lf |= LINUX_IUTF8;
    return lf;
}
static tcflag_t linux_oflag_to_mac(uint32_t lf)
{
    tcflag_t mf = 0;
    if (lf & LINUX_OPOST) mf |= OPOST;
    if (lf & LINUX_ONLCR) mf |= ONLCR;
    if (lf & LINUX_OCRNL) mf |= OCRNL;
    if (lf & LINUX_ONOCR) mf |= ONOCR;
    if (lf & LINUX_ONLRET) mf |= ONLRET;
    if (lf & LINUX_OFILL) mf |= OFILL;
    if (lf & LINUX_OFDEL) mf |= OFDEL;
    return mf;
}
static uint32_t mac_oflag_to_linux(tcflag_t mf)
{
    uint32_t lf = 0;
    if (mf & OPOST) lf |= LINUX_OPOST;
    if (mf & ONLCR) lf |= LINUX_ONLCR;
    if (mf & OCRNL) lf |= LINUX_OCRNL;
    if (mf & ONOCR) lf |= LINUX_ONOCR;
    if (mf & ONLRET) lf |= LINUX_ONLRET;
    if (mf & OFILL) lf |= LINUX_OFILL;
    if (mf & OFDEL) lf |= LINUX_OFDEL;
    return lf;
}
static tcflag_t linux_cflag_to_mac(uint32_t lf)
{
    tcflag_t mf = 0;
    switch (lf & LINUX_CSIZE) {
    case LINUX_CS5: mf |= CS5; break;
    case LINUX_CS6: mf |= CS6; break;
    case LINUX_CS7: mf |= CS7; break;
    default: mf |= CS8; break;
    }
    if (lf & LINUX_CSTOPB) mf |= CSTOPB;
    if (lf & LINUX_CREAD) mf |= CREAD;
    if (lf & LINUX_PARENB) mf |= PARENB;
    if (lf & LINUX_PARODD) mf |= PARODD;
    if (lf & LINUX_HUPCL) mf |= HUPCL;
    if (lf & LINUX_CLOCAL) mf |= CLOCAL;
    return mf;
}
static uint32_t mac_cflag_to_linux(tcflag_t mf)
{
    uint32_t lf = 0;
    switch (mf & CSIZE) {
    case CS5: lf |= LINUX_CS5; break;
    case CS6: lf |= LINUX_CS6; break;
    case CS7: lf |= LINUX_CS7; break;
    default: lf |= LINUX_CS8; break;
    }
    if (mf & CSTOPB) lf |= LINUX_CSTOPB;
    if (mf & CREAD) lf |= LINUX_CREAD;
    if (mf & PARENB) lf |= LINUX_PARENB;
    if (mf & PARODD) lf |= LINUX_PARODD;
    if (mf & HUPCL) lf |= LINUX_HUPCL;
    if (mf & CLOCAL) lf |= LINUX_CLOCAL;
    return lf;
}
static tcflag_t linux_lflag_to_mac(uint32_t lf)
{
    tcflag_t mf = 0;
    if (lf & LINUX_ISIG) mf |= ISIG;
    if (lf & LINUX_ICANON) mf |= ICANON;
    if (lf & LINUX_ECHO) mf |= ECHO;
    if (lf & LINUX_ECHOE) mf |= ECHOE;
    if (lf & LINUX_ECHOK) mf |= ECHOK;
    if (lf & LINUX_ECHONL) mf |= ECHONL;
    if (lf & LINUX_NOFLSH) mf |= NOFLSH;
    if (lf & LINUX_TOSTOP) mf |= TOSTOP;
    if (lf & LINUX_ECHOCTL) mf |= ECHOCTL;
    if (lf & LINUX_ECHOPRT) mf |= ECHOPRT;
    if (lf & LINUX_ECHOKE) mf |= ECHOKE;
    if (lf & LINUX_FLUSHO) mf |= FLUSHO;
    if (lf & LINUX_PENDIN) mf |= PENDIN;
    if (lf & LINUX_IEXTEN) mf |= IEXTEN;
    if (lf & LINUX_EXTPROC) mf |= EXTPROC;
    return mf;
}
static uint32_t mac_lflag_to_linux(tcflag_t mf)
{
    uint32_t lf = 0;
    if (mf & ISIG) lf |= LINUX_ISIG;
    if (mf & ICANON) lf |= LINUX_ICANON;
    if (mf & ECHO) lf |= LINUX_ECHO;
    if (mf & ECHOE) lf |= LINUX_ECHOE;
    if (mf & ECHOK) lf |= LINUX_ECHOK;
    if (mf & ECHONL) lf |= LINUX_ECHONL;
    if (mf & NOFLSH) lf |= LINUX_NOFLSH;
    if (mf & TOSTOP) lf |= LINUX_TOSTOP;
    if (mf & ECHOCTL) lf |= LINUX_ECHOCTL;
    if (mf & ECHOPRT) lf |= LINUX_ECHOPRT;
    if (mf & ECHOKE) lf |= LINUX_ECHOKE;
    if (mf & FLUSHO) lf |= LINUX_FLUSHO;
    if (mf & PENDIN) lf |= LINUX_PENDIN;
    if (mf & IEXTEN) lf |= LINUX_IEXTEN;
    if (mf & EXTPROC) lf |= LINUX_EXTPROC;
    return lf;
}
/* Linux c_cc index -> macOS c_cc index (-1: none). */
static const int linux_to_mac_cc[19] = { 8, 9, 3, 5, 0, 17, 16, -1, 12, 13, 10, 1, 6, 15, 4, 14, 2, -1, -1 };
/* end of import */

/* Linux B* codes in CBAUD (include/uapi/asm-generic/termbits.h). */
static const struct { speed_t sp; uint32_t code; } speeds[] = {
    { 0, 0 }, { 50, 1 }, { 75, 2 }, { 110, 3 }, { 134, 4 }, { 150, 5 }, { 200, 6 }, { 300, 7 }, { 600, 8 },
    { 1200, 9 }, { 1800, 10 }, { 2400, 11 }, { 4800, 12 }, { 9600, 13 }, { 19200, 14 }, { 38400, 15 },
    { 57600, 0x1001 }, { 115200, 0x1002 }, { 230400, 0x1003 }, { 460800, 0x1004 },
};

static uint32_t speed_to_code(speed_t s)
{
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) if (speeds[i].sp == s) return speeds[i].code;
    return 15; /* B38400 for speeds without a code (pseudo terminals report 38400 on Linux) */
}
static speed_t code_to_speed(uint32_t c)
{
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) if (speeds[i].code == c) return speeds[i].sp;
    return 38400;
}

static int32_t host_pgid_to_guest(lkd_task *t, pid_t hp)
{
    if (hp == getpgrp() && t && t->proc) return t->proc->pgid;
    /* A process group outside this guest process: invisible ids read as 0 (pid_vnr). */
    return 0;
}

int64_t fs_tty_ioctl(lkd_task *t, int fd, uint32_t cmd, uint64_t arg)
{
    switch (cmd) {
    case LX_TCGETS: {
        struct termios ht;
        if (tcgetattr(fd, &ht) < 0) return LKD_HOST_ERR();
        struct lx_termios lt;
        memset(&lt, 0, sizeof lt);
        lt.c_iflag = mac_iflag_to_linux(ht.c_iflag);
        lt.c_oflag = mac_oflag_to_linux(ht.c_oflag);
        lt.c_cflag = mac_cflag_to_linux(ht.c_cflag) | speed_to_code(cfgetospeed(&ht));
        lt.c_lflag = mac_lflag_to_linux(ht.c_lflag);
        for (int i = 0; i < 19; i++) {
            int mi = linux_to_mac_cc[i];
            lt.c_cc[i] = (mi >= 0 && mi < NCCS) ? ht.c_cc[mi] : 0;
        }
        return lkd_user_write(arg, &lt, sizeof lt) < 0 ? -LX_EFAULT : 0;
    }
    case LX_TCSETS: case LX_TCSETSW: case LX_TCSETSF: {
        struct lx_termios lt;
        if (lkd_user_read(arg, &lt, sizeof lt) < 0) return -LX_EFAULT;
        struct termios ht;
        if (tcgetattr(fd, &ht) < 0) return LKD_HOST_ERR();
        ht.c_iflag = linux_iflag_to_mac(lt.c_iflag);
        ht.c_oflag = linux_oflag_to_mac(lt.c_oflag);
        ht.c_cflag = linux_cflag_to_mac(lt.c_cflag);
        ht.c_lflag = linux_lflag_to_mac(lt.c_lflag);
        for (int i = 0; i < 19; i++) {
            int mi = linux_to_mac_cc[i];
            if (mi >= 0 && mi < NCCS) ht.c_cc[mi] = lt.c_cc[i];
        }
        speed_t sp = code_to_speed(lt.c_cflag & LINUX_CBAUD);
        cfsetispeed(&ht, sp);
        cfsetospeed(&ht, sp);
        int act = cmd == LX_TCSETSF ? TCSAFLUSH : cmd == LX_TCSETSW ? TCSADRAIN : TCSANOW;
        return tcsetattr(fd, act, &ht) < 0 ? LKD_HOST_ERR() : 0;
    }
    case LX_TIOCGWINSZ: {
        struct winsize ws;
        if (ioctl(fd, TIOCGWINSZ, &ws) < 0) return LKD_HOST_ERR();
        struct lx_winsize lw = { ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel };
        return lkd_user_write(arg, &lw, sizeof lw) < 0 ? -LX_EFAULT : 0;
    }
    case LX_TIOCSWINSZ: {
        struct lx_winsize lw;
        if (lkd_user_read(arg, &lw, sizeof lw) < 0) return -LX_EFAULT;
        struct winsize ws = { lw.ws_row, lw.ws_col, lw.ws_xpixel, lw.ws_ypixel };
        return ioctl(fd, TIOCSWINSZ, &ws) < 0 ? LKD_HOST_ERR() : 0;
    }
    case LX_TIOCGPGRP: {
        pid_t p = tcgetpgrp(fd);
        if (p < 0) return LKD_HOST_ERR();
        int32_t v = host_pgid_to_guest(t, p);
        return lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
    }
    case LX_TIOCSPGRP: {
        int32_t v;
        if (lkd_user_read(arg, &v, sizeof v) < 0) return -LX_EFAULT;
        if (!t || !t->proc || v != t->proc->pgid) return -LX_EPERM; /* only our own group maps to a host group */
        return tcsetpgrp(fd, getpgrp()) < 0 ? LKD_HOST_ERR() : 0;
    }
    case LX_TIOCGSID: {
        pid_t s = tcgetsid(fd);
        if (s < 0) return LKD_HOST_ERR();
        int32_t v = (s == getsid(0) && t && t->proc) ? t->proc->sid : 0;
        return lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
    }
    default:
        return -LX_ENOTTY;
    }
}
