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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/signal.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>
#include <cf.h>

#include "dirplex.h"

time_t now;

static void chinit(void *idata)
{
    char *twd = idata;
    
    if(twd != NULL) {
	/* This should never be able to fail other than for critical
	 * I/O errors or some such, since the path has already been
	 * traversed. */
	if(chdir(twd))
	    exit(127);
    }
}

static void childerror(struct hthead *req, int fd)
{
    if(errno == EAGAIN)
	simpleerror(fd, 500, "Server Error", "The request handler is overloaded.");
    else
	simpleerror(fd, 500, "Server Error", "The request handler crashed.");
}

static void handle(struct hthead *req, int fd, char *path, struct pattern *pat)
{
    struct child *ch;
    struct config *ccf;
    struct headmod *head;
    char *twd;

    for(head = pat->headers; head != NULL; head = head->next) {
	headrmheader(req, head->name);
	headappheader(req, head->name, head->value);
    }
    if(!strncmp(path, "./", 2) && path[2])
	path += 2;
    if(pat->fchild) {
	headappheader(req, "X-Ash-File", path);
	stdforkserve(pat->fchild, req, fd, NULL, NULL);
    } else {
	if((ch = findchild(path, pat->childnm, &ccf)) == NULL) {
	    flog(LOG_ERR, "child %s requested, but was not declared", pat->childnm);
	    simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", pat->childnm);
	    return;
	}
	if((twd = ccf?ccf->path:NULL) != NULL) {
	    if(!strcmp(twd, ".")) {
		twd = NULL;
	    } else if(strncmp(path, twd, strlen(twd)) || (path[strlen(twd)] != '/')) {
		/* Should be an impossible case under the current (and
		 * foreseeable) scheme. */
		simpleerror(fd, 500, "Server Error", "An internal server error occurred.");
		return;
	    } else {
		path = path + strlen(twd) + 1;
	    }
	}
	headappheader(req, "X-Ash-File", path);
	if(childhandle(ch, req, fd, chinit, twd))
	    childerror(req, fd);
    }
}

static void handle404(struct hthead *req, int fd, char *path)
{
    struct child *ch;
    struct config *ccf;
    char *tmp;
    
    tmp = sstrdup(path);
    ch = findchild(tmp, ".notfound", &ccf);
    if(childhandle(ch, req, fd, chinit, ccf?ccf->path:NULL))
	childerror(req, fd);
    free(tmp);
}

static void handlefile(struct hthead *req, int fd, char *path)
{
    struct pattern *pat;

    if((pat = findmatch(path, 0, 0)) == NULL) {
	handle404(req, fd, path);
	return;
    }
    handle(req, fd, path, pat);
}

static char *findfile(char *path, char *name, struct stat *sb)
{
    DIR *dir;
    struct stat sbuf;
    struct dirent *dent;
    char *p, *fp, *ret;
    
    if(sb == NULL)
	sb = &sbuf;
    if((dir = opendir(path)) == NULL)
	return(NULL);
    ret = NULL;
    while((dent = readdir(dir)) != NULL) {
	/* Ignore backup files.
	 * XXX: There is probably a better and more extensible way to
	 * do this. */
	if(dent->d_name[strlen(dent->d_name) - 1] == '~')
	    continue;
	if((p = strchr(dent->d_name, '.')) == NULL)
	    continue;
	if(p - dent->d_name != strlen(name))
	    continue;
	if(strncmp(dent->d_name, name, strlen(name)))
	    continue;
	fp = sprintf3("%s/%s", path, dent->d_name);
	if(stat(fp, sb))
	    continue;
	if(!S_ISREG(sb->st_mode))
	    continue;
	ret = sstrdup(fp);
	break;
    }
    closedir(dir);
    return(ret);
}

static void handledir(struct hthead *req, int fd, char *path)
{
    struct config **cfs;
    int i, o;
    struct stat sb;
    char *inm, *ipath, *cpath;
    struct pattern *pat;
    
    cpath = sprintf2("%s/", path);
    cfs = getconfigs(cpath);
    for(i = 0; cfs[i] != NULL; i++) {
	if(cfs[i]->index != NULL) {
	    for(o = 0; cfs[i]->index[o] != NULL; o++) {
		inm = cfs[i]->index[o];
		ipath = sprintf2("%s/%s", path, inm);
		if(!stat(ipath, &sb) && S_ISREG(sb.st_mode)) {
		    handlefile(req, fd, ipath);
		    free(ipath);
		    goto out;
		}
		free(ipath);
		
		if(!strchr(inm, '.') && ((ipath = findfile(path, inm, NULL)) != NULL)) {
		    handlefile(req, fd, ipath);
		    free(ipath);
		    goto out;
		}
	    }
	    break;
	}
    }
    if((pat = findmatch(cpath, 0, 1)) != NULL) {
	handle(req, fd, cpath, pat);
	goto out;
    }
    simpleerror(fd, 403, "Not Authorized", "Will not send listings for this directory.");
    
out:
    free(cpath);
}

static int checkpath(struct hthead *req, int fd, char *path, char *rest);

