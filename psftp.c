/*
 * psftp.c: (platform-independent) front end for PSFTP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>

#include "putty.h"
#include "psftp.h"
#include "storage.h"
#include "ssh.h"
#include "ssh/sftp.h"

#ifdef WINSFTP_BUILD
/* Progress callbacks implemented in windows/winsftp.c */
void sftp_progress_init(const char *fname, uint64_t total);
void sftp_progress_update(uint64_t done);
/* When non-empty, psftp_connect launches this exec command instead of
 * the SFTP subsystem.  Set before calling psftp_connect, cleared after. */
static char scp_remote_cmd[1024];
/* Host and remote path remembered for per-file transfer log lines. */
static char scp_log_host[256];
static char scp_log_remotepath[1024];
#endif

/*
 * Since SFTP is a request-response oriented protocol, it requires
 * no buffer management: when we send data, we stop and wait for an
 * acknowledgement _anyway_, and so we can't possibly overfill our
 * send buffer.
 */

static int psftp_connect(char *userhost, char *user, int portnumber);
static int do_sftp_init(void);
static void do_sftp_cleanup(void);

/* ----------------------------------------------------------------------
 * sftp client state.
 */

static char *pwd, *homedir;
static LogContext *psftp_logctx = NULL;
static Backend *backend;
#ifdef WINSFTP_BUILD
Conf *conf;
#else
static Conf *conf;
#endif
static bool sent_eof = false;
static bufchain received_data;

/* ------------------------------------------------------------
 * Seat vtable.
 */

static size_t psftp_output(Seat *, SeatOutputType type, const void *, size_t);
static bool psftp_eof(Seat *);

static const SeatVtable psftp_seat_vt = {
    .output = psftp_output,
    .eof = psftp_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = filexfer_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = nullseat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = console_connection_fatal,
    .nonfatal = console_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = console_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = console_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = console_confirm_weak_cached_hostkey,
    .prompt_descriptions = console_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_x_display = nullseat_get_x_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = console_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_yes,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_no,
    .verbose = cmdline_seat_verbose,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = nullseat_get_cursor_position,
};
static Seat psftp_seat[1] = {{ &psftp_seat_vt }};

/* ----------------------------------------------------------------------
 * A nasty loop macro that lets me get an escape-sequence sanitised
 * version of a string for display, and free it automatically
 * afterwards.
 */
static StripCtrlChars *string_scc;
#define with_stripctrl(varname, input)                                  \
    for (char *varname = stripctrl_string(string_scc, input); varname;  \
         sfree(varname), varname = NULL)

/* ----------------------------------------------------------------------
 * Manage sending requests and waiting for replies.
 */
struct sftp_packet *sftp_wait_for_reply(struct sftp_request *req)
{
    struct sftp_packet *pktin;
    struct sftp_request *rreq;

    sftp_register(req);
    pktin = sftp_recv();
    if (pktin == NULL) {
        seat_connection_fatal(
            psftp_seat, "did not receive SFTP response packet from server");
    }
    rreq = sftp_find_request(pktin);
    if (rreq != req) {
        seat_connection_fatal(
            psftp_seat,
            "unable to understand SFTP response packet from server: %s",
            fxp_error());
    }
    return pktin;
}

/* ----------------------------------------------------------------------
 * Higher-level helper functions used in commands.
 */

/*
 * Attempt to canonify a pathname starting from the pwd. If
 * canonification fails, at least fall back to returning a _valid_
 * pathname (though it may be ugly, eg /home/simon/../foobar).
 */
char *canonify(const char *name)
{
    char *fullname, *canonname;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (name[0] == '/') {
        fullname = dupstr(name);
    } else {
        const char *slash;
        if (pwd[strlen(pwd) - 1] == '/')
            slash = "";
        else
            slash = "/";
        fullname = dupcat(pwd, slash, name);
    }

    req = fxp_realpath_send(fullname);
    pktin = sftp_wait_for_reply(req);
    canonname = fxp_realpath_recv(pktin, req);

    if (canonname) {
        sfree(fullname);
        return canonname;
    } else {
        /*
         * Attempt number 2. Some FXP_REALPATH implementations
         * (glibc-based ones, in particular) require the _whole_
         * path to point to something that exists, whereas others
         * (BSD-based) only require all but the last component to
         * exist. So if the first call failed, we should strip off
         * everything from the last slash onwards and try again,
         * then put the final component back on.
         *
         * Special cases:
         *
         *  - if the last component is "/." or "/..", then we don't
         *    bother trying this because there's no way it can work.
         *
         *  - if the thing actually ends with a "/", we remove it
         *    before we start. Except if the string is "/" itself
         *    (although I can't see why we'd have got here if so,
         *    because surely "/" would have worked the first
         *    time?), in which case we don't bother.
         *
         *  - if there's no slash in the string at all, give up in
         *    confusion (we expect at least one because of the way
         *    we constructed the string).
         */

        int i;
        char *returnname;

        i = strlen(fullname);
        if (i > 2 && fullname[i - 1] == '/')
            fullname[--i] = '\0';      /* strip trailing / unless at pos 0 */
        while (i > 0 && fullname[--i] != '/');

        /*
         * Give up on special cases.
         */
        if (fullname[i] != '/' ||      /* no slash at all */
            !strcmp(fullname + i, "/.") ||      /* ends in /. */
            !strcmp(fullname + i, "/..") ||     /* ends in /.. */
            !strcmp(fullname, "/")) {
            return fullname;
        }

        /*
         * Now i points at the slash. Deal with the final special
         * case i==0 (ie the whole path was "/nonexistentfile").
         */
        fullname[i] = '\0';            /* separate the string */
        if (i == 0) {
            req = fxp_realpath_send("/");
        } else {
            req = fxp_realpath_send(fullname);
        }
        pktin = sftp_wait_for_reply(req);
        canonname = fxp_realpath_recv(pktin, req);

        if (!canonname) {
            /* Even that failed. Restore our best guess at the
             * constructed filename and give up */
            fullname[i] = '/';  /* restore slash and last component */
            return fullname;
        }

        /*
         * We have a canonical name for all but the last path
         * component. Concatenate the last component and return.
         */
        returnname = dupcat(canonname,
                            (strendswith(canonname, "/") ? "" : "/"),
                            fullname + i + 1);
        sfree(fullname);
        sfree(canonname);
        return returnname;
    }
}

static int bare_name_compare(const void *av, const void *bv)
{
    const char **a = (const char **) av;
    const char **b = (const char **) bv;
    return strcmp(*a, *b);
}

static void not_connected(void)
{
    printf("psftp: not connected to a host; use \"open host.name\"\n");
}

/* ----------------------------------------------------------------------
 * The meat of the `get' and `put' commands.
 */
