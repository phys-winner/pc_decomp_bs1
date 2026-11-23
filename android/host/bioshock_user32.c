#include "host.h"

WINAPI_EXPORT("user32.dll", FindWindowExA, 4)
{
    void_set_last_error(0);
    return 0;
}

WINAPI_EXPORT("user32.dll", FindWindowExW, 4)
{
    void_set_last_error(0);
    return 0;
}

WINAPI_EXPORT("kernel32.dll", SetConsoleCtrlHandler, 2)
{
    return 1;
}

static uint32_t registered_message = 0xC000u;
WINAPI_EXPORT("user32.dll", RegisterWindowMessageA, 1)
{
    return registered_message < 0xFFFFu ? ++registered_message : 0;
}
WINAPI_EXPORT("user32.dll", RegisterWindowMessageW, 1)
{
    return registered_message < 0xFFFFu ? ++registered_message : 0;
}

WINAPI_EXPORT("user32.dll", GetMessageTime, 0)
{
    return void_tick_ms();
}

WINAPI_ORDINAL("comctl32.dll", InitCommonControls, 17, 0) { return 1; }
WINAPI_EXPORT("comctl32.dll", _TrackMouseEvent, 1) { return 1; }
