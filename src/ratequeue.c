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
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <assert.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <resp.h>
#include <proc.h>
#include <cf.h>

#define SBUCKETS 7

struct source {
    int type;
    char data[16];
    unsigned int len, hash;
};

struct waiting {
    struct hthead *req;
    int fd;
};

struct bucket {
    struct source id;
    double level, last, etime, wtime;
    typedbuf(struct waiting) brim;
    int thpos, blocked;
};

struct btime {
    struct bucket *bk;
    double tm;
};

struct config {
    double size, rate, retain, warnrate;
    int brimsize;
};

static struct bucket *sbuckets[1 << SBUCKETS];
static struct bucket **buckets = sbuckets;
static int hashlen = SBUCKETS, nbuckets = 0;
static typedbuf(struct btime) timeheap;
static int child, reload;
static double now;
static const struct config defcfg = {
    .size = 100, .rate = 10, .warnrate = 60,
    .retain = 10, .brimsize = 10,
};
static struct config cf;

static double rtime(void)
{
    static int init = 0;
    static struct timespec or;
    struct timespec ts;
    
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!init) {
	or = ts;
	init = 1;
    }
    return((ts.tv_sec - or.tv_sec) + ((ts.tv_nsec - or.tv_nsec) / 1000000000.0));
}

static struct source reqsource(struct hthead *req)
{
    int i;
    char *sa;
    struct in_addr a4;
    struct in6_addr a6;
    struct source ret;
    
    ret = (struct source){};
    if((sa = getheader(req, "X-Ash-Address")) != NULL) {
	if(inet_pton(AF_INET, sa, &a4) == 1) {
	    ret.type = AF_INET;
	    memcpy(ret.data, &a4, ret.len = sizeof(a4));
	} else if(inet_pton(AF_INET6, sa, &a6) == 1) {
	    ret.type = AF_INET6;
	    memcpy(ret.data, &a6, ret.len = sizeof(a6));
	}
    }
    for(i = 0, ret.hash = ret.type; i < ret.len; i++)
	ret.hash = (ret.hash * 31) + ret.data[i];
    return(ret);
}

static int srccmp(const struct source *a, const struct source *b)
{
    int c;
    
    if((c = a->len - b->len) != 0)
	return(c);
    if((c = a->type - b->type) != 0)
	return(c);
    return(memcmp(a->data, b->data, a->len));
}

static const char *formatsrc(const struct source *src)
{
    static char buf[128];
    struct in_addr a4;
    struct in6_addr a6;
    
    switch(src->type) {
    case AF_INET:
	memcpy(&a4, src->data, sizeof(a4));
	if(!inet_ntop(AF_INET, &a4, buf, sizeof(buf)))
	    return("<invalid ipv4>");
	return(buf);
    case AF_INET6:
	memcpy(&a6, src->data, sizeof(a6));
	if(!inet_ntop(AF_INET6, &a6, buf, sizeof(buf)))
	    return("<invalid ipv6>");
	return(buf);
    default:
	return("<invalid source record>");
    }
}

static void rehash(int nlen)
{
    unsigned int i, o, n, m, pl, nl;
    struct bucket **new, **old;
    
    old = buckets;
    if(nlen <= SBUCKETS) {
	nlen = SBUCKETS;
	new = sbuckets;
    } else {
	new = szmalloc(sizeof(*new) * (1 << nlen));
    }
    if(nlen == hashlen)
	return;
    if(new == sbuckets)
	memset(sbuckets, 0, sizeof(sbuckets));
    assert(old != new);
    pl = 1 << hashlen; nl = 1 << nlen; m = nl - 1;
    for(i = 0; i < pl; i++) {
	if(!old[i])
	    continue;
	for(o = old[i]->id.hash & m, n = 0; n < nl; o = (o + 1) & m, n++) {
	    if(!new[o]) {
		new[o] = old[i];
		break;
	    }
	}
    }
    if(old != sbuckets)
	free(old);
    buckets = new;
    hashlen = nlen;
}

static struct bucket *hashget(const struct source *src)
{
    unsigned int i, n, N, m;
    struct bucket *bk;
    
    m = (N = (1 << hashlen)) - 1;
    for(i = src->hash & m, n = 0; n < N; i = (i + 1) & m, n++) {
	bk = buckets[i];
	if(bk && !srccmp(&bk->id, src))
	    return(bk);
    }
    for(i = src->hash & m; buckets[i]; i = (i + 1) & m);
    buckets[i] = bk = szmalloc(sizeof(*bk));
    memcpy(&bk->id, src, sizeof(*src));
    bk->last = bk->etime = now;
    bk->thpos = -1;
    bk->blocked = -1;
    if(++nbuckets > (1 << (hashlen - 1)))
	rehash(hashlen + 1);
    return(bk);
}