bool sftp_get_file(char *fname, char *outfname, bool recurse, bool restart)
{
    struct fxp_handle *fh;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_xfer *xfer;
    uint64_t offset;
    WFile *file;
    bool toret, shown_err = false;
    struct fxp_attrs attrs;

    /*
     * In recursive mode, see if we're dealing with a directory.
     * (If we're not in recursive mode, we need not even check: the
     * subsequent FXP_OPEN will return a usable error message.)
     */
    if (recurse) {
        bool result;

        req = fxp_stat_send(fname);
        pktin = sftp_wait_for_reply(req);
        result = fxp_stat_recv(pktin, req, &attrs);

        if (result &&
            (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
            (attrs.permissions & 0040000)) {

            struct fxp_handle *dirhandle;
            size_t nnames, namesize;
            struct fxp_name **ournames;
            struct fxp_names *names;
            int i;

            /*
             * First, attempt to create the destination directory,
             * unless it already exists.
             */
            if (file_type(outfname) != FILE_TYPE_DIRECTORY &&
                !create_directory(outfname)) {
                with_stripctrl(san, outfname)
                    printf("%s: Cannot create directory\n", san);
                return false;
            }

            /*
             * Now get the list of filenames in the remote
             * directory.
             */
            req = fxp_opendir_send(fname);
            pktin = sftp_wait_for_reply(req);
            dirhandle = fxp_opendir_recv(pktin, req);

            if (!dirhandle) {
                with_stripctrl(san, fname)
                    printf("%s: unable to open directory: %s\n",
                           san, fxp_error());
                return false;
            }
            nnames = namesize = 0;
            ournames = NULL;
            while (1) {
                int i;

                req = fxp_readdir_send(dirhandle);
                pktin = sftp_wait_for_reply(req);
                names = fxp_readdir_recv(pktin, req);

                if (names == NULL) {
                    if (fxp_error_type() == SSH_FX_EOF)
                        break;
                    with_stripctrl(san, fname)
                        printf("%s: reading directory: %s\n",
                               san, fxp_error());

                    req = fxp_close_send(dirhandle);
                    pktin = sftp_wait_for_reply(req);
                    fxp_close_recv(pktin, req);

                    sfree(ournames);
                    return false;
                }
                if (names->nnames == 0) {
                    fxp_free_names(names);
                    break;
                }
                sgrowarrayn(ournames, namesize, nnames, names->nnames);
                for (i = 0; i < names->nnames; i++)
                    if (strcmp(names->names[i].filename, ".") &&
                        strcmp(names->names[i].filename, "..")) {
                        if (!vet_filename(names->names[i].filename)) {
                            with_stripctrl(san, names->names[i].filename)
                                printf("ignoring potentially dangerous server-"
                                       "supplied filename '%s'\n", san);
                        } else {
                            ournames[nnames++] =
                                fxp_dup_name(&names->names[i]);
                        }
                    }
                fxp_free_names(names);
            }
            req = fxp_close_send(dirhandle);
            pktin = sftp_wait_for_reply(req);
            fxp_close_recv(pktin, req);

            /*
             * Sort the names into a clear order. This ought to
             * make things more predictable when we're doing a
             * reget of the same directory, just in case two
             * readdirs on the same remote directory return a
             * different order.
             */
            if (nnames > 0)
                qsort(ournames, nnames, sizeof(*ournames), sftp_name_compare);

            /*
             * If we're in restart mode, find the last filename on
             * this list that already exists. We may have to do a
             * reget on _that_ file, but shouldn't have to do
             * anything on the previous files.
             *
             * If none of them exists, of course, we start at 0.
             */
            i = 0;
            if (restart) {
                while (i < nnames) {
                    char *nextoutfname;
                    bool nonexistent;
                    nextoutfname = dir_file_cat(outfname,
                                                ournames[i]->filename);
                    nonexistent = (file_type(nextoutfname) ==
                                   FILE_TYPE_NONEXISTENT);
                    sfree(nextoutfname);
                    if (nonexistent)
                        break;
                    i++;
                }
                if (i > 0)
                    i--;
            }

            /*
             * Now we're ready to recurse. Starting at ournames[i]
             * and continuing on to the end of the list, we
             * construct a new source and target file name, and
             * call sftp_get_file again.
             */
            for (; i < nnames; i++) {
                char *nextfname, *nextoutfname;
                bool retd;

                nextfname = dupcat(fname, "/", ournames[i]->filename);
                nextoutfname = dir_file_cat(outfname, ournames[i]->filename);
                retd = sftp_get_file(
                    nextfname, nextoutfname, recurse, restart);
                restart = false;       /* after first partial file, do full */
                sfree(nextoutfname);
                sfree(nextfname);
                if (!retd) {
                    for (i = 0; i < nnames; i++) {
                        fxp_free_name(ournames[i]);
                    }
                    sfree(ournames);
                    return false;
                }
            }

            /*
             * Done this recursion level. Free everything.
             */
            for (i = 0; i < nnames; i++) {
                fxp_free_name(ournames[i]);
            }
            sfree(ournames);

            return true;
        }
    }

    req = fxp_stat_send(fname);
    pktin = sftp_wait_for_reply(req);
    if (!fxp_stat_recv(pktin, req, &attrs))
        attrs.flags = 0;

    req = fxp_open_send(fname, SSH_FXF_READ, NULL);
    pktin = sftp_wait_for_reply(req);
    fh = fxp_open_recv(pktin, req);

    if (!fh) {
        with_stripctrl(san, fname)
            printf("%s: open for read: %s\n", san, fxp_error());
        return false;
    }

    if (restart) {
        file = open_existing_wfile(outfname, NULL);
    } else {
        file = open_new_file(outfname, GET_PERMISSIONS(attrs, -1));
    }

    if (!file) {
        with_stripctrl(san, outfname)
            printf("local: unable to open %s\n", san);

        req = fxp_close_send(fh);
        pktin = sftp_wait_for_reply(req);
        fxp_close_recv(pktin, req);

        return false;
    }

    if (restart) {
        if (seek_file(file, 0, FROM_END) == -1) {
            close_wfile(file);
            with_stripctrl(san, outfname)
                printf("reget: cannot restart %s - file too large\n", san);
            req = fxp_close_send(fh);
            pktin = sftp_wait_for_reply(req);
            fxp_close_recv(pktin, req);

            return false;
        }

        offset = get_file_posn(file);
        printf("reget: restarting at file position %"PRIu64"\n", offset);
    } else {
        offset = 0;
    }

#ifdef WINSFTP_BUILD
    sftp_progress_init(fname,
        (attrs.flags & SSH_FILEXFER_ATTR_SIZE) ? attrs.size : 0);
    {
        uint64_t bytes_done = offset;
#endif
    toret = true;
    xfer = xfer_download_init(fh, offset);
    while (!xfer_done(xfer)) {
        void *vbuf;
        int retd, len;
        int wpos, wlen;

        xfer_download_queue(xfer);
        pktin = sftp_recv();
        retd = xfer_download_gotpkt(xfer, pktin);
        if (retd <= 0) {
            if (!shown_err) {
                printf("error while reading: %s\n", fxp_error());
                shown_err = true;
            }
            if (retd == INT_MIN)        /* pktin not even freed */
                sfree(pktin);
            toret = false;
        }

        while (xfer_download_data(xfer, &vbuf, &len)) {
            unsigned char *buf = (unsigned char *)vbuf;

            wpos = 0;
            while (wpos < len) {
                wlen = write_to_file(file, buf + wpos, len - wpos);
                if (wlen <= 0) {
                    printf("error while writing local file\n");
                    toret = false;
                    xfer_set_error(xfer);
                    break;
                }
                wpos += wlen;
            }
            if (wpos < len) {          /* we had an error */
                toret = false;
                xfer_set_error(xfer);
            }

#ifdef WINSFTP_BUILD
            bytes_done += len;
            sftp_progress_update(bytes_done);
#endif
            sfree(vbuf);
        }
    }
#ifdef WINSFTP_BUILD
    } /* close bytes_done scope */
#endif

    xfer_cleanup(xfer);

    close_wfile(file);

    req = fxp_close_send(fh);
    pktin = sftp_wait_for_reply(req);
    fxp_close_recv(pktin, req);

#ifdef WINSFTP_BUILD
    with_stripctrl(san, fname) {
        with_stripctrl(sano, outfname)
            printf("remote:%s => local:%s  [%s]\n", san, sano, toret ? "OK" : "FAILED");
    }
#endif
    return toret;
}

bool sftp_put_file(char *fname, char *outfname, bool recurse, bool restart)
{
    struct fxp_handle *fh;
    struct fxp_xfer *xfer;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    uint64_t offset;
    RFile *file;
    bool err = false, eof;
    struct fxp_attrs attrs;
    long permissions;

    /*
     * In recursive mode, see if we're dealing with a directory.
     * (If we're not in recursive mode, we need not even check: the
     * subsequent fopen will return an error message.)
     */
    if (recurse && file_type(fname) == FILE_TYPE_DIRECTORY) {
        bool result;
        size_t nnames, namesize;
        char *name, **ournames;
        const char *opendir_err;
        DirHandle *dh;
        size_t i;

        /*
         * First, attempt to create the destination directory,
         * unless it already exists.
         */
        req = fxp_stat_send(outfname);
        pktin = sftp_wait_for_reply(req);
        result = fxp_stat_recv(pktin, req, &attrs);
        if (!result ||
            !(attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) ||
            !(attrs.permissions & 0040000)) {
            req = fxp_mkdir_send(outfname, NULL);
            pktin = sftp_wait_for_reply(req);
            result = fxp_mkdir_recv(pktin, req);

            if (!result) {
                printf("%s: create directory: %s\n",
                       outfname, fxp_error());
                return false;
            }
        }

        /*
         * Now get the list of filenames in the local directory.
         */
        nnames = namesize = 0;
        ournames = NULL;

        dh = open_directory(fname, &opendir_err);
        if (!dh) {
            printf("%s: unable to open directory: %s\n", fname, opendir_err);
            return false;
        }
        while ((name = read_filename(dh)) != NULL) {
            sgrowarray(ournames, namesize, nnames);
            ournames[nnames++] = name;
        }
        close_directory(dh);

        /*
         * Sort the names into a clear order. This ought to make
         * things more predictable when we're doing a reput of the
         * same directory, just in case two readdirs on the same
         * local directory return a different order.
         */
        if (nnames > 0)
            qsort(ournames, nnames, sizeof(*ournames), bare_name_compare);

        /*
         * If we're in restart mode, find the last filename on this
         * list that already exists. We may have to do a reput on
         * _that_ file, but shouldn't have to do anything on the
         * previous files.
         *
         * If none of them exists, of course, we start at 0.
         */
        i = 0;
        if (restart) {
            while (i < nnames) {
                char *nextoutfname;
                nextoutfname = dupcat(outfname, "/", ournames[i]);
                req = fxp_stat_send(nextoutfname);
                pktin = sftp_wait_for_reply(req);
                result = fxp_stat_recv(pktin, req, &attrs);
                sfree(nextoutfname);
                if (!result)
                    break;
                i++;
            }
            if (i > 0)
                i--;
        }

        /*
         * Now we're ready to recurse. Starting at ournames[i]
         * and continuing on to the end of the list, we
         * construct a new source and target file name, and
         * call sftp_put_file again.
         */
        for (; i < nnames; i++) {
            char *nextfname, *nextoutfname;
            bool retd;

            nextfname = dir_file_cat(fname, ournames[i]);
            nextoutfname = dupcat(outfname, "/", ournames[i]);
            retd = sftp_put_file(nextfname, nextoutfname, recurse, restart);
            restart = false;           /* after first partial file, do full */
            sfree(nextoutfname);
            sfree(nextfname);
            if (!retd) {
                for (size_t i = 0; i < nnames; i++) {
                    sfree(ournames[i]);
                }
                sfree(ournames);
                return false;
            }
        }

        /*
         * Done this recursion level. Free everything.
         */
        for (size_t i = 0; i < nnames; i++) {
            sfree(ournames[i]);
        }
        sfree(ournames);

        return true;
    }

#ifdef WINSFTP_BUILD
    uint64_t local_file_size = 0;
    file = open_existing_file(fname, &local_file_size, NULL, NULL, &permissions);
#else
    file = open_existing_file(fname, NULL, NULL, NULL, &permissions);
#endif
    if (!file) {
        printf("local: unable to open %s\n", fname);
        return false;
    }
    attrs.flags = 0;
    PUT_PERMISSIONS(attrs, permissions);
    if (restart) {
        req = fxp_open_send(outfname, SSH_FXF_WRITE, &attrs);
    } else {
        req = fxp_open_send(outfname,
                            SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC,
                            &attrs);
    }
    pktin = sftp_wait_for_reply(req);
    fh = fxp_open_recv(pktin, req);

    if (!fh) {
        close_rfile(file);
        printf("%s: open for write: %s\n", outfname, fxp_error());
        return false;
    }

    if (restart) {
        struct fxp_attrs attrs;
        bool retd;

        req = fxp_fstat_send(fh);
        pktin = sftp_wait_for_reply(req);
        retd = fxp_fstat_recv(pktin, req, &attrs);

        if (!retd) {
            printf("read size of %s: %s\n", outfname, fxp_error());
            err = true;
            goto cleanup;
        }
        if (!(attrs.flags & SSH_FILEXFER_ATTR_SIZE)) {
            printf("read size of %s: size was not given\n", outfname);
            err = true;
            goto cleanup;
        }
        offset = attrs.size;
        printf("reput: restarting at file position %"PRIu64"\n", offset);

        if (seek_file((WFile *)file, offset, FROM_START) != 0)
            seek_file((WFile *)file, 0, FROM_END);    /* *shrug* */
    } else {
        offset = 0;
    }

#ifdef WINSFTP_BUILD
    sftp_progress_init(fname, local_file_size);
    {
        uint64_t bytes_sent = offset;
#endif
    xfer = xfer_upload_init(fh, offset);
    eof = false;
    while ((!err && !eof) || !xfer_done(xfer)) {
        char buffer[4096];
        int len, ret;

        while (xfer_upload_ready(xfer) && !err && !eof) {
            len = read_from_file(file, buffer, sizeof(buffer));
            if (len == -1) {
                printf("error while reading local file\n");
                err = true;
            } else if (len == 0) {
                eof = true;
            } else {
                xfer_upload_data(xfer, buffer, len);
#ifdef WINSFTP_BUILD
                bytes_sent += len;
                sftp_progress_update(bytes_sent);
#endif
            }
        }

        if (toplevel_callback_pending() && !err && !eof) {
            /* If we have pending callbacks, they might make
             * xfer_upload_ready start to return true. So we should
             * run them and then re-check xfer_upload_ready, before
             * we go as far as waiting for an entire packet to
             * arrive. */
            run_toplevel_callbacks();
            continue;
        }

        if (!xfer_done(xfer)) {
            pktin = sftp_recv();
            ret = xfer_upload_gotpkt(xfer, pktin);
            if (ret <= 0) {
                if (ret == INT_MIN)        /* pktin not even freed */
                    sfree(pktin);
                if (!err) {
                    printf("error while writing: %s\n", fxp_error());
                    err = true;
                }
            }
        }
    }

    xfer_cleanup(xfer);
#ifdef WINSFTP_BUILD
    } /* close bytes_sent block */
#endif

  cleanup:
    req = fxp_close_send(fh);
    pktin = sftp_wait_for_reply(req);
    if (!fxp_close_recv(pktin, req)) {
        if (!err) {
            printf("error while closing: %s", fxp_error());
            err = true;
        }
    }

    close_rfile(file);

#ifdef WINSFTP_BUILD
    with_stripctrl(sano, outfname)
        printf("local:%s => remote:%s  [%s]\n", fname, sano, err ? "FAILED" : "OK");
#endif
    return !err;
}

/* ----------------------------------------------------------------------
 * A remote wildcard matcher, providing a similar interface to the
 * local one in psftp.h.
 */

typedef struct SftpWildcardMatcher {
    struct fxp_handle *dirh;
    struct fxp_names *names;
    int namepos;
    char *wildcard, *prefix;
} SftpWildcardMatcher;

SftpWildcardMatcher *sftp_begin_wildcard_matching(char *name)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    char *wildcard;
    char *unwcdir, *tmpdir, *cdir;
    int len;
    bool check;
    SftpWildcardMatcher *swcm;
    struct fxp_handle *dirh;

    /*
     * We don't handle multi-level wildcards; so we expect to find
     * a fully specified directory part, followed by a wildcard
     * after that.
     */
    wildcard = stripslashes(name, false);

    unwcdir = dupstr(name);
    len = wildcard - name;
    unwcdir[len] = '\0';
    if (len > 0 && unwcdir[len-1] == '/')
        unwcdir[len-1] = '\0';
    tmpdir = snewn(1 + len, char);
    check = wc_unescape(tmpdir, unwcdir);
    sfree(tmpdir);

    if (!check) {
        printf("Multiple-level wildcards are not supported\n");
        sfree(unwcdir);
        return NULL;
    }

    cdir = canonify(unwcdir);

    req = fxp_opendir_send(cdir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (dirh) {
        swcm = snew(SftpWildcardMatcher);
        swcm->dirh = dirh;
        swcm->names = NULL;
        swcm->wildcard = dupstr(wildcard);
        swcm->prefix = unwcdir;
    } else {
        printf("Unable to open %s: %s\n", cdir, fxp_error());
        swcm = NULL;
        sfree(unwcdir);
    }

    sfree(cdir);

    return swcm;
}

char *sftp_wildcard_get_filename(SftpWildcardMatcher *swcm)
{
    struct fxp_name *name;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    while (1) {
        if (swcm->names && swcm->namepos >= swcm->names->nnames) {
            fxp_free_names(swcm->names);
            swcm->names = NULL;
        }

        if (!swcm->names) {
            req = fxp_readdir_send(swcm->dirh);
            pktin = sftp_wait_for_reply(req);
            swcm->names = fxp_readdir_recv(pktin, req);

            if (!swcm->names) {
                if (fxp_error_type() != SSH_FX_EOF) {
                    with_stripctrl(san, swcm->prefix)
                        printf("%s: reading directory: %s\n",
                               san, fxp_error());
                }
                return NULL;
            } else if (swcm->names->nnames == 0) {
                /*
                 * Another failure mode which we treat as EOF is if
                 * the server reports success from FXP_READDIR but
                 * returns no actual names. This is unusual, since
                 * from most servers you'd expect at least "." and
                 * "..", but there's nothing forbidding a server from
                 * omitting those if it wants to.
                 */
                return NULL;
            }

            swcm->namepos = 0;
        }

        assert(swcm->names && swcm->namepos < swcm->names->nnames);

        name = &swcm->names->names[swcm->namepos++];

        if (!strcmp(name->filename, ".") || !strcmp(name->filename, ".."))
            continue;                  /* expected bad filenames */

        if (!vet_filename(name->filename)) {
            with_stripctrl(san, name->filename)
                printf("ignoring potentially dangerous server-"
                       "supplied filename '%s'\n", san);
            continue;                  /* unexpected bad filename */
        }

        if (!wc_match(swcm->wildcard, name->filename))
            continue;                  /* doesn't match the wildcard */

        /*
         * We have a working filename. Return it.
         */
        return dupprintf("%s%s%s", swcm->prefix,
                         (!swcm->prefix[0] ||
                          swcm->prefix[strlen(swcm->prefix)-1]=='/' ?
                          "" : "/"),
                         name->filename);
    }
}

void sftp_finish_wildcard_matching(SftpWildcardMatcher *swcm)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    req = fxp_close_send(swcm->dirh);
    pktin = sftp_wait_for_reply(req);
    fxp_close_recv(pktin, req);

    if (swcm->names)
        fxp_free_names(swcm->names);

    sfree(swcm->prefix);
    sfree(swcm->wildcard);

    sfree(swcm);
}

/*
 * General function to match a potential wildcard in a filename
 * argument and iterate over every matching file. Used in several
 * PSFTP commands (rmdir, rm, chmod, mv).
 */
