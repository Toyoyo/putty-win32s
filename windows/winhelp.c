/*
 * winhelp.c: centralised functions to launch Windows HTML Help files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "putty.h"
#include "win_res.h"

bool has_help(void) { return false; }
void init_help(void) { }
void shutdown_help(void) { }
void launch_help(HWND hwnd, const char *topic) { }
void quit_help(HWND hwnd) { }
