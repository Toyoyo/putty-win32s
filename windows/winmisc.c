/*
 * winmisc.c: miscellaneous Windows-specific things
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "putty.h"
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>

DWORD osMajorVersion, osMinorVersion, osPlatformId;

char *platform_get_x_display(void) {
    /* We may as well check for DISPLAY in case it's useful. */
    return dupstr(getenv("DISPLAY"));
}

Filename *filename_from_str(const char *str)
{
    Filename *ret = snew(Filename);
    ret->path = dupstr(str);
    return ret;
}

Filename *filename_copy(const Filename *fn)
{
    return filename_from_str(fn->path);
}

const char *filename_to_str(const Filename *fn)
{
    return fn->path;
}

bool filename_equal(const Filename *f1, const Filename *f2)
{
    return !strcmp(f1->path, f2->path);
}

bool filename_is_null(const Filename *fn)
{
    return !*fn->path;
}

void filename_free(Filename *fn)
{
    sfree(fn->path);
    sfree(fn);
}

void filename_serialise(BinarySink *bs, const Filename *f)
{
    put_asciz(bs, f->path);
}
Filename *filename_deserialise(BinarySource *src)
{
    return filename_from_str(get_asciz(src));
}

char filename_char_sanitise(char c)
{
    if (strchr("<>:\"/\\|?*", c))
        return '.';
    return c;
}

char *get_username(void)
{
    char *user = "win32s";
}

void dll_hijacking_protection(void)
{
    return;
}

void init_winver(void)
{
    OSVERSIONINFO osVersion;
/*    ZeroMemory(&osVersion, sizeof(osVersion));
    osMajorVersion = 3;
    osMinorVersion = 1;
    osPlatformId = VER_PLATFORM_WIN32s; */
}

HMODULE load_system32_dll(const char *libname)
{
  /* We're on win32s, there's no system32 anyway.
     Also, seriously, it's not really useful on this target.
     So let's just make this function a shim */
    HMODULE ret;
    ret = LoadLibrary(libname);
    return ret;
}

/*
 * A tree234 containing mappings from system error codes to strings.
 */

struct errstring {
    int error;
    char *text;
};

static int errstring_find(void *av, void *bv)
{
    int *a = (int *)av;
    struct errstring *b = (struct errstring *)bv;
    if (*a < b->error)
        return -1;
    if (*a > b->error)
        return +1;
    return 0;
}
static int errstring_compare(void *av, void *bv)
{
    struct errstring *a = (struct errstring *)av;
    return errstring_find(&a->error, bv);
}

static tree234 *errstrings = NULL;

const char *win_strerror(int error)
{
    struct errstring *es;

    if (!errstrings)
        errstrings = newtree234(errstring_compare);

    es = find234(errstrings, &error, errstring_find);

    if (!es) {
        char msgtext[65536]; /* maximum size for FormatMessage is 64K */

        es = snew(struct errstring);
        es->error = error;
        if (!FormatMessage((FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS), NULL, error,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           msgtext, lenof(msgtext)-1, NULL)) {
            sprintf(msgtext,
                    "(unable to format: FormatMessage returned %u)",
                    (unsigned int)GetLastError());
        } else {
            int len = strlen(msgtext);
            if (len > 0 && msgtext[len-1] == '\n')
                msgtext[len-1] = '\0';
        }
        es->text = dupprintf("Error %d: %s", error, msgtext);
        add234(errstrings, es);
    }

    return es->text;
}

FontSpec *fontspec_new(const char *name, bool bold, int height, int charset)
{
    FontSpec *f = snew(FontSpec);
    f->name = dupstr(name);
    f->isbold = bold;
    f->height = height;
    f->charset = charset;
    return f;
}
FontSpec *fontspec_copy(const FontSpec *f)
{
    return fontspec_new(f->name, f->isbold, f->height, f->charset);
}
void fontspec_free(FontSpec *f)
{
    sfree(f->name);
    sfree(f);
}
void fontspec_serialise(BinarySink *bs, FontSpec *f)
{
    put_asciz(bs, f->name);
    put_uint32(bs, f->isbold);
    put_uint32(bs, f->height);
    put_uint32(bs, f->charset);
}
FontSpec *fontspec_deserialise(BinarySource *src)
{
    const char *name = get_asciz(src);
    unsigned isbold = get_uint32(src);
    unsigned height = get_uint32(src);
    unsigned charset = get_uint32(src);
    return fontspec_new(name, isbold, height, charset);
}

bool open_for_write_would_lose_data(const Filename *fn)
{
    return false;
}

void escape_registry_key(const char *in, strbuf *out)
{
    bool candot = false;
    static const char hex[16] = "0123456789ABCDEF";

    while (*in) {
        if (*in == ' ' || *in == '\\' || *in == '*' || *in == '?' ||
            *in == '%' || *in < ' ' || *in > '~' || (*in == '.'
                                                     && !candot)) {
            put_byte(out, '%');
            put_byte(out, hex[((unsigned char) *in) >> 4]);
            put_byte(out, hex[((unsigned char) *in) & 15]);
        } else
            put_byte(out, *in);
        in++;
        candot = true;
    }
}

void unescape_registry_key(const char *in, strbuf *out)
{
    while (*in) {
        if (*in == '%' && in[1] && in[2]) {
            int i, j;

            i = in[1] - '0';
            i -= (i > 9 ? 7 : 0);
            j = in[2] - '0';
            j -= (j > 9 ? 7 : 0);

            put_byte(out, (i << 4) + j);
            in += 3;
        } else {
            put_byte(out, *in++);
        }
    }
}

#ifdef DEBUG
static FILE *debug_fp = NULL;
static HANDLE debug_hdl = INVALID_HANDLE_VALUE;
static int debug_got_console = 0;

void dputs(const char *buf)
{
    DWORD dw;

    if (!debug_got_console) {
        if (AllocConsole()) {
            debug_got_console = 1;
            debug_hdl = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (!debug_fp) {
        debug_fp = fopen("debug.log", "w");
    }

    if (debug_hdl != INVALID_HANDLE_VALUE) {
        WriteFile(debug_hdl, buf, strlen(buf), &dw, NULL);
    }
    fputs(buf, debug_fp);
    fflush(debug_fp);
}
#endif

char *registry_get_string(HKEY root, const char *path, const char *leaf)
{
    HKEY key = root;
    bool need_close_key = false;
    char *toret = NULL, *str = NULL;

    if (path) {
        if (RegCreateKey(key, path, &key) != ERROR_SUCCESS)
            goto out;
        need_close_key = true;
    }

    DWORD type, size;
    if (RegQueryValueEx(key, leaf, 0, &type, NULL, &size) != ERROR_SUCCESS)
        goto out;
    if (type != REG_SZ)
        goto out;

    str = snewn(size + 1, char);
    DWORD size_got = size;
    if (RegQueryValueEx(key, leaf, 0, &type, (LPBYTE)str,
                        &size_got) != ERROR_SUCCESS)
        goto out;
    if (type != REG_SZ || size_got > size)
        goto out;
    str[size_got] = '\0';

    toret = str;
    str = NULL;

  out:
    if (need_close_key)
        RegCloseKey(key);
    sfree(str);
    return toret;
}