bool wildcard_iterate(char *filename, bool (*func)(void *, char *), void *ctx)
{
    char *unwcfname, *newname, *cname;
    bool is_wc, toret;

    unwcfname = snewn(strlen(filename)+1, char);
    is_wc = !wc_unescape(unwcfname, filename);

    if (is_wc) {
        SftpWildcardMatcher *swcm = sftp_begin_wildcard_matching(filename);
        bool matched = false;
        sfree(unwcfname);

        if (!swcm)
            return false;

        toret = true;

        while ( (newname = sftp_wildcard_get_filename(swcm)) != NULL ) {
            cname = canonify(newname);
            sfree(newname);
            matched = true;
            if (!func(ctx, cname))
                toret = false;
            sfree(cname);
        }

        if (!matched) {
            /* Politely warn the user that nothing matched. */
            printf("%s: nothing matched\n", filename);
        }

        sftp_finish_wildcard_matching(swcm);
    } else {
        cname = canonify(unwcfname);
        toret = func(ctx, cname);
        sfree(cname);
        sfree(unwcfname);
    }

    return toret;
}

/*
 * Handy helper function.
 */
bool is_wildcard(char *name)
{
    char *unwcfname = snewn(strlen(name)+1, char);
    bool is_wc = !wc_unescape(unwcfname, name);
    sfree(unwcfname);
    return is_wc;
}

/* ----------------------------------------------------------------------
 * Actual sftp commands.
 */
struct sftp_command {
    char **words;
    size_t nwords, wordssize;
    int (*obey) (struct sftp_command *);        /* returns <0 to quit */
};

int sftp_cmd_null(struct sftp_command *cmd)
{
    return 1;                          /* success */
}

int sftp_cmd_unknown(struct sftp_command *cmd)
{
    printf("psftp: unknown command \"%s\"\n", cmd->words[0]);
    return 0;                          /* failure */
}

int sftp_cmd_quit(struct sftp_command *cmd)
{
    return -1;
}

int sftp_cmd_close(struct sftp_command *cmd)
{
    if (!backend) {
        not_connected();
        return 0;
    }

    if (backend_connected(backend)) {
        char ch;
        backend_special(backend, SS_EOF, 0);
        sent_eof = true;
        sftp_recvdata(&ch, 1);
    }
    do_sftp_cleanup();

    return 1;
}

#ifndef WINSFTP_BUILD
void list_directory_from_sftp_warn_unsorted(void)
{
    printf("Directory is too large to sort; writing file names unsorted\n");
}

void list_directory_from_sftp_print(struct fxp_name *name)
{
    with_stripctrl(san, name->longname)
        printf("%s\n", san);
}
#endif /* !WINSFTP_BUILD */

/*
 * List a directory. If no arguments are given, list pwd; otherwise
 * list the directory given in words[1].
 */
int sftp_cmd_ls(struct sftp_command *cmd)
{
    struct fxp_handle *dirh;
    struct fxp_names *names;
    const char *dir;
    char *cdir, *unwcdir, *wildcard;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2)
        dir = ".";
    else
        dir = cmd->words[1];

    unwcdir = snewn(1 + strlen(dir), char);
    if (wc_unescape(unwcdir, dir)) {
        dir = unwcdir;
        wildcard = NULL;
    } else {
        char *tmpdir;
        int len;
        bool check;

        sfree(unwcdir);
        wildcard = stripslashes(dir, false);
        unwcdir = dupstr(dir);
        len = wildcard - dir;
        unwcdir[len] = '\0';
        if (len > 0 && unwcdir[len-1] == '/')
            unwcdir[len-1] = '\0';
        tmpdir = snewn(1 + len, char);
        check = wc_unescape(tmpdir, unwcdir);
        sfree(tmpdir);
        if (!check) {
            printf("Multiple-level wildcards are not supported\n");
            sfree(unwcdir);
            return 0;
        }
        dir = unwcdir;
    }

    cdir = canonify(dir);

    with_stripctrl(san, cdir)
        printf("Listing directory %s\n", san);

    req = fxp_opendir_send(cdir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (dirh == NULL) {
        printf("Unable to open %s: %s\n", dir, fxp_error());
        sfree(cdir);
        sfree(unwcdir);
        return 0;
    } else {
        struct list_directory_from_sftp_ctx *ctx =
            list_directory_from_sftp_new();

        while (1) {

            req = fxp_readdir_send(dirh);
            pktin = sftp_wait_for_reply(req);
            names = fxp_readdir_recv(pktin, req);

            if (names == NULL) {
                if (fxp_error_type() == SSH_FX_EOF)
                    break;
                printf("Reading directory %s: %s\n", dir, fxp_error());
                break;
            }
            if (names->nnames == 0) {
                fxp_free_names(names);
                break;
            }

            for (size_t i = 0; i < names->nnames; i++)
                if (!wildcard || wc_match(wildcard, names->names[i].filename))
                    list_directory_from_sftp_feed(ctx, &names->names[i]);

            fxp_free_names(names);
        }

        req = fxp_close_send(dirh);
        pktin = sftp_wait_for_reply(req);
        fxp_close_recv(pktin, req);

        list_directory_from_sftp_finish(ctx);
        list_directory_from_sftp_free(ctx);
    }

    sfree(cdir);
    sfree(unwcdir);

    return 1;
}

/*
 * Change directories. We do this by canonifying the new name, then
 * trying to OPENDIR it. Only if that succeeds do we set the new pwd.
 */
int sftp_cmd_cd(struct sftp_command *cmd)
{
    struct fxp_handle *dirh;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    char *dir;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2)
        dir = dupstr(homedir);
    else {
        dir = canonify(cmd->words[1]);
    }

    req = fxp_opendir_send(dir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (!dirh) {
        with_stripctrl(san, dir)
            printf("Directory %s: %s\n", san, fxp_error());
        sfree(dir);
        return 0;
    }

    req = fxp_close_send(dirh);
    pktin = sftp_wait_for_reply(req);
    fxp_close_recv(pktin, req);

    sfree(pwd);
    pwd = dir;
    with_stripctrl(san, pwd)
        printf("Remote directory is now %s\n", san);

    return 1;
}

/*
 * Print current directory. Easy as pie.
 */
int sftp_cmd_pwd(struct sftp_command *cmd)
{
    if (!backend) {
        not_connected();
        return 0;
    }

    with_stripctrl(san, pwd)
        printf("Remote directory is %s\n", san);
    return 1;
}

/*
 * Get a file and save it at the local end. We have three very
 * similar commands here. The basic one is `get'; `reget' differs
 * in that it checks for the existence of the destination file and
 * starts from where a previous aborted transfer left off; `mget'
 * differs in that it interprets all its arguments as files to
 * transfer (never as a different local name for a remote file) and
 * can handle wildcards.
 */
int sftp_general_get(struct sftp_command *cmd, bool restart, bool multiple)
{
    char *fname, *unwcfname, *origfname, *origwfname, *outfname;
    int i, toret;
    bool recurse = false;

    if (!backend) {
        not_connected();
        return 0;
    }

    i = 1;
    while (i < cmd->nwords && cmd->words[i][0] == '-') {
        if (!strcmp(cmd->words[i], "--")) {
            /* finish processing options */
            i++;
            break;
        } else if (!strcmp(cmd->words[i], "-r")) {
            recurse = true;
        } else {
            printf("%s: unrecognised option '%s'\n", cmd->words[0], cmd->words[i]);
            return 0;
        }
        i++;
    }

    if (i >= cmd->nwords) {
        printf("%s: expects a filename\n", cmd->words[0]);
        return 0;
    }

    toret = 1;
    do {
        SftpWildcardMatcher *swcm;

        origfname = cmd->words[i++];
        unwcfname = snewn(strlen(origfname)+1, char);

        if (multiple && !wc_unescape(unwcfname, origfname)) {
            swcm = sftp_begin_wildcard_matching(origfname);
            if (!swcm) {
                sfree(unwcfname);
                continue;
            }
            origwfname = sftp_wildcard_get_filename(swcm);
            if (!origwfname) {
                /* Politely warn the user that nothing matched. */
                printf("%s: nothing matched\n", origfname);
                sftp_finish_wildcard_matching(swcm);
                sfree(unwcfname);
                continue;
            }
        } else {
            origwfname = origfname;
            swcm = NULL;
        }

        while (origwfname) {
            fname = canonify(origwfname);

            if (!multiple && i < cmd->nwords)
                outfname = cmd->words[i++];
            else
                outfname = stripslashes(origwfname, false);

            toret = sftp_get_file(fname, outfname, recurse, restart);

            sfree(fname);

            if (swcm) {
                sfree(origwfname);
                origwfname = sftp_wildcard_get_filename(swcm);
            } else {
                origwfname = NULL;
            }
        }
        sfree(unwcfname);
        if (swcm)
            sftp_finish_wildcard_matching(swcm);
        if (!toret)
            return toret;

    } while (multiple && i < cmd->nwords);

    return toret;
}
int sftp_cmd_get(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, false, false);
}
int sftp_cmd_mget(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, false, true);
}
int sftp_cmd_reget(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, true, false);
}

/*
 * Send a file and store it at the remote end. We have three very
 * similar commands here. The basic one is `put'; `reput' differs
 * in that it checks for the existence of the destination file and
 * starts from where a previous aborted transfer left off; `mput'
 * differs in that it interprets all its arguments as files to
 * transfer (never as a different remote name for a local file) and
 * can handle wildcards.
 */
int sftp_general_put(struct sftp_command *cmd, bool restart, bool multiple)
{
    char *fname, *wfname, *origoutfname, *outfname;
    int i;
    int toret;
    bool recurse = false;

    if (!backend) {
        not_connected();
        return 0;
    }

    i = 1;
    while (i < cmd->nwords && cmd->words[i][0] == '-') {
        if (!strcmp(cmd->words[i], "--")) {
            /* finish processing options */
            i++;
            break;
        } else if (!strcmp(cmd->words[i], "-r")) {
            recurse = true;
        } else {
            printf("%s: unrecognised option '%s'\n", cmd->words[0], cmd->words[i]);
            return 0;
        }
        i++;
    }

    if (i >= cmd->nwords) {
        printf("%s: expects a filename\n", cmd->words[0]);
        return 0;
    }

    toret = 1;
    do {
        WildcardMatcher *wcm;
        fname = cmd->words[i++];

        if (multiple && test_wildcard(fname, false) == WCTYPE_WILDCARD) {
            wcm = begin_wildcard_matching(fname);
            wfname = wildcard_get_filename(wcm);
            if (!wfname) {
                /* Politely warn the user that nothing matched. */
                printf("%s: nothing matched\n", fname);
                finish_wildcard_matching(wcm);
                continue;
            }
        } else {
            wfname = fname;
            wcm = NULL;
        }

        while (wfname) {
            if (!multiple && i < cmd->nwords)
                origoutfname = cmd->words[i++];
            else
                origoutfname = stripslashes(wfname, true);

            outfname = canonify(origoutfname);
            toret = sftp_put_file(wfname, outfname, recurse, restart);
            sfree(outfname);

            if (wcm) {
                sfree(wfname);
                wfname = wildcard_get_filename(wcm);
            } else {
                wfname = NULL;
            }
        }

        if (wcm)
            finish_wildcard_matching(wcm);

        if (!toret)
            return toret;

    } while (multiple && i < cmd->nwords);

    return toret;
}
int sftp_cmd_put(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, false, false);
}
int sftp_cmd_mput(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, false, true);
}
int sftp_cmd_reput(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, true, false);
}

int sftp_cmd_mkdir(struct sftp_command *cmd)
{
    char *dir;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("mkdir: expects a directory\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++) {
        dir = canonify(cmd->words[i]);

        req = fxp_mkdir_send(dir, NULL);
        pktin = sftp_wait_for_reply(req);
        result = fxp_mkdir_recv(pktin, req);

        if (!result) {
            with_stripctrl(san, dir)
                printf("mkdir %s: %s\n", san, fxp_error());
            ret = 0;
        } else
            with_stripctrl(san, dir)
                printf("mkdir %s: OK\n", san);

        sfree(dir);
    }

    return ret;
}

static bool sftp_action_rmdir(void *vctx, char *dir)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;

    req = fxp_rmdir_send(dir);
    pktin = sftp_wait_for_reply(req);
    result = fxp_rmdir_recv(pktin, req);

    if (!result) {
        printf("rmdir %s: %s\n", dir, fxp_error());
        return false;
    }

    printf("rmdir %s: OK\n", dir);

    return true;
}