static void hashdel(struct bucket *bk)
{
    unsigned int i, o, p, n, N, m;
    struct bucket *sb;
    
    m = (N = (1 << hashlen)) - 1;
    for(i = bk->id.hash & m, n = 0; n < N; i = (i + 1) & m, n++) {
	assert((sb = buckets[i]) != NULL);
	if(!srccmp(&sb->id, &bk->id))
	    break;
    }
    assert(sb == bk);
    buckets[i] = NULL;
    for(o = (i + 1) & m; buckets[o] != NULL; o = (o + 1) & m) {
	sb = buckets[o];
	p = (sb->id.hash - i) & m;
	if((p == 0) || (p > ((o - i) & m))) {
	    buckets[i] = sb;
	    buckets[o] = NULL;
	    i = o;
	}
    }
    if(--nbuckets <= (1 << (hashlen - 3)))
	rehash(hashlen - 1);
}

static void thraise(struct btime bt, int n)
{
    int p;
    
    while(n > 0) {
	p = (n - 1) >> 1;
	if(timeheap.b[p].tm <= bt.tm)
	    break;
	(timeheap.b[n] = timeheap.b[p]).bk->thpos = n;
	n = p;
    }
    (timeheap.b[n] = bt).bk->thpos = n;
}

static void thlower(struct btime bt, int n)
{
    int c1, c2, c;
    
    while(1) {
	c2 = (c1 = (n << 1) + 1) + 1;
	if(c1 >= timeheap.d)
	    break;
	c = ((c2 < timeheap.d) && (timeheap.b[c2].tm < timeheap.b[c1].tm)) ? c2 : c1;
	if(timeheap.b[c].tm > bt.tm)
	    break;
	(timeheap.b[n] = timeheap.b[c]).bk->thpos = n;
	n = c;
    }
    (timeheap.b[n] = bt).bk->thpos = n;
}

static void thadjust(struct btime bt, int n)
{
    if((n > 0) && (timeheap.b[(n - 1) >> 1].tm > bt.tm))
	thraise(bt, n);
    else
	thlower(bt, n);
}

static void freebucket(struct bucket *bk)
{
    int i, n;
    struct btime r;
    
    hashdel(bk);
    if((n = bk->thpos) >= 0) {
	r = timeheap.b[--timeheap.d];
	if(n < timeheap.d)
	    thadjust(r, n);
    }
    for(i = 0; i < bk->brim.d; i++) {
	freehthead(bk->brim.b[i].req);
	close(bk->brim.b[i].fd);
    }
    buffree(bk->brim);
    free(bk);
}

static void updbtime(struct bucket *bk)
{
    double tm, tm2;
    
    tm = (bk->level == 0) ? (bk->etime + cf.retain) : (bk->last + (bk->level / cf.rate) + cf.retain);
    if((bk->blocked > 0) && ((tm2 = bk->wtime + cf.warnrate) > tm))
	tm = tm2;
    
    if((bk->brim.d > 0) && ((tm2 = bk->last + ((bk->level - cf.size) / cf.rate)) < tm))
	tm = tm2;
    if((bk->blocked > 0) && ((tm2 = bk->wtime + cf.warnrate) < tm))
	tm = tm2;
    
    if(bk->thpos < 0) {
	sizebuf(timeheap, ++timeheap.d);
	thraise((struct btime){bk, tm}, timeheap.d - 1);
    } else {
	thadjust((struct btime){bk, tm}, bk->thpos);
    }
}

static void tickbucket(struct bucket *bk)
{
    double delta, ll;
    
    delta = now - bk->last;
    bk->last = now;
    ll = bk->level;
    if((bk->level -= delta * cf.rate) < 0) {
	if(ll > 0)
	    bk->etime = now + (bk->level / cf.rate);
	bk->level = 0;
    }
    while((bk->brim.d > 0) && (bk->level < cf.size)) {
	if(sendreq(child, bk->brim.b[0].req, bk->brim.b[0].fd)) {
	    flog(LOG_ERR, "ratequeue: could not pass request to child: %s", strerror(errno));
	    exit(1);
	}
	freehthead(bk->brim.b[0].req);
	close(bk->brim.b[0].fd);
	bufdel(bk->brim, 0);
	bk->level += 1;
    }
    if((bk->blocked > 0) && (now - bk->wtime >= cf.warnrate)) {
	flog(LOG_NOTICE, "ratequeue: blocked %i requests from %s", bk->blocked, formatsrc(&bk->id));
	bk->blocked = 0;
	bk->wtime = now;
    }
}

static void checkbtime(struct bucket *bk)
{
    tickbucket(bk);
    if((bk->level == 0) && (now >= bk->etime + cf.retain) && (bk->blocked <= 0)) {
	freebucket(bk);
	return;
    }
    updbtime(bk);
}

