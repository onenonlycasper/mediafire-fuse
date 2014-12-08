/*
 * Copyright (C) 2014 Johannes Schauer <j.schauer@email.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define _POSIX_C_SOURCE 200809L // for strdup and struct timespec
#define _XOPEN_SOURCE 700       // for S_IFDIR and S_IFREG (on linux,
                                // posix_c_source is enough but this is needed
                                // on freebsd)

#define FUSE_USE_VERSION 30

#include <fuse/fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef __linux
#include <bits/fcntl-linux.h>
#endif
#include <fuse/fuse_common.h>
#include <stdint.h>
#include <libgen.h>

#include "../mfapi/mfconn.h"
#include "../mfapi/apicalls.h"
#include "hashtbl.h"
#include "operations.h"

/* what you can safely assume about requests to your filesystem
 *
 * from: http://sourceforge.net/p/fuse/wiki/FuseInvariants/
 *
 * There are a number of assumptions that one can safely make when
 * implementing a filesystem using fuse. This page should be completed with a
 * set of such assumptions.
 *
 *  - All requests are absolute, i.e. all paths begin with "/" and include the
 *    complete path to a file or a directory. Symlinks, "." and ".." are
 *    already resolved.
 *
 *  - For every request you can get except for "Getattr()", "Read()" and
 *    "Write()", usually for every path argument (both source and destination
 *    for link and rename, but only the source for symlink), you will get a
 *    "Getattr()" request just before the callback.
 *
 * For example, suppose I store file names of files in a filesystem also into a
 * database. To keep data in sync, I would like, for each filesystem operation
 * that succeeds, to check if the file exists on the database. I just do this in
 * the "Getattr()" call, since all other calls will be preceded by a getattr.
 *
 *  - The value of the "st_dev" attribute in the "Getattr()" call are ignored
 *    by fuse and an appropriate anomynous device number is inserted instead.
 *
 *  - The arguments for every request are already verified as much as
 *    possible. This means that, for example "readdir()" is only called with
 *    an existing directory name, "Readlink()" is only called with an existing
 *    symlink, "Symlink()" is only called if there isn't already another
 *    object with the requested linkname, "read()" and "Write()" are only
 *    called if the file has been opened with the correct flags.
 *
 *  - The VFS also takes care of avoiding race conditions:
 *
 *  - while "Unlink()" is running on a specific file, it cannot be interrupted
 *    by a "Chmod()", "Link()" or "Open()" call from a different thread on the
 *    same file.
 *
 *  - while "Rmdir()" is running, no files can be created in the directory
 *    that "Rmdir()" is acting on.
 *
 *  - If a request returns invalid values (e.g. in the structure returned by
 *    "Getattr()" or in the link target returned by "Symlink()") or if a
 *    request appears to have failed (e.g. if a "Create()" request succeds but
 *    a subsequent "Getattr()" (that fuse calls automatically) ndicates that
 *    no regular file has been created), the syscall returns EIO to the
 *    caller.
 */

struct mediafirefs_openfile {
    // to fread and fwrite from/to the file
    int             fd;
    char           *path;
    // whether or not a patch has to be uploaded when closing
    bool            is_readonly;
    // whether or not to do a new file upload when closing
    bool            is_local;
};

int mediafirefs_getattr(const char *path, struct stat *stbuf)
{
    /*
     * since getattr is called before every other call (except for getattr,
     * read and write) wee only call folder_tree_update in the getattr call
     * and not the others
     *
     * FIXME: only call folder_tree_update if it has not been called for a set
     * amount of time
     */
    struct mediafirefs_context_private *ctx;
    int             retval;

    ctx = fuse_get_context()->private_data;
    folder_tree_update(ctx->tree, ctx->conn, false);
    retval = folder_tree_getattr(ctx->tree, ctx->conn, path, stbuf);

    if (retval != 0 && stringv_mem(ctx->sv_writefiles, path)) {
        stbuf->st_uid = geteuid();
        stbuf->st_gid = getegid();
        stbuf->st_ctime = 0;
        stbuf->st_mtime = 0;
        stbuf->st_mode = S_IFREG | 0666;
        stbuf->st_nlink = 1;
        stbuf->st_atime = 0;
        stbuf->st_size = 0;
        retval = 0;
    }

    return retval;
}

