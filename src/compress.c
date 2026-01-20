/*
    ashd - A Sane HTTP Daemon
    Copyright (C) 2008  Fredrik Tolf <fredrik@dolda2000.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <req.h>

#include "compress.h"

struct etype {
    char *name;
    int (*compress)(int, int);
};

static char *cachedir(void)
{
    static char *ret = NULL;
    
    if(ret == NULL)
	ret = sprintf2("/tmp/sendfile-cc-%i", getuid());
    return(ret);
}

#ifdef HAVE_ZLIB

#include <zlib.h>

static int deflatefile(int dfd, int sfd)
{
    int rv, zr, b, w;
    char *ib, *ob;
    z_stream zs;
    
    ib = smalloc(65536);
    ob = smalloc(65536);
    memset(&zs, 0, sizeof(zs));
    deflateInit(&zs, 9);
    while(1) {
	rv = read(sfd, ib, 6553);
	if(rv < 0)
	    goto out;
	if(rv == 0)
	    break;
	zs.next_in = (Bytef *)ib;
	zs.avail_in = rv;
	while(zs.avail_in > 0) {
	    zs.next_out = (Bytef *)ob;
	    zs.avail_out = 65536;
	    if(deflate(&zs, Z_NO_FLUSH) != Z_OK) {
		rv = -1;
		goto out;
	    }
	    for(w = 0, b = (char *)zs.next_out - ob; w < b; w += rv) {
		rv = write(dfd, ob + w, b - w);
		if(rv < 0)
		    goto out;
	    }
	}
    }
    zs.next_in = (Bytef *)ib;
    zs.avail_in = 0;
    do {
	zs.next_out = (Bytef *)ob;
	zs.avail_out = 65536;
	zr = deflate(&zs, Z_FINISH);
	if((zr != Z_OK) && (zr != Z_STREAM_END)) {
	    rv = -1;
	    goto out;
	}
	for(w = 0, b = (char *)zs.next_out - ob; w < b; w += rv) {
	    rv = write(dfd, ob + w, b - w);
	    if(rv < 0)
		goto out;
	}
    } while(zr != Z_STREAM_END);
    rv = 0;
out:
    free(ib);
    free(ob);
    deflateEnd(&zs);
    lseek(sfd, 0, SEEK_SET);
    return(rv);
}

static struct etype t_gz = {
    .name = "deflate",
    .compress = deflatefile,
};

#endif

static struct etype *types[] = {
#ifdef HAVE_ZLIB
    &t_gz,
#endif
    NULL,
};

static struct etype *findtype(char *name)
{
    int i;
    
    for(i = 0; types[i] != NULL; i++) {
	if(!strcmp(name, types[i]->name))
	    return(types[i]);
    }
    return(NULL);
}

static void checkclean(void)
{
    char *path;
    int fd;
    struct stat sb;
    DIR *dp;
    struct dirent *ent;
    time_t now;
    
    if(stat(sprintf3("%s/lastclean", cachedir()), &sb))
	sb.st_mtime = 0;
    now = time(NULL);
    if(now - sb.st_mtime < 3600 * 24)
	return;
    if((fd = open(path = sprintf3("%s/cleaning", cachedir()), O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
	if(errno == EEXIST) {
	    if(stat(path, &sb))
		return;
	    if(now - sb.st_mtime < 3600)
		return;
	} else {
	    return;
	}
    }
    close(fd);
    if((fd = open(sprintf3("%s/lastclean", cachedir()), O_WRONLY | O_CREAT, 0600)) >= 0)
	close(fd);
    if((dp = opendir(cachedir())) == NULL)
	return;
    while((ent = readdir(dp)) != NULL) {
	if((ent->d_name[0] == '.') || !strcmp(ent->d_name, "cleaning") || !strcmp(ent->d_name, "lastclean"))
	    continue;
	path = sprintf3("%s/%s", cachedir(), ent->d_name);
	if(stat(path, &sb))
	    continue;
	if(now - sb.st_atime > 3600 * 48)
	    unlink(path);
    }
    closedir(dp);
    unlink(sprintf3("%s/cleaning", cachedir()));
}

static int openbytype(struct etype *type, int ofd, struct stat *info, struct stat *oinfo)
{
    char *epath, *npath;
    int fd;
    
    npath = NULL;
    epath = sprintf2("%s/%jx:%jx:%s", cachedir(), (uintmax_t)info->st_dev, (uintmax_t)info->st_ino, type->name);
    if((fd = open(epath, O_RDONLY)) >= 0) {
	if(fstat(fd, oinfo))
	    goto error;
	if(oinfo->st_mtime >= info->st_mtime)
	    goto out;
	close(fd);
	fd = -1;
    }
    npath = sprintf2("%s/tmp-%ji", cachedir(), (intmax_t)getpid());
    if((fd = open(npath, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0) {
	if(errno != ENOENT)
	    goto error;
	if(!mkdir(cachedir(), 0700)) {
	    if((fd = open(sprintf3("%s/lastclean", cachedir()), O_WRONLY, O_CREAT, O_TRUNC, 0600)) < 0)
		goto error;
	    close(fd);
	}
	if((fd = open(npath, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0)
	    goto error;
    }
    if(type->compress(fd, ofd)) {
	unlink(npath);
	goto error;
    }
    if(rename(npath, epath)) {
	unlink(npath);
	goto error;
    }
    fstat(fd, oinfo);
    lseek(fd, 0, SEEK_SET);
    checkclean();
    goto out;
    
error:
    if(fd >= 0) {
	close(fd);
	fd = -1;
    }
out:
    free(epath);
    if(npath)
	free(npath);
    return(fd);
}

static void parsetypes(struct charvbuf *dst, char *head)
{
    char *p;
    struct charbuf type;
    
    p = head;
    while(*p) {
	for(; *p && isspace(*p); p++);
	for(bufinit(type); *p && (*p != ',') && (*p != ';') && !isspace(*p); p++)
	    bufadd(type, *p);
	if(type.b) {
	    bufadd(type, 0);
	    bufadd(*dst, type.b);
	}
	for(; *p && (*p != ','); p++);
	if(*p)
	    p++;
    }
}

int ccopen(char *path, struct stat *sb, char *accept, const char **encoding)
{
    int i, fd, efd, mfd;
    off_t minsz;
    struct charvbuf types;
    struct etype *type, *mtype;
    struct stat esb, msb;
    
    *encoding = NULL;
    bufinit(types);
    mfd = -1;
    if((fd = open(path, O_RDONLY)) < 0)
	return(-1);
    if(fstat(fd, sb))
	goto error;
    if(accept == NULL)
	goto out;
    parsetypes(&types, accept);
    for(i = 0; i < types.d; i++) {
	if((type = findtype(types.b[i])) != NULL) {
	    if((efd = openbytype(type, fd, sb, &esb)) >= 0) {
		if((esb.st_size < sb->st_size) && ((mfd < 0) || (esb.st_size < minsz))) {
		    if(mfd >= 0)
			close(mfd);
		    mfd = efd;
		    minsz = esb.st_size;
		    msb = esb;
		    msb.st_atime = sb->st_atime;
		    msb.st_mtime = sb->st_mtime;
		    msb.st_ctime = sb->st_ctime;
		    mtype = type;
		}
	    }
	}
    }
    if(mfd < 0)
	goto out;
    close(fd);
    *sb = msb;
    fd = mfd;
    mfd = -1;
    *encoding = mtype->name;
    goto out;
error:
    close(fd);
    fd = -1;
out:
    if(mfd >= 0)
	close(mfd);
    for(i = 0; i < types.d; i++)
	free(types.b[i]);
    buffree(types);
    return(fd);
}
