/*
 * winsftp.c — Windows GUI wrapper for PSFTP (winsftp.exe).
 *
 * Dual-pane SFTP browser in WS_FTP / Norton Commander style:
 *   - Left SysListView32: local filesystem (always live)
 *   - Right SysListView32: remote SFTP directory (populated via ls hook)
 *   - Path labels above each pane
 *   - Button row: < Get | Put > | Refresh | MkDir | Delete
 *   - Log EDIT: read-only multiline output area
 *   - Command EDIT + Send button for manual psftp commands
 *   - Status bar: transfer progress
 *   - Menu bar: File | Transfer | View (with Theme submenu)
 *   - Three themes: Classic, Blue (WinSCP-style), Green-on-Black
 *
 * Compiles with OpenWatcom wcl386 (-bt=nt -DWIN32S_COMPAT).
 * Uses comctl32.dll for SysListView32 (LVS_REPORT, full-row select).
 *
 * psftp.c is compiled with -DWINSFTP_BUILD=1
 * -Dprintf=winsftp_printf -Dfprintf=winsftp_fprintf
 * so all its printf/fprintf calls route here.
 *
 * list_directory_from_sftp_print / _warn_unsorted are guarded in
 * psftp.c with #ifndef WINSFTP_BUILD so our definitions here win.
 */

#include <winsock.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

/* Defensive defines for comctl32 items not in all SDK headers */
#ifndef LVS_EX_FULLROWSELECT
#define LVS_EX_FULLROWSELECT 0x00000020
#endif
#ifndef LVS_EX_GRIDLINES
#define LVS_EX_GRIDLINES     0x00000001
#endif
#ifndef LVM_SETEXTENDEDLISTVIEWSTYLE
#define LVM_SETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 54)
#endif
#ifndef ILC_COLORDDB
#define ILC_COLORDDB 0x00FE  /* device-dependent bitmap — matches CreateCompatibleBitmap */
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define NEED_DECLARATION_OF_SELECT
#include "putty.h"
#include "psftp.h"
#include "ssh.h"
#include "ssh/sftp.h"
#include "console.h"
#include "storage.h"
#include "security-api.h"
#include "putty-rc.h"
#include "version.h"

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void winsftp_append(const char *text);
static void do_layout(HWND hwnd);
static void populate_local_list(void);
static void refresh_remote_list(void);
static void pane_clear(HWND hw);
static void lv_insert(HWND hw, int idx, const char *name, int itype);
static int  lv_get_focused(HWND hw, char *name, int namelen, int *type_out);
static int  lv_get_item_at(HWND hw, int idx, char *name, int namelen, int *type_out);
static LRESULT CALLBACK InfoDlgProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK LocalListProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK RemoteListProc(HWND, UINT, WPARAM, LPARAM);
static void do_mkdir_dialog(int remote); /* remote=1 → SFTP mkdir, 0 → local CreateDirectory */

/* ================================================================
 * Control IDs
 * ================================================================ */
#define IDC_OUTPUT       100
#define IDC_INPUT        101
#define IDC_STATUS       102
#define IDC_LOCAL_LIST   103
#define IDC_REMOTE_LIST  104
#define IDC_LOCAL_PATH   105
#define IDC_REMOTE_PATH  106
#define IDC_GET_BTN      107
#define IDC_PUT_BTN      108
#define IDC_REFRESH_BTN  109
#define IDC_MKDIR_BTN    110
#define IDC_DELETE_BTN   111
/* IDC_SEND == IDOK == 1, so IsDialogMessage routes Enter to it */

/* ================================================================
 * Menu IDs
 * ================================================================ */
#define IDM_FILE_CONNECT   200
#define IDM_FILE_EXIT      201
#define IDM_XFER_GET       210
#define IDM_XFER_PUT       211
#define IDM_XFER_REFRESH   212
#define IDM_VIEW_CLEAR     220
#define IDM_THEME_CLASSIC  230
#define IDM_THEME_BLUE     231
#define IDM_THEME_GREEN    232
#define IDM_FILE_SAVEDCONN 202

/* Connect dialog control IDs */
#define IDCD_SESS_LIST   300
#define IDCD_HOST        301
#define IDCD_USER        302
#define IDCD_PORT        303
#define IDCD_KEYFILE     304
#define IDCD_BROWSE      305
#define IDCD_SESS_NAME   306
#define IDCD_SAVE        307
#define IDCD_DELETE_SESS 308

/* Toolbar and extra buttons */
#define IDC_TOOLBAR_CONNECT  112
#define IDC_TOOLBAR_REFRESH  113
#define IDC_TOOLBAR_LUP      114
#define IDC_TOOLBAR_RUP      115
#define IDC_RMDIR_BTN        116

/* Context menu IDs */
#define IDM_CTX_LOCAL_UPLOAD    240
#define IDM_CTX_LOCAL_NAVINTO   241
#define IDM_CTX_LOCAL_REFRESH   242
#define IDM_CTX_REMOTE_DOWNLOAD 243
#define IDM_CTX_REMOTE_NAVINTO  244
#define IDM_CTX_REMOTE_DELETE   245
#define IDM_CTX_REMOTE_MKDIR    246
#define IDM_CTX_REMOTE_REFRESH  247
#define IDM_CTX_REMOTE_INFO     248
#define IDM_CTX_LOCAL_INFO      249

/* Help menu */
#define IDM_HELP_ABOUT          260

/* Info dialog buttons */
#define IDC_INFO_CALC   1002
#define IDC_INFO_CLOSE  1003

/* ================================================================
 * Layout constants
 * ================================================================ */
#define STATUS_H   18
#define INPUT_H    24
#define MARGIN      4
#define BTN_W      50
#define PATH_H     20   /* address-bar EDIT height */
#define BTNROW_H    30   /* button row height */
#define PBTN_W      76   /* width of each button */
#define LOG_H       96   /* output log fixed height (~6 lines) */
#define BTNROW_PAD   8   /* blank space above and below the button row */
#define BTN_GAP      8   /* horizontal gap between buttons */
#define TBREF_W     96   /* width of "Refresh Both" toolbar button */

/* ListView item type stored in LVIF_PARAM lParam */
#define LV_FILE    0
#define LV_FOLDER  1
#define LV_PARENT  2   /* ".." entry */

/* ================================================================
 * Theme definitions
 * ================================================================ */
#define THEME_CLASSIC  0   /* system colours, no custom brush */
#define THEME_BLUE     1   /* white text on dark blue (early WinSCP feel) */
#define THEME_GREEN    2   /* green text on black (terminal feel) */

/* ================================================================
 * Global GUI state
 * ================================================================ */
HINSTANCE hinst;

static HWND g_hwnd;           /* main window */
static HWND g_local_list;     /* local file LISTBOX */
static HWND g_remote_list;    /* remote file LISTBOX */
static HWND g_local_path;     /* local path address EDIT */
static HWND g_remote_path;    /* remote path address EDIT */
static HWND g_get_btn;        /* "< Get" button */
static HWND g_put_btn;        /* "Put >" button */
static HWND g_refresh_btn;    /* "Refresh" button */
static HWND g_mkdir_btn;      /* "MkDir" button */
static HWND g_delete_btn;     /* "Delete" button */
static HWND g_rmdir_btn;      /* "RmDir" button */
static HWND g_toolbar_connect;
static HWND g_toolbar_refresh;
static HWND g_toolbar_lup;
static HWND g_toolbar_rup;
static HWND g_output;         /* read-only multiline EDIT (log area) */
static HWND g_input;          /* single-line command EDIT */
static HWND g_status;         /* STATIC for progress/status text */
static HWND g_send;           /* "Send" push button */

static WNDPROC g_input_orig_proc;
static WNDPROC g_output_orig_proc;
static WNDPROC g_local_path_orig_proc;
static WNDPROC g_remote_path_orig_proc;

static HIMAGELIST g_img_list    = NULL;
static bool       g_has_listview = false; /* true if SysListView32 available */

static int      g_active_pane          = 1;    /* 0=local, 1=remote */
static HBRUSH   g_active_path_brush    = NULL; /* blue bg for active path label */
static WNDPROC  g_local_list_orig_proc  = NULL;
static WNDPROC  g_remote_list_orig_proc = NULL;

/* ----------------------------------------------------------------
 * Modeless "Get Info" dialog state
 * ---------------------------------------------------------------- */
static HWND     g_info_hwnd        = NULL;
static HWND     g_info_calc_label  = NULL; /* "Disk usage: ..." result static */
static HWND     g_info_calc_btn    = NULL; /* "Calculate" button */
static char     g_info_dirname[MAX_PATH];  /* directory being sized */
static bool     g_pending_size_count = false; /* triggers ls in get_cmdline */
static bool     g_size_count_mode    = false; /* ls is running for size calc */
static char     g_size_count_dirname[MAX_PATH];
static uint64_t g_info_size_sum    = 0;
static int      g_info_file_count  = 0;
/* BFS queue for recursive directory size traversal */
#define SIZE_QUEUE_MAX 256
static char     g_size_dir_queue[SIZE_QUEUE_MAX][MAX_PATH];
static int      g_size_queue_head  = 0;
static int      g_size_queue_tail  = 0;
static char     g_size_current_dir[MAX_PATH]; /* dir currently being ls'd */
/* populated before CreateWindowEx so WM_CREATE can read them synchronously */
static char     g_info_name_buf[MAX_PATH + 1];
static char     g_info_perm_buf[512];
static bool     g_info_is_dir      = false;

/* ================================================================
 * Theme state
 * ================================================================ */
static int      g_theme       = THEME_CLASSIC;
static HBRUSH   g_theme_brush = NULL;
static COLORREF g_theme_fg    = RGB(0, 0, 0);
static COLORREF g_theme_bg    = RGB(255, 255, 255);

