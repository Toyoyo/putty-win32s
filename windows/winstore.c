/*
 * uxstore.c: Unix-specific implementation of the interface defined
 * in storage.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
//#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
//#include <pwd.h>
#include "putty.h"
#include "storage.h"
#include "tree234.h"
#include "direct.h"

#ifdef PATH_MAX
#define FNLEN PATH_MAX
#else
#define FNLEN 1024 /* XXX */
#endif

enum {
    INDEX_DIR, INDEX_HOSTKEYS, INDEX_HOSTKEYS_TMP, INDEX_RANDSEED,
    INDEX_SESSIONDIR, INDEX_SESSION,
};

static const char hex[16] = "0123456789ABCDEF";

char *make_dir_path(const char *path, mode_t mode)
{
    int pos = 0;
    char *prefix;

    while (1) {
        pos += strcspn(path + pos, "/");

        if (pos > 0) {
            prefix = dupprintf("%.*s", pos, path);

            if (mkdir(prefix) < 0 && errno != EACCES) {
                char *ret = dupprintf("%s: mkdir: %s",
                                      prefix, strerror(errno));
                sfree(prefix);
                return ret;
            }

            sfree(prefix);
        }

        if (!path[pos])
            return NULL;
        pos += strspn(path + pos, "/");
    }
}

static void make_session_filename(const char *in, strbuf *out)
{
    if (!in || !*in)
        in = "DEFAULT";

    while (*in) {
        /*
         * There are remarkably few punctuation characters that
         * aren't shell-special in some way or likely to be used as
         * separators in some file format or another! Hence we use
         * opt-in for safe characters rather than opt-out for
         * specific unsafe ones...
         */
        if (*in!='+' && *in!='-' && *in!='.' && *in!='@' && *in!='_' &&
            !(*in >= '0' && *in <= '9') &&
            !(*in >= 'A' && *in <= 'Z') &&
            !(*in >= 'a' && *in <= 'z')) {
            put_byte(out, '%');
            put_byte(out, hex[((unsigned char) *in) >> 4]);
            put_byte(out, hex[((unsigned char) *in) & 15]);
        } else
            put_byte(out, *in);
        in++;
    }
}

static void decode_session_filename(const char *in, strbuf *out)
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

