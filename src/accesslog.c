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
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

#define DEFFORMAT "%{%Y-%m-%d %H:%M:%S}t %m %u %A \"%G\""

static int ch;
static char *outname = NULL;
static FILE *out;
static int flush = 1, locklog = 1;
static char *format;
static struct timeval now;
static volatile int reopen = 0;

static void qputs(char *s, FILE *o)
{
    for(; *s; s++) {
	if(*s == '\"') {
	    fputs("\\\"", o);
	} else if(*s == '\\') {
	    fputs("\\\\", o);
	} else if(*s == '\n') {
	    fputs("\\n", o);
	} else if(*s == '\t') {
	    fputs("\\t", o);
	} else if((*s < 32) || (*s >= 128)) {
	    fprintf(o, "\\x%02x", (int)(unsigned char)*s);
	} else {
	    fputc(*s, o);
	}
    }
}

static void logitem(struct hthead *req, char o, char *d)
{
    char *h, *p;
    char buf[1024];
    
    switch(o) {
    case '%':
	putc('%', out);
	break;
    case 'h':
	if((h = getheader(req, d)) == NULL) {
	    putc('-', out);
	} else {
	    qputs(h, out);
	}
	break;
    case 'u':
	qputs(req->url, out);
	break;
    case 'U':
	strcpy(buf, req->url);
	if((p = strchr(buf, '?')) != NULL)
	    *p = 0;
	qputs(buf, out);
	break;
    case 'm':
	qputs(req->method, out);
	break;
    case 'r':
	qputs(req->rest, out);
	break;
    case 'v':
	qputs(req->ver, out);
	break;
    case 't':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, localtime(&now.tv_sec));
	qputs(buf, out);
	break;
    case 'T':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, gmtime(&now.tv_sec));
	qputs(buf, out);
	break;
    case 's':
	fprintf(out, "%06i", (int)now.tv_usec);
	break;
    case 'A':
	logitem(req, 'h', "X-Ash-Address");
	break;
    case 'H':
	logitem(req, 'h', "Host");
	break;
    case 'R':
	logitem(req, 'h', "Referer");
	break;
    case 'G':
	logitem(req, 'h', "User-Agent");
	break;
    }
}

static void logreq(struct hthead *req)
{
    char *p, *p2;
    char d[strlen(format)];
    char o;
    
    p = format;
    while(*p) {
	if(*p == '%') {
	    p++;
	    if(*p == '{') {
		p++;
		if((p2 = strchr(p, '}')) == NULL)
		    continue;
		memcpy(d, p, p2 - p);
		d[p2 - p] = 0;
		p = p2 + 1;
	    } else {
		d[0] = 0;
	    }
	    o = *p++;
	    if(o == 0)
		break;
	    logitem(req, o, d);
	} else {
	    fputc(*p++, out);
	}
    }
    fputc('\n', out);
    if(flush)
	fflush(out);
}

static void serve(struct hthead *req, int fd)
{
    gettimeofday(&now, NULL);
    if(sendreq(ch, req, fd)) {
	flog(LOG_ERR, "accesslog: could not pass request to child: %s", strerror(errno));
	exit(1);
    }
    logreq(req);
}

static void sighandler(int sig)
{
    if(sig == SIGHUP)
	reopen = 1;
}

static int lockfile(FILE *file)
{
    struct flock ld;
    
    memset(&ld, 0, sizeof(ld));
    ld.l_type = F_WRLCK;
    ld.l_whence = SEEK_SET;
    ld.l_start = 0;
    ld.l_len = 0;
    return(fcntl(fileno(file), F_SETLK, &ld));
}

static void fetchpid(char *filename)
{
    int fd, ret;
    struct flock ld;
    
    if((fd = open(filename, O_WRONLY)) < 0) {
	fprintf(stderr, "accesslog: %s: %s\n", filename, strerror(errno));
	exit(1);
    }
    memset(&ld, 0, sizeof(ld));
    ld.l_type = F_WRLCK;
    ld.l_whence = SEEK_SET;
    ld.l_start = 0;
    ld.l_len = 0;
    ret = fcntl(fd, F_GETLK, &ld);
    close(fd);
    if(ret) {
	fprintf(stderr, "accesslog: %s: %s\n", filename, strerror(errno));
	exit(1);
    }
    if(ld.l_type == F_UNLCK) {
	fprintf(stderr, "accesslog: %s: not locked\n", filename);
	exit(1);
    }
    printf("%i\n", (int)ld.l_pid);
}