static void apply_theme(int theme)
{
    if (g_theme_brush) {
        DeleteObject(g_theme_brush);
        g_theme_brush = NULL;
    }
    g_theme = theme;
    switch (theme) {
      case THEME_BLUE:
        g_theme_bg = RGB(0, 0, 128);
        g_theme_fg = RGB(255, 255, 255);
        g_theme_brush = CreateSolidBrush(g_theme_bg);
        break;
      case THEME_GREEN:
        g_theme_bg = RGB(0, 0, 0);
        g_theme_fg = RGB(0, 200, 0);
        g_theme_brush = CreateSolidBrush(g_theme_bg);
        break;
      default: /* THEME_CLASSIC */
        g_theme_bg = GetSysColor(COLOR_WINDOW);
        g_theme_fg = GetSysColor(COLOR_WINDOWTEXT);
        break;
    }
    if (g_local_list) {
        if (g_has_listview) {
            SendMessage(g_local_list, LVM_SETBKCOLOR,    0, (LPARAM)g_theme_bg);
            SendMessage(g_local_list, LVM_SETTEXTCOLOR,  0, (LPARAM)g_theme_fg);
            SendMessage(g_local_list, LVM_SETTEXTBKCOLOR,0, (LPARAM)g_theme_bg);
        }
        InvalidateRect(g_local_list, NULL, TRUE);
    }
    if (g_remote_list) {
        if (g_has_listview) {
            SendMessage(g_remote_list, LVM_SETBKCOLOR,    0, (LPARAM)g_theme_bg);
            SendMessage(g_remote_list, LVM_SETTEXTCOLOR,  0, (LPARAM)g_theme_fg);
            SendMessage(g_remote_list, LVM_SETTEXTBKCOLOR,0, (LPARAM)g_theme_bg);
        }
        InvalidateRect(g_remote_list, NULL, TRUE);
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ================================================================
 * Command-line input synchronisation
 * ================================================================ */
static char  *g_cmd_line      = NULL;
static bool   g_cmd_ready     = false;
static bool   g_running       = true;
static bool   g_suppress_echo = false;

/* ----------------------------------------------------------------
 * Password / credential popup state
 * ---------------------------------------------------------------- */
static HWND g_pass_hwnd  = NULL;   /* non-NULL while dialog is open */
static HWND g_pass_edit  = NULL;
static char g_pass_buf[512];
static bool g_pass_ok    = false;
static char g_pass_label[512];
static bool g_pass_echo  = true;

/* ================================================================
 * Remote listing state
 * ================================================================ */
static bool g_collecting_remote = false; /* add to remote listbox */
static bool g_pending_refresh   = false; /* inject "ls" next cmd cycle */

/* Sort buffer — collect remote entries, sort dirs-first, flush on ls end */
#define REMOTE_BUF_MAX 512
static char g_remote_buf[REMOTE_BUF_MAX][MAX_PATH + 1];
static int  g_remote_type[REMOTE_BUF_MAX];          /* LV_FOLDER or LV_FILE */
static char g_remote_longname[REMOTE_BUF_MAX][256]; /* ls -l style line per entry */
static int  g_remote_sort_idx[REMOTE_BUF_MAX];      /* scratch indices for qsort */
static int  g_remote_buf_count = 0;

/* Post-flush display buffer — mirrors sorted list items (index 0 = first non-".." item) */
static char g_displayed_longname[REMOTE_BUF_MAX][256];
static int  g_displayed_count = 0;

/* Command queue for multi-select transfers (batch get/put) */
#define CMD_QUEUE_MAX 256
static char *g_cmd_queue[CMD_QUEUE_MAX];
static int   g_cmd_queue_head = 0;
static int   g_cmd_queue_tail = 0;

/* ================================================================
 * Progress state
 * ================================================================ */
static uint64_t g_progress_total = 0;
static char     g_progress_fname[256];
static char     g_pending_keyfile[MAX_PATH];
static char     g_rmdir_pending[MAX_PATH];
static int      g_rmdir_step;           /* 0=idle 1=rm-glob 2=rmdir */
static char     g_last_completed_cmd[512];

/* ================================================================
 * Command history (Up/Down arrow recall)
 * ================================================================ */
#define HISTORY_MAX 100
static char *g_history[HISTORY_MAX];
static int   g_history_count = 0;
static int   g_history_pos   = -1;
static char  *g_history_saved = NULL;

static void history_add(const char *cmd)
{
    if (!cmd || !*cmd) return;
    if (g_history_count == HISTORY_MAX) {
        sfree(g_history[0]);
        memmove(g_history, g_history + 1,
                (HISTORY_MAX - 1) * sizeof(g_history[0]));
        g_history_count--;
    }
    g_history[g_history_count++] = dupstr(cmd);
    g_history_pos = -1;
    sfree(g_history_saved);
    g_history_saved = NULL;
}

/* ================================================================
 * Command queue (multi-select batch transfers)
 * ================================================================ */

static void cmd_queue_push(const char *cmd)
{
    int next = (g_cmd_queue_tail + 1) % CMD_QUEUE_MAX;
    if (next == g_cmd_queue_head) return; /* full — drop */
    g_cmd_queue[g_cmd_queue_tail] = dupstr(cmd);
    g_cmd_queue_tail = next;
}

static char *cmd_queue_pop(void)
{
    char *cmd;
    if (g_cmd_queue_head == g_cmd_queue_tail) return NULL;
    cmd = g_cmd_queue[g_cmd_queue_head];
    g_cmd_queue[g_cmd_queue_head] = NULL;
    g_cmd_queue_head = (g_cmd_queue_head + 1) % CMD_QUEUE_MAX;
    return cmd;
}

/* ================================================================
 * Remote sort buffer helpers
 * ================================================================ */

static int remote_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int ta = g_remote_type[ia];
    int tb = g_remote_type[ib];
    if (ta != tb) return (ta > tb) ? -1 : 1; /* LV_FOLDER(1) before LV_FILE(0) */
    return stricmp(g_remote_buf[ia], g_remote_buf[ib]);
}

static void flush_remote_buf(void)
{
    int i;
    if (!g_remote_list || g_remote_buf_count == 0) {
        g_remote_buf_count = 0;
        return;
    }
    for (i = 0; i < g_remote_buf_count; i++)
        g_remote_sort_idx[i] = i;
    qsort(g_remote_sort_idx, (size_t)g_remote_buf_count,
          sizeof(g_remote_sort_idx[0]), remote_cmp);
    g_displayed_count = g_remote_buf_count;
    for (i = 0; i < g_remote_buf_count; i++) {
        int si = g_remote_sort_idx[i];
        lv_insert(g_remote_list, 1 + i, g_remote_buf[si], g_remote_type[si]);
        strncpy(g_displayed_longname[i], g_remote_longname[si], 255);
        g_displayed_longname[i][255] = '\0';
    }
    g_remote_buf_count = 0;
}

/* ================================================================
 * Output helpers
 * ================================================================ */

static void winsftp_append(const char *text)
{
    int len;
    const char *p;
    char *buf, *q;
    if (!g_output)
        return;
    /* Suppress everything during background size-calculation ls */
    if (g_size_count_mode) return;

    /* When psftp reports which dir it's listing, update the remote path label */
    if (g_remote_path && strncmp(text, "Listing directory ", 18) == 0) {
        const char *s = text + 18;
        const char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            e--;
        if (e > s) {
            char tmp[512];
            int n = (int)(e - s);
            if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
            memcpy(tmp, s, n);
            tmp[n] = '\0';
            SetWindowText(g_remote_path, tmp);
        }
    }

    /* Suppress log output during auto-refresh enumeration.
     * Path-label updates above still fire; errors after enumeration are visible. */
    if (g_collecting_remote) return;

    /* On first connect psftp prints "Remote working directory is /path" */
    if (strncmp(text, "Remote working directory is ", 28) == 0) {
        const char *s = text + 28;
        const char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            e--;
        if (e > s && g_remote_path) {
            char tmp[512];
            int n = (int)(e - s);
            if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
            memcpy(tmp, s, n); tmp[n] = '\0';
            SetWindowText(g_remote_path, tmp);
        }
        if (!g_pending_refresh)
            g_pending_refresh = true;
    }

    /* Convert lone \n to \r\n for the EDIT control */
    buf = snewn(2 * strlen(text) + 1, char);
    for (p = text, q = buf; *p; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r'))
            *q++ = '\r';
        *q++ = *p;
    }
    *q = '\0';
    text = buf;

    len = GetWindowTextLength(g_output);

    /* Trim output buffer if too large (~30 KB EDIT limit) */
    if (len > 28000) {
        int cut = 14000;
        int line = (int)SendMessage(g_output, EM_LINEFROMCHAR, cut, 0);
        int next = (int)SendMessage(g_output, EM_LINEINDEX, line + 1, 0);
        if (next > cut) cut = next;
        SendMessage(g_output, EM_SETSEL, 0, cut);
        SendMessage(g_output, EM_REPLACESEL, FALSE, (LPARAM)"");
        len = GetWindowTextLength(g_output);
    }

    SendMessage(g_output, EM_SETSEL, len, len);
    SendMessage(g_output, EM_REPLACESEL, FALSE, (LPARAM)text);
    sfree(buf);
}

/* Called as printf/fprintf by psftp.c via -D macros */
int winsftp_printf(const char *fmt, ...)
{
    char buf[4096];
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vsprintf(buf, fmt, ap);
    va_end(ap);
    if (r > 0) winsftp_append(buf);
    return r;
}

int winsftp_fprintf(FILE *f, const char *fmt, ...)
{
    char buf[4096];
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vsprintf(buf, fmt, ap);
    va_end(ap);
    if (r > 0) winsftp_append(buf);
    return r;
}

/* ================================================================
 * Remote directory listing callbacks
 * (psftp.c guards these with #ifndef WINSFTP_BUILD)
 * ================================================================ */

void list_directory_from_sftp_warn_unsorted(void)
{
    winsftp_append("Directory is too large to sort; listing unsorted\n");
}

void list_directory_from_sftp_print(struct fxp_name *name)
{
    bool is_dir = (name->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
                  (name->attrs.permissions & 0170000U) == 0040000U;

    if (g_size_count_mode) {
        if (strcmp(name->filename, ".") != 0 &&
            strcmp(name->filename, "..") != 0) {
            if (is_dir) {
                /* Queue subdirectory for BFS traversal */
                if (g_size_queue_tail < SIZE_QUEUE_MAX) {
                    char child[MAX_PATH];
                    sprintf(child, "%s/%s",
                            g_size_current_dir, name->filename);
                    strncpy(g_size_dir_queue[g_size_queue_tail],
                            child, MAX_PATH - 1);
                    g_size_dir_queue[g_size_queue_tail][MAX_PATH - 1] = '\0';
                    g_size_queue_tail++;
                }
            } else if (name->attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
                g_info_size_sum += name->attrs.size;
                g_info_file_count++;
            }
        }
        return;
    }

    if (g_collecting_remote && g_remote_list) {
        /* Auto-refresh: buffer for sorted flush, no log echo */
        if (strcmp(name->filename, ".") == 0 ||
            strcmp(name->filename, "..") == 0)
            return;
        if (g_remote_buf_count < REMOTE_BUF_MAX) {
            strncpy(g_remote_buf[g_remote_buf_count], name->filename, MAX_PATH);
            g_remote_buf[g_remote_buf_count][MAX_PATH] = '\0';
            g_remote_type[g_remote_buf_count] = is_dir ? LV_FOLDER : LV_FILE;
            strncpy(g_remote_longname[g_remote_buf_count],
                    name->longname ? name->longname : "", 255);
            g_remote_longname[g_remote_buf_count][255] = '\0';
            g_remote_buf_count++;
        }
    } else {
        /* User-typed ls: echo to log as before */
        winsftp_append(name->longname);
        winsftp_append("\n");
    }
}

/* ================================================================
 * Local filesystem listing
 * ================================================================ */

static int local_dir_cmp(const void *a, const void *b)
{
    return stricmp(*(const char * const *)a, *(const char * const *)b);
}

static int local_file_cmp(const void *a, const void *b)
{
    return stricmp(*(const char * const *)a, *(const char * const *)b);
}

static void populate_local_list(void)
{
    char curdir[MAX_PATH];
    char search[MAX_PATH + 2];
    HANDLE hf;
    WIN32_FIND_DATA fd;
    char **dirs  = NULL, **files = NULL;
    int ndirs = 0, nfiles = 0, mdirs = 0, mfiles = 0;
    int i;

    if (!g_local_list) return;
    pane_clear(g_local_list);

    GetCurrentDirectory(MAX_PATH, curdir);
    if (g_local_path) SetWindowText(g_local_path, curdir);

    sprintf(search, "%s\\*", curdir);
    hf = FindFirstFile(search, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (ndirs >= mdirs) {
                    mdirs = mdirs ? mdirs * 2 : 32;
                    dirs  = sresize(dirs, mdirs, char *);
                }
                dirs[ndirs++] = dupstr(fd.cFileName);
            } else {
                if (nfiles >= mfiles) {
                    mfiles = mfiles ? mfiles * 2 : 32;
                    files  = sresize(files, mfiles, char *);
                }
                files[nfiles++] = dupstr(fd.cFileName);
            }
        } while (FindNextFile(hf, &fd));
        FindClose(hf);
    }

    if (ndirs  > 0) qsort(dirs,  (size_t)ndirs,  sizeof(dirs[0]),  local_dir_cmp);
    if (nfiles > 0) qsort(files, (size_t)nfiles, sizeof(files[0]), local_file_cmp);

    lv_insert(g_local_list, 0, "..", LV_PARENT);
    for (i = 0; i < ndirs;  i++) {
        lv_insert(g_local_list, 1 + i, dirs[i], LV_FOLDER);
        sfree(dirs[i]);
    }
    for (i = 0; i < nfiles; i++) {
        lv_insert(g_local_list, 1 + ndirs + i, files[i], LV_FILE);
        sfree(files[i]);
    }
    sfree(dirs);
    sfree(files);
}

/* ================================================================
 * Remote directory refresh
 * ================================================================ */

static void refresh_remote_list(void)
{
    if (!g_remote_list) return;
    pane_clear(g_remote_list);
    lv_insert(g_remote_list, 0, "..", LV_PARENT);
    g_collecting_remote = true;
    sfree(g_cmd_line);
    g_cmd_line = dupstr("ls");
    g_cmd_ready = true;
}

/* ================================================================
 * Progress callbacks
 * ================================================================ */

void sftp_progress_init(const char *fname, uint64_t total)
{
    char buf[300];
    int n;
    g_progress_total = total;
    n = (int)strlen(fname);
    if (n > 40) fname += n - 40;
    strncpy(g_progress_fname, fname, sizeof(g_progress_fname) - 1);
    g_progress_fname[sizeof(g_progress_fname) - 1] = '\0';
    if (total > 0)
        sprintf(buf, "%s  [0 / %lu KB]", g_progress_fname,
                (unsigned long)(total / 1024));
    else
        sprintf(buf, "%s  [transferring...]", g_progress_fname);
    if (g_status) SetWindowText(g_status, buf);
}

void sftp_progress_update(uint64_t done)
{
    char buf[300];
    if (!g_status) return;
    if (g_progress_total > 0) {
        unsigned pct = (unsigned)((done * 100) / g_progress_total);
        sprintf(buf, "%s  [%lu / %lu KB  %u%%]",
                g_progress_fname,
                (unsigned long)(done / 1024),
                (unsigned long)(g_progress_total / 1024),
                pct);
    } else {
        sprintf(buf, "%s  [%lu KB]",
                g_progress_fname,
                (unsigned long)(done / 1024));
    }
    SetWindowText(g_status, buf);
    {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            if (!IsDialogMessage(g_hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }
}

/* ================================================================
 * Platform helpers
 * ================================================================ */

void platform_get_x11_auth(struct X11Display *display, Conf *conf)
{
}
const bool platform_uses_x11_unix_by_default = true;

extern Conf *conf;

void platform_psftp_pre_conn_setup(LogPolicy *lp)
{
    char buf[256];
    const char *host = conf_get_str(conf, CONF_host);
    bool user_utf8;
    const char *user = conf_get_str_ambi(conf, CONF_username, &user_utf8);
    int port = conf_get_int(conf, CONF_port);

    if (user && user[0])
        sprintf(buf, "Connecting to %s@%s port %d...\r\n", user, host, port);
    else
        sprintf(buf, "Connecting to %s port %d...\r\n", host, port);
    winsftp_append(buf);
    if (g_status) SetWindowText(g_status, "Connecting...");

    if (restricted_acl())
        lp_eventlog(lp, "Running with restricted process ACL");

    if (g_pending_keyfile[0]) {
        Filename *fn = filename_from_str(g_pending_keyfile);
        conf_set_filename(conf, CONF_keyfile, fn);
        filename_free(fn);
        g_pending_keyfile[0] = '\0';
    }
}

static LRESULT CALLBACK InfoDlgProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_CREATE: {
        HFONT hf = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
        HWND hw;
        char title[MAX_PATH + 8];
        sprintf(title, "Name:  %s", g_info_name_buf);
        hw = CreateWindow("STATIC", title,
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            8, 8, 380, 18, hwnd, NULL, hinst, NULL);
        SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        hw = CreateWindow("STATIC",
            g_info_perm_buf[0] ? g_info_perm_buf : "(no attribute data)",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            8, 30, 380, 18, hwnd, NULL, hinst, NULL);
        SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        if (g_info_is_dir) {
            hw = CreateWindow("STATIC", "Disk usage:",
                WS_CHILD|WS_VISIBLE|SS_LEFT,
                8, 56, 88, 18, hwnd, NULL, hinst, NULL);
            SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
            g_info_calc_label = CreateWindow("STATIC", "Click Calculate...",
                WS_CHILD|WS_VISIBLE|SS_LEFT,
                100, 56, 278, 18, hwnd, NULL, hinst, NULL);
            SendMessage(g_info_calc_label, WM_SETFONT, (WPARAM)hf, FALSE);
            g_info_calc_btn = CreateWindow("BUTTON", "Calculate",
                WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                8, 82, 80, 24, hwnd, (HMENU)IDC_INFO_CALC, hinst, NULL);
            SendMessage(g_info_calc_btn, WM_SETFONT, (WPARAM)hf, FALSE);
            hw = CreateWindow("BUTTON", "Close",
                WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                96, 82, 80, 24, hwnd, (HMENU)IDC_INFO_CLOSE, hinst, NULL);
            SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        } else {
            g_info_calc_label = NULL;
            g_info_calc_btn   = NULL;
            hw = CreateWindow("BUTTON", "Close",
                WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                8, 56, 80, 24, hwnd, (HMENU)IDC_INFO_CLOSE, hinst, NULL);
            SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        }
        return 0;
      }
      case WM_COMMAND:
        if (LOWORD(wParam) == IDC_INFO_CLOSE ||
            LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
        } else if (LOWORD(wParam) == IDC_INFO_CALC && g_info_is_dir) {
            if (g_info_calc_label)
                SetWindowText(g_info_calc_label, "Calculating...");
            if (g_info_calc_btn)
                EnableWindow(g_info_calc_btn, FALSE);
            g_info_size_sum   = 0;
            g_info_file_count = 0;
            strncpy(g_size_count_dirname, g_info_dirname, MAX_PATH - 1);
            g_size_count_dirname[MAX_PATH - 1] = '\0';
            g_pending_size_count = true;
            sfree(g_cmd_line);
            g_cmd_line = dupstr("__size_count__");
            g_cmd_ready = true;
        }
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        g_info_hwnd       = NULL;
        g_info_calc_label = NULL;
        g_info_calc_btn   = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK PassDlgProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_CREATE: {
        HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND hw;
        hw = CreateWindow("STATIC", g_pass_label,
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            8, 8, 356, 40, hwnd, NULL, hinst, NULL);
        SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        g_pass_edit = CreateWindow("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            8, 54, 356, 22, hwnd, (HMENU)1001, hinst, NULL);
        if (!g_pass_echo)
            SendMessage(g_pass_edit, EM_SETPASSWORDCHAR, (WPARAM)'*', 0);
        SendMessage(g_pass_edit, WM_SETFONT, (WPARAM)hf, FALSE);
        hw = CreateWindow("BUTTON", "OK",
            WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            196, 86, 80, 24, hwnd, (HMENU)IDOK, hinst, NULL);
        SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        hw = CreateWindow("BUTTON", "Cancel",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            284, 86, 80, 24, hwnd, (HMENU)IDCANCEL, hinst, NULL);
        SendMessage(hw, WM_SETFONT, (WPARAM)hf, FALSE);
        return 0;
      }
      case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            if (g_pass_edit)
                GetWindowText(g_pass_edit, g_pass_buf, (int)sizeof(g_pass_buf));
            g_pass_ok = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wParam) == IDCANCEL) {
            g_pass_ok = false;
            DestroyWindow(hwnd);
        }
        return 0;
      case WM_DESTROY:
        g_pass_hwnd = NULL;
        g_pass_edit = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

SeatPromptResult filexfer_get_userpass_input(Seat *seat, prompts_t *p)
{
    size_t i;

    {
        static cmdline_get_passwd_input_state st =
            CMDLINE_GET_PASSWD_INPUT_STATE_INIT;
        SeatPromptResult spr = cmdline_get_passwd_input(p, &st, false);
        if (spr.kind != SPRK_INCOMPLETE)
            return spr;
    }

    for (i = 0; i < p->n_prompts; i++) {
        prompt_t *pr = p->prompts[i];
        MSG msg;
        RECT wr, dr;
        int dw, dh;

        strncpy(g_pass_label, pr->prompt, sizeof(g_pass_label) - 1);
        g_pass_label[sizeof(g_pass_label) - 1] = '\0';
        g_pass_echo  = pr->echo ? true : false;
        g_pass_ok    = false;
        g_pass_buf[0] = '\0';

        g_pass_hwnd = CreateWindowEx(
            WS_EX_DLGMODALFRAME,
            "WinSFTPPass", "Credential Required",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            0, 0, 380, 140,
            g_hwnd, NULL, hinst, NULL);

        if (!g_pass_hwnd)
            return SPR_USER_ABORT;

        /* centre over main window */
        GetWindowRect(g_hwnd,      &wr);
        GetWindowRect(g_pass_hwnd, &dr);
        dw = dr.right  - dr.left;
        dh = dr.bottom - dr.top;
        SetWindowPos(g_pass_hwnd, HWND_TOP,
            wr.left + (wr.right  - wr.left - dw) / 2,
            wr.top  + (wr.bottom - wr.top  - dh) / 2,
            0, 0, SWP_NOSIZE);

        ShowWindow(g_pass_hwnd, SW_SHOW);
        if (g_pass_edit) SetFocus(g_pass_edit);

        while (g_pass_hwnd) {
            if (!GetMessage(&msg, NULL, 0, 0)) {
                g_running = false;
                if (g_pass_hwnd) DestroyWindow(g_pass_hwnd);
                return SPR_USER_ABORT;
            }
            if (!IsDialogMessage(g_pass_hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            run_toplevel_callbacks();
        }

        if (!g_pass_ok)
            return SPR_USER_ABORT;

        prompt_set_result(pr, g_pass_buf);
        smemclr(g_pass_buf, sizeof(g_pass_buf));
    }

    return SPR_OK;
}

/* ================================================================
 * Console interface
 * ================================================================ */

void cleanup_exit(int code)
{
    sk_cleanup();
    random_save_seed();
    exit(code);
}

void console_print_error_msg(const char *prefix, const char *msg)
{
    char buf[1024];
    sprintf(buf, "%s: %s\r\n", prefix, msg);
    winsftp_append(buf);
    MessageBox(g_hwnd, msg, prefix, MB_OK | MB_ICONERROR);
}

void console_print_error_msg_fmt_v(
    const char *prefix, const char *fmt, va_list ap)
{
    char *msg = dupvprintf(fmt, ap);
    console_print_error_msg(prefix, msg);
    sfree(msg);
}

void console_print_error_msg_fmt(const char *prefix, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_print_error_msg_fmt_v(prefix, fmt, ap);
    va_end(ap);
}

void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    winsftp_append("Connection error: ");
    winsftp_append(msg);
    winsftp_append("\r\n");
    sfree(msg);
}

void nonfatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_print_error_msg_fmt_v("ERROR", fmt, ap);
    va_end(ap);
}

bool console_batch_mode = false;

bool console_set_batch_mode(bool newvalue)
{
    console_batch_mode = newvalue;
    return true;
}

void timer_change_notify(unsigned long next)
{
}

static char *sdt_to_string(SeatDialogText *text)
{
    strbuf *sb = strbuf_new();
    size_t i;
    for (i = 0; i < text->nitems; i++) {
        SeatDialogTextItem *item = &text->items[i];
        switch (item->type) {
          case SDT_PARA:
          case SDT_SCARY_HEADING:
          case SDT_DISPLAY:
          case SDT_TITLE:
            put_dataz(sb, item->text);
            put_dataz(sb, "\n\n");
            break;
          case SDT_MORE_INFO_KEY:
            put_dataz(sb, item->text);
            put_dataz(sb, ": ");
            break;
          case SDT_MORE_INFO_VALUE_SHORT:
            put_dataz(sb, item->text);
            put_dataz(sb, "\n");
            break;
          case SDT_MORE_INFO_VALUE_BLOB:
            put_dataz(sb, item->text);
            put_dataz(sb, "\n");
            break;
          default:
            break;
        }
    }
    return strbuf_to_str(sb);
}

SeatPromptResult console_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char *msg = sdt_to_string(text);
    int r = MessageBox(g_hwnd, msg, "Host Key Verification",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    sfree(msg);
    if (r == IDYES) {
        store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    }
    return SPR_USER_ABORT;
}

SeatPromptResult console_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char *msg = sdt_to_string(text);
    int r = MessageBox(g_hwnd, msg, "Weak Cryptography Warning",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    sfree(msg);
    return (r == IDYES) ? SPR_OK : SPR_USER_ABORT;
}

SeatPromptResult console_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char *msg = sdt_to_string(text);
    int r = MessageBox(g_hwnd, msg, "Cached Host Key Warning",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    sfree(msg);
    return (r == IDYES) ? SPR_OK : SPR_USER_ABORT;
}

const SeatDialogPromptDescriptions *console_prompt_descriptions(Seat *seat)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "click Yes",
        .hk_connect_once_action = "click No",
        .hk_cancel_action = "click No",
        .hk_cancel_action_Participle = "Clicking No",
        .weak_accept_action = "click Yes",
        .weak_cancel_action = "click No",
    };
    return &descs;
}

