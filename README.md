# putty-win32s
A port of putty 0.76 to Windows 3.1+Win32s.

Maybe this may help close this bug:
https://www.chiark.greenend.org.uk/~sgtatham/putty/wishlist/port-win32s.html

![putty-win32s](/assets/putty-about.png)

The whole windows sourcetree builds, and loads.

## Building
To build this, you need the following:
* GNU make
* WATCOM C (used OpenWatcom 2.0 beta)
* mingw32-windres (for resource files, should work with wrc too)

## Prerequistes
* Windows 3.11
* Microsoft TCP/IP-32 version 3.11b (or a compatible winsock 1.1 stack)
* Microsoft Win32s 1.30c (might work in other versions, not tested)

## Running
* As the UNIX codepath for sessions saving/loading is used, you should set a HOME environment variable in AUTOEXEC.BAT
* If not, it defaults to C:\HOME
* run putty.exe

## What works
* the GUI apps, at least putty.exe, puttytel.exe and puttygen.exe

## What doesn't work
* Unicode. really. at all.
* pageant.exe.
* console applications.
  They'll load, and that's all.
  They would require an additional DOS port, probably possible, since win32s can't handle console applications.
