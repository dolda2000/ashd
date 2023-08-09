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
#include <stdint.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <mt.h>
#include <mtio.h>
#include <bufio.h>

#define DEFFORMAT "%{%Y-%m-%d %H:%M:%S}t %m %u %A \"%G\""

static struct logdata {
    struct hthead *req, *resp;
    struct timeval start, end;
    off_t bytesin, bytesout;
} defdata = {
    .bytesin = -1,
    .bytesout = -1,
};

static int ch, filter;
static char *outname = NULL;
static FILE *out;
static int flush = 1, locklog = 1;
static char *format;
static volatile int reopen = 0;

static void qputs(char *sp, FILE *o)
{
    unsigned char *s = (unsigned char *)sp;
    
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
	    fprintf(o, "\\x%02x", (int)*s);
	} else {
	    fputc(*s, o);
	}
    }
}

static void logitem(struct logdata *data, char o, char *d)
{
    char *h, *p;
    char buf[1024];
    
    switch(o) {
    case '%':
	putc('%', out);
	break;
    case 'h':
	if((h = getheader(data->req, d)) == NULL) {
	    putc('-', out);
	} else {
	    qputs(h, out);
	}
	break;
    case 'p':
	if(!data->resp || ((h = getheader(data->req, d)) == NULL)) {
	    putc('-', out);
	} else {
	    qputs(h, out);
	}
	break;
    case 'P':
	logitem(data, 'p', sprintf3("X-Ash-%s", d));
	break;
    case 'u':
	qputs(data->req->url, out);
	break;
    case 'U':
	strncpy(buf, data->req->url, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	if((p = strchr(buf, '?')) != NULL)
	    *p = 0;
	qputs(buf, out);
	break;
    case 'm':
	qputs(data->req->method, out);
	break;
    case 'r':
	qputs(data->req->rest, out);
	break;
    case 'v':
	qputs(data->req->ver, out);
	break;
    case 't':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, localtime(&data->start.tv_sec));
	qputs(buf, out);
	break;
    case 'T':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, gmtime(&data->start.tv_sec));
	qputs(buf, out);
	break;
    case 's':
	fprintf(out, "%06i", (int)data->start.tv_usec);
	break;
    case 'c':
	if(!data->resp)
	    putc('-', out);
	else
	    fprintf(out, "%i", data->resp->code);
	break;
    case 'i':
	if(data->bytesin < 0)
	    putc('-', out);
	else
	    fprintf(out, "%ji", (intmax_t)data->bytesin);
	break;
    case 'o':
	if(data->bytesout < 0)
	    putc('-', out);
	else
	    fprintf(out, "%ji", (intmax_t)data->bytesout);
	break;
    case 'd':
	if((data->end.tv_sec == 0) && (data->end.tv_usec == 0))
	    fputc('-', out);
	else
	    fprintf(out, "%.6f", (data->end.tv_sec - data->start.tv_sec) + ((data->end.tv_usec - data->start.tv_usec) / 1000000.0));
	break;
    case 'A':
	logitem(data, 'h', "X-Ash-Address");
	break;
    case 'H':
	logitem(data, 'h', "Host");
	break;
    case 'R':
	logitem(data, 'h', "Referer");
	break;
    case 'G':
	logitem(data, 'h', "User-Agent");
	break;
    }
}

static void logreq(struct logdata *data)
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
	    logitem(data, o, d);
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
    struct logdata data;
    
    data = defdata;
    data.req = req;
    gettimeofday(&data.start, NULL);
    if(sendreq(ch, req, fd)) {
	flog(LOG_ERR, "accesslog: could not pass request to child: %s", strerror(errno));
	exit(1);
    }
    logreq(&data);
}

static int passdata(struct bufio *in, struct bufio *out, off_t *passed)
{
    ssize_t read;
    off_t total;
    
    total = 0;
    while(!bioeof(in)) {
	if((read = biordata(in)) > 0) {
	    if((read = biowritesome(out, in->rbuf.b + in->rh, read)) < 0)
		return(-1);
	    in->rh += read;
	    total += read;
	}
	if(biorspace(in) && (biofillsome(in) < 0))
	    return(-1);
    }
    if(passed)
	*passed = total;
    return(0);
}

