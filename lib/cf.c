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
#include <ctype.h>
#include <glob.h>
#include <libgen.h>
#include <sys/socket.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <cf.h>
#include <mt.h>
#include <proc.h>
#include <log.h>

#define CH_SOCKET 0
#define CH_FORK 1

struct stdchild {
    int type;
    char **argv;
    char **envp;
    int fd;
    int agains;
};

static int parsefile(struct cfstate *s, FILE *in);
static void stdmerge(struct child *old, struct child *new);
static int stdhandle(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *idata);
static void stddestroy(struct child *ch);

static int doinclude(struct cfstate *s, char *spec)
{
    int rv, i;
    FILE *inc;
    glob_t globm;
    char *fbk, *dir, *fspec;

    rv = 0;
    fbk = s->file;
    if(spec[0] == '/') {
	fspec = spec;
    } else {
	dir = sstrdup(fbk);
	fspec = sprintf3("%s/%s", dirname(dir), spec);
	free(dir);
    }
    if(glob(fspec, 0, NULL, &globm))
	return(0);
    for(i = 0; i < globm.gl_pathc; i++) {
	if((inc = fopen(globm.gl_pathv[i], "r")) != NULL) {
	    s->file = globm.gl_pathv[i];
	    if(parsefile(s, inc)) {
		fclose(inc);
		rv = 1;
		goto out;
	    }
	    fclose(inc);
	    inc = NULL;
	}
    }
    
out:
    globfree(&globm);
    s->file = fbk;
    return(rv);
}

static int parsefile(struct cfstate *s, FILE *in)
{
    int i;
    char line[1024];
    int eof, argc;
    int ind, indst[80], indl;
    char *p, **w;
    
    s->lno = 0;
    indst[indl = 0] = 0;
    eof = 0;
    while(1) {
	if(fgets(line, sizeof(line), in) == NULL) {
	    eof = 1;
	    line[0] = 0;
	}
	s->lno++;
	if(line[0]) {
	    for(p = line + strlen(line) - 1; p >= line; p--) {
		if(isspace(*p))
		    *p = 0;
		else
		    break;
	    }
	}
	for(ind = 0, p = line; *p; p++) {
	    if(*p == ' ') {
		ind++;
	    } else if(*p == '\t') {
		ind = ind - (ind % 8) + 8;
	    } else {
		break;
	    }
	}
	if(!eof && (!*p || (*p == '#')))
	    continue;
	
    reindent:
	if(ind > indst[indl]) {
	    indst[++indl] = ind;
	    if(!s->expstart) {
		s->res = tokenize("start");
		if(yield())
		    return(1);
	    } else {
		s->expstart = 0;
	    }
	} else {
	    if(s->expstart) {
		s->res = tokenize("end");
		if(yield())
		    return(1);
		s->expstart = 0;
	    }
	    while(ind < indst[indl]) {
		indl--;
		s->res = tokenize("end");
		if(yield())
		    return(1);
	    }
	    if(ind > indst[indl]) {
		flog(LOG_WARNING, "%s:%i: unexpected indentation level", s->file, s->lno);
		goto reindent;
	    }
	}
	
	if(eof)
	    return(0);
	
	argc = calen(w = tokenize(line));
	if(argc < 1) {
	    /* Shouldn't happen, but... */
	    freeca(w);
	    continue;
	}
	
	if(indl == 0) {
	    if(!strcmp(w[0], "include")) {
		for(i = 1; i < argc; i++) {
		    if(doinclude(s, w[i])) {
			freeca(w);
			return(1);
		    }
		}
		freeca(w);
		continue;
	    }
	}
	
	if(!strcmp(w[0], "start") ||
	   !strcmp(w[0], "end") || 
	   !strcmp(w[0], "eof")) {
	    flog(LOG_WARNING, "%s:%i: illegal directive: %s", s->file, s->lno, w[0]);
	} else {
	    s->res = w;
	    if(yield())
		return(1);
	}
    }
}

static void parsefn(struct muth *mt, va_list args)
{
    vavar(struct cfstate *, s);
    vavar(FILE *, in);
    vavar(char *, file);
    
    s->file = sstrdup(file);
    if(parsefile(s, in))
	goto out;
    do {
	s->res = tokenize("eof");
    } while(!yield());
    
out:
    free(s->file);
}

char **getcfline(struct cfstate *s)
{
    freeca(s->argv);
    if(s->res == NULL)
	resume(s->pf, 0);
    s->argc = calen(s->argv = s->res);
    s->res = NULL;
    return(s->argv);
}

struct cfstate *mkcfparser(FILE *in, char *name)
{
    struct cfstate *s;
    