static void reopenlog(void)
{
    FILE *new;
    struct stat olds, news;
    
    if(outname == NULL) {
	flog(LOG_WARNING, "accesslog: received SIGHUP but logging to stdout, so ignoring");
	return;
    }
    if(locklog) {
	if(fstat(fileno(out), &olds)) {
	    flog(LOG_ERR, "accesslog: could not stat current logfile(?!): %s", strerror(errno));
	    return;
	}
	if(!stat(outname, &news)) {
	    if((olds.st_dev == news.st_dev) && (olds.st_ino == news.st_ino)) {
		/*
		 * This needs to be ignored, because if the same logfile
		 * is opened and then closed, the lock is lost. To quote
		 * the Linux fcntl(2) manpage: "This is bad." No kidding.
		 *
		 * Technically, there is a race condition here when the
		 * file has been stat'ed but not yet opened, where the old
		 * log file, having been previously renamed, changes name
		 * back to the name accesslog knows and is thus reopened
		 * regardlessly, but I think that might fit under the
		 * idiom "pathological case". It should, at least, not be
		 * a security problem.
		 */
		flog(LOG_INFO, "accesslog: received SIGHUP, but logfile has not changed, so ignoring");
		return;
	    }
	}
    }
    if((new = fopen(outname, "a")) == NULL) {
	flog(LOG_WARNING, "accesslog: could not reopen log file `%s' on SIGHUP: %s", outname, strerror(errno));
	return;
    }
    fcntl(fileno(new), F_SETFD, FD_CLOEXEC);
    if(locklog) {
	if(lockfile(new)) {
	    if((errno == EAGAIN) || (errno == EACCES)) {
		flog(LOG_ERR, "accesslog: logfile is already locked; reverting to current log", strerror(errno));
		fclose(new);
		return;
	    } else {
		flog(LOG_WARNING, "accesslog: could not lock logfile, so no lock will be held: %s", strerror(errno));
	    }
	}
    }
    fclose(out);
    out = new;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: accesslog [-hFaL] [-f FORMAT] [-p PIDFILE] OUTFILE CHILD [ARGS...]\n");
    fprintf(out, "       accesslog -P LOGFILE\n");
}

int main(int argc, char **argv)
{
    int c, ret;
    struct hthead *req;
    int fd;
    struct pollfd pfd[2];
    char *pidfile;
    FILE *pidout;
    
    pidfile = NULL;
    while((c = getopt(argc, argv, "+hFaLf:p:P:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'F':
	    flush = 0;
	    break;
	case 'L':
	    locklog = 0;
	    break;
	case 'f':
	    format = optarg;
	    break;
	case 'P':
	    fetchpid(optarg);
	    exit(0);
	case 'p':
	    pidfile = optarg;
	    break;
	case 'a':
	    format = "%A - - [%{%d/%b/%Y:%H:%M:%S %z}t] \"%m %u %v\" - - \"%R\" \"%G\"";
	    break;
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(argc - optind < 2) {
	usage(stderr);
	exit(1);
    }
    if(format == NULL)
	format = DEFFORMAT;
    if(!strcmp(argv[optind], "-"))
	outname = NULL;
    else
	outname = argv[optind];
    if(outname == NULL) {
	out = stdout;
	locklog = 0;
    } else {
	if((out = fopen(argv[optind], "a")) == NULL) {
	    flog(LOG_ERR, "accesslog: could not open %s for logging: %s", argv[optind], strerror(errno));
	    exit(1);
	}
	fcntl(fileno(out), F_SETFD, FD_CLOEXEC);
    }
    if(locklog) {
	if(lockfile(out)) {
	    if((errno == EAGAIN) || (errno == EACCES)) {
		flog(LOG_ERR, "accesslog: logfile is already locked", strerror(errno));
		exit(1);
	    } else {
		flog(LOG_WARNING, "accesslog: could not lock logfile: %s", strerror(errno));
	    }
	}
    }
    if((ch = stdmkchild(argv + optind + 1, NULL, NULL)) < 0) {
	flog(LOG_ERR, "accesslog: could not fork child: %s", strerror(errno));
	exit(1);
    }
    signal(SIGHUP, sighandler);
    if(pidfile) {
	if(!strcmp(pidfile, "-")) {
	    if(!outname) {
		flog(LOG_ERR, "accesslog: cannot derive PID file name without an output file");
		exit(1);
	    }
	    pidfile = sprintf2("%s.pid", outname);
	}
	if((pidout = fopen(pidfile, "w")) == NULL) {
	    flog(LOG_ERR, "accesslog: could not open PID file %s for writing: %s", pidfile);
	    exit(1);
	}
	fprintf(pidout, "%i\n", (int)getpid());
	fclose(pidout);
    }
    while(1) {
	if(reopen) {
	    reopenlog();
	    reopen = 0;
	}
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = 0;
	pfd[0].events = POLLIN;
	pfd[1].fd = ch;
	pfd[1].events = POLLHUP;
	if((ret = poll(pfd, 2, -1)) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "accesslog: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	if(pfd[0].revents) {
	    if((fd = recvreq(0, &req)) < 0) {
		if(errno == 0)
		    break;
		flog(LOG_ERR, "accesslog: error in recvreq: %s", strerror(errno));
		exit(1);
	    }
	    serve(req, fd);
	    freehthead(req);
	    close(fd);
	}
	if(pfd[1].revents & POLLHUP)
	    break;
    }
    fclose(out);
    if(pidfile != NULL)
	unlink(pidfile);
    return(0);
}
