/*
 * Copyright (C) 2013 Bryan Christ <bryan.christ@mediafire.com>
 *               2014 Johannes Schauer <j.schauer@email.de>
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>

#include "../utils/http.h"
#include "../utils/strings.h"
#include "file.h"

struct mffile {
    char            quickkey[18];
    char            hash[65];
    char            name[256];
    char            mtime[16];
    uint64_t        revision;

    char           *share_link;
    char           *direct_link;
    char           *onetime_link;
};

mffile         *file_alloc(void)
{
    mffile         *file;

    file = (mffile *) calloc(1, sizeof(mffile));

    return file;
}

void file_free(mffile * file)
{
    if (file == NULL)
        return;

    if (file->share_link != NULL)
        free(file->share_link);
    if (file->direct_link != NULL)
        free(file->direct_link);
    if (file->onetime_link != NULL)
        free(file->onetime_link);

    free(file);

    return;
}

int file_set_key(mffile * file, const char *key)
{
    int             len;

    if (file == NULL)
        return -1;
    if (key == NULL)
        return -1;

    len = strlen(key);
    if (len != 11 && len != 15)
        return -1;

    memset(file->quickkey, 0, sizeof(file->quickkey));
    strncpy(file->quickkey, key, sizeof(file->quickkey) - 1);

    return 0;
}

const char     *file_get_key(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->quickkey;
}

int file_set_hash(mffile * file, const char *hash)
{
    if (file == NULL)
        return -1;
    if (hash == NULL)
        return -1;

    // system supports SHA256 (current) and MD5 (legacy)
    if (strlen(hash) < 32)
        return -1;

    memset(file->hash, 0, sizeof(file->hash));
    strncpy(file->hash, hash, sizeof(file->hash) - 1);

    return 0;
}

const char     *file_get_hash(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->hash;
}

int file_set_name(mffile * file, const char *name)
{
    if (file == NULL)
        return -1;
    if (name == NULL)
        return -1;

    if (strlen(name) > 255)
        return -1;

    memset(file->name, 0, sizeof(file->name));
    strncpy(file->name, name, sizeof(file->name) - 1);

    return 0;
}

const char     *file_get_name(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->name;
}

int file_set_share_link(mffile * file, const char *share_link)
{
    if (file == NULL)
        return -1;
    if (share_link == NULL)
        return -1;

    if (file->share_link != NULL) {
        free(file->share_link);
        file->share_link = NULL;
    }

    file->share_link = strdup(share_link);

    return 0;
}

const char     *file_get_share_link(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->share_link;
}

int file_set_direct_link(mffile * file, const char *direct_link)
{
    if (file == NULL)
        return -1;
    if (direct_link == NULL)
        return -1;

    if (file->direct_link != NULL) {
        free(file->direct_link);
        file->direct_link = NULL;
    }

    file->direct_link = strdup(direct_link);

    return 0;
}

const char     *file_get_direct_link(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->direct_link;
}

int file_set_onetime_link(mffile * file, const char *onetime_link)
{
    if (file == NULL)
        return -1;
    if (onetime_link == NULL)
        return -1;

    if (file->onetime_link != NULL) {
        free(file->onetime_link);
        file->onetime_link = NULL;
    }

    file->onetime_link = strdup(onetime_link);

    return 0;
}

const char     *file_get_onetime_link(mffile * file)
{
    if (file == NULL)
        return NULL;

    return file->onetime_link;
}

ssize_t file_download_direct(mffile * file, char *local_dir)
{
    const char     *url;
    const char     *file_name;
    char           *file_path;
    struct stat     file_info;
    ssize_t         bytes_read = 0;
    int             retval;
    mfhttp         *conn;

    if (file == NULL)
        return -1;
    if (local_dir == NULL)
        return -1;

    url = file_get_direct_link(file);
    if (url == NULL)
        return -1;

    file_name = file_get_name(file);
    if (file_name == NULL)
        return -1;
    if (strlen(file_name) < 1)
        return -1;

    if (local_dir[strlen(local_dir) - 1] == '/')
        file_path = strdup_printf("%s%s", local_dir, file_name);
    else
        file_path = strdup_printf("%s/%s", local_dir, file_name);

    conn = http_create();
    retval = http_get_file(conn, url, file_path);
    http_destroy(conn);

    /*
       it is preferable to have the vfs tell us how many bytes the
       transfer actually is.  it's really all that matters.
     */
    memset(&file_info, 0, sizeof(file_info));
    retval = stat(file_path, &file_info);

    free(file_path);

    if (retval != 0)
        return -1;

    bytes_read = file_info.st_size;

    return bytes_read;
}