void console_connection_fatal(Seat *seat, const char *msg)
{
    winsftp_append("Connection error: ");
    winsftp_append(msg);
    winsftp_append("\r\n");
}

void console_nonfatal(Seat *seat, const char *msg)
{
    console_print_error_msg("ERROR", msg);
}

void console_set_trust_status(Seat *seat, bool trusted) { }
bool console_can_set_trust_status(Seat *seat) { return false; }
bool console_has_mixed_input_stream(Seat *seat) { return false; }

int console_askappend(LogPolicy *lp, Filename *filename,
                      void (*callback)(void *ctx, int result), void *ctx)
{
    char buf[512];
    sprintf(buf, "Log file \"%s\" already exists.\n"
            "Append to it?", filename_to_str(filename));
    int r = MessageBox(g_hwnd, buf, "WinSFTP",
                       MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDYES)   return 1;
    if (r == IDNO)    return 2;
    return 0;
}

void console_logging_error(LogPolicy *lp, const char *msg)
{
    char buf[512];
    sprintf(buf, "Log error: %s\r\n", msg);
    winsftp_append(buf);
}

void console_eventlog(LogPolicy *lp, const char *msg) { }

StripCtrlChars *console_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    return stripctrl_new(bs_out, false, L'\0');
}

bool console_set_stdio_prompts(bool newvalue) { return true; }
bool set_legacy_charset_handling(bool newvalue) { return true; }

void old_keyfile_warning(void)
{
    MessageBox(g_hwnd,
        "You are loading an SSH-2 private key in an old file format.\n"
        "The key is not fully tamperproof. Consider re-saving it with\n"
        "PuTTYgen to convert it to the new format.",
        "Old key file format", MB_OK | MB_ICONWARNING);
}

void pgp_fingerprints(void)
{
    message_box(g_hwnd,
        "These are the fingerprints of the PuTTY PGP Master Keys. They can\n"
        "be used to establish a trust path from this executable to another\n"
        "one. See the manual for more information.\n"
        "(Note: these fingerprints have nothing to do with SSH!)\n\n"
        "PuTTY Master Key as of " PGP_MASTER_KEY_YEAR
        " (" PGP_MASTER_KEY_DETAILS "):\n"
        "  " PGP_MASTER_KEY_FP "\n\n"
        "Previous Master Key (" PGP_PREV_MASTER_KEY_YEAR
        ", " PGP_PREV_MASTER_KEY_DETAILS "):\n"
        "  " PGP_PREV_MASTER_KEY_FP,
        "PGP fingerprints", MB_ICONINFORMATION | MB_OK,
        false, HELPCTXID(pgp_fingerprints));
}

static const LogPolicyVtable winsftp_logpolicy_vt = {
    .eventlog       = console_eventlog,
    .askappend      = console_askappend,
    .logging_error  = console_logging_error,
    .verbose        = cmdline_lp_verbose,
};
LogPolicy console_cli_logpolicy[1] = {{ &winsftp_logpolicy_vt }};

/* ================================================================
 * Event loop
 * ================================================================ */

