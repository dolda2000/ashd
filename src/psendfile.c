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
#include <signal.h>
#include <magic.h>
#include <locale.h>
#include <langinfo.h>
#include <sys/socket.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <resp.h>
#include <mt.h>
#include <mtio.h>

#ifdef HAVE_XATTR
#include <attr/xattr.h>
#endif

static magic_t cookie;

static char *attrmimetype(char *file)
{
#ifdef HAVE_XATTR
    char buf[1024];
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
	if((buf[i] < 32) || (buf[i] >= 128))
	    return(NULL);
    }
    buf[sz] = 0;
    return(sstrdup(buf));
#else
    return(NULL);
#endif
}

static char *getmimetype(char *file, struct stat *sb)
{
    char *ret;
    const char *cret;
    
    if((ret = attrmimetype(file)) != NULL)
	return(ret);
    if((cret = magic_file(cookie, file)) != NULL)
	return(sstrdup(cret));
    return(sstrdup("application/octet-stream"));
}

/* XXX: This could be made far better and check for other attributes
 * and stuff, but not now. */
static char *ckctype(char *ctype)
{
    char *buf;
    
    if(!strncmp(ctype, "text/", 5) && (strchr(ctype, ';') == NULL)) {
	buf = sprintf2("%s; charset=%s", ctype, nl_langinfo(CODESET));
	free(ctype);
	return(buf);
    }
    return(ctype);
}

static int checkcache(struct hthead *req, FILE *out, char *file, struct stat *sb)
{
    char *hdr;
    
    if((hdr = getheader(req, "If-Modified-Since")) != NULL) {
	if(parsehttpdate(hdr) < sb->st_mtime)
	    return(0);
	fprintf(out, "HTTP/1.1 304 Not Modified\n");
	fprintf(out, "Date: %s\n", fmthttpdate(time(NULL)));
	fprintf(out, "Content-Length: 0\n");
	fprintf(out, "\n");
	return(1);
    }
    return(0);
}

static off_t passdata(FILE *in, FILE *out, off_t max)
{
    size_t read;
    off_t total;
    char buf[8192];
    
    total = 0;
    while(!feof(in) && ((max < 0) || (total < max))) {
	read = sizeof(buf);
	if(max >= 0)
	    read = min(max - total, read);
	read = fread(buf, 1, read, in);
	if(ferror(in))
	    return(-1);
	if(fwrite(buf, 1, read, out) != read)
	    return(-1);
	total += read;
    }
    return(total);
}

static void sendwhole(struct hthead *req, FILE *out, FILE *sfile, struct stat *sb, char *contype, int head)
{
    fprintf(out, "HTTP/1.1 200 OK\n");
    fprintf(out, "Content-Type: %s\n", contype);
    fprintf(out, "Content-Length: %ji\n", (intmax_t)sb->st_size);
    fprintf(out, "Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
    fprintf(out, "Date: %s\n", fmthttpdate(time(NULL)));
    fprintf(out, "\n");
    if(!head)
	passdata(sfile, out, -1);
}

static void sendrange(struct hthead *req, FILE *out, FILE *sfile, struct stat *sb, char *contype, char *spec, int head)
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
	fprintf(out, "HTTP/1.1 416 Not satisfiable\n");
	fprintf(out, "Content-Range: */%ji\n", (intmax_t)sb->st_size);
	fprintf(out, "Content-Length: 0\n");
	fprintf(out, "Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
	fprintf(out, "Date: %s\n", fmthttpdate(time(NULL)));
	fprintf(out, "\n");
	return;
    }
    if((start < 0) || (start >= end))
	goto error;
    if(end > sb->st_size)
	end = sb->st_size;
    errno = 0;
    if(fseeko(sfile, start, SEEK_SET)) {
	simpleerror2(out, 500, "Internal Error", "Could not seek properly to beginning of requested byte range.");
	flog(LOG_ERR, "sendfile: could not seek properly when serving partial content: %s", strerror(errno));
	return;
    }
    fprintf(out, "HTTP/1.1 206 Partial content\n");
    fprintf(out, "Content-Range: bytes %ji-%ji/%ji\n", (intmax_t)start, (intmax_t)(end - 1), (intmax_t)sb->st_size);
    fprintf(out, "Content-Length: %ji\n", (intmax_t)(end - start));
    fprintf(out, "Content-Type: %s\n", contype);
    fprintf(out, "Last-Modified: %s\n", fmthttpdate(sb->st_mtime));
    fprintf(out, "Date: %s\n", fmthttpdate(time(NULL)));
    fprintf(out, "\n");
    if(!head)
	passdata(sfile, out, end - start);
    return;
    
error:
    sendwhole(req, out, sfile, sb, contype, head);
}

static void serve(struct muth *muth, va_list args)
{
    vavar(struct hthead *, req);
    vavar(int, fd);
    FILE *out, *sfile;
    int ishead;
    char *file, *contype, *hdr;
    struct stat sb;
    
    sfile = NULL;
    contype = NULL;
    out = mtstdopen(fd, 1, 60, "r+", NULL);
    
    if((file = getheader(req, "X-Ash-File")) == NULL) {
	flog(LOG_ERR, "psendfile: needs to be called with the X-Ash-File header");
	simpleerror2(out, 500, "Internal Error", "The server is incorrectly configured.");
	goto out;
    }
    if(*req->rest) {
	simpleerror2(out, 404, "Not Found", "The requested URL has no corresponding resource.");
	goto out;
    }
    if(((sfile = fopen(file, "r")) == NULL) || fstat(fileno(sfile), &sb)) {
	flog(LOG_ERR, "psendfile: could not stat input file %s: %s", file, strerror(errno));
	simpleerror2(out, 500, "Internal Error", "The server could not access its own data.");
	goto out;
    }
    if(!strcasecmp(req->method, "get")) {
	ishead = 0;
    } else if(!strcasecmp(req->method, "head")) {
	ishead = 1;
    } else {
	simpleerror2(out, 405, "Method not allowed", "The requested method is not defined for this resource.");
	goto out;
    }
    if((hdr = getheader(req, "X-Ash-Content-Type")) == NULL)
	contype = getmimetype(file, &sb);
    else
	contype = sstrdup(hdr);
    contype = ckctype(contype);
    
    if(checkcache(req, out, file, &sb))
	goto out;

    if((hdr = getheader(req, "Range")) != NULL)
	sendrange(req, out, sfile, &sb, contype, hdr, ishead);
    else
	sendwhole(req, out, sfile, &sb, contype, ishead);
    
out:
    if(sfile != NULL)
	fclose(sfile);
    if(contype != NULL)
	free(contype);
    fclose(out);
    freehthead(req);
}

static void listenloop(struct muth *muth, va_list args)
{
    vavar(int, lfd);
    int fd;
    struct hthead *req;
    
    while(1) {
	block(lfd, EV_READ, 0);
	if((fd = recvreq(lfd, &req)) < 0) {
	    if(errno != 0)
		flog(LOG_ERR, "recvreq: %s", strerror(errno));
	    break;
	}
	mustart(serve, req, fd);
    }
}

static void sigterm(int sig)
{
    shutdown(0, SHUT_RDWR);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: psendfile [-h]\n");
}

int main(int argc, char **argv)
{
    int c;
    
    setlocale(LC_ALL, "");
    while((c = getopt(argc, argv, "h")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    cookie = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK);
    magic_load(cookie, NULL);
    mustart(listenloop, 0);
    signal(SIGINT, sigterm);
    signal(SIGTERM, sigterm);
    ioloop();
    return(0);
}