int mediafirefs_readdir(const char *path, void *buf, fuse_fill_dir_t filldir,
                        off_t offset, struct fuse_file_info *info)
{
    (void)offset;
    (void)info;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;
    return folder_tree_readdir(ctx->tree, ctx->conn, path, buf, filldir);
}

void mediafirefs_destroy()
{
    FILE           *fd;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    fprintf(stderr, "storing hashtable\n");

    fd = fopen(ctx->dircache, "w+");

    if (fd == NULL) {
        fprintf(stderr, "cannot open %s for writing\n", ctx->dircache);
        return;
    }

    folder_tree_store(ctx->tree, fd);

    fclose(fd);

    folder_tree_destroy(ctx->tree);

    mfconn_destroy(ctx->conn);
}

int mediafirefs_mkdir(const char *path, mode_t mode)
{
    (void)mode;

    char           *dirname;
    int             retval;
    char           *basename;
    const char     *key;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    /* we don't need to check whether the path already existed because the
     * getattr call made before this one takes care of that
     */

    /* before calling the remote function we check locally */

    dirname = strdup(path);

    /* remove possible trailing slash */
    if (dirname[strlen(dirname) - 1] == '/') {
        dirname[strlen(dirname) - 1] = '\0';
    }

    /* split into dirname and basename */

    basename = strrchr(dirname, '/');
    if (basename == NULL) {
        fprintf(stderr, "cannot find slash\n");
        return -ENOENT;
    }

    /* replace the slash by a terminating zero */
    basename[0] = '\0';

    /* basename points to the string after the last slash */
    basename++;

    /* check if the dirname is of zero length now. If yes, then the directory
     * is to be created in the root */
    if (dirname[0] == '\0') {
        key = NULL;
    } else {
        key = folder_tree_path_get_key(ctx->tree, ctx->conn, dirname);
    }

    retval = mfconn_api_folder_create(ctx->conn, key, basename);
    if (retval != 0) {
        fprintf(stderr, "mfconn_api_folder_create unsuccessful\n");
        // FIXME: find better errno in this case
        return -EAGAIN;
    }

    free(dirname);

    folder_tree_update(ctx->tree, ctx->conn, true);

    return 0;
}

int mediafirefs_rmdir(const char *path)
{
    const char     *key;
    int             retval;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    /* no need to check
     *  - if path is directory
     *  - if directory is empty
     *  - if directory is root
     *
     * because getattr was called before and already made sure
     */

    key = folder_tree_path_get_key(ctx->tree, ctx->conn, path);
    if (key == NULL) {
        fprintf(stderr, "key is NULL\n");
        return -ENOENT;
    }

    retval = mfconn_api_folder_delete(ctx->conn, key);
    if (retval != 0) {
        fprintf(stderr, "mfconn_api_folder_create unsuccessful\n");
        // FIXME: find better errno in this case
        return -EAGAIN;
    }

    /* retrieve remote changes to not get out of sync */
    folder_tree_update(ctx->tree, ctx->conn, true);

    return 0;
}

int mediafirefs_unlink(const char *path)
{
    const char     *key;
    int             retval;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    /* no need to check
     *  - if path is directory
     *  - if directory is empty
     *  - if directory is root
     *
     * because getattr was called before and already made sure
     */

    key = folder_tree_path_get_key(ctx->tree, ctx->conn, path);
    if (key == NULL) {
        fprintf(stderr, "key is NULL\n");
        return -ENOENT;
    }

    retval = mfconn_api_file_delete(ctx->conn, key);
    if (retval != 0) {
        fprintf(stderr, "mfconn_api_file_create unsuccessful\n");
        // FIXME: find better errno in this case
        return -EAGAIN;
    }

    /* retrieve remote changes to not get out of sync */
    folder_tree_update(ctx->tree, ctx->conn, true);

    return 0;
}