static bool pump_messages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_running = false;
            return false;
        }
        if (!IsDialogMessage(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    run_toplevel_callbacks();
    return true;
}

int ssh_sftp_loop_iteration(void)
{
    fd_set readfds;
    int ret;
    unsigned long now = GETTICKCOUNT(), then;
    SOCKET skt = winselcli_unique_socket();

    if (skt == INVALID_SOCKET)
        return -1;

    if (!pump_messages())
        return -1;

    if (toplevel_callback_pending()) {
        while (toplevel_callback_pending())
            run_toplevel_callbacks();
        return 0;
    }

    if (socket_writable(skt))
        select_result((WPARAM)skt, (LPARAM)FD_WRITE);

    do {
        unsigned long next;
        long ticks;
        struct timeval tv;

        if (run_timers(now, &next)) {
            then = now;
            now = GETTICKCOUNT();
            if (now - then > next - then)
                ticks = 0;
            else
                ticks = next - now;
            if (ticks > 100) ticks = 100;
            tv.tv_sec  = ticks / 1000;
            tv.tv_usec = (ticks % 1000) * 1000;
        } else {
            tv.tv_sec  = 0;
            tv.tv_usec = 100000;
        }

        FD_ZERO(&readfds);
        FD_SET(skt, &readfds);
        ret = p_select(1, &readfds, NULL, NULL, &tv);

        if (ret < 0)
            return -1;

        if (ret == 0) {
            now = GETTICKCOUNT();
            if (!pump_messages())
                return -1;
        } else {
            now = GETTICKCOUNT();
        }

    } while (ret == 0);

    select_result((WPARAM)skt, (LPARAM)FD_READ);
    return 0;
}

char *ssh_sftp_get_cmdline(const char *prompt, bool no_fds_ok)
{
    MSG msg;
    char *ret;

    /* Size-count ls finished: recurse into next queued dir, or show result */
    if (g_size_count_mode) {
        if (g_size_queue_head < g_size_queue_tail) {
            /* More subdirectories to scan — pop next dir and issue ls */
            char cmd[MAX_PATH + 8];
            strncpy(g_size_current_dir,
                    g_size_dir_queue[g_size_queue_head], MAX_PATH - 1);
            g_size_current_dir[MAX_PATH - 1] = '\0';
            g_size_queue_head++;
            sprintf(cmd, "ls %s", g_size_current_dir);
            strncpy(g_last_completed_cmd, cmd,
                    sizeof(g_last_completed_cmd) - 1);
            return dupstr(cmd);
        }
        /* Queue exhausted — display final result */
        {
            char result[128];
            uint64_t sz = g_info_size_sum;
            int fc = g_info_file_count;
            g_size_count_mode = false;
            if (g_info_calc_label) {
                if (sz > (uint64_t)1073741824)
                    sprintf(result, "%lu GB  (%d files)",
                            (unsigned long)(sz >> 30), fc);
                else if (sz > (uint64_t)1048576)
                    sprintf(result, "%lu MB  (%d files)",
                            (unsigned long)(sz >> 20), fc);
                else if (sz > 1024)
                    sprintf(result, "%lu KB  (%d files)",
                            (unsigned long)(sz >> 10), fc);
                else
                    sprintf(result, "%lu bytes  (%d files)",
                            (unsigned long)sz, fc);
                SetWindowText(g_info_calc_label, result);
            }
            if (g_info_calc_btn)
                EnableWindow(g_info_calc_btn, TRUE);
        }
    }

    /* Previous ls command finished — flush sorted remote listing */
    if (g_collecting_remote) {
        g_collecting_remote = false;
        flush_remote_buf();
    }

    /* Refresh panes based on what the previous command was */
    if (g_last_completed_cmd[0]) {
        if (strncmp(g_last_completed_cmd, "get ", 4) == 0)
            populate_local_list();
        if (strncmp(g_last_completed_cmd, "put ",  4) == 0 ||
            strncmp(g_last_completed_cmd, "rm ",   3) == 0 ||
            strncmp(g_last_completed_cmd, "rmdir ", 6) == 0)
            g_pending_refresh = true;
        g_last_completed_cmd[0] = '\0';
    }

    /* rmdir step 2: rm /* already sent; now send rmdir */
    if (g_rmdir_step == 2) {
        char cmd[MAX_PATH + 8];
        g_rmdir_step = 0;
        sprintf(cmd, "rmdir %s", g_rmdir_pending);
        g_rmdir_pending[0] = '\0';
        g_pending_refresh = true;
        winsftp_append("> "); winsftp_append(cmd); winsftp_append("\r\n");
        strncpy(g_last_completed_cmd, cmd, sizeof(g_last_completed_cmd) - 1);
        return dupstr(cmd);
    }

    /* Auto-refresh remote listing if requested (e.g. after a cd) */
    if (g_pending_refresh) {
        g_pending_refresh = false;
        if (g_remote_list) {
            pane_clear(g_remote_list);
            lv_insert(g_remote_list, 0, "..", LV_PARENT);
        }
        g_collecting_remote = true;
        return dupstr("ls");
    }

    /* Drain command queue (multi-select batch transfers) */
    if (!g_cmd_ready) {
        char *queued = cmd_queue_pop();
        if (queued) {
            if (strncmp(queued, "cd ", 3) == 0 || strcmp(queued, "cd") == 0)
                g_pending_refresh = true;
            if (strncmp(queued, "lcd ", 4) == 0)
                populate_local_list();
            strncpy(g_last_completed_cmd, queued,
                    sizeof(g_last_completed_cmd) - 1);
            winsftp_append("> "); winsftp_append(queued); winsftp_append("\r\n");
            return queued;
        }
    }

    /* Clear progress status when ready for a new command */
    if (g_status) SetWindowText(g_status, "Ready");

    /* FIX: only wait if no command has been pre-queued (e.g. from connect dialog) */
    if (!g_cmd_ready) {
        g_cmd_line = NULL;
        while (!g_cmd_ready) {
            if (!GetMessage(&msg, NULL, 0, 0)) {
                g_running = false;
                return NULL;
            }
            if (!IsDialogMessage(g_hwnd, &msg) &&
                !(g_info_hwnd && IsDialogMessage(g_info_hwnd, &msg))) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            run_toplevel_callbacks();
        }
    }

    ret = g_cmd_line;
    g_cmd_line  = NULL;
    g_cmd_ready = false; /* reset for next call */

    /* Intercept Calculate button trigger */
    if (g_pending_size_count) {
        char cmd[MAX_PATH + 8];
        g_pending_size_count = false;
        g_info_size_sum      = 0;
        g_info_file_count    = 0;
        g_size_queue_head    = 0;
        g_size_queue_tail    = 0;
        strncpy(g_size_current_dir, g_size_count_dirname, MAX_PATH - 1);
        g_size_current_dir[MAX_PATH - 1] = '\0';
        g_size_count_mode    = true;
        sfree(ret);
        sprintf(cmd, "ls %s", g_size_count_dirname);
        strncpy(g_last_completed_cmd, cmd, sizeof(g_last_completed_cmd) - 1);
        return dupstr(cmd);
    }

    /* Local commands handled here */
    if (ret && strcmp(ret, "/cls") == 0) {
        sfree(ret);
        if (g_output) SetWindowText(g_output, "");
        return ssh_sftp_get_cmdline(prompt, no_fds_ok);
    }

    if (ret && strcmp(ret, "/quit") == 0) {
        sfree(ret);
        g_running = false;
        if (g_hwnd) DestroyWindow(g_hwnd);
        return NULL;
    }

    /* After a cd or lcd command, refresh the appropriate pane */
    if (ret) {
        if (strncmp(ret, "cd ",  3) == 0 ||
            strncmp(ret, "CD ",  3) == 0 ||
            strcmp(ret,  "cd")    == 0 ||
            strcmp(ret,  "CD")    == 0)
            g_pending_refresh = true;

        if (strncmp(ret, "lcd ", 4) == 0 ||
            strncmp(ret, "LCD ", 4) == 0)
            populate_local_list();
    }

    if (ret)
        strncpy(g_last_completed_cmd, ret,
                sizeof(g_last_completed_cmd) - 1);
    return ret;
}

/* ================================================================
 * Tab completion
 * ================================================================ */

static void split_path(const char *token,
                        char **dir_out, char **prefix_out)
{
    const char *p = token + strlen(token);
    while (p > token && p[-1] != '\\' && p[-1] != '/')
        p--;
    if (p == token) {
        *dir_out    = dupstr("");
        *prefix_out = dupstr(token);
    } else {
        int dir_len = (int)(p - token);
        *dir_out    = snewn(dir_len + 1, char);
        memcpy(*dir_out, token, dir_len);
        (*dir_out)[dir_len] = '\0';
        *prefix_out = dupstr(p);
    }
}

static void tab_complete(void)
{
    char input_buf[1024];
    int input_len, cursor_pos, token_offset, token_len, i;
    char *token, *tail, *dir_part, *prefix_part;
    char search_pat[MAX_PATH + 4];
    HANDLE hFind;
    WIN32_FIND_DATA fd;
    char **matches = NULL;
    int n_matches = 0, max_matches = 0;
    char common[MAX_PATH];
    int common_len;

    if (!g_input) return;

    input_len = GetWindowText(g_input, input_buf, (int)sizeof(input_buf) - 1);
    input_buf[input_len] = '\0';

    {
        DWORD sel_start, sel_end;
        SendMessage(g_input, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
        cursor_pos = (int)sel_start;
        if (cursor_pos > input_len) cursor_pos = input_len;
    }

    token_offset = 0;
    for (i = 0; i < cursor_pos; i++)
        if (input_buf[i] == ' ')
            token_offset = i + 1;

    token_len = cursor_pos - token_offset;
    token = snewn(token_len + 1, char);
    memcpy(token, input_buf + token_offset, token_len);
    token[token_len] = '\0';
    tail = dupstr(input_buf + cursor_pos);

    split_path(token, &dir_part, &prefix_part);

    if ((int)(strlen(dir_part) + strlen(prefix_part)) + 2 > MAX_PATH)
        goto cleanup;
    sprintf(search_pat, "%s%s*", dir_part, prefix_part);

    hFind = FindFirstFile(search_pat, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const char *name = fd.cFileName;
            bool is_dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            char *entry;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (n_matches >= max_matches) {
                max_matches = max_matches ? max_matches * 2 : 16;
                matches = sresize(matches, max_matches, char *);
            }
            if (is_dir)
                entry = dupcat(dir_part, name, "\\");
            else
                entry = dupcat(dir_part, name);
            matches[n_matches++] = entry;
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }

    if (n_matches == 0) {
        MessageBeep((UINT)-1);
        goto cleanup;
    }

    if (n_matches == 1) {
        const char *m = matches[0];
        int mlen = (int)strlen(m);
        bool add_space = (mlen == 0 || m[mlen - 1] != '\\');
        int tail_len = (int)strlen(tail);
        int new_cursor;
        char *new_text = snewn(token_offset + mlen + 1 + tail_len + 1, char);
        memcpy(new_text, input_buf, token_offset);
        memcpy(new_text + token_offset, m, mlen);
        new_cursor = token_offset + mlen;
        if (add_space)
            new_text[new_cursor++] = ' ';
        memcpy(new_text + new_cursor, tail, tail_len);
        new_text[new_cursor + tail_len] = '\0';
        SetWindowText(g_input, new_text);
        SendMessage(g_input, EM_SETSEL, new_cursor, new_cursor);
        sfree(new_text);
        goto cleanup;
    }

    winsftp_append("Completions:\r\n");
    for (i = 0; i < n_matches; i++) {
        winsftp_append("  ");
        winsftp_append(matches[i]);
        winsftp_append("\r\n");
    }

    {
        int dir_len = (int)strlen(dir_part);
        const char *first = matches[0] + dir_len;
        common_len = (int)strlen(first);
        if (common_len >= MAX_PATH) common_len = MAX_PATH - 1;
        strncpy(common, first, common_len);
        common[common_len] = '\0';
        for (i = 1; i < n_matches; i++) {
            const char *name = matches[i] + dir_len;
            int j;
            for (j = 0; j < common_len && name[j]; j++) {
                if (tolower((unsigned char)common[j]) !=
                    tolower((unsigned char)name[j]))
                    break;
            }
            common_len = j;
        }
        common[common_len] = '\0';
    }

    if (common_len > (int)strlen(prefix_part)) {
        char *completed = dupcat(dir_part, common);
        int clen = (int)strlen(completed);
        int tail_len = (int)strlen(tail);
        int new_cursor = token_offset + clen;
        char *new_text = snewn(new_cursor + tail_len + 1, char);
        memcpy(new_text, input_buf, token_offset);
        memcpy(new_text + token_offset, completed, clen);
        memcpy(new_text + new_cursor, tail, tail_len);
        new_text[new_cursor + tail_len] = '\0';
        SetWindowText(g_input, new_text);
        SendMessage(g_input, EM_SETSEL, new_cursor, new_cursor);
        sfree(completed);
        sfree(new_text);
    }

  cleanup:
    for (i = 0; i < n_matches; i++)
        sfree(matches[i]);
    sfree(matches);
    sfree(token);
    sfree(tail);
    sfree(dir_part);
    sfree(prefix_part);
}

/* ================================================================
 * Input/Output subclass procedures
 * ================================================================ */

static LRESULT CALLBACK OutputSubclassProc(HWND hwnd, UINT umsg,
                                            WPARAM wParam, LPARAM lParam)
{
    if (umsg == WM_GETDLGCODE)
        return CallWindowProc(g_output_orig_proc, hwnd, umsg, wParam, lParam)
               & ~DLGC_WANTTAB;
    if (umsg == WM_LBUTTONUP) {
        LRESULT r = CallWindowProc(g_output_orig_proc, hwnd, umsg, wParam, lParam);
        if (g_input) SetFocus(g_input);
        return r;
    }
    return CallWindowProc(g_output_orig_proc, hwnd, umsg, wParam, lParam);
}

static LRESULT CALLBACK InputSubclassProc(HWND hwnd, UINT umsg,
                                           WPARAM wParam, LPARAM lParam)
{
    if (umsg == WM_GETDLGCODE)
        return CallWindowProc(g_input_orig_proc, hwnd, umsg, wParam, lParam)
               | DLGC_WANTTAB;

    if (umsg == WM_KEYDOWN && wParam == 'C' &&
            (GetKeyState(VK_CONTROL) & 0x8000) && g_output) {
        int sel_start = 0, sel_end = 0;
        SendMessage(g_output, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
        if (sel_start != sel_end) {
            SendMessage(g_output, WM_COPY, 0, 0);
            return 0;
        }
    }

    if (umsg == WM_KEYDOWN && wParam == VK_UP) {
        if (g_history_count == 0) return 0;
        if (g_history_pos == -1) {
            int len = GetWindowTextLength(hwnd);
            sfree(g_history_saved);
            g_history_saved = snewn(len + 1, char);
            GetWindowText(hwnd, g_history_saved, len + 1);
            g_history_pos = g_history_count - 1;
        } else if (g_history_pos > 0) {
            g_history_pos--;
        } else {
            return 0;
        }
        SetWindowText(hwnd, g_history[g_history_pos]);
        {
            int len = (int)strlen(g_history[g_history_pos]);
            SendMessage(hwnd, EM_SETSEL, len, len);
        }
        return 0;
    }

    if (umsg == WM_KEYDOWN && wParam == VK_DOWN) {
        if (g_history_pos == -1) return 0;
        if (g_history_pos < g_history_count - 1) {
            g_history_pos++;
            SetWindowText(hwnd, g_history[g_history_pos]);
            {
                int len = (int)strlen(g_history[g_history_pos]);
                SendMessage(hwnd, EM_SETSEL, len, len);
            }
        } else {
            g_history_pos = -1;
            SetWindowText(hwnd, g_history_saved ? g_history_saved : "");
            sfree(g_history_saved);
            g_history_saved = NULL;
            {
                int len = GetWindowTextLength(hwnd);
                SendMessage(hwnd, EM_SETSEL, len, len);
            }
        }
        return 0;
    }

    if (umsg == WM_KEYDOWN && wParam == VK_TAB) {
        tab_complete();
        return 0;
    }
    if (umsg == WM_CHAR && wParam == '\t')
        return 0;

    return CallWindowProc(g_input_orig_proc, hwnd, umsg, wParam, lParam);
}

/* ================================================================
 * Address-bar EDIT subclass procs
 * ================================================================ */

static LRESULT CALLBACK LocalPathProc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    /* Tell IsDialogMessage to hand Enter to us instead of the default button */
    if (msg == WM_GETDLGCODE) {
        LRESULT r = CallWindowProc(g_local_path_orig_proc, hwnd, msg, wp, lp);
        if (lp) {
            MSG *pmsg = (MSG *)lp;
            if (pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
                r |= DLGC_WANTMESSAGE;
        }
        return r;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        char path[MAX_PATH];
        char *err;
        GetWindowText(hwnd, path, MAX_PATH);
        if (path[0]) { err = psftp_lcd(path); if (err) sfree(err); populate_local_list(); }
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r') return 0;
    return CallWindowProc(g_local_path_orig_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK RemotePathProc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    /* Tell IsDialogMessage to hand Enter to us instead of the default button */
    if (msg == WM_GETDLGCODE) {
        LRESULT r = CallWindowProc(g_remote_path_orig_proc, hwnd, msg, wp, lp);
        if (lp) {
            MSG *pmsg = (MSG *)lp;
            if (pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
                r |= DLGC_WANTMESSAGE;
        }
        return r;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        char path[MAX_PATH];
        char cmd[MAX_PATH + 8];
        GetWindowText(hwnd, path, MAX_PATH);
        if (path[0]) {
            sprintf(cmd, "cd %s", path);
            sfree(g_cmd_line);
            g_cmd_line = dupstr(cmd);
            g_cmd_ready = true;
            g_pending_refresh = true;
        }
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r') return 0;
    return CallWindowProc(g_remote_path_orig_proc, hwnd, msg, wp, lp);
}

/* ================================================================
 * Navigate helpers (shared by Enter key, double-click, context menus)
 * ================================================================ */

static void navigate_local_focused(void)
{
    char entry[MAX_PATH]; int itype; char *err;
    if (!g_local_list) return;
    if (lv_get_focused(g_local_list, entry, MAX_PATH, &itype) < 0) return;
    if (itype == LV_PARENT) {
        err = psftp_lcd(".."); if (err) sfree(err);
        populate_local_list();
    } else if (itype == LV_FOLDER) {
        err = psftp_lcd(entry); if (err) sfree(err);
        populate_local_list();
    }
}

static void navigate_remote_focused(void)
{
    char entry[MAX_PATH]; int itype;
    if (!g_remote_list) return;
    if (lv_get_focused(g_remote_list, entry, MAX_PATH, &itype) < 0) return;
    if (itype == LV_PARENT) {
        sfree(g_cmd_line); g_cmd_line = dupstr("cd ..");
        g_cmd_ready = true; g_pending_refresh = true;
    } else if (itype == LV_FOLDER) {
        char cmd[MAX_PATH + 8];
        sprintf(cmd, "cd %s", entry);
        sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
        g_cmd_ready = true; g_pending_refresh = true;
    }
}

/* ================================================================
 * Layout
 * ================================================================ */

static void do_layout(HWND hwnd)
{
    RECT rc;
    int w, h, half;
    int browser_h, panes_y, btnrow_y, log_y, input_y, status_y;
    int bx;

    GetClientRect(hwnd, &rc);
    w = rc.right  - rc.left;
    h = rc.bottom - rc.top;

    /* Layout built bottom-up so panes claim all remaining space */
    status_y  = h - STATUS_H;
    input_y   = status_y - MARGIN - INPUT_H;
    log_y     = input_y  - MARGIN - LOG_H;   /* fixed 6-line log height */
    if (log_y < MARGIN) log_y = MARGIN;
    btnrow_y  = log_y - MARGIN - BTNROW_PAD - BTNROW_H; /* gap below buttons */
    if (btnrow_y < PATH_H) btnrow_y = PATH_H;
    panes_y   = PATH_H;
    browser_h = btnrow_y - BTNROW_PAD - panes_y; /* gap above buttons */
    if (browser_h < 60) browser_h = 60;

    half = w / 2;

    /* Address bars */
    if (g_local_path)  MoveWindow(g_local_path,  0,    0, half-1, PATH_H, TRUE);
    if (g_remote_path) MoveWindow(g_remote_path, half, 0, w-half, PATH_H, TRUE);
    /* Browser panes */
    if (g_local_list)  MoveWindow(g_local_list,  0,    panes_y, half-1, browser_h, TRUE);
    if (g_remote_list) MoveWindow(g_remote_list, half, panes_y, w-half, browser_h, TRUE);
    /* Resize ListView "Name" column — only when ListView panes are active */
    if (g_has_listview) {
        int nw_loc, nw_rem;
        nw_loc = (half - 1) - 70; if (nw_loc < 40) nw_loc = 40;
        nw_rem = (w - half)  - 70; if (nw_rem < 40) nw_rem = 40;
        if (g_local_list)
            SendMessage(g_local_list,  LVM_SETCOLUMNWIDTH, 0, (LPARAM)nw_loc);
        if (g_remote_list)
            SendMessage(g_remote_list, LVM_SETCOLUMNWIDTH, 0, (LPARAM)nw_rem);
    }
    /* Transfer buttons — left-aligned, BTN_GAP spacing */
    bx = MARGIN;
    if (g_get_btn)     { MoveWindow(g_get_btn,     bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); bx += PBTN_W+BTN_GAP; }
    if (g_put_btn)     { MoveWindow(g_put_btn,     bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); bx += PBTN_W+BTN_GAP; }
    if (g_refresh_btn) { MoveWindow(g_refresh_btn, bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); bx += PBTN_W+BTN_GAP; }
    if (g_mkdir_btn)   { MoveWindow(g_mkdir_btn,   bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); bx += PBTN_W+BTN_GAP; }
    if (g_delete_btn)  { MoveWindow(g_delete_btn,  bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); bx += PBTN_W+BTN_GAP; }
    if (g_rmdir_btn)   { MoveWindow(g_rmdir_btn,   bx, btnrow_y, PBTN_W, BTNROW_H, TRUE); }
    /* Nav buttons — right-aligned, BTN_GAP spacing */
    bx = w - MARGIN;
    if (g_toolbar_refresh) { bx -= TBREF_W; MoveWindow(g_toolbar_refresh, bx, btnrow_y, TBREF_W, BTNROW_H, TRUE); bx -= BTN_GAP; }
    if (g_toolbar_rup)     { bx -= PBTN_W;  MoveWindow(g_toolbar_rup,     bx, btnrow_y, PBTN_W,  BTNROW_H, TRUE); bx -= BTN_GAP; }
    if (g_toolbar_lup)     { bx -= PBTN_W;  MoveWindow(g_toolbar_lup,     bx, btnrow_y, PBTN_W,  BTNROW_H, TRUE); bx -= BTN_GAP; }
    if (g_toolbar_connect) { bx -= PBTN_W;  MoveWindow(g_toolbar_connect, bx, btnrow_y, PBTN_W,  BTNROW_H, TRUE); }
    /* Fixed-height log area */
    if (g_output) MoveWindow(g_output,  0, log_y,   w,              LOG_H,   TRUE);
    /* Input row */
    if (g_input)  MoveWindow(g_input,   0, input_y, w-BTN_W-MARGIN, INPUT_H, TRUE);
    if (g_send)   MoveWindow(g_send,    w-BTN_W, input_y, BTN_W,    INPUT_H, TRUE);
    /* Status */
    if (g_status) MoveWindow(g_status,  0, status_y, w,             STATUS_H, TRUE);
}

/* ================================================================
 * Small icons for owner-draw listboxes (pure GDI, no bitmaps)
 * ================================================================ */

static void draw_icon_folder(HDC hdc, int x, int y)
{
    RECT r;
    HBRUSH hbr;
    r.left = x; r.top = y+1; r.right = x+5; r.bottom = y+4;
    hbr = CreateSolidBrush(RGB(200,160,40));
    FillRect(hdc, &r, hbr); DeleteObject(hbr);
    r.left = x; r.top = y+3; r.right = x+14; r.bottom = y+12;
    hbr = CreateSolidBrush(RGB(240,200,60));
    FillRect(hdc, &r, hbr); DeleteObject(hbr);
    FrameRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

static void draw_icon_file(HDC hdc, int x, int y)
{
    RECT r;
    HPEN pen, oldpen;
    HBRUSH hbr;
    r.left = x; r.top = y+1; r.right = x+12; r.bottom = y+13;
    hbr = CreateSolidBrush(RGB(255,255,255));
    FillRect(hdc, &r, hbr); DeleteObject(hbr);
    FrameRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    pen = CreatePen(PS_SOLID, 1, RGB(128,128,128));
    oldpen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x+9, y+1, NULL); LineTo(hdc, x+12, y+4);
    SelectObject(hdc, oldpen); DeleteObject(pen);
}

/* ================================================================
 * ListView helpers
 * ================================================================ */

static void pane_clear(HWND hw)
{
    if (!hw) return;
    if (g_has_listview)
        SendMessage(hw, LVM_DELETEALLITEMS, 0, 0);
    else
        SendMessage(hw, LB_RESETCONTENT, 0, 0);
}

static void lv_insert(HWND hw, int idx, const char *name, int itype)
{
    LV_ITEM lvi;
    int actual;
    const char *type_str;

    if (!g_has_listview) {
        int pos;
        const char *display = (itype == LV_PARENT) ? ".." : name;
        pos = (int)SendMessage(hw, LB_INSERTSTRING, (WPARAM)idx, (LPARAM)display);
        if (pos >= 0 && pos != LB_ERR)
            SendMessage(hw, LB_SETITEMDATA, (WPARAM)pos, (LPARAM)(DWORD)itype);
        return;
    }
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask     = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    lvi.iItem    = idx;
    lvi.iSubItem = 0;
    lvi.pszText  = (char *)name;
    lvi.iImage   = (itype == LV_FILE) ? 1 : 0;
    lvi.lParam   = (LPARAM)itype;
    actual = (int)SendMessage(hw, LVM_INSERTITEM, 0, (LPARAM)&lvi);
    if (actual < 0) return;
    type_str = (itype == LV_PARENT) ? "" :
               (itype == LV_FOLDER) ? "Folder" : "File";
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask     = LVIF_TEXT;
    lvi.iItem    = actual;
    lvi.iSubItem = 1;
    lvi.pszText  = (char *)type_str;
    SendMessage(hw, LVM_SETITEM, 0, (LPARAM)&lvi);
}

static int lv_get_focused(HWND hw, char *name, int namelen, int *type_out)
{
    LV_ITEM lvi;
    int idx;

    if (!g_has_listview) {
        char buf[MAX_PATH + 2];
        int itype;
        idx = (int)SendMessage(hw, LB_GETCARETINDEX, 0, 0);
        if (idx < 0 || idx == LB_ERR)
            idx = (int)SendMessage(hw, LB_GETCURSEL, 0, 0);
        if (idx < 0 || idx == LB_ERR) return -1;
        if (SendMessage(hw, LB_GETTEXT, (WPARAM)idx, (LPARAM)buf) == LB_ERR) return -1;
        itype = (int)(DWORD)SendMessage(hw, LB_GETITEMDATA, (WPARAM)idx, 0);
        strncpy(name, buf, namelen - 1);
        name[namelen - 1] = '\0';
        if (type_out) *type_out = itype;
        return idx;
    }

    idx = (int)SendMessage(hw, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    if (idx < 0) idx = (int)SendMessage(hw, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (idx < 0) return -1;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask       = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem      = idx;
    lvi.pszText    = name;
    lvi.cchTextMax = namelen;
    if (!SendMessage(hw, LVM_GETITEM, 0, (LPARAM)&lvi)) return -1;
    if (type_out) *type_out = (int)lvi.lParam;
    return idx;
}

static int lv_get_item_at(HWND hw, int idx, char *name, int namelen, int *type_out)
{
    LV_ITEM lvi;

    if (!g_has_listview) {
        char buf[MAX_PATH + 2];
        int itype;
        if (SendMessage(hw, LB_GETTEXT, (WPARAM)idx, (LPARAM)buf) == LB_ERR) return -1;
        itype = (int)(DWORD)SendMessage(hw, LB_GETITEMDATA, (WPARAM)idx, 0);
        strncpy(name, buf, namelen - 1);
        name[namelen - 1] = '\0';
        if (type_out) *type_out = itype;
        return idx;
    }

    memset(&lvi, 0, sizeof(lvi));
    lvi.mask       = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem      = idx;
    lvi.pszText    = name;
    lvi.cchTextMax = namelen;
    if (!SendMessage(hw, LVM_GETITEM, 0, (LPARAM)&lvi)) return -1;
    if (type_out) *type_out = (int)lvi.lParam;
    return idx;
}

static HIMAGELIST create_file_imagelist(HWND hwnd)
{
    HIMAGELIST himl;
    HDC hdc, hmdc;
    HBITMAP hbmp, holdbmp;
    HBRUSH hbr;
    RECT r;
    COLORREF mask_clr = RGB(255, 0, 255);

    himl = ImageList_Create(16, 16, ILC_MASK | ILC_COLORDDB, 2, 0);
    if (!himl) return NULL;

    hdc  = GetDC(NULL);   /* desktop DC — always valid, correct screen depth */
    hmdc = CreateCompatibleDC(hdc);

    /* Image 0: folder */
    hbmp    = CreateCompatibleBitmap(hdc, 16, 16);
    holdbmp = (HBITMAP)SelectObject(hmdc, hbmp);
    r.left = 0; r.top = 0; r.right = 16; r.bottom = 16;
    hbr = CreateSolidBrush(mask_clr);
    FillRect(hmdc, &r, hbr); DeleteObject(hbr);
    draw_icon_folder(hmdc, 1, 1);
    SelectObject(hmdc, holdbmp);
    ImageList_AddMasked(himl, hbmp, mask_clr);
    DeleteObject(hbmp);

    /* Image 1: file */
    hbmp    = CreateCompatibleBitmap(hdc, 16, 16);
    holdbmp = (HBITMAP)SelectObject(hmdc, hbmp);
    r.left = 0; r.top = 0; r.right = 16; r.bottom = 16;
    hbr = CreateSolidBrush(mask_clr);
    FillRect(hmdc, &r, hbr); DeleteObject(hbr);
    draw_icon_file(hmdc, 1, 1);
    SelectObject(hmdc, holdbmp);
    ImageList_AddMasked(himl, hbmp, mask_clr);
    DeleteObject(hbmp);

    DeleteDC(hmdc);
    ReleaseDC(NULL, hdc);
    return himl;
}

/* ================================================================
 * Saved-sessions connect dialog
 * ================================================================ */

static char  g_dlg_host_buf[256];
static char  g_dlg_user_buf[128];
static char  g_dlg_port_buf[16];
static char  g_dlg_key_buf[MAX_PATH];
static char  g_dlg_sess_name[256];
static bool  g_dlg_ok;
static bool  g_dlg_sess_selected;

static HWND  g_dlg_sesslist_wnd;
static HWND  g_dlg_sessname_wnd;
static HWND  g_dlg_host_wnd;
static HWND  g_dlg_user_wnd;
static HWND  g_dlg_port_wnd;
static HWND  g_dlg_key_wnd;

static void conn_dlg_populate_sessions(HWND hList)
{
    HKEY hk;
    DWORD i;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
            "Software\\SimonTatham\\PuTTY\\Sessions",
            0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    for (i = 0; ; i++) {
        char name[256];
        DWORD len = sizeof(name);
        if (RegEnumKeyEx(hk, i, name, &len, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)name);
    }
    RegCloseKey(hk);
}

static void conn_dlg_load_session(const char *sess)
{
    char path[512];
    HKEY hk;
    char val[MAX_PATH];
    DWORD sz, type, portnum;

    if (g_dlg_sessname_wnd) SetWindowText(g_dlg_sessname_wnd, sess);

    sprintf(path, "Software\\SimonTatham\\PuTTY\\Sessions\\%s", sess);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, path, 0, KEY_READ, &hk)
            != ERROR_SUCCESS)
        return;

    sz = sizeof(val); type = REG_SZ; val[0] = '\0';
    RegQueryValueEx(hk, "HostName", NULL, &type, (LPBYTE)val, &sz);
    if (g_dlg_host_wnd) SetWindowText(g_dlg_host_wnd, val);

    sz = sizeof(val); type = REG_SZ; val[0] = '\0';
    RegQueryValueEx(hk, "UserName", NULL, &type, (LPBYTE)val, &sz);
    if (g_dlg_user_wnd) SetWindowText(g_dlg_user_wnd, val);

    portnum = 22; sz = sizeof(portnum); type = REG_DWORD;
    RegQueryValueEx(hk, "PortNumber", NULL, &type, (LPBYTE)&portnum, &sz);
    { char p[16]; sprintf(p, "%lu", portnum);
      if (g_dlg_port_wnd) SetWindowText(g_dlg_port_wnd, p); }

    sz = sizeof(val); type = REG_SZ; val[0] = '\0';
    RegQueryValueEx(hk, "PublicKeyFile", NULL, &type, (LPBYTE)val, &sz);
    if (g_dlg_key_wnd) SetWindowText(g_dlg_key_wnd, val);

    RegCloseKey(hk);
}

static void conn_dlg_save_session(HWND hwnd)
{
    char name[256];
    char host[256], user[128], port[16], key[MAX_PATH];
    char regpath[512];
    HKEY hk;
    DWORD portnum;

    name[0] = '\0';
    if (g_dlg_sessname_wnd) GetWindowText(g_dlg_sessname_wnd, name, sizeof(name));
    if (name[0] == '\0') {
        MessageBox(hwnd, "Enter a session name before saving.",
                   "WinSFTP", MB_OK | MB_ICONWARNING);
        return;
    }
    host[0] = user[0] = port[0] = key[0] = '\0';
    if (g_dlg_host_wnd) GetWindowText(g_dlg_host_wnd, host, sizeof(host));
    if (g_dlg_user_wnd) GetWindowText(g_dlg_user_wnd, user, sizeof(user));
    if (g_dlg_port_wnd) GetWindowText(g_dlg_port_wnd, port, sizeof(port));
    if (g_dlg_key_wnd)  GetWindowText(g_dlg_key_wnd,  key,  sizeof(key));

    sprintf(regpath, "Software\\SimonTatham\\PuTTY\\Sessions\\%s", name);
    if (RegCreateKeyEx(HKEY_CURRENT_USER, regpath, 0, NULL, 0,
                       KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) {
        MessageBox(hwnd, "Failed to write registry.", "WinSFTP",
                   MB_OK | MB_ICONERROR);
        return;
    }
    RegSetValueEx(hk, "HostName",      0, REG_SZ,
                  (LPBYTE)host, (DWORD)strlen(host) + 1);
    RegSetValueEx(hk, "UserName",      0, REG_SZ,
                  (LPBYTE)user, (DWORD)strlen(user) + 1);
    portnum = (DWORD)atoi(port[0] ? port : "22");
    RegSetValueEx(hk, "PortNumber",    0, REG_DWORD,
                  (LPBYTE)&portnum, sizeof(portnum));
    RegSetValueEx(hk, "PublicKeyFile", 0, REG_SZ,
                  (LPBYTE)key, (DWORD)strlen(key) + 1);
    RegCloseKey(hk);

    SendMessage(g_dlg_sesslist_wnd, LB_RESETCONTENT, 0, 0);
    conn_dlg_populate_sessions(g_dlg_sesslist_wnd);

    MessageBox(hwnd, "Session saved.", "WinSFTP", MB_OK | MB_ICONINFORMATION);
}

static void conn_dlg_delete_session(HWND hwnd)
{
    int sel;
    char name[256];
    char msg2[300];
    char regpath[512];

    sel = g_dlg_sesslist_wnd ?
          (int)SendMessage(g_dlg_sesslist_wnd, LB_GETCURSEL, 0, 0) : LB_ERR;
    if (sel == LB_ERR) {
        MessageBox(hwnd, "Select a session to delete.",
                   "WinSFTP", MB_OK | MB_ICONWARNING);
        return;
    }
    SendMessage(g_dlg_sesslist_wnd, LB_GETTEXT, sel, (LPARAM)name);
    sprintf(msg2, "Delete saved session \"%s\"?", name);
    if (MessageBox(hwnd, msg2, "WinSFTP",
                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    sprintf(regpath, "Software\\SimonTatham\\PuTTY\\Sessions\\%s", name);
    RegDeleteKey(HKEY_CURRENT_USER, regpath);

    SendMessage(g_dlg_sesslist_wnd, LB_RESETCONTENT, 0, 0);
    conn_dlg_populate_sessions(g_dlg_sesslist_wnd);

    if (g_dlg_sessname_wnd) SetWindowText(g_dlg_sessname_wnd, "");
    if (g_dlg_host_wnd) SetWindowText(g_dlg_host_wnd, "");
    if (g_dlg_user_wnd) SetWindowText(g_dlg_user_wnd, "");
    if (g_dlg_port_wnd) SetWindowText(g_dlg_port_wnd, "22");
    if (g_dlg_key_wnd)  SetWindowText(g_dlg_key_wnd,  "");
}

static void conn_dlg_commit(HWND hwnd)
{
    int sel;
    if (g_dlg_host_wnd)
        GetWindowText(g_dlg_host_wnd, g_dlg_host_buf, sizeof(g_dlg_host_buf));
    if (g_dlg_user_wnd)
        GetWindowText(g_dlg_user_wnd, g_dlg_user_buf, sizeof(g_dlg_user_buf));
    if (g_dlg_port_wnd)
        GetWindowText(g_dlg_port_wnd, g_dlg_port_buf, sizeof(g_dlg_port_buf));
    if (g_dlg_key_wnd)
        GetWindowText(g_dlg_key_wnd, g_dlg_key_buf, sizeof(g_dlg_key_buf));
    sel = g_dlg_sesslist_wnd ?
        (int)SendMessage(g_dlg_sesslist_wnd, LB_GETCURSEL, 0, 0) : LB_ERR;
    if (sel != LB_ERR) {
        SendMessage(g_dlg_sesslist_wnd, LB_GETTEXT, sel, (LPARAM)g_dlg_sess_name);
        g_dlg_sess_selected = true;
    } else {
        g_dlg_sess_name[0] = '\0';
        g_dlg_sess_selected = false;
    }
    if (!g_dlg_sess_selected && g_dlg_host_buf[0] == '\0') {
        MessageBox(hwnd, "Enter a host name or select a saved session.",
                   "WinSFTP", MB_OK | MB_ICONWARNING);
        return;
    }
    g_dlg_ok = true;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK ConnDlgProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_CREATE: {
        HFONT hf = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);

        /* Left: session list (x=8..188) */
        CreateWindow("STATIC", "Saved Sessions:",
            WS_CHILD|WS_VISIBLE, 8, 8, 180, 16,
            hwnd, NULL, hinst, NULL);
        g_dlg_sesslist_wnd = CreateWindow("LISTBOX", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|
            LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
            8, 26, 180, 164,
            hwnd, (HMENU)IDCD_SESS_LIST, hinst, NULL);
        SendMessage(g_dlg_sesslist_wnd, WM_SETFONT, (WPARAM)hf, FALSE);
        conn_dlg_populate_sessions(g_dlg_sesslist_wnd);

        /* Right: connection fields (x=196..492) */
        CreateWindow("STATIC", "Session:", WS_CHILD|WS_VISIBLE,
            196, 8, 66, 16, hwnd, NULL, hinst, NULL);
        g_dlg_sessname_wnd = CreateWindow("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            266, 6, 326, 20, hwnd, (HMENU)IDCD_SESS_NAME, hinst, NULL);
        SendMessage(g_dlg_sessname_wnd, WM_SETFONT, (WPARAM)hf, FALSE);

        CreateWindow("STATIC", "Host:", WS_CHILD|WS_VISIBLE,
            196, 32, 66, 16, hwnd, NULL, hinst, NULL);
        g_dlg_host_wnd = CreateWindow("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            266, 30, 326, 20, hwnd, (HMENU)IDCD_HOST, hinst, NULL);
        SendMessage(g_dlg_host_wnd, WM_SETFONT, (WPARAM)hf, FALSE);

        CreateWindow("STATIC", "User:", WS_CHILD|WS_VISIBLE,
            196, 56, 66, 16, hwnd, NULL, hinst, NULL);
        g_dlg_user_wnd = CreateWindow("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            266, 54, 326, 20, hwnd, (HMENU)IDCD_USER, hinst, NULL);
        SendMessage(g_dlg_user_wnd, WM_SETFONT, (WPARAM)hf, FALSE);

        CreateWindow("STATIC", "Port:", WS_CHILD|WS_VISIBLE,
            196, 80, 66, 16, hwnd, NULL, hinst, NULL);
        g_dlg_port_wnd = CreateWindow("EDIT", "22",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            266, 78, 60, 20, hwnd, (HMENU)IDCD_PORT, hinst, NULL);
        SendMessage(g_dlg_port_wnd, WM_SETFONT, (WPARAM)hf, FALSE);

        CreateWindow("STATIC", "Key File:", WS_CHILD|WS_VISIBLE,
            196, 104, 66, 16, hwnd, NULL, hinst, NULL);
        g_dlg_key_wnd = CreateWindow("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            266, 102, 290, 20, hwnd, (HMENU)IDCD_KEYFILE, hinst, NULL);
        SendMessage(g_dlg_key_wnd, WM_SETFONT, (WPARAM)hf, FALSE);
        CreateWindow("BUTTON", "...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            560, 102, 32, 20, hwnd, (HMENU)IDCD_BROWSE, hinst, NULL);

        /* All four buttons on one row: Save+Delete left, Connect+Cancel right */
        CreateWindow("BUTTON", "Save",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            8, 214, 82, 26, hwnd, (HMENU)IDCD_SAVE, hinst, NULL);
        CreateWindow("BUTTON", "Delete",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            98, 214, 82, 26, hwnd, (HMENU)IDCD_DELETE_SESS, hinst, NULL);
        CreateWindow("BUTTON", "Connect",
            WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            400, 214, 92, 26, hwnd, (HMENU)IDOK, hinst, NULL);
        CreateWindow("BUTTON", "Cancel",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            500, 214, 92, 26, hwnd, (HMENU)IDCANCEL, hinst, NULL);
        return 0;
      }

      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDCD_SESS_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = (int)SendMessage(g_dlg_sesslist_wnd,
                                           LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    char name[256];
                    SendMessage(g_dlg_sesslist_wnd, LB_GETTEXT,
                                sel, (LPARAM)name);
                    conn_dlg_load_session(name);
                }
            } else if (HIWORD(wParam) == LBN_DBLCLK) {
                conn_dlg_commit(hwnd);
            }
            return 0;

          case IDCD_SAVE:
            conn_dlg_save_session(hwnd);
            return 0;

          case IDCD_DELETE_SESS:
            conn_dlg_delete_session(hwnd);
            return 0;

          case IDCD_BROWSE: {
            OPENFILENAME ofn;
            char path[MAX_PATH];
            path[0] = '\0';
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = "PuTTY Key Files (*.ppk)\0*.ppk\0All Files\0*.*\0\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileName(&ofn) && g_dlg_key_wnd)
                SetWindowText(g_dlg_key_wnd, path);
            return 0;
          }

          case IDOK:
            conn_dlg_commit(hwnd);
            return 0;

          case IDCANCEL:
            g_dlg_ok = false;
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

      case WM_CLOSE:
        g_dlg_ok = false;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ================================================================
 * Pane subclass procs — focus tracking + LISTBOX keyboard nav
 * ================================================================ */

static LRESULT CALLBACK LocalListProc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETFOCUS && g_active_pane != 0) {
        g_active_pane = 0;
        if (g_local_path)  InvalidateRect(g_local_path,  NULL, TRUE);
        if (g_remote_path) InvalidateRect(g_remote_path, NULL, TRUE);
    }
    if (!g_has_listview) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            navigate_local_focused();
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == (WPARAM)'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessage(hwnd, LB_SETSEL, TRUE, (LPARAM)-1);
            return 0;
        }
    }
    return CallWindowProc(g_local_list_orig_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK RemoteListProc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETFOCUS && g_active_pane != 1) {
        g_active_pane = 1;
        if (g_local_path)  InvalidateRect(g_local_path,  NULL, TRUE);
        if (g_remote_path) InvalidateRect(g_remote_path, NULL, TRUE);
    }
    if (!g_has_listview) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            navigate_remote_focused();
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == (WPARAM)'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessage(hwnd, LB_SETSEL, TRUE, (LPARAM)-1);
            return 0;
        }
    }
    return CallWindowProc(g_remote_list_orig_proc, hwnd, msg, wp, lp);
}

/* ================================================================
 * MkDir dialog — reuses WinSFTPPass window class
 * ================================================================ */

static void do_mkdir_dialog(int remote)
{
    RECT wr, dr; int dw, dh;
    MSG msg;

    strncpy(g_pass_label,
            remote ? "New remote folder name:" : "New local folder name:",
            sizeof(g_pass_label) - 1);
    g_pass_label[sizeof(g_pass_label) - 1] = '\0';
    g_pass_echo  = true;
    g_pass_ok    = false;
    g_pass_buf[0] = '\0';

    g_pass_hwnd = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        "WinSFTPPass", "New Folder",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 380, 140,
        g_hwnd, NULL, hinst, NULL);

    if (!g_pass_hwnd) return;

    GetWindowRect(g_hwnd,      &wr);
    GetWindowRect(g_pass_hwnd, &dr);
    dw = dr.right - dr.left; dh = dr.bottom - dr.top;
    SetWindowPos(g_pass_hwnd, HWND_TOP,
        wr.left + (wr.right  - wr.left - dw) / 2,
        wr.top  + (wr.bottom - wr.top  - dh) / 2,
        0, 0, SWP_NOSIZE);
    ShowWindow(g_pass_hwnd, SW_SHOW);
    if (g_pass_edit) SetFocus(g_pass_edit);

    while (g_pass_hwnd) {
        if (!GetMessage(&msg, NULL, 0, 0)) { g_running = false; return; }
        if (!IsDialogMessage(g_pass_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        run_toplevel_callbacks();
    }

    if (!g_pass_ok || !g_pass_buf[0]) return;

    if (remote) {
        char cmd[MAX_PATH + 8];
        sprintf(cmd, "mkdir %s", g_pass_buf);
        sfree(g_cmd_line);
        g_cmd_line = dupstr(cmd);
        g_cmd_ready = true;
        g_pending_refresh = true;
        winsftp_append("> "); winsftp_append(cmd); winsftp_append("\r\n");
    } else {
        char curdir[MAX_PATH]; char full[MAX_PATH * 2];
        GetCurrentDirectory(MAX_PATH, curdir);
        sprintf(full, "%s\\%s", curdir, g_pass_buf);
        if (!CreateDirectory(full, NULL)) {
            char errmsg[MAX_PATH * 2 + 40];
            sprintf(errmsg, "Could not create folder \"%s\".", full);
            MessageBox(g_hwnd, errmsg, "WinSFTP", MB_OK | MB_ICONWARNING);
        }
        populate_local_list();
    }
}

static void do_connect_dialog(void)
{
    WNDCLASS wc;
    HWND hdlg;
    MSG msg;
    RECT pr, dr;
    int px, py, dw, dh;

    memset(g_dlg_host_buf, 0, sizeof(g_dlg_host_buf));
    memset(g_dlg_user_buf, 0, sizeof(g_dlg_user_buf));
    strcpy(g_dlg_port_buf, "22");
    memset(g_dlg_key_buf, 0, sizeof(g_dlg_key_buf));
    g_dlg_sess_name[0] = '\0';
    g_dlg_ok = false;
    g_dlg_sess_selected = false;
    g_dlg_sesslist_wnd = g_dlg_sessname_wnd = NULL;
    g_dlg_host_wnd = g_dlg_user_wnd = NULL;
    g_dlg_port_wnd = g_dlg_key_wnd = NULL;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = ConnDlgProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WinSFTPConn";
    RegisterClass(&wc);

    hdlg = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        "WinSFTPConn", "Connect",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 600, 275,
        g_hwnd, NULL, hinst, NULL);

    if (!hdlg) { UnregisterClass("WinSFTPConn", hinst); return; }

    GetWindowRect(g_hwnd, &pr);
    GetWindowRect(hdlg,   &dr);
    dw = dr.right - dr.left; dh = dr.bottom - dr.top;
    px = pr.left + ((pr.right - pr.left) - dw) / 2;
    py = pr.top  + ((pr.bottom - pr.top) - dh) / 2;
    SetWindowPos(hdlg, NULL, px, py, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(g_hwnd, FALSE);
    ShowWindow(hdlg, SW_SHOW);
    UpdateWindow(hdlg);

    while (IsWindow(hdlg)) {
        if (!GetMessage(&msg, NULL, 0, 0)) break;
        if (!IsDialogMessage(hdlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    UnregisterClass("WinSFTPConn", hinst);

    if (!g_dlg_ok) return;

    {
        char cmd[512];
        if (g_dlg_sess_selected) {
            sprintf(cmd, "open %s", g_dlg_sess_name);
        } else if (g_dlg_user_buf[0]) {
            sprintf(cmd, "open %s@%s %s", g_dlg_user_buf, g_dlg_host_buf,
                    g_dlg_port_buf[0] ? g_dlg_port_buf : "22");
        } else {
            sprintf(cmd, "open %s %s", g_dlg_host_buf,
                    g_dlg_port_buf[0] ? g_dlg_port_buf : "22");
        }
        if (g_dlg_key_buf[0])
            strncpy(g_pending_keyfile, g_dlg_key_buf, MAX_PATH - 1);
        sfree(g_cmd_line);
        g_cmd_line = dupstr(cmd);
        g_cmd_ready = true;
        winsftp_append("> ");
        winsftp_append(cmd);
        winsftp_append("\r\n");
    }
}

/* ================================================================
 * Window procedure
 * ================================================================ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

      case WM_CREATE: {
        HFONT hf = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
        HMENU hMenuBar, hFile, hXfer, hView, hTheme, hHelp;

        /* --- Menu bar ------------------------------------------ */
        hMenuBar = CreateMenu();
        hFile    = CreatePopupMenu();
        hXfer    = CreatePopupMenu();
        hView    = CreatePopupMenu();
        hTheme   = CreatePopupMenu();
        hHelp    = CreatePopupMenu();

        AppendMenu(hFile, MF_STRING,    IDM_FILE_CONNECT,   "&Connect...");
        AppendMenu(hFile, MF_STRING,    IDM_FILE_SAVEDCONN, "&Saved Sessions...");
        AppendMenu(hFile, MF_SEPARATOR, 0,                  NULL);
        AppendMenu(hFile, MF_STRING,    IDM_FILE_EXIT,      "E&xit");

        AppendMenu(hXfer, MF_STRING,    IDM_XFER_GET,     "< &Get");
        AppendMenu(hXfer, MF_STRING,    IDM_XFER_PUT,     "&Put >");
        AppendMenu(hXfer, MF_SEPARATOR, 0,                NULL);
        AppendMenu(hXfer, MF_STRING,    IDM_XFER_REFRESH, "&Refresh remote");

        AppendMenu(hTheme, MF_STRING,   IDM_THEME_CLASSIC, "&Classic");
        AppendMenu(hTheme, MF_STRING,   IDM_THEME_BLUE,    "&Blue  (WinSCP)");
        AppendMenu(hTheme, MF_STRING,   IDM_THEME_GREEN,   "&Green (terminal)");

        AppendMenu(hView, MF_STRING,    IDM_VIEW_CLEAR,   "&Clear log");
        AppendMenu(hView, MF_SEPARATOR, 0,                NULL);
        AppendMenu(hView, MF_POPUP,     (UINT)hTheme,     "&Theme");

        AppendMenu(hHelp, MF_STRING,    IDM_HELP_ABOUT,   "&About WinSFTP...");

        AppendMenu(hMenuBar, MF_POPUP,  (UINT)hFile,  "&File");
        AppendMenu(hMenuBar, MF_POPUP,  (UINT)hXfer,  "&Transfer");
        AppendMenu(hMenuBar, MF_POPUP,  (UINT)hView,  "&View");
        AppendMenu(hMenuBar, MF_POPUP,  (UINT)hHelp,  "&Help");
        SetMenu(hwnd, hMenuBar);

        /* --- Toolbar ------------------------------------------- */
        g_toolbar_connect = CreateWindow("BUTTON", "Connect",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TOOLBAR_CONNECT, hinst, NULL);
        g_toolbar_lup = CreateWindow("BUTTON", "Local ^",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TOOLBAR_LUP, hinst, NULL);
        g_toolbar_rup = CreateWindow("BUTTON", "Remote ^",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TOOLBAR_RUP, hinst, NULL);
        g_toolbar_refresh = CreateWindow("BUTTON", "Refresh Both",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TOOLBAR_REFRESH, hinst, NULL);

        /* --- Browser panes ---------------------------------------- */
        if (g_has_listview) {
            /* SysListView32 in report mode (comctl32 4.0+ present) */
            g_local_list = CreateWindowEx(WS_EX_CLIENTEDGE,
                "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, (HMENU)IDC_LOCAL_LIST, hinst, NULL);

            g_remote_list = CreateWindowEx(WS_EX_CLIENTEDGE,
                "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, (HMENU)IDC_REMOTE_LIST, hinst, NULL);

            SendMessage(g_local_list,  LVM_SETEXTENDEDLISTVIEWSTYLE,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            SendMessage(g_remote_list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            {
                LV_COLUMN lvc;
                memset(&lvc, 0, sizeof(lvc));
                lvc.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.iSubItem = 0;
                lvc.pszText  = "Name";
                lvc.cx       = 200;
                SendMessage(g_local_list,  LVM_INSERTCOLUMN, 0, (LPARAM)&lvc);
                SendMessage(g_remote_list, LVM_INSERTCOLUMN, 0, (LPARAM)&lvc);
                lvc.iSubItem = 1;
                lvc.pszText  = "Type";
                lvc.cx       = 60;
                SendMessage(g_local_list,  LVM_INSERTCOLUMN, 1, (LPARAM)&lvc);
                SendMessage(g_remote_list, LVM_INSERTCOLUMN, 1, (LPARAM)&lvc);
            }

            g_img_list = create_file_imagelist(hwnd);
            if (g_img_list) {
                SendMessage(g_local_list,  LVM_SETIMAGELIST,
                            LVSIL_SMALL, (LPARAM)g_img_list);
                SendMessage(g_remote_list, LVM_SETIMAGELIST,
                            LVSIL_SMALL, (LPARAM)g_img_list);
            }
        } else {
            /* Owner-draw LISTBOX fallback (no comctl32) */
            g_local_list = CreateWindowEx(WS_EX_CLIENTEDGE,
                "LISTBOX", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_EXTENDEDSEL,
                0, 0, 0, 0, hwnd, (HMENU)IDC_LOCAL_LIST, hinst, NULL);

            g_remote_list = CreateWindowEx(WS_EX_CLIENTEDGE,
                "LISTBOX", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_EXTENDEDSEL,
                0, 0, 0, 0, hwnd, (HMENU)IDC_REMOTE_LIST, hinst, NULL);
        }

        /* Subclass both panes for focus tracking and keyboard nav */
        g_local_list_orig_proc  = (WNDPROC)SetWindowLong(
            g_local_list,  GWL_WNDPROC, (LONG)LocalListProc);
        g_remote_list_orig_proc = (WNDPROC)SetWindowLong(
            g_remote_list, GWL_WNDPROC, (LONG)RemoteListProc);

        /* Brush for active-pane path label highlight */
        g_active_path_brush = CreateSolidBrush(RGB(0, 80, 160));

        /* Address bar EDITs — editable path, Enter navigates */
        g_local_path = CreateWindow("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LOCAL_PATH, hinst, NULL);
        g_local_path_orig_proc = (WNDPROC)SetWindowLong(
            g_local_path, GWL_WNDPROC, (LONG)LocalPathProc);

        g_remote_path = CreateWindow("EDIT", "Remote:",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_REMOTE_PATH, hinst, NULL);
        g_remote_path_orig_proc = (WNDPROC)SetWindowLong(
            g_remote_path, GWL_WNDPROC, (LONG)RemotePathProc);

        /* Transfer buttons */
        g_get_btn = CreateWindow("BUTTON", "< Get",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GET_BTN, hinst, NULL);

        g_put_btn = CreateWindow("BUTTON", "Put >",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_PUT_BTN, hinst, NULL);

        g_refresh_btn = CreateWindow("BUTTON", "Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_REFRESH_BTN, hinst, NULL);

        g_mkdir_btn = CreateWindow("BUTTON", "MkDir",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MKDIR_BTN, hinst, NULL);

        g_delete_btn = CreateWindow("BUTTON", "Delete",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_DELETE_BTN, hinst, NULL);

        g_rmdir_btn = CreateWindow("BUTTON", "RmDir",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_RMDIR_BTN, hinst, NULL);

        /* --- Log area ------------------------------------------ */
        g_output = CreateWindow("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | ES_MULTILINE |
            ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_OUTPUT, hinst, NULL);
        g_output_orig_proc = (WNDPROC)SetWindowLong(
            g_output, GWL_WNDPROC, (LONG)OutputSubclassProc);

        /* --- Command input ------------------------------------- */
        g_input = CreateWindow("EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_INPUT, hinst, NULL);
        g_input_orig_proc = (WNDPROC)SetWindowLong(
            g_input, GWL_WNDPROC, (LONG)InputSubclassProc);

        g_send = CreateWindow("BUTTON", "Send",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDOK, hinst, NULL);

        /* --- Status bar ---------------------------------------- */
        g_status = CreateWindow("STATIC", "Ready",
            WS_CHILD | WS_VISIBLE | SS_SUNKEN,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, hinst, NULL);

        /* Apply fixed-width font to log, input and status */
        SendMessage(g_output, WM_SETFONT, (WPARAM)hf, FALSE);
        SendMessage(g_input,  WM_SETFONT, (WPARAM)hf, FALSE);
        SendMessage(g_status, WM_SETFONT, (WPARAM)hf, FALSE);
        /* Apply fixed-width font to listviews and path labels */
        SendMessage(g_local_list,  WM_SETFONT, (WPARAM)hf, FALSE);
        SendMessage(g_remote_list, WM_SETFONT, (WPARAM)hf, FALSE);
        SendMessage(g_local_path,  WM_SETFONT, (WPARAM)hf, FALSE);
        SendMessage(g_remote_path, WM_SETFONT, (WPARAM)hf, FALSE);

        do_layout(hwnd);
        return 0;
      }

      case WM_SIZE:
        do_layout(hwnd);
        return 0;

      case WM_SETFOCUS:
        if (g_input) SetFocus(g_input);
        return 0;

      /* ---- Theme colouring for all child controls ------------- */
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORSTATIC: {
        HWND hctl = (HWND)lParam;
        /* Active-pane indicator: highlight the focused pane's path bar */
        if (msg == WM_CTLCOLOREDIT && g_active_path_brush &&
            ((hctl == g_local_path  && g_active_pane == 0) ||
             (hctl == g_remote_path && g_active_pane == 1))) {
            SetTextColor((HDC)wParam, RGB(255, 255, 255));
            SetBkColor((HDC)wParam,   RGB(0, 80, 160));
            return (LRESULT)g_active_path_brush;
        }
        if (g_theme != THEME_CLASSIC && g_theme_brush) {
            SetTextColor((HDC)wParam, g_theme_fg);
            SetBkColor((HDC)wParam, g_theme_bg);
            return (LRESULT)g_theme_brush;
        }
        break;
      }

      case WM_ERASEBKGND:
        if (g_theme != THEME_CLASSIC && g_theme_brush) {
            RECT rc2;
            GetClientRect(hwnd, &rc2);
            FillRect((HDC)wParam, &rc2, g_theme_brush);
            return 1;
        }
        break;

      /* ---- Right-click context menus for both panes ----------- */
      case WM_CONTEXTMENU: {
        HWND htarget = (HWND)wParam;
        int sx = (int)(short)LOWORD(lParam);
        int sy = (int)(short)HIWORD(lParam);
        if (htarget == g_local_list && g_local_list) {
            HMENU hm = CreatePopupMenu();
            AppendMenu(hm, MF_STRING,    IDM_CTX_LOCAL_UPLOAD,  "Upload");
            AppendMenu(hm, MF_STRING,    IDM_CTX_LOCAL_NAVINTO, "Navigate Into");
            AppendMenu(hm, MF_SEPARATOR, 0,                     NULL);
            AppendMenu(hm, MF_STRING,    IDM_CTX_LOCAL_INFO,    "Get Info");
            AppendMenu(hm, MF_SEPARATOR, 0,                     NULL);
            AppendMenu(hm, MF_STRING,    IDM_CTX_LOCAL_REFRESH, "Refresh");
            TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                           sx, sy, 0, hwnd, NULL);
            DestroyMenu(hm);
        } else if (htarget == g_remote_list && g_remote_list) {
            HMENU hm = CreatePopupMenu();
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_DOWNLOAD, "Download");
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_NAVINTO,  "Navigate Into");
            AppendMenu(hm, MF_SEPARATOR, 0,                       NULL);
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_DELETE,   "Delete");
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_MKDIR,    "New Folder");
            AppendMenu(hm, MF_SEPARATOR, 0,                       NULL);
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_INFO,     "Get Info");
            AppendMenu(hm, MF_SEPARATOR, 0,                       NULL);
            AppendMenu(hm, MF_STRING,    IDM_CTX_REMOTE_REFRESH,  "Refresh");
            TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                           sx, sy, 0, hwnd, NULL);
            DestroyMenu(hm);
        }
        return 0;
      }

      /* ---- Owner-draw LISTBOX fallback (no comctl32) ------------ */
      case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
        if (!g_has_listview &&
            (mis->CtlID == IDC_LOCAL_LIST || mis->CtlID == IDC_REMOTE_LIST)) {
            mis->itemHeight = 18;
            return TRUE;
        }
        break;
      }

      case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        if (!g_has_listview &&
            (dis->hwndItem == g_local_list || dis->hwndItem == g_remote_list)) {
            HDC     hdc  = dis->hDC;
            RECT    r    = dis->rcItem;
            COLORREF bg, fg;
            HBRUSH  hbr;
            char    text[MAX_PATH + 2];
            int     itype;

            if (dis->itemID == (UINT)-1) { return TRUE; }

            itype = (int)(DWORD)SendMessage(dis->hwndItem,
                                            LB_GETITEMDATA, dis->itemID, 0);
            text[0] = '\0';
            SendMessage(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)text);

            bg = (dis->itemState & ODS_SELECTED) ? g_theme_fg : g_theme_bg;
            fg = (dis->itemState & ODS_SELECTED) ? g_theme_bg : g_theme_fg;

            hbr = CreateSolidBrush(bg);
            FillRect(hdc, &r, hbr);
            DeleteObject(hbr);

            if (itype == LV_FOLDER || itype == LV_PARENT)
                draw_icon_folder(hdc, r.left + 1, r.top + 1);
            else
                draw_icon_file(hdc, r.left + 1, r.top + 1);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);
            {
                RECT tr = r;
                tr.left += 18;
                DrawText(hdc, text, -1, &tr,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            if (dis->itemState & ODS_FOCUS)
                DrawFocusRect(hdc, &r);
            return TRUE;
        }
        break;
      }

      /* ---- ListView notifications: dbl-click, Enter, Ctrl+A ---- */
      case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->hwndFrom == g_local_list || nm->hwndFrom == g_remote_list) {
            int is_local = (nm->hwndFrom == g_local_list);
            HWND hw = nm->hwndFrom;
            if (nm->code == NM_DBLCLK) {
                char entry[MAX_PATH]; int itype;
                int sel = (int)SendMessage(hw, LVM_GETNEXTITEM,
                                           (WPARAM)-1, LVNI_FOCUSED);
                if (sel < 0) break;
                lv_get_item_at(hw, sel, entry, MAX_PATH, &itype);
                if (is_local) {
                    if (itype == LV_PARENT || itype == LV_FOLDER) {
                        char *err = (itype == LV_PARENT)
                                    ? psftp_lcd("..") : psftp_lcd(entry);
                        if (err) sfree(err);
                        populate_local_list();
                    } else {
                        char cmd[MAX_PATH + 8];
                        sprintf(cmd, "put %s", entry);
                        sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                        g_cmd_ready = true;
                    }
                } else {
                    if (itype == LV_PARENT) {
                        sfree(g_cmd_line); g_cmd_line = dupstr("cd ..");
                        g_cmd_ready = true; g_pending_refresh = true;
                    } else if (itype == LV_FOLDER) {
                        char cmd[MAX_PATH + 8];
                        sprintf(cmd, "cd %s", entry);
                        sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                        g_cmd_ready = true; g_pending_refresh = true;
                    } else {
                        char cmd[MAX_PATH + 8];
                        sprintf(cmd, "get %s", entry);
                        sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                        g_cmd_ready = true;
                    }
                }
                if (g_input) SetFocus(g_input);
            } else if (nm->code == LVN_KEYDOWN) {
                LV_KEYDOWN *kd = (LV_KEYDOWN *)lParam;
                if (kd->wVKey == VK_RETURN) {
                    if (is_local) navigate_local_focused();
                    else          navigate_remote_focused();
                } else if (kd->wVKey == 'A' &&
                           (GetKeyState(VK_CONTROL) & 0x8000)) {
                    LV_ITEM lvi;
                    memset(&lvi, 0, sizeof(lvi));
                    lvi.mask      = LVIF_STATE;
                    lvi.stateMask = LVIS_SELECTED;
                    lvi.state     = LVIS_SELECTED;
                    SendMessage(hw, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);
                }
            }
        }
        break;
      }

      /* ---- Menu and button commands --------------------------- */
      case WM_COMMAND:
        switch (LOWORD(wParam)) {

          /* --- LISTBOX double-click navigation (fallback mode) -- */
          case IDC_LOCAL_LIST:
            if (!g_has_listview && HIWORD(wParam) == LBN_DBLCLK)
                navigate_local_focused();
            break;
          case IDC_REMOTE_LIST:
            if (!g_has_listview && HIWORD(wParam) == LBN_DBLCLK)
                navigate_remote_focused();
            break;

          /* --- Send / Enter in input field -------------------- */
          case IDOK: {
            int len = GetWindowTextLength(g_input);
            if (len > 0) {
                g_cmd_line = snewn(len + 2, char);
                GetWindowText(g_input, g_cmd_line, len + 1);
                SetWindowText(g_input, "");
                if (!g_suppress_echo) {
                    winsftp_append("> ");
                    winsftp_append(g_cmd_line);
                    winsftp_append("\r\n");
                }
                if (!g_suppress_echo)
                    history_add(g_cmd_line);
                g_cmd_ready = true;
            }
            if (g_input) SetFocus(g_input);
            return 0;
          }

          /* --- Toolbar buttons -------------------------------- */
          case IDC_TOOLBAR_CONNECT:
            do_connect_dialog();
            return 0;

          case IDC_TOOLBAR_REFRESH:
            populate_local_list();
            refresh_remote_list();
            return 0;

          case IDC_TOOLBAR_LUP: {
            char *err = psftp_lcd("..");
            if (err) sfree(err);
            populate_local_list();
            return 0;
          }

          case IDC_TOOLBAR_RUP:
            sfree(g_cmd_line);
            g_cmd_line = dupstr("cd ..");
            g_cmd_ready = true;
            g_pending_refresh = true;
            return 0;

          /* --- File menu -------------------------------------- */
          case IDM_FILE_CONNECT:
          case IDM_FILE_SAVEDCONN:
            do_connect_dialog();
            return 0;

          case IDM_FILE_EXIT:
            g_running = false;
            DestroyWindow(hwnd);
            return 0;

          /* --- Transfer menu ---------------------------------- */
          case IDM_XFER_GET:
          case IDC_GET_BTN: {
            int first = 1;
            if (!g_remote_list) return 0;
            if (g_has_listview) {
                int idx = -1;
                while ((idx = (int)SendMessage(g_remote_list, LVM_GETNEXTITEM,
                                               idx, LVNI_SELECTED)) >= 0) {
                    char fname[MAX_PATH]; char cmd[MAX_PATH + 8]; int itype;
                    lv_get_item_at(g_remote_list, idx, fname, MAX_PATH, &itype);
                    if (itype != LV_FILE) continue;
                    sprintf(cmd, "get %s", fname);
                    if (first) { sfree(g_cmd_line); g_cmd_line=dupstr(cmd); g_cmd_ready=true; first=0; }
                    else { cmd_queue_push(cmd); }
                }
            } else {
                int j, count; int sel_arr[CMD_QUEUE_MAX];
                count = (int)SendMessage(g_remote_list, LB_GETSELCOUNT, 0, 0);
                if (count > CMD_QUEUE_MAX) count = CMD_QUEUE_MAX;
                if (count > 0) {
                    SendMessage(g_remote_list, LB_GETSELITEMS, (WPARAM)count, (LPARAM)sel_arr);
                    for (j = 0; j < count; j++) {
                        char fname[MAX_PATH]; char cmd[MAX_PATH+8]; int itype;
                        lv_get_item_at(g_remote_list, sel_arr[j], fname, MAX_PATH, &itype);
                        if (itype != LV_FILE) continue;
                        sprintf(cmd, "get %s", fname);
                        if (first) { sfree(g_cmd_line); g_cmd_line=dupstr(cmd); g_cmd_ready=true; first=0; }
                        else { cmd_queue_push(cmd); }
                    }
                }
            }
            return 0;
          }

          case IDM_XFER_PUT:
          case IDC_PUT_BTN: {
            int first = 1;
            if (!g_local_list) return 0;
            if (g_has_listview) {
                int idx = -1;
                while ((idx = (int)SendMessage(g_local_list, LVM_GETNEXTITEM,
                                               idx, LVNI_SELECTED)) >= 0) {
                    char fname[MAX_PATH]; char cmd[MAX_PATH + 8]; int itype;
                    lv_get_item_at(g_local_list, idx, fname, MAX_PATH, &itype);
                    if (itype != LV_FILE) continue;
                    sprintf(cmd, "put %s", fname);
                    if (first) { sfree(g_cmd_line); g_cmd_line=dupstr(cmd); g_cmd_ready=true; first=0; }
                    else { cmd_queue_push(cmd); }
                }
            } else {
                int j, count; int sel_arr[CMD_QUEUE_MAX];
                count = (int)SendMessage(g_local_list, LB_GETSELCOUNT, 0, 0);
                if (count > CMD_QUEUE_MAX) count = CMD_QUEUE_MAX;
                if (count > 0) {
                    SendMessage(g_local_list, LB_GETSELITEMS, (WPARAM)count, (LPARAM)sel_arr);
                    for (j = 0; j < count; j++) {
                        char fname[MAX_PATH]; char cmd[MAX_PATH+8]; int itype;
                        lv_get_item_at(g_local_list, sel_arr[j], fname, MAX_PATH, &itype);
                        if (itype != LV_FILE) continue;
                        sprintf(cmd, "put %s", fname);
                        if (first) { sfree(g_cmd_line); g_cmd_line=dupstr(cmd); g_cmd_ready=true; first=0; }
                        else { cmd_queue_push(cmd); }
                    }
                }
            }
            return 0;
          }

          case IDM_XFER_REFRESH:
            refresh_remote_list();
            return 0;

          case IDC_REFRESH_BTN:
            if (g_active_pane == 0)
                populate_local_list();
            else
                refresh_remote_list();
            return 0;

          case IDC_MKDIR_BTN:
            do_mkdir_dialog(g_active_pane);
            return 0;

          case IDC_DELETE_BTN: {
            char fname[MAX_PATH]; int itype, sel;
            if (g_active_pane == 0 && g_local_list) {
                /* Delete local file */
                char full[MAX_PATH]; char curdir[MAX_PATH]; char msg2[MAX_PATH + 40];
                sel = lv_get_focused(g_local_list, fname, MAX_PATH, &itype);
                if (sel < 0 || itype != LV_FILE) return 0;
                sprintf(msg2, "Delete local file \"%s\"?", fname);
                if (MessageBox(hwnd, msg2, "WinSFTP",
                               MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    GetCurrentDirectory(MAX_PATH, curdir);
                    sprintf(full, "%s\\%s", curdir, fname);
                    DeleteFile(full);
                    populate_local_list();
                }
            } else {
                if (!g_remote_list) return 0;
                sel = lv_get_focused(g_remote_list, fname, MAX_PATH, &itype);
                if (sel < 0 || itype == LV_PARENT) return 0;
                {
                    char msg2[MAX_PATH + 40]; int r;
                    sprintf(msg2, "Delete \"%s\" on remote server?", fname);
                    r = MessageBox(hwnd, msg2, "WinSFTP", MB_YESNO | MB_ICONQUESTION);
                    if (r == IDYES) {
                        char cmd[MAX_PATH + 8];
                        if (itype == LV_FOLDER) sprintf(cmd, "rmdir %s", fname);
                        else                    sprintf(cmd, "rm %s",    fname);
                        sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                        g_cmd_ready = true; g_pending_refresh = true;
                    }
                }
            }
            return 0;
          }

          case IDC_RMDIR_BTN: {
            char entry[MAX_PATH]; int itype, sel;
            if (!g_remote_list) return 0;
            sel = lv_get_focused(g_remote_list, entry, MAX_PATH, &itype);
            if (sel < 0) return 0;
            if (itype != LV_FOLDER) return 0;
            {
                char msg2[MAX_PATH + 80]; int r;
                sprintf(msg2,
                    "Delete folder \"%s\" and all its contents?\n"
                    "(Files are removed first, then the directory.)", entry);
                r = MessageBox(hwnd, msg2, "WinSFTP",
                               MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
                if (r == IDYES) {
                    char cmd[MAX_PATH + 8];
                    strncpy(g_rmdir_pending, entry, MAX_PATH-1);
                    g_rmdir_step = 2;
                    sprintf(cmd, "rm %s/*", entry);
                    sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                    g_cmd_ready = true;
                    winsftp_append("> "); winsftp_append(cmd); winsftp_append("\r\n");
                }
            }
            return 0;
          }

          /* --- Context menu — local pane ---------------------- */
          case IDM_CTX_LOCAL_UPLOAD: {
            char fname[MAX_PATH]; char cmd[MAX_PATH + 8]; int itype, sel;
            if (!g_local_list) return 0;
            sel = lv_get_focused(g_local_list, fname, MAX_PATH, &itype);
            if (sel < 0) return 0;
            if (itype != LV_FILE) return 0;
            sprintf(cmd, "put %s", fname);
            sfree(g_cmd_line); g_cmd_line = dupstr(cmd); g_cmd_ready = true;
            return 0;
          }
          case IDM_CTX_LOCAL_NAVINTO:
            navigate_local_focused();
            return 0;
          case IDM_CTX_LOCAL_REFRESH:
            populate_local_list();
            return 0;

          /* --- Context menu — remote pane --------------------- */
          case IDM_CTX_REMOTE_DOWNLOAD: {
            char fname[MAX_PATH]; char cmd[MAX_PATH + 8]; int itype, sel;
            if (!g_remote_list) return 0;
            sel = lv_get_focused(g_remote_list, fname, MAX_PATH, &itype);
            if (sel < 0) return 0;
            if (itype != LV_FILE) return 0;
            sprintf(cmd, "get %s", fname);
            sfree(g_cmd_line); g_cmd_line = dupstr(cmd); g_cmd_ready = true;
            return 0;
          }
          case IDM_CTX_REMOTE_NAVINTO:
            navigate_remote_focused();
            return 0;
          case IDM_CTX_REMOTE_DELETE: {
            char fname[MAX_PATH]; char msg2[MAX_PATH + 40]; int itype, r, sel;
            if (!g_remote_list) return 0;
            sel = lv_get_focused(g_remote_list, fname, MAX_PATH, &itype);
            if (sel < 0) return 0;
            if (itype == LV_PARENT) return 0;
            sprintf(msg2, "Delete \"%s\" on remote server?", fname);
            r = MessageBox(hwnd, msg2, "WinSFTP", MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) {
                char cmd[MAX_PATH + 8];
                if (itype == LV_FOLDER) sprintf(cmd, "rmdir %s", fname);
                else                    sprintf(cmd, "rm %s",    fname);
                sfree(g_cmd_line); g_cmd_line = dupstr(cmd);
                g_cmd_ready = true; g_pending_refresh = true;
            }
            return 0;
          }
          case IDM_CTX_REMOTE_MKDIR:
            do_mkdir_dialog(1);
            return 0;
          case IDM_CTX_REMOTE_REFRESH:
            refresh_remote_list();
            return 0;

          /* --- Get Info — remote pane ------------------------- */
          case IDM_CTX_REMOTE_INFO: {
            char fname[MAX_PATH]; int itype, sel;
            if (!g_remote_list) return 0;
            sel = lv_get_focused(g_remote_list, fname, MAX_PATH, &itype);
            if (sel < 0 || itype == LV_PARENT) return 0;

            /* Close previous info window if still open */
            if (g_info_hwnd) { DestroyWindow(g_info_hwnd); g_info_hwnd = NULL; }

            /* Populate state read synchronously by InfoDlgProc WM_CREATE */
            strncpy(g_info_name_buf, fname, MAX_PATH);
            g_info_name_buf[MAX_PATH] = '\0';
            g_info_is_dir = (itype == LV_FOLDER);
            if (g_info_is_dir) {
                strncpy(g_info_dirname, fname, MAX_PATH - 1);
                g_info_dirname[MAX_PATH - 1] = '\0';
            }
            g_info_perm_buf[0] = '\0';
            if (sel >= 1 && (sel - 1) < g_displayed_count &&
                g_displayed_longname[sel - 1][0])
                strncpy(g_info_perm_buf, g_displayed_longname[sel - 1],
                        sizeof(g_info_perm_buf) - 1);

            /*
             * TODO — chmod modal (future work):
             *
             * 1. Parse permissions field (chars 1-9 of longname) into three
             *    rwx triplets for user/group/other, plus the setuid/setgid/sticky bits.
             * 2. Register a "WinSFTPChmod" window class (same pattern as WinSFTPPass).
             * 3. Show a 280x180 modal with:
             *      - Static label showing filename
             *      - Three rows (Owner / Group / Other), each with three checkboxes (R W X)
             *      - Checkboxes for setuid (u+s), setgid (g+s), sticky (o+t)
             *      - Current numeric mode shown in a read-only EDIT, updated live as
             *        checkboxes change (subclass the checkboxes with WM_COMMAND BN_CLICKED)
             *      - OK → build "chmod NNNN <fname>" and push to g_cmd_line / g_cmd_ready
             *      - Cancel → dismiss
             * 4. The numeric mode is built as:
             *      mode  = (r<<8)|(w<<7)|(x<<6)   // owner
             *            | (r<<5)|(w<<4)|(x<<3)   // group
             *            | (r<<2)|(w<<1)|(x<<0)   // other
             *            | (setuid<<11)|(setgid<<10)|(sticky<<9)
             *    Formatted as octal: sprintf(buf, "%04o", mode)
             * 5. Add IDM_CTX_REMOTE_CHMOD to the remote context menu.
             */

            {
                int dlg_h = g_info_is_dir ? 155 : 120;
                RECT wr, dr; int dw, dh;
                g_info_hwnd = CreateWindowEx(
                    WS_EX_DLGMODALFRAME,
                    "WinSFTPInfo", "Get Info",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU,
                    0, 0, 400, dlg_h,
                    g_hwnd, NULL, hinst, NULL);
                if (g_info_hwnd) {
                    GetWindowRect(g_hwnd,      &wr);
                    GetWindowRect(g_info_hwnd, &dr);
                    dw = dr.right - dr.left; dh = dr.bottom - dr.top;
                    SetWindowPos(g_info_hwnd, HWND_TOP,
                        wr.left + (wr.right  - wr.left - dw) / 2,
                        wr.top  + (wr.bottom - wr.top  - dh) / 2,
                        0, 0, SWP_NOSIZE);
                    ShowWindow(g_info_hwnd, SW_SHOW);
                }
            }
            return 0;
          }

          /* --- Get Info — local pane -------------------------- */
          case IDM_CTX_LOCAL_INFO: {
            char fname[MAX_PATH]; int itype, sel;
            char info[1024]; char path[MAX_PATH];
            WIN32_FIND_DATA fd;
            HANDLE hf;
            if (!g_local_list) return 0;
            sel = lv_get_focused(g_local_list, fname, MAX_PATH, &itype);
            if (sel < 0 || itype == LV_PARENT) return 0;

            GetCurrentDirectory(MAX_PATH, path);
            /* Build full path for FindFirstFile */
            { char full[MAX_PATH];
              sprintf(full, "%s\\%s", path, fname);
              hf = FindFirstFile(full, &fd); }

            if (hf != INVALID_HANDLE_VALUE) {
                SYSTEMTIME st; FILETIME lft;
                char datestr[64]; char sizestr[32];
                char attrs[64];

                FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lft);
                FileTimeToSystemTime(&lft, &st);
                sprintf(datestr, "%04d-%02d-%02d %02d:%02d:%02d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);

                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    strcpy(sizestr, "(directory)");
                else {
                    DWORD lo = fd.nFileSizeLow;
                    if (lo > 1024*1024)
                        sprintf(sizestr, "%lu MB", lo / (1024*1024));
                    else if (lo > 1024)
                        sprintf(sizestr, "%lu KB", lo / 1024);
                    else
                        sprintf(sizestr, "%lu bytes", lo);
                }

                attrs[0] = '\0';
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)   strcat(attrs, "Read-only  ");
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)     strcat(attrs, "Hidden  ");
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)     strcat(attrs, "System  ");
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)    strcat(attrs, "Archive  ");
                if (!attrs[0]) strcpy(attrs, "Normal");

                sprintf(info,
                    "Name:     %s\r\n"
                    "Size:     %s\r\n"
                    "Modified: %s\r\n"
                    "Attrs:    %s",
                    fname, sizestr, datestr, attrs);
                FindClose(hf);
            } else {
                sprintf(info, "Name: %s\r\n\r\n(Could not read file attributes)", fname);
            }

            MessageBox(hwnd, info, "Get Info", MB_OK | MB_ICONINFORMATION);
            return 0;
          }

          /* --- Help menu -------------------------------------- */
          case IDM_HELP_ABOUT:
            MessageBox(hwnd,
                "WinSFTP\r\n"
                "GUI SFTP client for Windows 3.1 + Win32s\r\n"
                "\r\n"
                "Based on PuTTY 0.83\r\n"
                "  by Simon Tatham and contributors\r\n"
                "  https://www.chiark.greenend.org.uk/~sgtatham/putty/\r\n"
                "\r\n"
                "Win32s port by Toyoyo\r\n"
                "  https://github.com/Toyoyo/putty-win32s\r\n"
                "\r\n"
                "WinSCP-style additions by github.com/knightmare2600",
                "About WinSFTP", MB_OK | MB_ICONINFORMATION);
            return 0;

          /* --- View menu -------------------------------------- */
          case IDM_VIEW_CLEAR:
            if (g_output) SetWindowText(g_output, "");
            return 0;

          case IDM_THEME_CLASSIC:
            apply_theme(THEME_CLASSIC);
            return 0;

          case IDM_THEME_BLUE:
            apply_theme(THEME_BLUE);
            return 0;

          case IDM_THEME_GREEN:
            apply_theme(THEME_GREEN);
            return 0;

        }
        return 0;

      case WM_CLOSE:
        g_running = false;
        DestroyWindow(hwnd);
        return 0;

      case WM_DESTROY:
        if (g_info_hwnd) {
            DestroyWindow(g_info_hwnd);
            g_info_hwnd = NULL;
        }
        if (g_active_path_brush) {
            DeleteObject(g_active_path_brush);
            g_active_path_brush = NULL;
        }
        if (g_theme_brush) {
            DeleteObject(g_theme_brush);
            g_theme_brush = NULL;
        }
        if (g_has_listview && g_img_list) {
            ImageList_Destroy(g_img_list);
            g_img_list = NULL;
        }
        g_hwnd        = NULL;
        g_local_list  = NULL;
        g_remote_list = NULL;
        g_local_path  = NULL;
        g_remote_path = NULL;
        g_get_btn          = NULL;
        g_put_btn          = NULL;
        g_refresh_btn      = NULL;
        g_mkdir_btn        = NULL;
        g_delete_btn       = NULL;
        g_rmdir_btn        = NULL;
        g_toolbar_connect  = NULL;
        g_toolbar_refresh  = NULL;
        g_toolbar_lup      = NULL;
        g_toolbar_rup      = NULL;
        g_output           = NULL;
        g_input       = NULL;
        g_send        = NULL;
        g_status      = NULL;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ================================================================
 * WinMain
 * ================================================================ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR cmdline, int nShow)
{
    WNDCLASS wc;
    int ret;

    hinst = hInst;

    InitCommonControls();

    /* Detect whether SysListView32 is available (comctl32 4.0+).
     * Base Win32s 1.30 does not include it; IE 3.0 for Win3.1 does.
     * If absent, both panes fall back to owner-draw LISTBOX (icons still shown). */
    {
        WNDCLASS wc_test;
        g_has_listview = (GetClassInfo(NULL, "SysListView32", &wc_test) != 0);
        if (!g_has_listview)
            MessageBox(NULL,
                "SysListView32 was not found (comctl32 4.0+ not installed).\r\n\r\n"
                "The file browser panes will use basic list mode.\r\n\r\n"
                "For the full file-manager view, install Internet Explorer 3.0\r\n"
                "for Windows 3.1 (ie301win31.exe) - freely available on archive.org.",
                "WinSFTP",
                MB_OK | MB_ICONINFORMATION);
    }

    dll_hijacking_protection();
    enable_dit();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WinSFTP";
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAINICON));
    if (!hPrev)
        RegisterClass(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = PassDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WinSFTPPass";
    RegisterClass(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = InfoDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WinSFTPInfo";
    RegisterClass(&wc);

    g_hwnd = CreateWindow(
        "WinSFTP", "WinSFTP",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        780, 560,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) {
        MessageBox(NULL, "Failed to create window", "WinSFTP", MB_OK);
        return 1;
    }

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    /* Populate local pane on startup */
    populate_local_list();

    /* Show connect dialog immediately on launch (WinSCP style) */
    do_connect_dialog();

    ret = 0;
    while (g_hwnd && g_running) {
        winsftp_append("WinSFTP " TEXTVER
            "  type \"open [user@]host [port]\" to connect\r\n\r\n");

        ret = psftp_main(NULL);

        if (g_hwnd && g_running) {
            winsftp_append("\r\nSession ended.\r\n\r\n");
            if (g_status) SetWindowText(g_status, "Ready");
            /* Clear the remote pane on disconnect */
            if (g_remote_list) {
                pane_clear(g_remote_list);
                SetWindowText(g_remote_path, "Remote:");
            }
        }
    }

    if (!hPrev)
        UnregisterClass("WinSFTP", hInst);
    UnregisterClass("WinSFTPPass", hInst);
    UnregisterClass("WinSFTPInfo", hInst);

    return ret;
}
