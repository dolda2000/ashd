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
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>

struct cache {
    struct cache *next, *prev;
    char *user, *pass;
    time_t lastuse;
};

static int ch;
static char **authcmd;
static char *realm;
static int docache = 1, reqssl;
static struct cache *cache;
static time_t now, lastclean;

static int auth(struct hthead *req, int fd, char *user, char *pass);

static void reqauth(struct hthead *req, int fd)
{
    struct charbuf buf;
    FILE *out;
    char *rn;
    
    rn = realm;
    if(rn == NULL)
	rn = "auth";
    bufinit(buf);
    bufcatstr(buf, "<?xml version=\"1.0\" encoding=\"US-ASCII\"?>\r\n");
    bufcatstr(buf, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\r\n");
    bufcatstr(buf, "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en-US\" xml:lang=\"en-US\">\r\n");
    bufcatstr(buf, "<head>\r\n");
    bprintf(&buf, "<title>Authentication Required</title>\r\n");
    bufcatstr(buf, "</head>\r\n");
    bufcatstr(buf, "<body>\r\n");
    bprintf(&buf, "<h1>Authentication Required</h1>\r\n");
    bprintf(&buf, "<p>You need to authenticate to access the requested resource.</p>\r\n");
    bufcatstr(buf, "</body>\r\n");
    bufcatstr(buf, "</html>\r\n");
    out = fdopen(dup(fd), "w");
    fprintf(out, "HTTP/1.1 401 Authentication Required\n");
    fprintf(out, "WWW-Authenticate: Basic realm=\"%s\"\n", rn);
    fprintf(out, "Content-Type: text/html\n");
    fprintf(out, "Content-Length: %zi\n", buf.d);
    fprintf(out, "\n");
    fwrite(buf.b, 1, buf.d, out);
    fclose(out);
    buffree(buf);
}

static void cleancache(int complete)
{
    struct cache *c, *n;
    
    for(c = cache; c != NULL; c = n) {
	n = c->next;
	if(complete || (now - c->lastuse > 3600)) {
	    if(c->next)
		c->next->prev = c->prev;
	    if(c->prev)
		c->prev->next = c->next;
	    if(c == cache)
		cache = c->next;
	    memset(c->pass, 0, strlen(c->pass));
	    free(c->user);
	    free(c->pass);
	    free(c);
	}
    }
    lastclean = now;
}

static int ckcache(char *user, char *pass)
{
    struct cache *c;
    
    for(c = cache; c != NULL; c = c->next) {
	if(!strcmp(user, c->user) && !strcmp(pass, c->pass)) {
	    c->lastuse = now;
	    return(1);
	}
    }
    return(0);
}

static struct cache *addcache(char *user, char *pass)
{
    struct cache *c;
    
    omalloc(c);
    c->user = sstrdup(user);
    c->pass = sstrdup(pass);
    c->lastuse = now;
    c->next = cache;
    if(cache != NULL)
	cache->prev = c;
    cache = c;
    return(c);
}

static void serve2(struct hthead *req, int fd, char *user)
{
    headappheader(req, "X-Ash-Remote-User", user);
    if(sendreq(ch, req, fd)) {
	flog(LOG_ERR, "htextauth: could not pass request to child: %s", strerror(errno));
	exit(1);
    }
}

static void serve(struct hthead *req, int fd)
{
    char *raw, *dec, *p;
    size_t declen;
    
    now = time(NULL);
    dec = NULL;
    if(reqssl && (((raw = getheader(req, "X-Ash-Protocol")) == NULL) || strcmp(raw, "https"))) {
	simpleerror(fd, 403, "Forbidden", "The requested resource must be requested over HTTPS.");
	goto out;
    }
    if(((raw = getheader(req, "Authorization")) == NULL) || strncasecmp(raw, "basic ", 6)) {
	reqauth(req, fd);
	goto out;
    }
    if((dec = base64decode(raw + 6, &declen)) == NULL) {
	simpleerror(fd, 400, "Invalid request", "The authentication data is not proper base64.");
	goto out;
    }
    memset(raw, 0, strlen(raw));
    headrmheader(req, "Authorization");
    for(p = dec; *p; p++) {
	if(*p < 32) {
	    simpleerror(fd, 400, "Invalid request", "The authentication data is invalid.");
	    goto out;
	}
    }
    if((p = strchr(dec, ':')) == NULL) {
	simpleerror(fd, 400, "Invalid request", "The authentication data is invalid.");
	goto out;
    }
    *(p++) = 0;
    if(docache && ckcache(dec, p)) {
	serve2(req, fd, dec);
	goto out;
    }
    if(auth(req, fd, dec, p)) {
	if(docache)
	    addcache(dec, p);
	serve2(req, fd, dec);
	goto out;
    }
    
out:
    if(dec != NULL) {
	memset(dec, 0, declen);
	free(dec);
    }
    if(docache && (now - lastclean > 60))
	cleancache(0);
}

static int auth(struct hthead *req, int fd, char *user, char *pass)
{
    int i, rv, status;
    ssize_t len;
    char *msg;
    struct charbuf ebuf;
    pid_t pid;
    int pfd[2], efd[2];
    FILE *out;
    
    rv = 0;
    msg = "The supplied credentials are invalid.";
    pipe(pfd);
    pipe(efd);
    if((pid = fork()) < 0) {
	flog(LOG_ERR, "htextauth: could not fork: %s", strerror(errno));
	simpleerror(fd, 500, "Server Error", "An internal error occurred.");
	close(pfd[0]); close(pfd[1]);
	close(efd[0]); close(efd[1]);
	return(0);
    }
    if(pid == 0) {
	dup2(pfd[0], 0);
	dup2(efd[1], 1);
	for(i = 3; i < FD_SETSIZE; i++)
	    close(i);
	execvp(authcmd[0], authcmd);
	flog(LOG_ERR, "htextauth: could not exec %s: %s", authcmd[0], strerror(errno));
	exit(127);
    }
    close(pfd[0]);
    close(efd[1]);
    out = fdopen(pfd[1], "w");
    fprintf(out, "%s\n", user);
    fprintf(out, "%s\n", pass);
    fclose(out);
    bufinit(ebuf);
    while(1) {
	sizebuf(ebuf, ebuf.d + 128);
	len = read(efd[0], ebuf.b + ebuf.d, ebuf.s - ebuf.d);
	if(len < 0) {
	    if(errno == EINTR)
		continue;
	    break;
	} else if(len == 0) {
	    break;
	}
	ebuf.d += len;
    }
    if(ebuf.d > 0) {
	bufadd(ebuf, 0);
	msg = ebuf.b;
    }
    close(efd[0]);
    if(waitpid(pid, &status, 0) < 0) {
	flog(LOG_ERR, "htextauth: could not wait: %s", strerror(errno));
	simpleerror(fd, 500, "Server Error", "An internal error occurred.");
	buffree(ebuf);
	return(0);
    }
    if(WIFEXITED(status) && (WEXITSTATUS(status) == 0))
	rv = 1;
    else
	simpleerror(fd, 401, "Invalid authentication", msg);
    buffree(ebuf);
    return(rv);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: htextauth [-hCs] [-r REALM] AUTHCMD [ARGS...] -- CHILD [ARGS...]\n");
}

static void sighandler(int sig)
{
}

int main(int argc, char **argv)
{
    int i, c, ret;
    struct charvbuf cbuf;
    struct pollfd pfd[2];
    struct hthead *req;
    int fd;
    
    while((c = getopt(argc, argv, "+hCsr:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    return(0);
	case 'C':
	    docache = 0;
	    break;
	case 's':
	    reqssl = 1;
	    break;
	case 'r':
	    realm = optarg;
	    break;
	default:
	    usage(stderr);
	    return(1);
	}
    }
    bufinit(cbuf);
    for(i = optind; i < argc; i++) {
	if(!strcmp(argv[i], "--"))
	    break;
	bufadd(cbuf, argv[i]);
    }
    if((cbuf.d == 0) || (i == argc)) {
	usage(stderr);
	return(1);
    }
    bufadd(cbuf, NULL);
    authcmd = cbuf.b;
    i++;
    if(i == argc) {
	usage(stderr);
	return(1);
    }
    if((ch = stdmkchild(argv + i, NULL, NULL)) < 0) {
	flog(LOG_ERR, "htextauth: could not fork child: %s", strerror(errno));
	return(1);
    }
    signal(SIGCHLD, sighandler);
    signal(SIGPIPE, sighandler);
    while(1) {
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = 0;
	pfd[0].events = POLLIN;
	pfd[1].fd = ch;
	pfd[1].events = POLLHUP;
	if((ret = poll(pfd, 2, -1)) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "htextauth: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	if(pfd[0].revents) {
	    if((fd = recvreq(0, &req)) < 0) {
		if(errno == 0)
		    break;
		flog(LOG_ERR, "htextauth: error in recvreq: %s", strerror(errno));
		exit(1);
	    }
	    serve(req, fd);
	    freehthead(req);
	    close(fd);
	}
	if(pfd[1].revents & POLLHUP)
	    break;
    }
    cleancache(1);
    return(0);
}