static void serve(struct hthead *req, int fd)
{
    struct source src;
    struct bucket *bk;
    
    now = rtime();
    src = reqsource(req);
    bk = hashget(&src);
    tickbucket(bk);
    if(bk->level < cf.size) {
	bk->level += 1;
	if(sendreq(child, req, fd)) {
	    flog(LOG_ERR, "ratequeue: could not pass request to child: %s", strerror(errno));
	    exit(1);
	}
	freehthead(req);
	close(fd);
    } else if(bk->brim.d < cf.brimsize) {
	bufadd(bk->brim, ((struct waiting){.req = req, .fd = fd}));
    } else {
	if(bk->blocked < 0) {
	    flog(LOG_NOTICE, "ratequeue: blocking requests from %s", formatsrc(&bk->id));
	    bk->blocked = 0;
	    bk->wtime = now;
	}
	simpleerror(fd, 429, "Too many requests", "Your client is being throttled for issuing too frequent requests.");
	freehthead(req);
	close(fd);
	bk->blocked++;
    }
    updbtime(bk);
}

static int parseint(const char *str, int *dst)
{
    long buf;
    char *p;
    
    buf = strtol(str, &p, 0);
    if((p == str) || *p)
	return(-1);
    *dst = buf;
    return(0);
}

static int parsefloat(const char *str, double *dst)
{
    double buf;
    char *p;
    
    buf = strtod(str, &p);
    if((p == str) || *p)
	return(-1);
    *dst = buf;
    return(0);
}

static int readconf(char *path, struct config *buf)
{
    FILE *fp;
    struct cfstate *s;
    int rv;
    
    if((fp = fopen(path, "r")) == NULL) {
	flog(LOG_ERR, "ratequeue: %s: %s", path, strerror(errno));
	return(-1);
    }
    *buf = defcfg;
    s = mkcfparser(fp, path);
    rv = -1;
    while(1) {
	getcfline(s);
	if(!strcmp(s->argv[0], "eof")) {
	    break;
	} else if(!strcmp(s->argv[0], "size")) {
	    if((s->argc < 2) || parsefloat(s->argv[1], &buf->size)) {
		flog(LOG_ERR, "%s:%i: missing or invalid `size' argument");
		goto err;
	    }
	} else if(!strcmp(s->argv[0], "rate")) {
	    if((s->argc < 2) || parsefloat(s->argv[1], &buf->rate)) {
		flog(LOG_ERR, "%s:%i: missing or invalid `rate' argument");
		goto err;
	    }
	} else if(!strcmp(s->argv[0], "brim")) {
	    if((s->argc < 2) || parseint(s->argv[1], &buf->brimsize)) {
		flog(LOG_ERR, "%s:%i: missing or invalid `brim' argument");
		goto err;
	    }
	} else {
	    flog(LOG_WARNING, "%s:%i: unknown directive `%s'", s->file, s->lno, s->argv[0]);
	}
    }
    rv = 0;
err:
    freecfparser(s);
    fclose(fp);
    return(rv);
}

static void huphandler(int sig)
{
    reload = 1;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: ratequeue [-h] [-s BUCKET-SIZE] [-r RATE] [-b BRIM-SIZE] PROGRAM [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c, rv;
    int fd;
    struct hthead *req;
    struct pollfd pfd;
    double timeout;
    char *cfname;
    struct config cfbuf;
    
    cf = defcfg;
    cfname = NULL;
    while((c = getopt(argc, argv, "+hc:s:r:b:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    return(0);
	case 'c':
	    cfname = optarg;
	    break;
	case 's':
	    parsefloat(optarg, &cf.size);
	    break;
	case 'r':
	    parsefloat(optarg, &cf.rate);
	    break;
	case 'b':
	    parseint(optarg, &cf.brimsize);
	    break;
	}
    }
    if(argc - optind < 1) {
	usage(stderr);
	return(1);
    }
    if(cfname) {
	if(readconf(cfname, &cfbuf))
	    return(1);
	cf = cfbuf;
    }
    if((child = stdmkchild(argv + optind, NULL, NULL)) < 0) {
	flog(LOG_ERR, "ratequeue: could not fork child: %s", strerror(errno));
	return(1);
    }
    sigaction(SIGHUP, &(struct sigaction){.sa_handler = huphandler}, NULL);
    while(1) {
	if(reload) {
	    if(cfname) {
		if(!readconf(cfname, &cfbuf))
		    cf = cfbuf;
	    }
	    reload = 0;
	}
	now = rtime();
	pfd = (struct pollfd){.fd = 0, .events = POLLIN};
	timeout = (timeheap.d > 0) ? timeheap.b[0].tm : -1;
	if((rv = poll(&pfd, 1, (timeout < 0) ? -1 : (int)((timeout + 0.1 - now) * 1000))) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "ratequeue: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	if(pfd.revents) {
	    if((fd = recvreq(0, &req)) < 0) {
		if(errno == EINTR)
		    continue;
		if(errno != 0)
		    flog(LOG_ERR, "recvreq: %s", strerror(errno));
		break;
	    }
	    serve(req, fd);
	}
	while((timeheap.d > 0) && ((now = rtime()) >= timeheap.b[0].tm))
	    checkbtime(timeheap.b[0].bk);
    }
    return(0);
}
