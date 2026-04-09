// Key symbol definitions.
// On Linux we use xkbcommon directly; on other platforms we define the
// subset of XKB keysyms that the app uses so the key handling code
// stays identical across platforms.

#pragma once

#ifdef FCN_LINUX
#include <xkbcommon/xkbcommon-keysyms.h>
#else
// XKB keysym values (X11/XKB standard, same values GLFW maps to)
#define XKB_KEY_BackSpace 0xff08
#define XKB_KEY_Return    0xff0d
#define XKB_KEY_Escape    0xff1b
#define XKB_KEY_Delete    0xffff
#define XKB_KEY_Home      0xff50
#define XKB_KEY_Left      0xff51
#define XKB_KEY_Up        0xff52
#define XKB_KEY_Right     0xff53
#define XKB_KEY_Down      0xff54
#define XKB_KEY_End       0xff57
#define XKB_KEY_KP_Enter  0xff8d
#define XKB_KEY_j         0x006a
#define XKB_KEY_k         0x006b
#endif