int sftp_cmd_rmdir(struct sftp_command *cmd)
{
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("rmdir: expects a directory\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_rmdir, NULL);

    return ret;
}

static bool sftp_action_rm(void *vctx, char *fname)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;

    req = fxp_remove_send(fname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_remove_recv(pktin, req);

    if (!result) {
        printf("rm %s: %s\n", fname, fxp_error());
        return false;
    }

    printf("rm %s: OK\n", fname);

    return true;
}

int sftp_cmd_rm(struct sftp_command *cmd)
{
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("rm: expects a filename\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_rm, NULL);

    return ret;
}

static bool check_is_dir(char *dstfname)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_attrs attrs;
    bool result;

    req = fxp_stat_send(dstfname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_stat_recv(pktin, req, &attrs);

    if (result &&
        (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
        (attrs.permissions & 0040000))
        return true;
    else
        return false;
}

struct sftp_context_mv {
    char *dstfname;
    bool dest_is_dir;
};

static bool sftp_action_mv(void *vctx, char *srcfname)
{
    struct sftp_context_mv *ctx = (struct sftp_context_mv *)vctx;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    const char *error;
    char *finalfname, *newcanon = NULL;
    bool toret, result;

    if (ctx->dest_is_dir) {
        char *p;
        char *newname;

        p = srcfname + strlen(srcfname);
        while (p > srcfname && p[-1] != '/') p--;
        newname = dupcat(ctx->dstfname, "/", p);
        newcanon = canonify(newname);
        sfree(newname);

        finalfname = newcanon;
    } else {
        finalfname = ctx->dstfname;
    }

    req = fxp_rename_send(srcfname, finalfname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_rename_recv(pktin, req);

    error = result ? NULL : fxp_error();

    if (error) {
        with_stripctrl(san, finalfname)
            printf("mv %s %s: %s\n", srcfname, san, error);
        toret = false;
    } else {
        with_stripctrl(san, finalfname)
            printf("%s -> %s\n", srcfname, san);
        toret = true;
    }

    sfree(newcanon);
    return toret;
}

int sftp_cmd_mv(struct sftp_command *cmd)
{
    struct sftp_context_mv ctx[1];
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 3) {
        printf("mv: expects two filenames\n");
        return 0;
    }

    ctx->dstfname = canonify(cmd->words[cmd->nwords-1]);

    /*
     * If there's more than one source argument, or one source
     * argument which is a wildcard, we _require_ that the
     * destination is a directory.
     */
    ctx->dest_is_dir = check_is_dir(ctx->dstfname);
    if ((cmd->nwords > 3 || is_wildcard(cmd->words[1])) && !ctx->dest_is_dir) {
        printf("mv: multiple or wildcard arguments require the destination"
               " to be a directory\n");
        sfree(ctx->dstfname);
        return 0;
    }

    /*
     * Now iterate over the source arguments.
     */
    ret = 1;
    for (i = 1; i < cmd->nwords-1; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_mv, ctx);

    sfree(ctx->dstfname);
    return ret;
}

struct sftp_context_chmod {
    unsigned attrs_clr, attrs_xor;
};

static bool sftp_action_chmod(void *vctx, char *fname)
{
    struct fxp_attrs attrs;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;
    unsigned oldperms, newperms;
    struct sftp_context_chmod *ctx = (struct sftp_context_chmod *)vctx;

    req = fxp_stat_send(fname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_stat_recv(pktin, req, &attrs);

    if (!result || !(attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS)) {
        printf("get attrs for %s: %s\n", fname,
               result ? "file permissions not provided" : fxp_error());
        return false;
    }

    attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS;   /* perms _only_ */
    oldperms = attrs.permissions & 07777;
    attrs.permissions &= ~ctx->attrs_clr;
    attrs.permissions ^= ctx->attrs_xor;
    newperms = attrs.permissions & 07777;

    if (oldperms == newperms)
        return true;                   /* no need to do anything! */

    req = fxp_setstat_send(fname, attrs);
    pktin = sftp_wait_for_reply(req);
    result = fxp_setstat_recv(pktin, req);

    if (!result) {
        printf("set attrs for %s: %s\n", fname, fxp_error());
        return false;
    }

    printf("%s: %04o -> %04o\n", fname, oldperms, newperms);

    return true;
}

int sftp_cmd_chmod(struct sftp_command *cmd)
{
    char *mode;
    int i, ret;
    struct sftp_context_chmod ctx[1];

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 3) {
        printf("chmod: expects a mode specifier and a filename\n");
        return 0;
    }

    /*
     * Attempt to parse the mode specifier in cmd->words[1]. We
     * don't support the full horror of Unix chmod; instead we
     * support a much simpler syntax in which the user can either
     * specify an octal number, or a comma-separated sequence of
     * [ugoa]*[-+=][rwxst]+. (The initial [ugoa] sequence may
     * _only_ be omitted if the only attribute mentioned is t,
     * since all others require a user/group/other specification.
     * Additionally, the s attribute may not be specified for any
     * [ugoa] specifications other than exactly u or exactly g.
     */
    ctx->attrs_clr = ctx->attrs_xor = 0;
    mode = cmd->words[1];
    if (mode[0] >= '0' && mode[0] <= '9') {
        if (mode[strspn(mode, "01234567")]) {
            printf("chmod: numeric file modes should"
                   " contain digits 0-7 only\n");
            return 0;
        }
        ctx->attrs_clr = 07777;
        sscanf(mode, "%o", &ctx->attrs_xor);
        ctx->attrs_xor &= ctx->attrs_clr;
    } else {
        while (*mode) {
            char *modebegin = mode;
            unsigned subset, perms;
            int action;

            subset = 0;
            while (*mode && *mode != ',' &&
                   *mode != '+' && *mode != '-' && *mode != '=') {
                switch (*mode) {
                  case 'u': subset |= 04700; break; /* setuid, user perms */
                  case 'g': subset |= 02070; break; /* setgid, group perms */
                  case 'o': subset |= 00007; break; /* just other perms */
                  case 'a': subset |= 06777; break; /* all of the above */
                  default:
                    printf("chmod: file mode '%.*s' contains unrecognised"
                           " user/group/other specifier '%c'\n",
                           (int)strcspn(modebegin, ","), modebegin, *mode);
                    return 0;
                }
                mode++;
            }
            if (!*mode || *mode == ',') {
                printf("chmod: file mode '%.*s' is incomplete\n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            action = *mode++;
            if (!*mode || *mode == ',') {
                printf("chmod: file mode '%.*s' is incomplete\n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            perms = 0;
            while (*mode && *mode != ',') {
                switch (*mode) {
                  case 'r': perms |= 00444; break;
                  case 'w': perms |= 00222; break;
                  case 'x': perms |= 00111; break;
                  case 't': perms |= 01000; subset |= 01000; break;
                  case 's':
                    if ((subset & 06777) != 04700 &&
                        (subset & 06777) != 02070) {
                        printf("chmod: file mode '%.*s': set[ug]id bit should"
                               " be used with exactly one of u or g only\n",
                               (int)strcspn(modebegin, ","), modebegin);
                        return 0;
                    }
                    perms |= 06000;
                    break;
                  default:
                    printf("chmod: file mode '%.*s' contains unrecognised"
                           " permission specifier '%c'\n",
                           (int)strcspn(modebegin, ","), modebegin, *mode);
                    return 0;
                }
                mode++;
            }
            if (!(subset & 06777) && (perms &~ subset)) {
                printf("chmod: file mode '%.*s' contains no user/group/other"
                       " specifier and permissions other than 't' \n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            perms &= subset;
            switch (action) {
              case '+':
                ctx->attrs_clr |= perms;
                ctx->attrs_xor |= perms;
                break;
              case '-':
                ctx->attrs_clr |= perms;
                ctx->attrs_xor &= ~perms;
                break;
              case '=':
                ctx->attrs_clr |= subset;
                ctx->attrs_xor |= perms;
                break;
            }
            if (*mode) mode++;         /* eat comma */
        }
    }

    ret = 1;
    for (i = 2; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_chmod, ctx);

    return ret;
}

static int sftp_cmd_open(struct sftp_command *cmd)
{
    int portnumber;

    if (backend) {
        printf("psftp: already connected\n");
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("open: expects a host name\n");
        return 0;
    }

    if (cmd->nwords > 2) {
        portnumber = atoi(cmd->words[2]);
        if (portnumber == 0) {
            printf("open: invalid port number\n");
            return 0;
        }
    } else
        portnumber = 0;

    if (psftp_connect(cmd->words[1], NULL, portnumber)) {
        backend = NULL;                /* connection is already closed */
        return -1;                     /* this is fatal */
    }
    if (do_sftp_init()) {
        do_sftp_cleanup();             /* reset backend/pwd to NULL on init failure */
        return 0;
    }
    return 1;
}

static int sftp_cmd_lcd(struct sftp_command *cmd)
{
    char *currdir, *errmsg;

    if (cmd->nwords < 2) {
        printf("lcd: expects a local directory name\n");
        return 0;
    }

    errmsg = psftp_lcd(cmd->words[1]);
    if (errmsg) {
        printf("lcd: unable to change directory: %s\n", errmsg);
        sfree(errmsg);
        return 0;
    }

    currdir = psftp_getcwd();
    printf("New local directory is %s\n", currdir);
    sfree(currdir);

    return 1;
}

static int sftp_cmd_lpwd(struct sftp_command *cmd)
{
    char *currdir;

    currdir = psftp_getcwd();
    printf("Current local directory is %s\n", currdir);
    sfree(currdir);

    return 1;
}

static int sftp_cmd_pling(struct sftp_command *cmd)
{
    int exitcode;

    exitcode = system(cmd->words[1]);
    return (exitcode == 0);
}

#ifdef WINSFTP_BUILD
/*
 * Local filesystem commands — Windows/winsftp.exe only.
 * printf() is redirected to winsftp_printf() via -Dprintf=... so all
 * output goes to the GUI output area.
 */
static void win_local_perror(const char *op, const char *path)
{
    DWORD err = GetLastError();
    char buf[256];
    int len;
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL, err, 0, buf, (DWORD)sizeof(buf), NULL);
    len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n'))
        buf[--len] = '\0';
    printf("%s: %s: %s\n", op, path, buf);
}

/*
 * Enumerate all local filesystem entries matching a pattern (which may
 * contain the Win32 wildcards * and ?), calling action() for each one.
 * action() receives the full path and the caller's context; returning
 * false continues enumeration but causes the overall return to be false.
 * Returns true only if at least one entry was found and all actions
 * succeeded.
 */
typedef bool (*local_action_fn)(const char *path, const char *op, void *ctx);

static bool local_wildcard_iterate(const char *pattern, const char *op,
                                    local_action_fn action, void *ctx)
{
    HANDLE hFind;
    WIN32_FIND_DATA fd;
    char dir[MAX_PATH];
    char fullpath[MAX_PATH];
    bool found = false, ok = true;
    const char *p = pattern + strlen(pattern);

    /* Split off the directory prefix so we can reconstruct full paths */
    while (p > pattern && p[-1] != '\\' && p[-1] != '/')
        p--;
    if (p == pattern) {
        dir[0] = '\0';
    } else {
        int n = (int)(p - pattern);
        memcpy(dir, pattern, n);
        dir[n] = '\0';
    }

    hFind = FindFirstFile(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        win_local_perror(op, pattern);
        return false;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        found = true;
        if (dir[0])
            sprintf(fullpath, "%s%s", dir, fd.cFileName);
        else
            strcpy(fullpath, fd.cFileName);
        if (!action(fullpath, op, ctx))
            ok = false;
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);

    if (!found) {
        printf("%s: %s: no matching files\n", op, pattern);
        return false;
    }
    return ok;
}

/* Returns true if the string contains any Win32 wildcard characters */
static bool has_wildcards(const char *s)
{
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

static int sftp_cmd_ldir(struct sftp_command *cmd)
{
    char pattern[MAX_PATH];
    const char *arg;
    HANDLE hFind;
    WIN32_FIND_DATA fd;
    int nfiles = 0, ndirs = 0;
    size_t arglen;

    arg = (cmd->nwords >= 2) ? cmd->words[1] : ".";
    arglen = strlen(arg);
    if (arglen + 5 > MAX_PATH) {
        printf("ldir: path too long\n");
        return 0;
    }
    /* If the argument already contains wildcards use it as the search
     * pattern directly; otherwise treat it as a directory and list all.
     * Use *.*  (not bare *) so that Win32s network redirectors, which
     * thunk through DOS FindFirst, match files with extensions too. */
    if (has_wildcards(arg)) {
        strcpy(pattern, arg);
    } else if (arg[arglen-1] == '\\' || arg[arglen-1] == '/') {
        sprintf(pattern, "%s*.*", arg);
    } else {
        sprintf(pattern, "%s\\*.*", arg);
    }

    hFind = FindFirstFile(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        win_local_perror("ldir", arg);
        return 0;
    }
    do {
        SYSTEMTIME st;
        FILETIME local_ft;
        char date[32];
        char sizestr[24];
        bool is_dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local_ft);
        if (FileTimeToSystemTime(&local_ft, &st))
            sprintf(date, "%04d-%02d-%02d %02d:%02d",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        else
            strcpy(date, "?\??\?-?\?-?\? ?\?:?\?");
        if (is_dir) {
            strcpy(sizestr, "      <DIR>     ");
            ndirs++;
        } else {
            ULARGE_INTEGER sz;
            sz.LowPart  = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            sprintf(sizestr, "%16llu", (unsigned long long)sz.QuadPart);
            nfiles++;
        }
        printf("%s  %s  %s\n", date, sizestr, fd.cFileName);
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
    printf("     %d file(s), %d director%s\n",
           nfiles, ndirs, ndirs == 1 ? "y" : "ies");
    return 1;
}

static bool ldel_action(const char *path, const char *op, void *ctx)
{
    if (!DeleteFile(path)) { win_local_perror(op, path); return false; }
    printf("ldel %s: OK\n", path);
    return true;
}

static int sftp_cmd_ldel(struct sftp_command *cmd)
{
    size_t i;
    int ret = 1;
    if (cmd->nwords < 2) {
        printf("ldel: expects a filename or wildcard\n");
        return 0;
    }
    for (i = 1; i < cmd->nwords; i++)
        ret &= (int)local_wildcard_iterate(cmd->words[i], "ldel",
                                           ldel_action, NULL);
    return ret;
}

static int sftp_cmd_lmkdir(struct sftp_command *cmd)
{
    if (cmd->nwords < 2) {
        printf("lmkdir: expects a directory name\n");
        return 0;
    }
    if (!CreateDirectory(cmd->words[1], NULL)) {
        win_local_perror("lmkdir", cmd->words[1]);
        return 0;
    }
    printf("lmkdir %s: OK\n", cmd->words[1]);
    return 1;
}

static int sftp_cmd_lren(struct sftp_command *cmd)
{
    if (cmd->nwords < 3) {
        printf("lren: expects source and destination names\n");
        return 0;
    }
    if (!MoveFile(cmd->words[1], cmd->words[2])) {
        win_local_perror("lren", cmd->words[1]);
        return 0;
    }
    printf("lren %s -> %s: OK\n", cmd->words[1], cmd->words[2]);
    return 1;
}

static bool lrmdir_action(const char *path, const char *op, void *ctx)
{
    if (!RemoveDirectory(path)) { win_local_perror(op, path); return false; }
    printf("lrmdir %s: OK\n", path);
    return true;
}

static int sftp_cmd_lrmdir(struct sftp_command *cmd)
{
    size_t i;
    int ret = 1;
    if (cmd->nwords < 2) {
        printf("lrmdir: expects a directory name or wildcard\n");
        return 0;
    }
    for (i = 1; i < cmd->nwords; i++)
        ret &= (int)local_wildcard_iterate(cmd->words[i], "lrmdir",
                                           lrmdir_action, NULL);
    return ret;
}

/* ----------------------------------------------------------------
 * SCP transfer command
 * ---------------------------------------------------------------- */

/* Shell-quote a string with single quotes ('). Output is written into
 * out[0..outsz-1]; always NUL-terminated. */
static void scp_shell_quote(char *out, size_t outsz, const char *s)
{
    char *p = out, *end = out + outsz - 2;
    if (p < end) *p++ = '\'';
    for (; *s && p < end - 3; s++) {
        if (*s == '\'') {
            /* ' -> '\'' */
            if (p + 4 > end) break;
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
        } else {
            *p++ = *s;
        }
    }
    if (p < end + 1) *p++ = '\'';
    *p = '\0';
}

/* Shell-quote a path for SCP get, leaving * ? [ ] unquoted so the remote
 * shell expands them.  Non-wildcard segments are single-quoted.
 * Example: /path/to/*.txt  ->  '/path/to/'*.txt */
static void scp_shell_quote_glob(char *out, size_t outsz, const char *s)
{
    char *p = out, *end = out + outsz - 5; /* 5: room for '\'' + one char + NUL */
    bool in_quote = false;

    for (; *s && p < end; s++) {
        if (*s == '*' || *s == '?' || *s == '[' || *s == ']') {
            if (in_quote) { *p++ = '\''; in_quote = false; }
            *p++ = *s;
        } else if (*s == '\'') {
            if (in_quote) { *p++ = '\''; in_quote = false; }
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
            in_quote = true;
        } else {
            if (!in_quote) { *p++ = '\''; in_quote = true; }
            *p++ = *s;
        }
    }
    if (in_quote) *p++ = '\'';
    *p = '\0';
}

/* Read bytes until newline, storing line (without \n) into buf. */
static bool scp_readline(char *buf, int maxlen)
{
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        if (!sftp_recvdata(&c, 1)) return false;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return true;
}

/* Read one-byte server response (0=OK, 1=warning, 2=fatal).
 * On error reads and prints the message. Returns -1 on connection loss. */
static int scp_response(void)
{
    char c;
    char msg[512];
    int i;
    if (!sftp_recvdata(&c, 1)) return -1;
    if (c == 0) return 0;
    i = 0;
    while (i < (int)sizeof(msg) - 1) {
        char ch;
        if (!sftp_recvdata(&ch, 1)) break;
        if (ch == '\n') break;
        msg[i++] = ch;
    }
    msg[i] = '\0';
    printf("scp: remote error: %s\n", msg);
    return (int)(unsigned char)c;
}

/* Inner receive loop for one directory level.  Runs until 'E' (end-of-dir)
 * or connection close.  Called recursively for each subdirectory. */
static bool scp_recv_loop(const char *localdir, bool recursive)
{
    char nul = '\0';
    bool ok = true;

    for (;;) {
        char type, rest[512];
        unsigned long perms;
        uint64_t size;
        char fname[256];
        const char *outname;
        char outpath[512];
        WFile *wf;

        if (!sftp_recvdata(&type, 1)) break; /* connection closed = done */

        if (type == 'E') {
            scp_readline(rest, sizeof(rest)); /* consume trailing \n */
            backend_send(backend, &nul, 1); /* ACK end-of-dir */
            break;
        }
        if (type == '\x01' || type == '\x02') {
            scp_readline(rest, sizeof(rest));
            printf("scp: %s\n", rest);
            ok = (type == '\x01');
            break;
        }
        if (type == 'T') {
            /* Timestamp — ACK and ignore */
            scp_readline(rest, sizeof(rest));
            backend_send(backend, &nul, 1);
            continue;
        }
        if (type == 'D') {
            scp_readline(rest, sizeof(rest));
            if (!recursive) {
                printf("scp: server sent directory; use -r for recursive transfer\n");
                backend_send(backend, "\x01scp: directories not supported\n", 32);
                ok = false;
                break;
            }
            if (sscanf(rest, "%lo 0 %255s", &perms, fname) != 2) {
                printf("scp: malformed directory header\n");
                backend_send(backend, "\x01scp: protocol error\n", 21);
                ok = false;
                break;
            }
            {
                char *newdir = localdir ? dir_file_cat(localdir, fname)
                                        : dupstr(fname);
                int existing = file_type(newdir);
                if (existing == FILE_TYPE_NONEXISTENT) {
                    if (!create_directory(newdir)) {
                        printf("scp: cannot create directory %s\n", newdir);
                        backend_send(backend, "\x01scp: cannot create dir\n", 24);
                        sfree(newdir);
                        ok = false;
                        break;
                    }
                } else if (existing != FILE_TYPE_DIRECTORY) {
                    printf("scp: %s: exists and is not a directory\n", newdir);
                    backend_send(backend, "\x01scp: not a directory\n", 22);
                    sfree(newdir);
                    ok = false;
                    break;
                }
                backend_send(backend, &nul, 1); /* ACK */
                if (!scp_recv_loop(newdir, recursive))
                    ok = false;
                sfree(newdir);
            }
            if (!ok) break;
            continue;
        }
        if (type != 'C') {
            printf("scp: unexpected protocol byte 0x%02x\n",
                   (unsigned char)type);
            ok = false;
            break;
        }

        /* File: rest = "<perms> <size> <name>" */
        scp_readline(rest, sizeof(rest));
        if (sscanf(rest, "%lo %"PRIu64" %255s",
                   &perms, &size, fname) != 3) {
            printf("scp: malformed file header\n");
            backend_send(backend, "\x01scp: protocol error\n", 21);
            ok = false;
            break;
        }

        /* Decide where to write */
        if (localdir && localdir[0]) {
            if (file_type(localdir) == FILE_TYPE_DIRECTORY) {
                char *p = dir_file_cat(localdir, fname);
                strncpy(outpath, p, sizeof(outpath) - 1);
                outpath[sizeof(outpath) - 1] = '\0';
                sfree(p);
                outname = outpath;
            } else {
                outname = localdir;
            }
        } else {
            outname = fname;
        }

        /* ACK header */
        backend_send(backend, &nul, 1);

        /* Open output file */
        wf = open_new_file(outname, (long)perms);
        if (!wf) {
            printf("scp: cannot create %s\n", outname);
            backend_send(backend, "\x01scp: cannot create file\n", 25);
            printf("remote:%s:%s => local:%s  [FAILED]\n",
                   scp_log_host, fname, outname);
            ok = false;
            break;
        }

        /* Receive data */
        {
            uint64_t remaining = size;
            sftp_progress_init(fname, size);
            while (remaining > 0) {
                char buf[32768];
                size_t chunk = remaining > sizeof(buf) ?
                               sizeof(buf) : (size_t)remaining;
                if (!sftp_recvdata(buf, chunk)) { ok = false; break; }
                if (write_to_file(wf, buf, (int)chunk) < 0) {
                    printf("scp: write error for %s\n", outname);
                    ok = false;
                    break;
                }
                remaining -= chunk;
                sftp_progress_update(size - remaining);
            }
        }
        close_wfile(wf);
        if (!ok) {
            printf("remote:%s:%s => local:%s  [FAILED]\n",
                   scp_log_host, fname, outname);
            break;
        }

        /* Read server's trailing NUL, then ACK it */
        if (scp_response() != 0) {
            printf("remote:%s:%s => local:%s  [FAILED]\n",
                   scp_log_host, fname, outname);
            ok = false;
            break;
        }
        backend_send(backend, &nul, 1);
        printf("remote:%s:%s => local:%s  [OK]\n", scp_log_host, fname, outname);
    }
    return ok;
}

/* Entry point: send the initial ready byte then hand off to the loop. */
static bool scp_recv_files(const char *localpath, bool recursive)
{
    char nul = '\0';
    backend_send(backend, &nul, 1);
    return scp_recv_loop(localpath, recursive);
}

/* Upload one local file through the already-connected SCP sink session.
 * Handles the per-file handshake but NOT the initial server-ready read. */
static bool scp_send_one_file(const char *localpath)
{
    uint64_t size;
    RFile *rf;
    char nul = '\0';
    char hdr[512];
    const char *fname;
    bool ok = true;

    rf = open_existing_file(localpath, &size, NULL, NULL, NULL);
    if (!rf) {
        printf("scp: cannot open %s\n", localpath);
        return false;
    }

    fname = stripslashes(localpath, true);
    sprintf(hdr, "C0644 %"PRIu64" %s\n", size, fname);
    backend_send(backend, hdr, strlen(hdr));

    if (scp_response() != 0) { close_rfile(rf); return false; }

    {
        uint64_t sent = 0;
        sftp_progress_init(fname, size);
        while (sent < size) {
            char buf[32768];
            int chunk = (size - sent) > sizeof(buf) ?
                        (int)sizeof(buf) : (int)(size - sent);
            int got = read_from_file(rf, buf, chunk);
            if (got <= 0) { ok = false; break; }
            backend_send(backend, buf, got);
            sent += got;
            sftp_progress_update(sent);
        }
    }
    close_rfile(rf);
    if (!ok) {
        printf("local:%s => remote:%s:%s  [FAILED]\n",
               localpath, scp_log_host, scp_log_remotepath);
        return false;
    }

    backend_send(backend, &nul, 1); /* end-of-data */
    {
        bool result = (scp_response() == 0);
        printf("local:%s => remote:%s:%s  [%s]\n",
               localpath, scp_log_host, scp_log_remotepath,
               result ? "OK" : "FAILED");
        return result;
    }
}

/* Forward declaration */
static bool scp_send_dir(const char *localpath, bool recursive);

/* Dispatch: send a file or (if -r) a directory. */
static bool scp_send_one(const char *localpath, bool recursive)
{
    int ftype = file_type(localpath);
    if (ftype == FILE_TYPE_DIRECTORY) {
        if (!recursive) {
            printf("scp: %s: is a directory (use -r)\n", localpath);
            return false;
        }
        return scp_send_dir(localpath, recursive);
    }
    if (ftype == FILE_TYPE_FILE) {
        return scp_send_one_file(localpath);
    }
    printf("scp: %s: not a regular file\n", localpath);
    return false;
}

/* Send a directory recursively: D header, contents, E trailer. */
static bool scp_send_dir(const char *localpath, bool recursive)
{
    char hdr[512];
    char nul = '\0';
    const char *dname = stripslashes(localpath, true);
    const char *errmsg = NULL;
    DirHandle *dh;
    char *fname;
    bool ok = true;

    sprintf(hdr, "D0755 0 %s\n", dname);
    backend_send(backend, hdr, strlen(hdr));
    if (scp_response() != 0) return false;

    dh = open_directory(localpath, &errmsg);
    if (!dh) {
        printf("scp: cannot open directory %s%s%s\n",
               localpath, errmsg ? ": " : "", errmsg ? errmsg : "");
        backend_send(backend, "E\n", 2);
        return false;
    }

    while (ok && (fname = read_filename(dh)) != NULL) {
        char *childpath = dir_file_cat(localpath, fname);
        sfree(fname);
        ok = scp_send_one(childpath, recursive);
        sfree(childpath);
    }
    close_directory(dh);

    backend_send(backend, "E\n", 2);
    if (scp_response() != 0) ok = false;
    return ok;
}

/* Upload files from local argument list cmd->words[first..last] using
 * wildcard expansion. */
static bool scp_send_files(struct sftp_command *cmd, int first, int last,
                           bool recursive)
{
    int i;
    bool any = false;

    /* Initial server ready */
    if (scp_response() != 0) return false;

    for (i = first; i <= last; i++) {
        const char *path = cmd->words[i];
        int wtype = test_wildcard(path, true);
        if (wtype == WCTYPE_NONEXISTENT) {
            printf("scp: %s: not found\n", path);
            continue;
        }
        if (wtype == WCTYPE_FILENAME) {
            if (scp_send_one(path, recursive)) any = true;
        } else {
            WildcardMatcher *wm = begin_wildcard_matching(path);
            char *match;
            while ((match = wildcard_get_filename(wm)) != NULL) {
                if (scp_send_one(match, recursive)) any = true;
                sfree(match);
            }
            finish_wildcard_matching(wm);
        }
    }
    return any;
}

static int sftp_cmd_scp(struct sftp_command *cmd)
{
    /*
     * scp [-r] get [-P port] [user@]host:remotepath [localpath]
     * scp [-r] put [-P port] localpath... [user@]host:remotepath
     *
     * Only available when not connected (like 'open').
     */
    bool upload, recursive = false;
    int i, port = 0;
    char *hostarg, *hostpart, *userpart, *pathpart, *colon, *at;
    char userhost[512], quoted[1024], remote_cmd[1100];
    bool ok;

    if (backend) {
        printf("scp: already connected; use 'close' first\n");
        return 0;
    }

    if (cmd->nwords < 3) {
        printf("scp: usage: scp [-r] get [-P port] [user@]host:path [local]\n"
               "           scp [-r] put [-P port] local... [user@]host:path\n");
        return 0;
    }

    /* Parse flags before verb: any combination of -P port and -r */
    i = 1;
    while (i < cmd->nwords) {
        if (strcmp(cmd->words[i], "-P") == 0) {
            if (i + 1 >= cmd->nwords) {
                printf("scp: -P requires a port number\n");
                return 0;
            }
            if (!port) port = atoi(cmd->words[i+1]);
            i += 2;
        } else if (strcmp(cmd->words[i], "-r") == 0 ||
                   strcmp(cmd->words[i], "-R") == 0) {
            recursive = true;
            i++;
        } else {
            break;
        }
    }

    if (i >= cmd->nwords) {
        printf("scp: usage: scp [-r] get [-P port] [user@]host:path [local]\n"
               "           scp [-r] put [-P port] local... [user@]host:path\n");
        return 0;
    }

    if (strcmp(cmd->words[i], "get") == 0)
        upload = false;
    else if (strcmp(cmd->words[i], "put") == 0)
        upload = true;
    else {
        printf("scp: expected 'get' or 'put'\n");
        return 0;
    }
    i++;

    /* Parse flags after verb: same -P / -r flags */
    while (i < cmd->nwords) {
        if (strcmp(cmd->words[i], "-P") == 0) {
            if (i + 1 >= cmd->nwords) {
                printf("scp: -P requires a port number\n");
                return 0;
            }
            if (!port) port = atoi(cmd->words[i+1]);
            i += 2;
        } else if (strcmp(cmd->words[i], "-r") == 0 ||
                   strcmp(cmd->words[i], "-R") == 0) {
            recursive = true;
            i++;
        } else {
            break;
        }
    }

    if (i >= cmd->nwords) {
        printf("scp: missing arguments\n");
        return 0;
    }

    /* Identify the [user@]host:path argument */
    if (upload) {
        /* last argument */
        if (cmd->nwords - 1 < i) {
            printf("scp: missing remote destination\n");
            return 0;
        }
        hostarg = dupstr(cmd->words[cmd->nwords - 1]);
    } else {
        hostarg = dupstr(cmd->words[i++]);
    }

    /* Split on first ':' to get host vs path */
    colon = strchr(hostarg, ':');
    if (!colon) {
        printf("scp: remote argument must be [user@]host:path\n");
        sfree(hostarg);
        return 0;
    }
    *colon = '\0';
    pathpart = colon + 1;

    /* Split user@host */
    at = strchr(hostarg, '@');
    if (at) {
        *at = '\0';
        userpart = hostarg;
        hostpart = at + 1;
    } else {
        userpart = NULL;
        hostpart = hostarg;
    }

    /* Build userhost string for psftp_connect */
    if (userpart)
        sprintf(userhost, "%s@%s", userpart, hostpart);
    else
        strncpy(userhost, hostpart, sizeof(userhost) - 1);

    /* Save display hostname before freeing hostarg (hostpart points into it) */
    {
        char saved_host[256];
        strncpy(saved_host, hostpart, sizeof(saved_host) - 1);
        saved_host[sizeof(saved_host) - 1] = '\0';

        /* Save for per-file log lines (pathpart also points into hostarg) */
        strncpy(scp_log_host, hostpart, sizeof(scp_log_host) - 1);
        scp_log_host[sizeof(scp_log_host) - 1] = '\0';
        strncpy(scp_log_remotepath, pathpart, sizeof(scp_log_remotepath) - 1);
        scp_log_remotepath[sizeof(scp_log_remotepath) - 1] = '\0';

        /* Build the remote command.  For get, use glob-aware quoting so that
         * wildcards like *.txt are expanded by the remote shell.
         * Fall back to "." when pathpart is empty (e.g. "host:") so the
         * remote scp gets a valid target instead of a quoted empty string. */
        {
            const char *ep = pathpart[0] ? pathpart : ".";
            if (upload)
                scp_shell_quote(quoted, sizeof(quoted), ep);
            else
                scp_shell_quote_glob(quoted, sizeof(quoted), ep);
        }
        sprintf(remote_cmd, "scp%s -%c %s",
                recursive ? " -r" : "", upload ? 't' : 'f', quoted);

        sfree(hostarg);

        /* Point psftp_connect at the SCP exec command */
        strncpy(scp_remote_cmd, remote_cmd, sizeof(scp_remote_cmd) - 1);
        scp_remote_cmd[sizeof(scp_remote_cmd) - 1] = '\0';

        printf("Connecting to %s for SCP...\n", saved_host);
    }
    if (psftp_connect(userhost, NULL, port)) {
        scp_remote_cmd[0] = '\0';
        backend = NULL;
        return 0;
    }
    scp_remote_cmd[0] = '\0';
    /* SCP server will close the channel when done; that's expected, not an error */
    sent_eof = true;

    /* Perform the transfer */
    if (upload)
        ok = scp_send_files(cmd, i, (int)cmd->nwords - 2, recursive);
    else
        ok = scp_recv_files(i < cmd->nwords ? cmd->words[i] : NULL, recursive);

    /* Close cleanly */
    do_sftp_cleanup();

    if (ok)
        printf("scp: transfer complete.\n");
    return ok ? 1 : 0;
}
#endif /* WINSFTP_BUILD */

static int sftp_cmd_help(struct sftp_command *cmd);

static struct sftp_cmd_lookup {
    const char *name;
    /*
     * For help purposes, there are two kinds of command:
     *
     *  - primary commands, in which `longhelp' is non-NULL. In
     *    this case `shorthelp' is descriptive text, and `longhelp'
     *    is longer descriptive text intended to be printed after
     *    the command name.
     *
     *  - alias commands, in which `longhelp' is NULL. In this case
     *    `shorthelp' is the name of a primary command, which
     *    contains the help that should double up for this command.
     */
    bool listed;                /* do we list this in primary help? */
    const char *shorthelp;
    const char *longhelp;
    int (*obey) (struct sftp_command *);
} sftp_lookup[] = {
    /*
     * List of sftp commands. This is binary-searched so it MUST be
     * in ASCII order.
     */
#ifndef WINSFTP_BUILD
    {
        "!", true, "run a local command",
            "<command>\n"
            /* FIXME: this example is crap for non-Windows. */
            "  Runs a local command. For example, \"!del myfile\".\n",
            sftp_cmd_pling
    },
#endif
    {
        "bye", true, "finish your SFTP session",
            "\n"
            "  Terminates your SFTP session and quits the PSFTP program.\n",
            sftp_cmd_quit
    },
    {
        "cd", true, "change your remote working directory",
            " [ <new working directory> ]\n"
            "  Change the remote working directory for your SFTP session.\n"
            "  If a new working directory is not supplied, you will be\n"
            "  returned to your home directory.\n",
            sftp_cmd_cd
    },
    {
        "chmod", true, "change file permissions and modes",
            " <modes> <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Change the file permissions on one or more remote files or\n"
            "  directories.\n"
            "  <modes> can be any octal Unix permission specifier.\n"
            "  Alternatively, <modes> can include the following modifiers:\n"
            "    u+r     make file readable by owning user\n"
            "    u+w     make file writable by owning user\n"
            "    u+x     make file executable by owning user\n"
            "    u-r     make file not readable by owning user\n"
            "    [also u-w, u-x]\n"
            "    g+r     make file readable by members of owning group\n"
            "    [also g+w, g+x, g-r, g-w, g-x]\n"
            "    o+r     make file readable by all other users\n"
            "    [also o+w, o+x, o-r, o-w, o-x]\n"
            "    a+r     make file readable by absolutely everybody\n"
            "    [also a+w, a+x, a-r, a-w, a-x]\n"
            "    u+s     enable the Unix set-user-ID bit\n"
            "    u-s     disable the Unix set-user-ID bit\n"
            "    g+s     enable the Unix set-group-ID bit\n"
            "    g-s     disable the Unix set-group-ID bit\n"
            "    +t      enable the Unix \"sticky bit\"\n"
            "  You can give more than one modifier for the same user (\"g-rwx\"), and\n"
            "  more than one user for the same modifier (\"ug+w\"). You can\n"
            "  use commas to separate different modifiers (\"u+rwx,g+s\").\n",
            sftp_cmd_chmod
    },
    {
        "close", true, "finish your SFTP session but do not quit PSFTP",
            "\n"
            "  Terminates your SFTP session, but does not quit the PSFTP\n"
            "  program. You can then use \"open\" to start another SFTP\n"
            "  session, to the same server or to a different one.\n",
            sftp_cmd_close
    },
    {
        "del", true, "delete files on the remote server",
            " <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Delete a file or files from the server.\n",
            sftp_cmd_rm
    },
    {
        "delete", false, "del", NULL, sftp_cmd_rm
    },
    {
        "dir", true, "list remote files",
            " [ <directory-name> ]/[ <wildcard> ]\n"
            "  List the contents of a specified directory on the server.\n"
            "  If <directory-name> is not given, the current working directory\n"
            "  is assumed.\n"
            "  If <wildcard> is given, it is treated as a set of files to\n"
            "  list; otherwise, all files are listed.\n",
            sftp_cmd_ls
    },
    {
        "exit", true, "bye", NULL, sftp_cmd_quit
    },
    {
        "get", true, "download a file from the server to your local machine",
            " [ -r ] [ -- ] <filename> [ <local-filename> ]\n"
            "  Downloads a file on the server and stores it locally under\n"
            "  the same name, or under a different one if you supply the\n"
            "  argument <local-filename>.\n"
            "  If -r specified, recursively fetch a directory.\n",
            sftp_cmd_get
    },
    {
        "help", true, "give help",
            " [ <command> [ <command> ... ] ]\n"
            "  Give general help if no commands are specified.\n"
            "  If one or more commands are specified, give specific help on\n"
            "  those particular commands.\n",
            sftp_cmd_help
    },
    {
        "lcd", true, "change local working directory",
            " <local-directory-name>\n"
            "  Change the local working directory of the PSFTP program (the\n"
            "  default location where the \"get\" command will save files).\n",
            sftp_cmd_lcd
    },
#ifdef WINSFTP_BUILD
    {
        "ldel", true, "delete a local file",
            " <filename>\n"
            "  Delete a file from the local filesystem.\n",
            sftp_cmd_ldel
    },
    {
        "ldir", true, "list local directory",
            " [ <directory> ]\n"
            "  List the contents of a local directory.\n"
            "  If no directory is given, the current local directory is listed.\n",
            sftp_cmd_ldir
    },
    {
        "lmkdir", true, "create a local directory",
            " <directory>\n"
            "  Create a directory on the local filesystem.\n",
            sftp_cmd_lmkdir
    },
#endif
    {
        "lpwd", true, "print local working directory",
            "\n"
            "  Print the local working directory of the PSFTP program (the\n"
            "  default location where the \"get\" command will save files).\n",
            sftp_cmd_lpwd
    },
#ifdef WINSFTP_BUILD
    {
        "lren", true, "rename a local file",
            " <oldname> <newname>\n"
            "  Rename or move a file on the local filesystem.\n",
            sftp_cmd_lren
    },
    {
        "lrmdir", true, "remove a local directory",
            " <directory>\n"
            "  Remove an empty directory from the local filesystem.\n",
            sftp_cmd_lrmdir
    },
#endif
    {
        "ls", true, "dir", NULL,
            sftp_cmd_ls
    },
    {
        "mget", true, "download multiple files at once",
            " [ -r ] [ -- ] <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Downloads many files from the server, storing each one under\n"
            "  the same name it has on the server side. You can use wildcards\n"
            "  such as \"*.c\" to specify lots of files at once.\n"
            "  If -r specified, recursively fetch files and directories.\n",
            sftp_cmd_mget
    },
    {
        "mkdir", true, "create directories on the remote server",
            " <directory-name> [ <directory-name>... ]\n"
            "  Creates directories with the given names on the server.\n",
            sftp_cmd_mkdir
    },
    {
        "mput", true, "upload multiple files at once",
            " [ -r ] [ -- ] <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Uploads many files to the server, storing each one under the\n"
            "  same name it has on the client side. You can use wildcards\n"
            "  such as \"*.c\" to specify lots of files at once.\n"
            "  If -r specified, recursively store files and directories.\n",
            sftp_cmd_mput
    },
    {
        "mv", true, "move or rename file(s) on the remote server",
            " <source> [ <source>... ] <destination>\n"
            "  Moves or renames <source>(s) on the server to <destination>,\n"
            "  also on the server.\n"
            "  If <destination> specifies an existing directory, then <source>\n"
            "  may be a wildcard, and multiple <source>s may be given; all\n"
            "  source files are moved into <destination>.\n"
            "  Otherwise, <source> must specify a single file, which is moved\n"
            "  or renamed so that it is accessible under the name <destination>.\n",
            sftp_cmd_mv
    },
    {
        "open", true, "connect to a host",
            " [<user>@]<hostname> [<port>]\n"
            "  Establishes an SFTP connection to a given host. Only usable\n"
            "  when you are not already connected to a server.\n",
            sftp_cmd_open
    },
    {
        "put", true, "upload a file from your local machine to the server",
            " [ -r ] [ -- ] <filename> [ <remote-filename> ]\n"
            "  Uploads a file to the server and stores it there under\n"
            "  the same name, or under a different one if you supply the\n"
            "  argument <remote-filename>.\n"
            "  If -r specified, recursively store a directory.\n",
            sftp_cmd_put
    },
    {
        "pwd", true, "print your remote working directory",
            "\n"
            "  Print the current remote working directory for your SFTP session.\n",
            sftp_cmd_pwd
    },
    {
        "quit", true, "bye", NULL,
            sftp_cmd_quit
    },
    {
        "reget", true, "continue downloading files",
            " [ -r ] [ -- ] <filename> [ <local-filename> ]\n"
            "  Works exactly like the \"get\" command, but the local file\n"
            "  must already exist. The download will begin at the end of the\n"
            "  file. This is for resuming a download that was interrupted.\n"
            "  If -r specified, resume interrupted \"get -r\".\n",
            sftp_cmd_reget
    },
    {
        "ren", true, "mv", NULL,
            sftp_cmd_mv
    },
    {
        "rename", false, "mv", NULL,
            sftp_cmd_mv
    },
    {
        "reput", true, "continue uploading files",
            " [ -r ] [ -- ] <filename> [ <remote-filename> ]\n"
            "  Works exactly like the \"put\" command, but the remote file\n"
            "  must already exist. The upload will begin at the end of the\n"
            "  file. This is for resuming an upload that was interrupted.\n"
            "  If -r specified, resume interrupted \"put -r\".\n",
            sftp_cmd_reput
    },
    {
        "rm", true, "del", NULL,
            sftp_cmd_rm
    },
    {
        "rmdir", true, "remove directories on the remote server",
            " <directory-name> [ <directory-name>... ]\n"
            "  Removes the directory with the given name on the server.\n"
            "  The directory will not be removed unless it is empty.\n"
            "  Wildcards may be used to specify multiple directories.\n",
            sftp_cmd_rmdir
    },
#ifdef WINSFTP_BUILD
    {
        "scp", true, "transfer files using SCP (only when not connected)",
            " get [-r] [-P port] [user@]host:remotepath [localpath]\n"
            " put [-r] [-P port] local... [user@]host:remotepath\n"
            "  Opens an SCP connection to the given host, transfers the\n"
            "  specified files, then closes the connection.\n"
            "  'scp get' downloads; 'scp put' uploads (wildcards accepted).\n"
            "  Use -r to transfer directories recursively.\n"
            "  Use this command when the server supports SCP but not SFTP.\n",
            sftp_cmd_scp
    },
#endif
};

const struct sftp_cmd_lookup *lookup_command(const char *name)
{
    int i, j, k, cmp;

    i = -1;
    j = lenof(sftp_lookup);
    while (j - i > 1) {
        k = (j + i) / 2;
        cmp = strcmp(name, sftp_lookup[k].name);
        if (cmp < 0)
            j = k;
        else if (cmp > 0)
            i = k;
        else {
            return &sftp_lookup[k];
        }
    }
    return NULL;
}

static int sftp_cmd_help(struct sftp_command *cmd)
{
    int i;
    if (cmd->nwords == 1) {
        /*
         * Give short help on each command.
         */
        int maxlen;
        maxlen = 0;
        for (i = 0; i < lenof(sftp_lookup); i++) {
            int len;
            if (!sftp_lookup[i].listed)
                continue;
            len = strlen(sftp_lookup[i].name);
            if (maxlen < len)
                maxlen = len;
        }
        for (i = 0; i < lenof(sftp_lookup); i++) {
            const struct sftp_cmd_lookup *lookup;
            if (!sftp_lookup[i].listed)
                continue;
            lookup = &sftp_lookup[i];
            printf("%-*s", maxlen+2, lookup->name);
            if (lookup->longhelp == NULL)
                lookup = lookup_command(lookup->shorthelp);
            printf("%s\n", lookup->shorthelp);
        }
    } else {
        /*
         * Give long help on specific commands.
         */
        for (i = 1; i < cmd->nwords; i++) {
            const struct sftp_cmd_lookup *lookup;
            lookup = lookup_command(cmd->words[i]);
            if (!lookup) {
                printf("help: %s: command not found\n", cmd->words[i]);
            } else {
                printf("%s", lookup->name);
                if (lookup->longhelp == NULL)
                    lookup = lookup_command(lookup->shorthelp);
                printf("%s", lookup->longhelp);
            }
        }
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * Command line reading and parsing.
 */
struct sftp_command *sftp_getcmd(FILE *fp, int mode, int modeflags)
{
    char *line;
    struct sftp_command *cmd;
    char *p, *q, *r;
    bool quoting;

    cmd = snew(struct sftp_command);
    cmd->words = NULL;
    cmd->nwords = 0;
    cmd->wordssize = 0;

    line = NULL;

    if (fp) {
        if (modeflags & 1)
            printf("psftp> ");
        line = fgetline(fp);
    } else {
        line = ssh_sftp_get_cmdline("psftp> ", !backend);
    }

    if (!line || !*line) {
        cmd->obey = sftp_cmd_quit;
        if ((mode == 0) || (modeflags & 1))
            printf("quit\n");
        sfree(line);
        return cmd;                    /* eof */
    }

    line[strcspn(line, "\r\n")] = '\0';

    if (modeflags & 1) {
        printf("%s\n", line);
    }

    p = line;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;

    if (*p == '!') {
        /*
         * Special case: the ! command. This is always parsed as
         * exactly two words: one containing the !, and the second
         * containing everything else on the line.
         */
        cmd->nwords = 2;
        sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
        cmd->words[0] = dupstr("!");
        cmd->words[1] = dupstr(p+1);
    } else if (*p == '#') {
        /*
         * Special case: comment. Entire line is ignored.
         */
        cmd->nwords = cmd->wordssize = 0;
    } else {

        /*
         * Parse the command line into words. The syntax is:
         *  - double quotes are removed, but cause spaces within to be
         *    treated as non-separating.
         *  - a double-doublequote pair is a literal double quote, inside
         *    _or_ outside quotes. Like this:
         *
         *      firstword "second word" "this has ""quotes"" in" and""this""
         *
         * becomes
         *
         *      >firstword<
         *      >second word<
         *      >this has "quotes" in<
         *      >and"this"<
         */
        while (1) {
            /* skip whitespace */
            while (*p && (*p == ' ' || *p == '\t'))
                p++;
            /* terminate loop */
            if (!*p)
                break;
            /* mark start of word */
            q = r = p;                 /* q sits at start, r writes word */
            quoting = false;
            while (*p) {
                if (!quoting && (*p == ' ' || *p == '\t'))
                    break;                     /* reached end of word */
                else if (*p == '"' && p[1] == '"')
                    p += 2, *r++ = '"';    /* a literal quote */
                else if (*p == '"')
                    p++, quoting = !quoting;
                else
                    *r++ = *p++;
            }
            if (*p)
                p++;                   /* skip over the whitespace */
            *r = '\0';
            sgrowarray(cmd->words, cmd->wordssize, cmd->nwords);
            cmd->words[cmd->nwords++] = dupstr(q);
        }
    }

    sfree(line);

    /*
     * Now parse the first word and assign a function.
     */

    if (cmd->nwords == 0)
        cmd->obey = sftp_cmd_null;
    else {
        const struct sftp_cmd_lookup *lookup;
        lookup = lookup_command(cmd->words[0]);
        if (!lookup)
            cmd->obey = sftp_cmd_unknown;
        else
            cmd->obey = lookup->obey;
    }

    return cmd;
}

static void sftp_cmd_free(struct sftp_command *cmd)
{
    if (cmd->words) {
        for (size_t i = 0; i < cmd->nwords; i++)
            sfree(cmd->words[i]);
        sfree(cmd->words);
    }
    sfree(cmd);
}

static int do_sftp_init(void)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    /*
     * Do protocol initialisation.
     */
    if (!fxp_init()) {
        fprintf(stderr,
                "Fatal: unable to initialise SFTP: %s\n", fxp_error());
        return 1;                      /* failure */
    }

    /*
     * Find out where our home directory is.
     */
    req = fxp_realpath_send(".");
    pktin = sftp_wait_for_reply(req);
    homedir = fxp_realpath_recv(pktin, req);

    if (!homedir) {
        fprintf(stderr,
                "Warning: failed to resolve home directory: %s\n",
                fxp_error());
        homedir = dupstr(".");
    } else {
        with_stripctrl(san, homedir)
            printf("Remote working directory is %s\n", san);
    }
    pwd = dupstr(homedir);
    return 0;
}

static void do_sftp_cleanup(void)
{
    char ch;
    if (backend) {
        backend_special(backend, SS_EOF, 0);
        sent_eof = true;
        sftp_recvdata(&ch, 1);
        backend_free(backend);
        sftp_cleanup_request();
        backend = NULL;
    }
    /* Discard any leftover bytes so they don't corrupt the next session. */
    bufchain_clear(&received_data);
    if (pwd) {
        sfree(pwd);
        pwd = NULL;
    }
    if (homedir) {
        sfree(homedir);
        homedir = NULL;
    }
    if (psftp_logctx) {
        log_free(psftp_logctx);
        psftp_logctx = NULL;
    }
}

int do_sftp(int mode, int modeflags, Filename *batchfile)
{
    FILE *fp;
    int ret;

    /*
     * Batch mode?
     */
    if (mode == 0) {

        /* ------------------------------------------------------------------
         * Now we're ready to do Real Stuff.
         */
        while (1) {
            struct sftp_command *cmd;
            cmd = sftp_getcmd(NULL, 0, 0);
            if (!cmd)
                break;
            ret = cmd->obey(cmd);
            sftp_cmd_free(cmd);
            if (ret < 0)
                break;
        }
    } else {
        fp = f_open(batchfile, "r", false);
        if (!fp) {
            printf("Fatal: unable to open %s\n", filename_to_str(batchfile));
            return 1;
        }
        ret = 0;
        while (1) {
            struct sftp_command *cmd;
            cmd = sftp_getcmd(fp, mode, modeflags);
            if (!cmd)
                break;
            ret = cmd->obey(cmd);
            sftp_cmd_free(cmd);
            if (ret < 0)
                break;
            if (ret == 0) {
                if (!(modeflags & 2))
                    break;
            }
        }
        fclose(fp);
        /*
         * In batch mode, and if exit on command failure is enabled,
         * any command failure causes the whole of PSFTP to fail.
         */
        if (ret == 0 && !(modeflags & 2)) return 2;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * Dirty bits: integration with PuTTY.
 */

static bool verbose = false;

void ldisc_echoedit_update(Ldisc *ldisc) { }
void ldisc_check_sendok(Ldisc *ldisc) { }

/*
 * Receive a block of data from the SSH link. Block until all data
 * is available.
 *
 * To do this, we repeatedly call the SSH protocol module, with our
 * own psftp_output() function to catch the data that comes back. We
 * do this until we have enough data.
 */
static BinarySink *stderr_bs;
static size_t psftp_output(
    Seat *seat, SeatOutputType type, const void *data, size_t len)
{
    /*
     * Non-stdout data (both stderr and SSH auth banners) is just
     * spouted to local stderr (optionally via a sanitiser) and
     * otherwise ignored.
     */
    if (type != SEAT_OUTPUT_STDOUT) {
        put_data(stderr_bs, data, len);
        return 0;
    }

    bufchain_add(&received_data, data, len);
    return 0;
}

static bool psftp_eof(Seat *seat)
{
    /*
     * We expect to be the party deciding when to close the
     * connection, so if we see EOF before we sent it ourselves, we
     * should panic.
     */
    if (!sent_eof) {
        seat_connection_fatal(
            psftp_seat, "Received unexpected end-of-file from SFTP server");
    }
    return false;
}

bool sftp_recvdata(char *buf, size_t len)
{
    while (len > 0) {
        while (bufchain_size(&received_data) == 0) {
            if (backend_exitcode(backend) >= 0 ||
                ssh_sftp_loop_iteration() < 0)
                return false;          /* doom */
        }

        size_t got = bufchain_fetch_consume_up_to(&received_data, buf, len);
        buf += got;
        len -= got;
    }

    return true;
}
bool sftp_senddata(const char *buf, size_t len)
{
    backend_send(backend, buf, len);
    return true;
}
size_t sftp_sendbuffer(void)
{
    return backend_sendbuffer(backend);
}

/*
 *  Short description of parameters.
 */
static void usage(void)
{
    printf("PuTTY Secure File Transfer (SFTP) client\n");
    printf("%s\n", ver);
    printf("Usage: psftp [options] [user@]host\n");
    printf("Options:\n");
    printf("  -V        print version information and exit\n");
    printf("  -pgpfp    print PGP key fingerprints and exit\n");
    printf("  -b file   use specified batchfile\n");
    printf("  -bc       output batchfile commands\n");
    printf("  -be       don't stop batchfile processing if errors\n");
    printf("  -v        show verbose messages\n");
    printf("  -load sessname  Load settings from saved session\n");
    printf("  -l user   connect with specified username\n");
    printf("  -P port   connect to specified port\n");
    printf("  -pwfile file   login with password read from specified file\n");
    printf("  -1 -2     force use of particular SSH protocol version\n");
    printf("  -ssh -ssh-connection\n");
    printf("            force use of particular SSH protocol variant\n");
    printf("  -4 -6     force use of IPv4 or IPv6\n");
    printf("  -C        enable compression\n");
    printf("  -i key    private key file for user authentication\n");
    printf("  -noagent  disable use of Pageant\n");
    printf("  -agent    enable use of Pageant\n");
    printf("  -no-trivial-auth\n");
    printf("            disconnect if SSH authentication succeeds trivially\n");
    printf("  -hostkey keyid\n");
    printf("            manually specify a host key (may be repeated)\n");
    printf("  -batch    disable all interactive prompts\n");
    printf("  -no-sanitise-stderr  don't strip control chars from"
           " standard error\n");
    printf("  -proxycmd command\n");
    printf("            use 'command' as local proxy\n");
    printf("  -sshlog file\n");
    printf("  -sshrawlog file\n");
    printf("            log protocol details to a file\n");
    printf("  -logoverwrite\n");
    printf("  -logappend\n");
    printf("            control what happens when a log file already exists\n");
}

static void version(void)
{
    char *buildinfo_text = buildinfo("\n");
    printf("psftp: %s\n%s\n", ver, buildinfo_text);
    sfree(buildinfo_text);
    exit(0);
}

/*
 * Connect to a host.
 */
static int psftp_connect(char *userhost, char *user, int portnumber)
{
    char *host, *realhost;
    const char *err;

    /* Separate host and username */
    host = userhost;
    host = strrchr(host, '@');
    if (host == NULL) {
        host = userhost;
    } else {
        *host++ = '\0';
        if (user) {
            printf("psftp: multiple usernames specified; using \"%s\"\n",
                   user);
        } else
            user = userhost;
    }

    /*
     * If we haven't loaded session details already (e.g., from -load),
     * try looking for a session called "host".
     */
    if (!cmdline_loaded_session()) {
        /* Try to load settings for `host' into a temporary config */
        Conf *conf2 = conf_new();
        conf_set_str(conf2, CONF_host, "");
        do_defaults(host, conf2);
        if (conf_get_str(conf2, CONF_host)[0] != '\0') {
            /* Settings present and include hostname */
            /* Re-load data into the real config. */
            do_defaults(host, conf);
        } else {
            /* Session doesn't exist or mention a hostname. */
            /* Use `host' as a bare hostname. */
            conf_set_str(conf, CONF_host, host);
        }
        conf_free(conf2);
    } else {
        /* Patch in hostname `host' to session details. */
        conf_set_str(conf, CONF_host, host);
    }

    /*
     * Force protocol to SSH if the user has somehow contrived to
     * select one we don't support (e.g. by loading an inappropriate
     * saved session). In that situation we assume the port number is
     * useless too.)
     */
    if (!backend_vt_from_proto(conf_get_int(conf, CONF_protocol))) {
        conf_set_int(conf, CONF_protocol, PROT_SSH);
        conf_set_int(conf, CONF_port, 22);
    }

    /*
     * If saved session / Default Settings says SSH-1 (`1 only' or `1'),
     * then change it to SSH-2, on the grounds that that's more likely to
     * work for SFTP. (Can be overridden with `-1' option.)
     * But if it says `2 only' or `2', respect which.
     */
    if ((conf_get_int(conf, CONF_sshprot) & ~1) != 2)   /* is it 2 or 3? */
        conf_set_int(conf, CONF_sshprot, 2);

    /*
     * Enact command-line overrides.
     */
    cmdline_run_saved(conf);

    /*
     * Muck about with the hostname in various ways.
     */
    {
        char *hostbuf = dupstr(conf_get_str(conf, CONF_host));
        char *host = hostbuf;
        char *p, *q;

        /*
         * Trim leading whitespace.
         */
        host += strspn(host, " \t");

        /*
         * See if host is of the form user@host, and separate out
         * the username if so.
         */
        if (host[0] != '\0') {
            char *atsign = strrchr(host, '@');
            if (atsign) {
                *atsign = '\0';
                conf_set_str(conf, CONF_username, host);
                host = atsign + 1;
            }
        }

        /*
         * Remove any remaining whitespace.
         */
        p = hostbuf;
        q = host;
        while (*q) {
            if (*q != ' ' && *q != '\t')
                *p++ = *q;
            q++;
        }
        *p = '\0';

        conf_set_str(conf, CONF_host, hostbuf);
        sfree(hostbuf);
    }

    /* Set username */
    if (user != NULL && user[0] != '\0') {
        conf_set_str(conf, CONF_username, user);
    }

    if (portnumber)
        conf_set_int(conf, CONF_port, portnumber);

    /*
     * Disable scary things which shouldn't be enabled for simple
     * things like SCP and SFTP: agent forwarding, port forwarding,
     * X forwarding.
     */
    conf_set_bool(conf, CONF_x11_forward, false);
    conf_set_bool(conf, CONF_agentfwd, false);
    conf_set_bool(conf, CONF_ssh_simple, true);
    {
        char *key;
        while ((key = conf_get_str_nthstrkey(conf, CONF_portfwd, 0)) != NULL)
            conf_del_str_str(conf, CONF_portfwd, key);
    }

    /* Set up subsystem name. */
#ifdef WINSFTP_BUILD
    if (scp_remote_cmd[0]) {
        /* SCP exec-channel mode */
        conf_set_str(conf, CONF_remote_cmd, scp_remote_cmd);
        conf_set_bool(conf, CONF_ssh_subsys, false);
        conf_set_bool(conf, CONF_nopty, true);
        conf_set_str(conf, CONF_remote_cmd2, "");
        conf_set_bool(conf, CONF_ssh_subsys2, false);
    } else {
#endif
    conf_set_str(conf, CONF_remote_cmd, "sftp");
    conf_set_bool(conf, CONF_ssh_subsys, true);
    conf_set_bool(conf, CONF_nopty, true);

    /*
     * Set up fallback option, for SSH-1 servers or servers with the
     * sftp subsystem not enabled but the server binary installed
     * in the usual place. We only support fallback on Unix
     * systems, and we use a kludgy piece of shellery which should
     * try to find sftp-server in various places (the obvious
     * systemwide spots /usr/lib and /usr/local/lib, and then the
     * user's PATH) and finally give up.
     *
     *   test -x /usr/lib/sftp-server && exec /usr/lib/sftp-server
     *   test -x /usr/local/lib/sftp-server && exec /usr/local/lib/sftp-server
     *   exec sftp-server
     *
     * the idea being that this will attempt to use either of the
     * obvious pathnames and then give up, and when it does give up
     * it will print the preferred pathname in the error messages.
     */
    conf_set_str(conf, CONF_remote_cmd2,
                 "test -x /usr/lib/sftp-server &&"
                 " exec /usr/lib/sftp-server\n"
                 "test -x /usr/local/lib/sftp-server &&"
                 " exec /usr/local/lib/sftp-server\n"
                 "exec sftp-server");
    conf_set_bool(conf, CONF_ssh_subsys2, false);
#ifdef WINSFTP_BUILD
    } /* end else (SCP override not set) */
#endif

    psftp_logctx = log_init(console_cli_logpolicy, conf);

    platform_psftp_pre_conn_setup(console_cli_logpolicy);

    err = backend_init(backend_vt_from_proto(
                           conf_get_int(conf, CONF_protocol)),
                       psftp_seat, &backend, psftp_logctx, conf,
                       conf_get_str(conf, CONF_host),
                       conf_get_int(conf, CONF_port),
                       &realhost, 0,
                       conf_get_bool(conf, CONF_tcp_keepalives));
    if (err != NULL) {
        fprintf(stderr, "ssh_init: %s\n", err);
        return 1;
    }
    while (!backend_sendok(backend)) {
        if (backend_exitcode(backend) >= 0)
            return 1;
        if (ssh_sftp_loop_iteration() < 0) {
            fprintf(stderr, "ssh_init: error during SSH connection setup\n");
            return 1;
        }
    }
    if (verbose && realhost != NULL)
        printf("Connected to %s\n", realhost);
    if (realhost != NULL)
        sfree(realhost);
    return 0;
}

void cmdline_error(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "psftp: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fprintf(stderr, "\n       try typing \"psftp -h\" for help\n");
    exit(1);
}

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = false;

static stdio_sink stderr_ss;
static StripCtrlChars *stderr_scc;

const unsigned cmdline_tooltype = TOOLTYPE_FILETRANSFER;

/*
 * Main program. Parse arguments etc.
 */
int psftp_main(CmdlineArgList *arglist)
{
    int toret;
    int portnumber = 0;
    char *userhost, *user;
    int mode = 0;
    int modeflags = 0;
    bool sanitise_stderr = true;
    Filename *batchfile = NULL;

    sk_init();
    enable_dit();

    userhost = user = NULL;

    /* Load Default Settings before doing anything else. */
    conf = conf_new();
    do_defaults(NULL, conf);
#ifdef WINSFTP_BUILD
    /*
     * winsftp.exe: always start fresh — no command-line parsing and no
     * Default Settings auto-connect.  sent_eof must be cleared so that
     * psftp_eof() works correctly for the new session.
     */
    sent_eof = false;
#else

    size_t arglistpos = 0;
    while (arglist->args[arglistpos]) {
        CmdlineArg *arg = arglist->args[arglistpos++];
        CmdlineArg *nextarg = arglist->args[arglistpos];
        const char *argstr = cmdline_arg_to_str(arg);

        if (argstr[0] != '-') {
            if (userhost)
                cmdline_error("unexpected extra argument \"%s\"", argstr);
            else
                userhost = dupstr(argstr);
            continue;
        }
        int retd = cmdline_process_param(arg, nextarg, 1, conf);
        if (retd == -2) {
            cmdline_error("option \"%s\" requires an argument", argstr);
        } else if (retd == 2) {
            arglistpos++;              /* skip next argument */
        } else if (retd == 1) {
            /* We have our own verbosity in addition to `flags'. */
            if (cmdline_verbose())
                verbose = true;
        } else if (strcmp(argstr, "-h") == 0 ||
                   strcmp(argstr, "-?") == 0 ||
                   strcmp(argstr, "--help") == 0) {
            usage();
            cleanup_exit(0);
        } else if (strcmp(argstr, "-pgpfp") == 0) {
            pgp_fingerprints();
            return 0;
        } else if (strcmp(argstr, "-V") == 0 ||
                   strcmp(argstr, "--version") == 0) {
            version();
        } else if (strcmp(argstr, "-b") == 0 && nextarg) {
            mode = 1;
            batchfile = cmdline_arg_to_filename(nextarg);
            arglistpos++;
        } else if (strcmp(argstr, "-bc") == 0) {
            modeflags = modeflags | 1;
        } else if (strcmp(argstr, "-be") == 0) {
            modeflags = modeflags | 2;
        } else if (strcmp(argstr, "-sanitise-stderr") == 0) {
            sanitise_stderr = true;
        } else if (strcmp(argstr, "-no-sanitise-stderr") == 0) {
            sanitise_stderr = false;
        } else if (strcmp(argstr, "--") == 0) {
            arglistpos++;
            break;
        } else {
            cmdline_error("unknown option \"%s\"", argstr);
        }
    }
#endif /* WINSFTP_BUILD */
    backend = NULL;

    stdio_sink_init(&stderr_ss, stderr);
    stderr_bs = BinarySink_UPCAST(&stderr_ss);
    if (sanitise_stderr) {
        stderr_scc = stripctrl_new(stderr_bs, false, L'\0');
        stderr_bs = BinarySink_UPCAST(stderr_scc);
    }

    string_scc = stripctrl_new(NULL, false, L'\0');

#ifndef WINSFTP_BUILD
    /*
     * If the loaded session provides a hostname, and a hostname has not
     * otherwise been specified, pop it in `userhost' so that
     * `psftp -load sessname' is sufficient to start a session.
     */
    if (!userhost && conf_get_str(conf, CONF_host)[0] != '\0') {
        userhost = dupstr(conf_get_str(conf, CONF_host));
    }
#endif

    /*
     * If a user@host string has already been provided, connect to
     * it now.
     */
    if (userhost) {
        int retd;
        retd = psftp_connect(userhost, user, portnumber);
        sfree(userhost);
        if (retd)
            return 1;
        if (do_sftp_init())
            return 1;
    } else {
#ifndef WINSFTP_BUILD
        printf("psftp: no hostname specified; use \"open host.name\""
               " to connect\n");
#endif
    }

#ifndef WINSFTP_BUILD
    cmdline_arg_list_free(arglist);
#endif

    toret = do_sftp(mode, modeflags, batchfile);

    if (backend && backend_connected(backend)) {
        char ch;
        backend_special(backend, SS_EOF, 0);
        sent_eof = true;
        sftp_recvdata(&ch, 1);
    }
    do_sftp_cleanup();
    random_save_seed();
    cmdline_cleanup();
    sk_cleanup();

    stripctrl_free(string_scc);
    stripctrl_free(stderr_scc);

    if (psftp_logctx)
        log_free(psftp_logctx);

    return toret;
}