    omalloc(s);
    s->pf = mustart(parsefn, s, in, name);
    return(s);
}

void freecfparser(struct cfstate *s)
{
    resume(s->pf, -1);
    freeca(s->argv);
    freeca(s->res);
    free(s);
}

char *findstdconf(char *name)
{
    char *home, *path, *p, *p2, *t;
    
    if((home = getenv("HOME")) != NULL) {
	if(!access(t = sprintf2("%s/.ashd/etc/%s", home, name), R_OK))
	    return(t);
	free(t);
    }
    if((path = getenv("PATH")) != NULL) {
	path = sstrdup(path);
	for(p = strtok(path, ":"); p != NULL; p = strtok(NULL, ":")) {
	    if((p2 = strrchr(p, '/')) == NULL)
		continue;
	    *p2 = 0;
	    if(!access(t = sprintf2("%s/etc/%s", p, name), R_OK)) {
		free(path);
		return(t);
	    }
	    free(t);
	}
	free(path);
    }
    return(NULL);
}

struct child *newchild(char *name, struct chandler *iface, void *pdata)
{
    struct child *ch;
    
    omalloc(ch);
    ch->name = sstrdup(name);
    ch->iface = iface;
    ch->pdata = pdata;
    return(ch);
}

void freechild(struct child *ch)
{
    if(ch->iface->destroy != NULL)
	ch->iface->destroy(ch);
    if(ch->name != NULL)
	free(ch->name);
    free(ch);
}

void mergechildren(struct child *dst, struct child *src)
{
    struct child *ch1, *ch2;
    
    for(ch1 = dst; ch1 != NULL; ch1 = ch1->next) {
	for(ch2 = src; ch2 != NULL; ch2 = ch2->next) {
	    if(ch1->iface->merge && !strcmp(ch1->name, ch2->name)) {
		ch1->iface->merge(ch1, ch2);
		break;
	    }
	}
    }
}

void skipcfblock(struct cfstate *s)
{
    char **w;
    
    while(1) {
	w = getcfline(s);
	if(!strcmp(w[0], "end") || !strcmp(w[0], "eof"))
	    return;
    }
}

static struct chandler stdhandler = {
    .handle = stdhandle,
    .merge = stdmerge,
    .destroy = stddestroy,
};

static char **expandargs(struct stdchild *sd)
{
    int i;
    char **ret, *p, *p2, *p3, *np, *env;
    struct charbuf exp;
    
    ret = szmalloc(sizeof(*ret) * (calen(sd->argv) + 1));
    bufinit(exp);
    for(i = 0; sd->argv[i] != NULL; i++) {
	if((p = strchr(sd->argv[i], '$')) == NULL) {
	    ret[i] = sstrdup(sd->argv[i]);
	} else {
	    exp.d = 0;
	    for(p2 = sd->argv[i]; p != NULL; p2 = np, p = strchr(np, '$')) {
		bufcat(exp, p2, p - p2);
		if(p[1] == '{') {
		    if((p3 = strchr((p += 2), '}')) == NULL)
			break;
		    np = p3 + 1;
		} else {
		    for(p3 = ++p; *p3; p3++) {
			if(!(((*p3 >= 'a') && (*p3 <= 'z')) ||
			     ((*p3 >= 'A') && (*p3 <= 'Z')) ||
			     ((*p3 >= '0') && (*p3 <= '9')) ||
			     (*p3 == '_'))) {
			    break;
			}
		    }
		    np = p3;
		}
		char temp[(p3 - p) + 1];
		memcpy(temp, p, p3 - p);
		temp[p3 - p] = 0;
		if((env = getenv(temp)) != NULL)
		    bufcatstr(exp, env);
	    }
	    bufcatstr2(exp, np);
	    ret[i] = sstrdup(exp.b);
	}
    }
    ret[i] = NULL;
    buffree(exp);
    return(ret);
}

struct sidata {
    struct stdchild *sd;
    void (*sinit)(void *);
    void *sdata;
};

static void stdinit(void *data)
{
    struct sidata *d = data;
    int i;
	
    for(i = 0; d->sd->envp[i]; i += 2)
	putenv(sprintf2("%s=%s", d->sd->envp[i], d->sd->envp[i + 1]));
    if(d->sinit != NULL)
	d->sinit(d->sdata);
}

