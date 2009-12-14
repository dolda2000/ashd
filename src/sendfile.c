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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <resp.h>

static magic_t cookie = NULL;

static void passdata(int in, int out)
{
    int ret, len, off;
    char *buf;
    
    buf = smalloc(65536);
    while(1) {
	len = read(in, buf, 65536);
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
    }
    free(buf);
}

static int strrcmp(char *str, char *end)
{
    return(strcmp(str + strlen(str) - strlen(end), end));
}

static const char *getmimetype(char *file, struct stat *sb)
{
    const char *ret;
    
    if(cookie == NULL) {
	cookie = magic_open(MAGIC_MIME_TYPE);
	magic_load(cookie, NULL);
    }
    if((ret = magic_file(cookie, file)) != NULL)
	return(ret);
    return("application/octet-stream");
}

static void checkcache(char *file, struct stat *sb)
{
    char *hdr;
    
    if((hdr = getenv("REQ_IF_MODIFIED_SINCE")) != NULL) {
	if(parsehttpdate(hdr) < sb->st_mtime)
	    return;
	printf("HTTP/1.1 304 Not Modified\r\n");
	printf("Date: %s\r\n", fmthttpdate(time(NULL)));
	printf("Content-Length: 0\r\n");
	printf("\r\n");
	exit(0);
    }
}

static void usage(void)
{
    flog(LOG_ERR, "usage: sendfile [-c CONTENT-TYPE] METHOD URL REST");
}

int main(int argc, char **argv)
{
    int c;
    char *file;
    struct stat sb;
    int fd;
    const char *contype;
    
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
    
    checkcache(file, &sb);
    
    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: %s\r\n", contype);
    printf("Content-Length: %ji\r\n", (intmax_t)sb.st_size);
    printf("Last-Modified: %s\r\n", fmthttpdate(sb.st_mtime));
    printf("Date: %s\r\n", fmthttpdate(time(NULL)));
    printf("\r\n");
    fflush(stdout);
    if(strcasecmp(argv[optind], "head"))
	passdata(fd, 1);
    return(0);
}