/*
 * the following restrictions apply:
 *  1. a file can be opened in read-only mode more than once at a time
 *  2. a file can only be be opened in write-only or read-write mode if it is
 *     not open for writing at the same time
 *  3. a file that is only local and has not been uploaded yet cannot be read
 *     from
 *  4. a file that has opened in any way will not be updated to its latest
 *     remote revision until all its opened handles are closed
 *
 *  Point 2 is enforced by a lookup in the writefiles string vector. If the
 *  path is in there then it was either just created locally or opened with
 *  write-only or read-write. In both cases it must not be opened for
 *  writing again.
 *
 *  Point 3 is enforced by the lookup in the hashtable failing.
 *
 *  Point 4 is enforced by checking if the current path is in the writefiles or
 *  readonlyfiles string vector and if yes, no updating will be done.
 */
int mediafirefs_open(const char *path, struct fuse_file_info *file_info)
{
    int             fd;
    bool            is_open;
    struct mediafirefs_openfile *openfile;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    /* if file is not opened read-only, check if it was already opened in a
     * not read-only mode and abort if yes */
    if ((file_info->flags & O_ACCMODE) != O_RDONLY
        && stringv_mem(ctx->sv_writefiles, path)) {
        fprintf(stderr, "file %s was already opened for writing\n", path);
        return -EACCES;
    }

    is_open = false;
    // check if the file was already opened
    // check read-only files first
    if (stringv_mem(ctx->sv_readonlyfiles, path)) {
        is_open = true;
    }
    // check writable files only if the file was
    //   - not yet found in the read-only files
    //   - the file is opened in read-only mode (because otherwise the
    //     writable files were already searched above without failing)
    if (!is_open && (file_info->flags & O_ACCMODE) == O_RDONLY
        && stringv_mem(ctx->sv_writefiles, path)) {
        is_open = true;
    }

    fd = folder_tree_open_file(ctx->tree, ctx->conn, path, file_info->flags,
                               !is_open);
    if (fd < 0) {
        fprintf(stderr, "folder_tree_file_open unsuccessful\n");
        return fd;
    }

    openfile = malloc(sizeof(struct mediafirefs_openfile));
    openfile->fd = fd;
    openfile->is_local = false;
    openfile->path = strdup(path);

    if ((file_info->flags & O_ACCMODE) == O_RDONLY) {
        openfile->is_readonly = true;
        // add to readonlyfiles
        stringv_add(ctx->sv_readonlyfiles, path);
    } else {
        openfile->is_readonly = false;
        // add to writefiles
        stringv_add(ctx->sv_writefiles, path);
    }

    file_info->fh = (uint64_t) openfile;
    return 0;
}

/*
 * this is called if the file does not exist yet. It will create a temporary
 * file and open it.
 * once the file gets closed, it will be uploaded.
 */
int mediafirefs_create(const char *path, mode_t mode,
                       struct fuse_file_info *file_info)
{
    (void)mode;

    int             fd;
    struct mediafirefs_openfile *openfile;
    struct mediafirefs_context_private *ctx;

    ctx = fuse_get_context()->private_data;

    fd = folder_tree_tmp_open(ctx->tree);
    if (fd < 0) {
        fprintf(stderr, "folder_tree_tmp_open failed\n");
        return -EACCES;
    }

    openfile = malloc(sizeof(struct mediafirefs_openfile));
    openfile->fd = fd;
    openfile->is_local = true;
    openfile->is_readonly = false;
    openfile->path = strdup(path);
    file_info->fh = (uint64_t) openfile;

    // add to writefiles
    stringv_add(ctx->sv_writefiles, path);