static char *make_filename(int index, const char *subname)
{
    char *env, *tmp, *ret;


    /*
     * Allow override of the PuTTY configuration location, and of
     * specific subparts of it, by means of environment variables.
     */
    if (index == INDEX_DIR) {
        struct passwd *pwd;
        char *xdg_dir, *old_dir, *old_dir2, *old_dir3, *home, *pwd_home;

        env = getenv("PUTTYDIR");
        if (env)
            return dupstr(env);

        home = getenv("HOME");
        if(!home) home="C:\HOME";

        xdg_dir = NULL;

        if (home) {
            old_dir = dupprintf("%s/putty", home);
        }

        if (old_dir && access(old_dir, F_OK) == 0) {
            ret = old_dir;
            goto out;
        }

        ret = old_dir;

      out:
        return ret;
    }
    if (index == INDEX_SESSIONDIR) {
        env = getenv("PUTTYSESSIONS");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/sessions", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_SESSION) {
        strbuf *sb = strbuf_new();
        tmp = make_filename(INDEX_SESSIONDIR, NULL);
        strbuf_catf(sb, "%s/", tmp);
        sfree(tmp);
        make_session_filename(subname, sb);
        return strbuf_to_str(sb);
    }
    if (index == INDEX_HOSTKEYS) {
        env = getenv("PUTTYSSHHOSTKEYS");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/hostkeys", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTKEYS_TMP) {
        tmp = make_filename(INDEX_HOSTKEYS, NULL);
        ret = dupprintf("%s.tmp", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_RANDSEED) {
        env = getenv("PUTTYRANDOMSEED");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/randomseed", tmp);
        sfree(tmp);
        return ret;
    }
    tmp = make_filename(INDEX_DIR, NULL);
    ret = dupprintf("%s/ERROR", tmp);
    sfree(tmp);
    return ret;

}

struct settings_w {
    FILE *fp;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    char *filename, *err;
    FILE *fp;

    *errmsg = NULL;

    /*
     * Start by making sure the .putty directory and its sessions
     * subdir actually exist.
     */
    filename = make_filename(INDEX_DIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSIONDIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "w");
    if (!fp) {
        *errmsg = dupprintf("Unable to save session: open(\"%s\") "
                            "returned '%s'", filename, strerror(errno));
        sfree(filename);
        return NULL;                   /* can't open */
    }
    sfree(filename);

    settings_w *toret = snew(settings_w);
    toret->fp = fp;
    return toret;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    fprintf(handle->fp, "%s=%s\n", key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    fprintf(handle->fp, "%s=%d\n", key, value);
}

void close_settings_w(settings_w *handle)
{
    fclose(handle->fp);
    sfree(handle);
}

/* ----------------------------------------------------------------------
 * System for treating X resources as a fallback source of defaults,
 * after data read from a saved-session disk file.
 *
 * The read_setting_* functions will call get_setting(key) as a
 * fallback if the setting isn't in the file they loaded. That in turn
 * will hand on to x_get_default, which the front end application
 * provides, and which actually reads resources from the X server (if
 * appropriate). In between, there's a tree234 of X-resource shaped
 * settings living locally in this file: the front end can call
 * provide_xrm_string() to insert a setting into this tree (typically
 * in response to an -xrm command line option or similar), and those
 * will override the actual X resources.
 */

struct skeyval {
    const char *key;
    const char *value;
};

static tree234 *xrmtree = NULL;

static int keycmp(void *av, void *bv)
{
    struct skeyval *a = (struct skeyval *)av;
    struct skeyval *b = (struct skeyval *)bv;
    return strcmp(a->key, b->key);
}

void provide_xrm_string(const char *string, const char *progname)
{
    const char *p, *q;
    char *key;
    struct skeyval *xrms, *ret;

    p = q = strchr(string, ':');
    if (!q) {
        fprintf(stderr, "%s: expected a colon in resource string"
                " \"%s\"\n", progname, string);
        return;
    }
    q++;
    while (p > string && p[-1] != '.' && p[-1] != '*')
        p--;
    xrms = snew(struct skeyval);
    key = snewn(q-p, char);
    memcpy(key, p, q-p);
    key[q-p-1] = '\0';
    xrms->key = key;
    while (*q && isspace((unsigned char)*q))
        q++;
    xrms->value = dupstr(q);

    if (!xrmtree)
        xrmtree = newtree234(keycmp);

    ret = add234(xrmtree, xrms);
    if (ret) {
        /* Override an existing string. */
        del234(xrmtree, ret);
        add234(xrmtree, xrms);
    }
}

static const char *get_setting(const char *key)
{
    struct skeyval tmp, *ret;
    tmp.key = key;
    if (xrmtree) {
        ret = find234(xrmtree, &tmp, NULL);
        if (ret)
            return ret->value;
    }
    return NULL;
}

/* ----------------------------------------------------------------------
 * Main code for reading settings from a disk file, calling the above
 * get_setting() as a fallback if necessary.
 */

struct settings_r {
    tree234 *t;
};

settings_r *open_settings_r(const char *sessionname)
{
    char *filename;
    FILE *fp;
    char *line;
    settings_r *toret;

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;                   /* can't open */

    toret = snew(settings_r);
    toret->t = newtree234(keycmp);

    while ( (line = fgetline(fp)) ) {
        char *value = strchr(line, '=');
        struct skeyval *kv;

        if (!value) {
            sfree(line);
            continue;
        }
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';   /* trim trailing NL */

        kv = snew(struct skeyval);
        kv->key = dupstr(line);
        kv->value = dupstr(value);
        add234(toret->t, kv);

        sfree(line);
    }

    fclose(fp);

    return toret;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    const char *val;
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        val = kv->value;
        assert(val != NULL);
    } else
        val = get_setting(key);

    if (!val)
        return NULL;
    else
        return dupstr(val);
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    const char *val;
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        val = kv->value;
        assert(val != NULL);
    } else
        val = get_setting(key);

    if (!val)
        return defvalue;
    else
        return atoi(val);
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    /*
     * In GTK1-only PuTTY, we used to store font names simply as a
     * valid X font description string (logical or alias), under a
     * bare key such as "Font".
     *
     * In GTK2 PuTTY, we have a prefix system where "client:"
     * indicates a Pango font and "server:" an X one; existing
     * configuration needs to be reinterpreted as having the
     * "server:" prefix, so we change the storage key from the
     * provided name string (e.g. "Font") to a suffixed one
     * ("FontName").
     */
    char *suffname = dupcat(name, "Name");
    char *fontname;
    char *fontisbold;
    char *fontheight;
    int fs_bold;
    int fs_height;

    fontname = read_setting_s(handle, "FontName");
    fontisbold = read_setting_s(handle, "FontIsBold");
    fontheight = read_setting_s(handle, "FontHeight");

    if(fontname && fontisbold && fontheight) {
      fs_bold = (bool)atoi(fontisbold);
      fs_height = atoi(fontheight);
      FontSpec *fs = fontspec_new(fontname, fs_bold, fs_height, 0);
      return fs;
    }
    return NULL;
}
Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_fontspec(settings_w *handle, const char *name, FontSpec *fs)
{
    /*
     * read_setting_fontspec had to handle two cases, but when
     * writing our settings back out we simply always generate the
     * new-style name.
     */
    char *suffname = dupcat(name, "Name");
    write_setting_s(handle, suffname, fs->name);

    char *s_isbold = malloc(256);
    sprintf(s_isbold, "%d", fs->isbold);
    write_setting_s(handle, "FontIsBold", s_isbold);

    char *s_height = malloc(256);
    sprintf(s_height, "%d", fs->height);
    write_setting_s(handle, "FontHeight", s_height);

    sfree(suffname);
}
void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    write_setting_s(handle, name, result->path);
}

