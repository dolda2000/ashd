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
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <locale.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <resp.h>
#include <log.h>
#include <resp.h>

struct dentry {
    char *name;
    struct stat sb;
    int w, x;
};

static int dispmtime = 0;
static int dispsize = 0;

static void checkcache(struct stat *sb)
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

static int dcmp(const void *ap, const void *bp)
{
    const struct dentry *a = ap, *b = bp;
    
    if(S_ISDIR(a->sb.st_mode) && !S_ISDIR(b->sb.st_mode))
	return(-1);
    if(!S_ISDIR(a->sb.st_mode) && S_ISDIR(b->sb.st_mode))
	return(1);
    return(strcoll(a->name, b->name));
}

static void head(char *name, struct charbuf *dst)
{
    char *title;
    
    bprintf(dst, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    bprintf(dst, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n");
    bprintf(dst, "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n");
    bprintf(dst, "<head>\n");
    title = htmlquote(name);
    bprintf(dst, "<title>Index of %s</title>\n", title);
    bprintf(dst, "</head>\n");
    bprintf(dst, "<body>\n");
    bprintf(dst, "<h1>Index of %s</h1>\n", title);
    free(title);
}

static void foot(struct charbuf *dst)
{
    bprintf(dst, "</body>\n");
    bprintf(dst, "</html>\n");
}

static void mkindex(char *name, DIR *dir, struct charbuf *dst)
{
    struct {
	struct dentry *b;
	size_t s, d;
    } dirbuf;
    struct dentry f;
    struct dirent *dent;
    char *fn;
    int i;
    
    bufinit(dirbuf);
    while((dent = readdir(dir)) != NULL) {
	if(*dent->d_name == '.')
	    continue;
	memset(&f, 0, sizeof(f));
	f.name = sstrdup(dent->d_name);
	fn = sprintf3("%s/%s", name, dent->d_name);
	if(access(fn, R_OK))
	    continue;
	if(stat(fn, &f.sb))
	    continue;
	if(!access(fn, W_OK))
	    f.w = 1;
	if(!access(fn, X_OK))
	    f.x = 1;
	else if(S_ISDIR(f.sb.st_mode))
	    continue;
	bufadd(dirbuf, f);
    }
    qsort(dirbuf.b, dirbuf.d, sizeof(struct dentry), dcmp);
    bprintf(dst, "<table class=\"dirindex\">\n");
    for(i = 0; i < dirbuf.d; i++) {
	bprintf(dst, "<tr class=\"dentry");
	if(S_ISDIR(dirbuf.b[i].sb.st_mode))
	    bprintf(dst, " dir");
	if(dirbuf.b[i].w)
	    bprintf(dst, " writable");
	if(dirbuf.b[i].x)
	    bprintf(dst, " exec");
	bprintf(dst, "\">");	
	fn = htmlquote(dirbuf.b[i].name);
	bprintf(dst, "<td class=\"filename\"><a href=\"%s\">%s</a></td>", fn, fn);
	free(fn);
	if(dispsize && !S_ISDIR(dirbuf.b[i].sb.st_mode))
	    bprintf(dst, "<td class=\"filesize\">%ji</td>", (intmax_t)dirbuf.b[i].sb.st_size);
	if(dispmtime)
	    bprintf(dst, "<td class=\"filemtime\">%s</td>", fmthttpdate(dirbuf.b[i].sb.st_mtime));
	bprintf(dst, "</tr>\n");
	free(dirbuf.b[i].name);
    }
    bprintf(dst, "</table>\n");
}

static void usage(void)
{
    flog(LOG_ERR, "usage: htls [-hms] METHOD URL REST");
}

int main(int argc, char **argv)
{
    int c;
    char *dname;
    DIR *dir;
    struct charbuf buf;
    struct stat sb;
    
    setlocale(LC_ALL, "");
    while((c = getopt(argc, argv, "hms")) >= 0) {
	switch(c) {
	case 'h':
	    usage();
	    exit(0);
	case 'm':
	    dispmtime = 1;
	    break;
	case 's':
	    dispsize = 1;
	    break;
	default:
	    usage();
	    exit(1);
	}
    }
    if(argc - optind < 3) {
	usage();
	exit(1);
    }
    if((dname = getenv("REQ_X_ASH_FILE")) == NULL) {
	flog(LOG_ERR, "htls: needs to be called with the X-Ash-File header");
	exit(1);
    }
    if(*argv[optind + 2]) {
	simpleerror(1, 404, "Not Found", "The requested URL has no corresponding resource.");
	exit(0);
    }

    if(stat(dname, &sb) || ((dir = opendir(dname)) == NULL)) {
	flog(LOG_ERR, "htls: could not open directory `%s': %s", dname, strerror(errno));
	simpleerror(1, 500, "Server Error", "Could not produce directory index.");
	exit(1);
    }
    checkcache(&sb);
    
    bufinit(buf);
    head(argv[optind + 1], &buf);
    mkindex(dname, dir, &buf);
    foot(&buf);
    closedir(dir);
    
    printf("HTTP/1.1 200 OK\n");
    printf("Content-Type: text/html; charset=UTF-8\n");
    printf("Last-Modified: %s\n", fmthttpdate(sb.st_mtime));
    printf("Content-Length: %zi\n", buf.d);
    printf("\n");
    fwrite(buf.b, 1, buf.d, stdout);
    buffree(buf);
    return(0);
}