    return 0;
}

int mediafirefs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *file_info)
{
    (void)path;
    return pread(((struct mediafirefs_openfile *)file_info->fh)->fd, buf, size,
                 offset);
}

int mediafirefs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *file_info)
{
    (void)path;
    return pwrite(((struct mediafirefs_openfile *)file_info->fh)->fd, buf,
                  size, offset);
}

/*
 * note: the return value of release() is ignored by fuse
 */
int mediafirefs_release(const char *path, struct fuse_file_info *file_info)
{
    (void)path;

    FILE           *fh;
    char           *file_name;
    char           *dir_name;
    const char     *folder_key;
    char           *upload_key;
    char           *temp1;
    char           *temp2;
    int             retval;
    struct mediafirefs_context_private *ctx;
    struct mediafirefs_openfile *openfile;
    int             status;
    int             fileerror;

    ctx = fuse_get_context()->private_data;

    openfile = (struct mediafirefs_openfile *)file_info->fh;

    // if file was opened as readonly then it just has to be closed
    if (openfile->is_readonly) {
        // remove this entry from readonlyfiles
        if (stringv_del(ctx->sv_readonlyfiles, openfile->path) != 0) {
            fprintf(stderr, "FATAL: readonly entry %s not found\n",
                    openfile->path);
            exit(1);
        }

        close(openfile->fd);
        free(openfile->path);
        free(openfile);
        return 0;
    }
    // if the file is not readonly, its entry in writefiles has to be removed
    if (stringv_del(ctx->sv_writefiles, openfile->path) != 0) {
        fprintf(stderr, "FATAL: writefiles entry %s not found\n",
                openfile->path);
        exit(1);
    }
    if (stringv_mem(ctx->sv_writefiles, openfile->path) != 0) {
        fprintf(stderr,
                "FATAL: writefiles entry %s was found more than once\n",
                openfile->path);
        exit(1);
    }
    // if the file only exists locally, an initial upload has to be done
    if (openfile->is_local) {
        // pass a copy because dirname and basename may modify their argument
        temp1 = strdup(openfile->path);
        file_name = basename(temp1);
        temp2 = strdup(openfile->path);
        dir_name = dirname(temp2);

        fh = fdopen(openfile->fd, "r");
        rewind(fh);

        folder_key = folder_tree_path_get_key(ctx->tree, ctx->conn, dir_name);

        upload_key = NULL;
        retval = mfconn_api_upload_simple(ctx->conn, folder_key,
                                          fh, file_name, &upload_key);

        fclose(fh);
        free(temp1);
        free(temp2);
        free(openfile->path);
        free(openfile);

        if (retval != 0 || upload_key == NULL) {
            fprintf(stderr, "mfconn_api_upload_simple failed\n");
            return -EACCES;
        }
        // poll for completion
        for (;;) {
            // no need to update the secret key after this
            retval = mfconn_api_upload_poll_upload(ctx->conn, upload_key,
                                                   &status, &fileerror);
            if (retval != 0) {
                fprintf(stderr, "mfconn_api_upload_poll_upload failed\n");
                return -1;
            }
            fprintf(stderr, "status: %d, filerror: %d\n", status, fileerror);
            if (status == 99) {
                fprintf(stderr, "done\n");
                break;
            }
            sleep(1);
        }

        free(upload_key);

        folder_tree_update(ctx->tree, ctx->conn, true);
        return 0;
    }
    // the file was not opened readonly and also existed on the remote
    // thus, we have to check whether any changes were made and if yes, upload
    // a patch

    close(openfile->fd);

    retval = folder_tree_upload_patch(ctx->tree, ctx->conn, openfile->path);
    free(openfile->path);
    free(openfile);

    if (retval != 0) {
        fprintf(stderr, "folder_tree_upload_patch failed\n");
        return -EACCES;
    }

    folder_tree_update(ctx->tree, ctx->conn, true);
    return 0;
}