void close_settings_r(settings_r *handle)
{
    struct skeyval *kv;

    if (!handle)
        return;

    while ( (kv = index234(handle->t, 0)) != NULL) {
        del234(handle->t, kv);
        sfree((char *)kv->key);
        sfree((char *)kv->value);
        sfree(kv);
    }

    freetree234(handle->t);
    sfree(handle);
}

void del_settings(const char *sessionname)
{
    char *filename;
    filename = make_filename(INDEX_SESSION, sessionname);
    unlink(filename);
    sfree(filename);
}

struct settings_e {
    DIR *dp;
};

settings_e *enum_settings_start(void)
{
    DIR *dp;
    char *filename;

    filename = make_filename(INDEX_SESSIONDIR, NULL);
    dp = opendir(filename);
    sfree(filename);

    settings_e *toret = snew(settings_e);
    toret->dp = dp;
    return toret;
}

bool enum_settings_next(settings_e *handle, strbuf *out)
{
    struct dirent *de;
    struct stat st;
    strbuf *fullpath;

    if (!handle->dp)
        return NULL;

    fullpath = strbuf_new();

    char *sessiondir = make_filename(INDEX_SESSIONDIR, NULL);
    put_datapl(fullpath, ptrlen_from_asciz(sessiondir));
    sfree(sessiondir);
    put_byte(fullpath, '/');

    size_t baselen = fullpath->len;

    while ( (de = readdir(handle->dp)) != NULL ) {
        strbuf_shrink_to(fullpath, baselen);
        put_datapl(fullpath, ptrlen_from_asciz(de->d_name));

        if (stat(fullpath->s, &st) < 0 || !S_ISREG(st.st_mode))
            continue;                  /* try another one */

        decode_session_filename(de->d_name, out);
        strbuf_free(fullpath);
        return true;
    }

    strbuf_free(fullpath);
    return false;
}

void enum_settings_finish(settings_e *handle)
{
    if (handle->dp)
        closedir(handle->dp);
    sfree(handle);
}

/*
 * Lines in the host keys file are of the form
 *
 *   type@port:hostname keydata
 *
 * e.g.
 *
 *   rsa@22:foovax.example.org 0x23,0x293487364395345345....2343
 */
int verify_host_key(const char *hostname, int port,
                    const char *keytype, const char *key)
{
    FILE *fp;
    char *filename;
    char *line;
    int ret;

    filename = make_filename(INDEX_HOSTKEYS, NULL);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return 1;                      /* key does not exist */

    ret = 1;
    while ( (line = fgetline(fp)) ) {
        int i;
        char *p = line;
        char porttext[20];

        line[strcspn(line, "\n")] = '\0';   /* strip trailing newline */

        i = strlen(keytype);
        if (strncmp(p, keytype, i))
            goto done;
        p += i;

        if (*p != '@')
            goto done;
        p++;

        sprintf(porttext, "%d", port);
        i = strlen(porttext);
        if (strncmp(p, porttext, i))
            goto done;
        p += i;

        if (*p != ':')
            goto done;
        p++;

        i = strlen(hostname);
        if (strncmp(p, hostname, i))
            goto done;
        p += i;

        if (*p != ' ')
            goto done;
        p++;

        /*
         * Found the key. Now just work out whether it's the right
         * one or not.
         */
        if (!strcmp(p, key))
            ret = 0;                   /* key matched OK */
        else
            ret = 2;                   /* key mismatch */

        done:
        sfree(line);
        if (ret != 1)
            break;
    }

    fclose(fp);
    return ret;
}

bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype)
{
    /*
     * If we have a host key, verify_host_key will return 0 or 2.
     * If we don't have one, it'll return 1.
     */
    return verify_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(const char *hostname, int port,
                    const char *keytype, const char *key)
{
    FILE *rfp, *wfp;
    char *newtext, *line;
    int headerlen;
    char *filename, *tmpfilename;

    /*
     * Open both the old file and a new file.
     */
    tmpfilename = make_filename(INDEX_HOSTKEYS_TMP, NULL);
    wfp = fopen(tmpfilename, "w");
    if (!wfp && errno == ENOENT) {
        char *dir, *errmsg;

        dir = make_filename(INDEX_DIR, NULL);
        if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
            nonfatal("Unable to store host key: %s", errmsg);
            sfree(errmsg);
            sfree(dir);
            sfree(tmpfilename);
            return;
        }
        sfree(dir);

        wfp = fopen(tmpfilename, "w");
    }
    if (!wfp) {
        nonfatal("Unable to store host key: open(\"%s\") "
                 "returned '%s'", tmpfilename, strerror(errno));
        sfree(tmpfilename);
        return;
    }
    filename = make_filename(INDEX_HOSTKEYS, NULL);
    rfp = fopen(filename, "r");

    newtext = dupprintf("%s@%d:%s %s\n", keytype, port, hostname, key);
    headerlen = 1 + strcspn(newtext, " ");   /* count the space too */

    /*
     * Copy all lines from the old file to the new one that _don't_
     * involve the same host key identifier as the one we're adding.
     */
    if (rfp) {
        while ( (line = fgetline(rfp)) ) {
            if (strncmp(line, newtext, headerlen))
                fputs(line, wfp);
            sfree(line);
        }
        fclose(rfp);
    }

    /*
     * Now add the new line at the end.
     */
    fputs(newtext, wfp);

    fclose(wfp);

    remove(filename);
    if (rename(tmpfilename, filename) < 0) {
        nonfatal("Unable to store host key: rename(\"%s\",\"%s\")"
                 " returned '%s'", tmpfilename, filename,
                 strerror(errno));
    }

    sfree(tmpfilename);
    sfree(filename);
    sfree(newtext);
}

void read_random_seed(noise_consumer_t consumer)
{
    int fd;
    char *fname;

    fname = make_filename(INDEX_RANDSEED, NULL);
    fd = open(fname, O_RDONLY);
    sfree(fname);
    if (fd >= 0) {
        char buf[512];
        int ret;
        while ( (ret = read(fd, buf, sizeof(buf))) > 0)
            consumer(buf, ret);
        close(fd);
    }
}

void write_random_seed(void *data, int len)
{
    int fd;
    char *fname;

    fname = make_filename(INDEX_RANDSEED, NULL);
    /*
     * Don't truncate the random seed file if it already exists; if
     * something goes wrong half way through writing it, it would
     * be better to leave the old data there than to leave it empty.
     */
    fd = open(fname, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno != ENOENT) {
            nonfatal("Unable to write random seed: open(\"%s\") "
                     "returned '%s'", fname, strerror(errno));
            sfree(fname);
            return;
        }
        char *dir, *errmsg;

        dir = make_filename(INDEX_DIR, NULL);
        if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
            nonfatal("Unable to write random seed: %s", errmsg);
            sfree(errmsg);
            sfree(fname);
            sfree(dir);
            return;
        }
        sfree(dir);

        fd = open(fname, O_CREAT | O_WRONLY, 0600);
        if (fd < 0) {
            nonfatal("Unable to write random seed: open(\"%s\") "
                     "returned '%s'", fname, strerror(errno));
            sfree(fname);
            return;
        }
    }

    while (len > 0) {
        int ret = write(fd, data, len);
        if (ret < 0) {
            nonfatal("Unable to write random seed: write "
                     "returned '%s'", strerror(errno));
            break;
        }
        len -= ret;
        data = (char *)data + len;
    }

    close(fd);
    sfree(fname);
}

void cleanup_all(void)
{
}

char *get_jumplist_registry_entries (void)
{
    char *list_value;

    if (transform_jumplist_registry(NULL,NULL,&list_value) != JUMPLISTREG_OK) {
        list_value = snewn(2, char);
        *list_value = '\0';
        *(list_value + 1) = '\0';
    }
    return list_value;
}
int add_to_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(item, item, NULL);
}

int remove_from_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(NULL, item, NULL);
}

static int transform_jumplist_registry
    (const char *add, const char *rem, char **out)
{
  return 1;
}