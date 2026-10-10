/* w25_steamclient: built as steamclient.so for w25_hostpid. Its getpid() goes through the PLT with a call, like
 * Valve's steamclient.so, so guest/steam-hostpid.so sees a return address inside a library of that name. */
#include <unistd.h>

long fake_steam_getpid(void)
{
    long p = getpid();
    __asm__ volatile("" ::: "memory");   /* keep the call a call, not a tail jump */
    return p;
}
