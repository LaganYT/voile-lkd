/* Prints what XInput reports for 4 slots; A starts a rumble, releasing it stops it. Usage: xinputtest [seconds]
 * Build: x86_64-w64-mingw32-clang -O2 -o xinputtest.exe xinputtest.c -lxinput1_3 (llvm-mingw).
 * With fake-input.py serving the engine's /run/obelisk, the run shows the scripted gamepad and the rumble
 * requests come back to the script; with obelisk-input, it shows the Mac's gamepads. */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 30, i;
    XINPUT_STATE last[4];
    BOOL connected[4] = {0};
    DWORD start = GetTickCount();

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(last, 0, sizeof(last));
    printf("xinputtest: polling for %d s\n", seconds);
    while (GetTickCount() - start < (DWORD)seconds * 1000)
    {
        for (i = 0; i < 4; i++)
        {
            XINPUT_STATE s;
            DWORD r = XInputGetState(i, &s);
            if (r == ERROR_SUCCESS && !connected[i])
            {
                XINPUT_CAPABILITIES caps;
                DWORD c = XInputGetCapabilities(i, 0, &caps);
                printf("[%5lu] slot %d connected (caps %lu type %u subtype %u flags %#x vibration %u/%u)\n",
                       GetTickCount() - start, i, c, caps.Type, caps.SubType, caps.Flags,
                       caps.Vibration.wLeftMotorSpeed, caps.Vibration.wRightMotorSpeed);
                connected[i] = TRUE;
            }
            else if (r != ERROR_SUCCESS && connected[i])
            {
                printf("[%5lu] slot %d disconnected (%lu)\n", GetTickCount() - start, i, r);
                connected[i] = FALSE;
                memset(&last[i], 0, sizeof(last[i]));
            }
            if (r != ERROR_SUCCESS || s.dwPacketNumber == last[i].dwPacketNumber) continue;
            printf("[%5lu] slot %d buttons %04x LT %3u RT %3u LX %6d LY %6d RX %6d RY %6d\n", GetTickCount() - start, i,
                   s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX,
                   s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
            if ((s.Gamepad.wButtons ^ last[i].Gamepad.wButtons) & XINPUT_GAMEPAD_A)
            {
                XINPUT_VIBRATION v = {0, 0};
                if (s.Gamepad.wButtons & XINPUT_GAMEPAD_A) v.wLeftMotorSpeed = v.wRightMotorSpeed = 40000;
                printf("[%5lu] slot %d rumble %u -> %lu\n", GetTickCount() - start, i, v.wLeftMotorSpeed, XInputSetState(i, &v));
            }
            last[i] = s;
        }
        Sleep(4);
    }
    printf("xinputtest: done\n");
    return 0;
}
