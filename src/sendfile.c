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
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <magic.h>
#include <locale.h>
#include <langinfo.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <resp.h>

#ifdef HAVE_XATTR
#include <attr/xattr.h>
#endif

static magic_t cookie = NULL;

static void passdata(int in, int out, off_t maxlen)
{
    int ret, len, off;
    char *buf;
    
    buf = smalloc(65536);
    while(1) {
	len = 65536;
	if((maxlen > 0) && (len > maxlen))
	    len = maxlen;
	len = read(in, buf, len);
	if(len < 0) {
	    flog(LOG_ERR, "sendfile: could not read input: %s", strerror(errno));
	    break;
	}
	if(len == 0)
	    break;
	for(off = 0; off < len; off += ret) {
	    ret = write(out, buf + off, len - off);
	    if(ret < 0) {
		flog(LOG_ERR, "sendfile: could not write output: %s", strerror(errno));
		break;
	    }
	}
	if(maxlen > 0) {
	    if((maxlen -= len) <= 0)
		break;
	}
    }
    free(buf);
}

static char *attrmimetype(char *file)
{
#ifdef HAVE_XATTR
    static char buf[1024];
    int i;
    ssize_t sz;
    
    if((sz = getxattr(file, "user.ash-mime-type", buf, sizeof(buf) - 1)) > 0)
	goto found;
    if((sz = getxattr(file, "user.mime-type", buf, sizeof(buf) - 1)) > 0)
	goto found;
    if((sz = getxattr(file, "user.mime_type", buf, sizeof(buf) - 1)) > 0)
	goto found;
    if((sz = getxattr(file, "user.Content-Type", buf, sizeof(buf) - 1)) > 0)
	goto found;
    return(NULL);
found:
    for(i = 0; i < sz; i++) {
	if((buf[sz] < 32) || (buf[sz] >= 128))
	    return(NULL);
    }
    buf[sz] = 0;
    return(buf);
#else
    return(NULL);
#endif
}

static const char *getmimetype(char *file, struct stat *sb)
{
    const char *ret;
    
    if((ret = attrmimetype(file)) != NULL)
	return(ret);
    if(cookie == NULL) {
	cookie = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK);
	magic_load(cookie, NULL);
    }
    if((ret = magic_file(cookie, file)) != NULL)
	return(ret);
    return("application/octet-stream");
}

/* XXX: This could be made far better and check for other attributes
 * and stuff, but not now. */
static const char *ckctype(const char *ctype)
{
    if(!strncmp(ctype, "text/", 5) && (strchr(ctype, ';') == NULL))
	return(sprintf2("%s; charset=%s", ctype, nl_langinfo(CODESET)));
    return(ctype);
}

static void checkcache(char *file, struct stat *sb)
{
    char *hdr;
    
    if((hdr = getenv("REQ_IF_MODIFIED_SINCE")) != NULL) {
	if(parsehttpdate(hdr) < sb->st_mtime)
	    return;
	printf("HTTP/1.1 304 Not Modified\n");
	printf("Date: %s\n", fmthttpdate(time(NULL)));
	printf("Content-Length: 0\n");
	printf("\n");
	exit(0);
    }
}

static void usage(void)
{
    flog(LOG_ERR, "usage: sendfile [-c CONTENT-TYPE] METHOD URL REST");
}

static void sendwhole(int fd, struct stat *sb, const char *contype, int head)
{
    printf("HTTP/1.1 200 OK\n");
    printf("Content-Type: %s\n", contype);
    printf("Content-Length: %ji\n", (intmax_t)sb->st_size);
    printf("Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
    printf("Date: %s\n", fmthttpdate(time(NULL)));
    printf("\n");
    fflush(stdout);
    if(!head)
	passdata(fd, 1, -1);
}

static void sendrange(int fd, struct stat *sb, const char *contype, char *spec, int head)
{
    char buf[strlen(spec) + 1];
    char *p, *e;
    off_t start, end;
    
    if(strncmp(spec, "bytes=", 6))
	goto error;
    strcpy(buf, spec + 6);
    if((p = strchr(buf, '-')) == NULL)
	goto error;
    if(p == buf) {
	if(!p[1])
	    goto error;
	end = sb->st_size;
	start = end - strtoll(p + 1, &e, 10);
	if(*e)
	    goto error;
	if(start < 0)
	    start = 0;
    } else {
	*(p++) = 0;
	start = strtoll(buf, &e, 10);
	if(*e)
	    goto error;
	if(*p) {
	    end = strtoll(p, &e, 10) + 1;
	    if(*e)
		goto error;
	} else {
	    end = sb->st_size;
	}
    }
    if(start >= sb->st_size) {
	printf("HTTP/1.1 416 Not satisfiable\n");
	printf("Content-Range: */%ji\n", (intmax_t)sb->st_size);
	printf("Content-Length: 0\n");
	printf("Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
	printf("Date: %s\n", fmthttpdate(time(NULL)));
	printf("\n");
	return;
    }
    if((start < 0) || (start >= end))
	goto error;
    if(end > sb->st_size)
	end = sb->st_size;
    errno = 0;
    if(lseek(fd, start, SEEK_SET) != start) {
	simpleerror(1, 500, "Internal Error", "Could not seek properly to beginning of requested byte range.");
	flog(LOG_ERR, "sendfile: could not seek properly when serving partial content: %s", strerror(errno));
	exit(1);
    }
    printf("HTTP/1.1 206 Partial content\n");
    printf("Content-Range: bytes %ji-%ji/%ji\n", (intmax_t)start, (intmax_t)(end - 1), (intmax_t)sb->st_size);
    printf("Content-Length: %ji\n", (intmax_t)(end - start));
    printf("Content-Type: %s\n", contype);
    printf("Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
    printf("Date: %s\n", fmthttpdate(time(NULL)));
    printf("\n");
    fflush(stdout);
    if(!head)
	passdata(fd, 1, end - start);
    return;
    
error:
    sendwhole(fd, sb, contype, head);
}

int main(int argc, char **argv)
{
    int c;
    char *file, *hdr;
    struct stat sb;
    int fd, ishead;
    const char *contype;
    
    setlocale(LC_ALL, "");
    contype = NULL;
    while((c = getopt(argc, argv, "c:")) >= 0) {
	switch(c) {
	case 'c':
	    contype = optarg;
	    break;
	default:
	    usage();
	    exit(1);
	    break;
	}
    }
    if(argc - optind < 3) {
	usage();
	exit(1);
    }
    if((file = getenv("REQ_X_ASH_FILE")) == NULL) {
	flog(LOG_ERR, "sendfile: needs to be called with the X-Ash-File header");
	exit(1);
    }
    if(*argv[optind + 2]) {
	simpleerror(1, 404, "Not Found", "The requested URL has no corresponding resource.");
	exit(0);
    }
    if(stat(file, &sb) || ((fd = open(file, O_RDONLY)) < 0)) {
	flog(LOG_ERR, "sendfile: could not stat input file %s: %s", file, strerror(errno));
	simpleerror(1, 500, "Internal Error", "The server could not access its own data.");
	exit(1);
    }
    if(contype == NULL)
	contype = getmimetype(file, &sb);
    contype = ckctype(contype);
    
    checkcache(file, &sb);
    
    ishead = !strcasecmp(argv[optind], "head");
    if((hdr = getenv("REQ_RANGE")) != NULL)
	sendrange(fd, &sb, contype, hdr, ishead);
    else
	sendwhole(fd, &sb, contype, ishead);
    return(0);
}