static void filterreq(struct muth *mt, va_list args)
{
    vavar(struct hthead *, req);
    vavar(int, fd);
    int pfds[2];
    struct hthead *resp;
    struct bufio *cl, *hd;
    struct stdiofd *cli, *hdi;
    struct logdata data;
    
    hd = NULL;
    resp = NULL;
    data = defdata;
    data.req = req;
    gettimeofday(&data.start, NULL);
    cl = mtbioopen(fd, 1, 600, "r+", &cli);
    if(socketpair(PF_UNIX, SOCK_STREAM, 0, pfds))
	goto out;
    hd = mtbioopen(pfds[1], 1, 600, "r+", &hdi);
    if(sendreq(ch, req, pfds[0])) {
	close(pfds[0]);
	goto out;
    }
    close(pfds[0]);
    
    if(passdata(cl, hd, &data.bytesin))
	goto out;
    if(bioflush(hd))
	goto out;
    shutdown(pfds[1], SHUT_WR);
    if((resp = parseresponseb(hd)) == NULL)
	goto out;
    cli->sendrights = hdi->rights;
    hdi->rights = -1;
    data.resp = resp;
    writerespb(cl, resp);
    bioprintf(cl, "\r\n");
    if(passdata(hd, cl, &data.bytesout))
	goto out;
    gettimeofday(&data.end, NULL);
    
out:
    logreq(&data);
    
    freehthead(req);
    if(resp != NULL)
	freehthead(resp);
    bioclose(cl);
    if(hd != NULL)
	bioclose(hd);
}

static void sighandler(int sig)
{
    if(sig == SIGHUP) {
	if(filter)
	    exitioloop(2);
	else
	    reopen = 1;
    }
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

static void listenloop(struct muth *mt, va_list args)
{
    vavar(int, lfd);
    int fd;
    struct hthead *req;
    
    while(1) {
	block(lfd, EV_READ, 0);
	if((fd = recvreq(lfd, &req)) < 0) {
	    if(errno != 0)
		flog(LOG_ERR, "accesslog: error in recvreq: %s", strerror(errno));
	    exit(1);
	}
	mustart(filterreq, req, fd);
    }
}

static void chwatch(struct muth *mt, va_list args)
{
    vavar(int, cfd);
    
    block(cfd, EV_READ, 0);
    exitioloop(1);
}

static void floop(void)
{
    mustart(listenloop, 0);
    mustart(chwatch, ch);
    while(1) {
	switch(ioloop()) {
	case 0:
	case 1:
	    return;
	case 2:
	    reopenlog();
	    break;
	}
    }
}

static void sloop(void)
{
    int fd, ret;
    struct hthead *req;
    struct pollfd pfd[2];
    
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
		    return;
		flog(LOG_ERR, "accesslog: error in recvreq: %s", strerror(errno));
		exit(1);
	    }
	    serve(req, fd);
	    freehthead(req);
	    close(fd);
	}
	if(pfd[1].revents & POLLHUP)
	    return;
    }
}

static void usage(FILE *out)
{
    fprintf(out, "usage: accesslog [-hFaL] [-f FORMAT] [-p PIDFILE] OUTFILE CHILD [ARGS...]\n");
    fprintf(out, "       accesslog -P LOGFILE\n");
}

int main(int argc, char **argv)
{
    int c;
    char *pidfile;
    FILE *pidout;
    
    pidfile = NULL;
    while((c = getopt(argc, argv, "+hFaeLf:p:P:")) >= 0) {
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
	case 'e':
	    filter = 1;
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
	    format = "%A - %{log-user}P [%{%d/%b/%Y:%H:%M:%S %z}t] \"%m %u %v\" %c %o \"%R\" \"%G\"";
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
    if(filter)
	floop();
    else
	sloop();
    fclose(out);
    if(pidfile != NULL)
	unlink(pidfile);
    return(0);
}