static int checkentry(struct hthead *req, int fd, char *path, char *rest, char *el)
{
    struct stat sb;
    char *newpath;
    int rv;
    
    if(*el == '.') {
	handle404(req, fd, sprintf3("%s/", path));
	return(1);
    }
    if(!stat(sprintf3("%s/%s", path, el), &sb)) {
	if(S_ISDIR(sb.st_mode)) {
	    if(!*rest) {
		stdredir(req, fd, 301, sprintf3("%s/", el));
		return(1);
	    }
	    newpath = sprintf2("%s/%s", path, el);
	    rv = checkpath(req, fd, newpath, rest + 1);
	    free(newpath);
	    return(rv);
	} else if(S_ISREG(sb.st_mode)) {
	    newpath = sprintf2("%s/%s", path, el);
	    replrest(req, rest);
	    handlefile(req, fd, newpath);
	    free(newpath);
	    return(1);
	}
	handle404(req, fd, sprintf3("%s/", path));
	return(1);
    }
    if(!strchr(el, '.') && ((newpath = findfile(path, el, NULL)) != NULL)) {
	replrest(req, rest);
	handlefile(req, fd, newpath);
	free(newpath);
	return(1);
    }
    return(0);
}

static int checkdir(struct hthead *req, int fd, char *path, char *rest)
{
    char *cpath;
    struct config *cf, *ccf;
    struct child *ch;
    
    cf = getconfig(path);
    if((cf->capture != NULL) && (cf->caproot || !cf->path || strcmp(cf->path, "."))) {
	cpath = sprintf2("%s/", path);
	if((ch = findchild(cpath, cf->capture, &ccf)) == NULL) {
	    free(cpath);
	    flog(LOG_ERR, "child %s requested for capture, but was not declared", cf->capture);
	    simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", cf->capture);
	    return(1);
	}
	free(cpath);
	if(*rest == '/')
	    rest++;
	replrest(req, rest);
	if(childhandle(ch, req, fd, chinit, ccf?ccf->path:NULL))
	    childerror(req, fd);
	return(1);
    }
    return(0);
}

static int checkpath(struct hthead *req, int fd, char *path, char *rest)
{
    char *p, *el;
    int rv;
    
    el = NULL;
    rv = 0;
    
    if(!strncmp(path, "./", 2))
	path += 2;
    if(checkdir(req, fd, path, rest))
	return(1);
    
    if((p = strchr(rest, '/')) == NULL) {
	el = unquoteurl(rest);
	rest = "";
    } else {
	char buf[p - rest + 1];
	memcpy(buf, rest, p - rest);
	buf[p - rest] = 0;
	el = unquoteurl(buf);
	rest = p;
    }
    if(el == NULL) {
	simpleerror(fd, 400, "Bad Request", "The requested URL contains an invalid escape sequence.");
	rv = 1;
	goto out;
    }
    if(strchr(el, '/') || (!*el && *rest)) {
	handle404(req, fd, sprintf3("%s/", path));
	rv = 1;
	goto out;
    }
    if(!*el) {
	replrest(req, rest);
	handledir(req, fd, path);
	rv = 1;
	goto out;
    }
    rv = checkentry(req, fd, path, rest, el);
    
out:
    if(el != NULL)
	free(el);
    return(rv);
}

static void serve(struct hthead *req, int fd)
{
    now = time(NULL);
    if(!checkpath(req, fd, ".", req->rest))
	handle404(req, fd, ".");
}

static void chldhandler(int sig)
{
    pid_t pid;
    int st;
    
    while((pid = waitpid(-1, &st, WNOHANG)) > 0) {
	if(WCOREDUMP(st))
	    flog(LOG_WARNING, "child process %i dumped core", pid);
    }
}

static void sighandler(int sig)
{
}

static void usage(FILE *out)
{
    fprintf(out, "usage: dirplex [-hN] [-c CONFIG] DIR\n");
}

int main(int argc, char **argv)
{
    int c;
    int nodef;
    char *gcf, *lcf, *clcf;
    struct hthead *req;
    int fd;
    
    nodef = 0;
    lcf = NULL;
    while((c = getopt(argc, argv, "hNc:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'N':
	    nodef = 1;
	    break;
	case 'c':
	    lcf = optarg;
	    break;
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(argc - optind < 1) {
	usage(stderr);
	exit(1);
    }
    if(!nodef) {
	if((gcf = findstdconf("ashd/dirplex.rc")) != NULL) {
	    gconfig = readconfig(gcf);
	    free(gcf);
	}
    }
    if(lcf != NULL) {
	if(strchr(lcf, '/') == NULL) {
	    if((clcf = findstdconf(sprintf3("ashd/%s", lcf))) == NULL) {
		flog(LOG_ERR, "could not find requested configuration `%s'", lcf);
		exit(1);
	    }
	    if((lconfig = readconfig(clcf)) == NULL)
		exit(1);
	    free(clcf);
	} else {
	    if((lconfig = readconfig(lcf)) == NULL)
		exit(1);
	}
    }
    if(chdir(argv[optind])) {
	flog(LOG_ERR, "could not change directory to %s: %s", argv[optind], strerror(errno));
	exit(1);
    }
    signal(SIGCHLD, chldhandler);
    signal(SIGPIPE, sighandler);
    while(1) {
	if((fd = recvreq(0, &req)) < 0) {
	    if(errno != 0)
		flog(LOG_ERR, "recvreq: %s", strerror(errno));
	    break;
	}
	serve(req, fd);
	freehthead(req);
	close(fd);
    }
    return(0);
}