static int stdhandle(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *sdata)
{
    struct stdchild *sd = ch->pdata;
    int serr;
    char **args;
    struct sidata idat;
    
    if(sd->type == CH_SOCKET) {
	idat = (struct sidata) {.sd = sd, .sinit = chinit, .sdata = sdata};
	if(sd->fd < 0) {
	    args = expandargs(sd);
	    sd->fd = stdmkchild(args, stdinit, &idat);
	    freeca(args);
	}
	if(sendreq2(sd->fd, req, fd, MSG_NOSIGNAL | MSG_DONTWAIT)) {
	    serr = errno;
	    if((serr == EPIPE) || (serr == ECONNRESET)) {
		/* Assume that the child has crashed and restart it. */
		close(sd->fd);
		args = expandargs(sd);
		sd->fd = stdmkchild(args, stdinit, &idat);
		freeca(args);
		if(!sendreq2(sd->fd, req, fd, MSG_NOSIGNAL | MSG_DONTWAIT))
		    goto ok;
		serr = errno;
	    }
	    if(serr == EAGAIN) {
		if(sd->agains++ == 0)
		    flog(LOG_WARNING, "request to child %s denied due to buffer overload", ch->name);
	    } else {
		flog(LOG_ERR, "could not pass on request to child %s: %s", ch->name, strerror(serr));
		close(sd->fd);
		sd->fd = -1;
	    }
	    return(-1);
	}
    ok:
	if(sd->agains > 0) {
	    flog(LOG_WARNING, "%i requests to child %s were denied due to buffer overload", sd->agains, ch->name);
	    sd->agains = 0;
	}
    } else if(sd->type == CH_FORK) {
	args = expandargs(sd);
	if(stdforkserve(args, req, fd, chinit, sdata) < 0) {
	    freeca(args);
	    return(-1);
	}
	freeca(args);
    }
    return(0);
}

static void stdmerge(struct child *dst, struct child *src)
{
    struct stdchild *od, *nd;
    
    if(src->iface == &stdhandler) {
	nd = dst->pdata;
	od = src->pdata;
	nd->fd = od->fd;
	od->fd = -1;
    }
}

static void stddestroy(struct child *ch)
{
    struct stdchild *d = ch->pdata;
    
    if(d->fd >= 0)
	close(d->fd);
    if(d->argv)
	freeca(d->argv);
    if(d->envp)
	freeca(d->envp);
    free(d);
}

struct child *parsechild(struct cfstate *s)
{
    struct child *ch;
    struct stdchild *d;
    struct charvbuf envbuf;
    int i;
    int sl;
    
    sl = s->lno;
    if(!strcmp(s->argv[0], "child")) {
	s->expstart = 1;
	if(s->argc < 2) {
	    flog(LOG_WARNING, "%s:%i: missing name in child declaration", s->file, s->lno);
	    skipcfblock(s);
	    return(NULL);
	}
	ch = newchild(s->argv[1], &stdhandler, omalloc(d));
	d->type = CH_SOCKET;
    } else if(!strcmp(s->argv[0], "fchild")) {
	s->expstart = 1;
	if(s->argc < 2) {
	    flog(LOG_WARNING, "%s:%i: missing name in child declaration", s->file, s->lno);
	    skipcfblock(s);
	    return(NULL);
	}
	ch = newchild(s->argv[1], &stdhandler, omalloc(d));
	d->type = CH_FORK;
    } else {
	return(NULL);
    }
    d->fd = -1;
    
    bufinit(envbuf);
    while(1) {
	getcfline(s);
	if(!strcmp(s->argv[0], "exec")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: too few parameters to `exec'", s->file, s->lno);
		continue;
	    }
	    d->argv = szmalloc(sizeof(*d->argv) * s->argc);
	    for(i = 0; i < s->argc - 1; i++)
		d->argv[i] = sstrdup(s->argv[i + 1]);
	} else if(!strcmp(s->argv[0], "env")) {
	    if(s->argc < 3) {
		flog(LOG_WARNING, "%s:%i: too few parameters to `env'", s->file, s->lno);
		continue;
	    }
	    bufadd(envbuf, sstrdup(s->argv[1]));
	    bufadd(envbuf, sstrdup(s->argv[2]));
	} else if(!strcmp(s->argv[0], "end") || !strcmp(s->argv[0], "eof")) {
	    break;
	} else {
	    flog(LOG_WARNING, "%s:%i: unknown directive `%s' in child declaration", s->file, s->lno, s->argv[0]);
	}
    }
    bufadd(envbuf, NULL);
    d->envp = envbuf.b;
    if(d->argv == NULL) {
	flog(LOG_WARNING, "%s:%i: missing `exec' in child declaration %s", s->file, sl, ch->name);
	freechild(ch);
	return(NULL);
    }
    return(ch);
}

int childhandle(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *idata)
{
    return(ch->iface->handle(ch, req, fd, chinit, idata));
}
