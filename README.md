# putty-win32s
A port of putty 0.83 to Windows 3.1+Win32s.

Maybe this may help close this bug:
https://www.chiark.greenend.org.uk/~sgtatham/putty/wishlist/port-win32s.html

![putty-win32s](/assets/putty-about.png)

The whole windows sourcetree builds, and putty.exe, puttytel.exe and puttygen.exe are tested working.

## Building
To build this, you need the following:
* OpenWatcom 2.0 beta

Then go in the 'windows' directory and type `wmake -f Makefile.wc win32s`

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
* winsftp.exe — a dual-pane SFTP browser (see below)

## winsftp — GUI SFTP client

`winsftp.exe` is a GUI wrapper around psftp in the style of WS_FTP LE / Norton Commander:

* **Left pane** — local filesystem browser; `[dirname]` for folders, double-click to navigate or upload
* **Right pane** — remote SFTP directory; populated automatically via the `ls` hook, double-click to navigate or download
* **Button row** — `< Get`, `Put >`, `Refresh`, `MkDir`, `Delete`
* **Menu bar** — File (Connect / Exit), Transfer, View
* **Three colour themes** (View → Theme): Classic (system colours), Blue (white on dark blue), Green on black
* **Command log** and manual command input with history (Up/Down) and Tab completion — full psftp command set still available

Path labels update automatically. The remote pane refreshes after `cd` commands. Uses only standard Win32 controls — no comctl32, safe for Win32s.

## What doesn't work
* Unicode. really. at all.
* pageant.exe.
* console applications.
  They'll load, and that's all, win32s doesn't support console applications.

As a replacement, there's an updated ssh2dos port with support of modern protocols here: https://github.com/Toyoyo/ssh2dos

Working scp2dos.exe/scp2d386.exe, sftpdos.exe/sftpd386.exe and ssh2dos.exe/ssh2d386.exe are now provided in releases.

The 386 variants are using the DOS32A extender which, in my experience, works correctly under win3.1.

Of course, they'll require a working wattcp environment, and either 2 NICs or something like ndis3pkt.
