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
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <regex.h>
#include <limits.h>
#include <sys/wait.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>
#include <cf.h>

#define PAT_REST 0
#define PAT_URL 1
#define PAT_METHOD 2
#define PAT_HEADER 3
#define PAT_ALL 4

#define PATFL_MSS 1
#define PATFL_UNQ 2

#define HND_CHILD 1
#define HND_REPARSE 2

struct config {
    struct child *children;
    struct pattern *patterns;
};

struct rule {
    int type;
    int fl;
    char *header;
    regex_t *pattern;
};

struct headmod {
    struct headmod *next;
    char *name, *value;
};

struct pattern {
    struct pattern *next;
    struct headmod *headers;
    char *childnm;
    struct rule **rules;
    char *restpat;
    int handler, prio, disable;
};

static struct config *gconfig, *lconfig;
static volatile int reload = 0;

static void freepattern(struct pattern *pat)
{
    struct rule **rule;
    struct headmod *head;
    
    for(rule = pat->rules; *rule; rule++) {
	if((*rule)->header != NULL)
	    free((*rule)->header);
	if((*rule)->pattern != NULL) {
	    regfree((*rule)->pattern);
	    free((*rule)->pattern);
	}
	free(*rule);
    }
    while((head = pat->headers) != NULL) {
	pat->headers = head->next;
	free(head->name);
	free(head->value);
	free(head);
    }
    if(pat->childnm != NULL)
	free(pat->childnm);
    free(pat);
}

static void freeconfig(struct config *cf)
{
    struct child *ch, *nch;
    struct pattern *pat, *npat;
    
    for(ch = cf->children; ch != NULL; ch = nch) {
	nch = ch->next;
	freechild(ch);
    }
    for(pat = cf->patterns; pat != NULL; pat = npat) {
	npat = pat->next;
	freepattern(pat);
    }
    free(cf);
}

static struct child *getchild(struct config *cf, char *name)
{
    struct child *ch;
    
    for(ch = cf->children; ch; ch = ch->next) {
	if(!strcmp(ch->name, name))
	    break;
    }
    return(ch);
}

static struct rule *newrule(struct pattern *pat)
{
    int i;
    struct rule *rule;
    
    for(i = 0; pat->rules[i]; i++);
    pat->rules = srealloc(pat->rules, sizeof(*pat->rules) * (i + 2));
    rule = pat->rules[i] = szmalloc(sizeof(*rule));
    pat->rules[i + 1] = NULL;
    return(rule);
}

static struct pattern *newpattern(void)
{
    struct pattern *pat;
    
    omalloc(pat);
    pat->rules = szmalloc(sizeof(*pat->rules));
    return(pat);
}

static regex_t *regalloc(char *regex, int flags)
{
    regex_t *ret;
    int res;
    char errbuf[256];
    
    omalloc(ret);
    if((res = regcomp(ret, regex, flags | REG_EXTENDED)) != 0) {
	regerror(res, ret, errbuf, sizeof(errbuf));
	flog(LOG_WARNING, "%s: %s", regex, errbuf);
	free(ret);
	return(NULL);
    }
    return(ret);
}

static struct pattern *parsepattern(struct cfstate *s)
{
    struct pattern *pat;
    int sl;
    struct rule *rule;
    struct headmod *head;
    regex_t *regex;
    int rxfl;
    
    if(!strcmp(s->argv[0], "match")) {
	s->expstart = 1;
	pat = newpattern();
    } else {
	return(NULL);
    }
    
    sl = s->lno;
    while(1) {
	getcfline(s);
	if(!strcmp(s->argv[0], "point") ||
	   !strcmp(s->argv[0], "url") ||
	   !strcmp(s->argv[0], "method")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing pattern for `%s' match", s->file, s->lno, s->argv[0]);
		continue;
	    }
	    rxfl = 0;
	    if(s->argc >= 3) {
		if(strchr(s->argv[2], 'i'))
		    rxfl |= REG_ICASE;
	    }
	    if((regex = regalloc(s->argv[1], rxfl)) == NULL) {
		flog(LOG_WARNING, "%s:%i: invalid regex for `%s' match", s->file, s->lno, s->argv[0]);
		continue;
	    }
	    rule = newrule(pat);
	    if(!strcmp(s->argv[0], "point"))
		rule->type = PAT_REST;
	    else if(!strcmp(s->argv[0], "url"))
		rule->type = PAT_URL;
	    else if(!strcmp(s->argv[0], "method"))
		rule->type = PAT_METHOD;
	    rule->pattern = regex;
	    if(s->argc >= 3) {
		if(strchr(s->argv[2], 's'))
		    rule->fl |= PATFL_MSS;
		if(strchr(s->argv[2], 'q'))
		    rule->fl |= PATFL_UNQ;
	    }
	} else if(!strcmp(s->argv[0], "header")) {
	    if(s->argc < 3) {
		flog(LOG_WARNING, "%s:%i: missing header name or pattern for `header' match", s->file, s->lno);
		continue;
	    }
	    rxfl = 0;
	    if(s->argc >= 4) {
		if(strchr(s->argv[3], 'i'))
		    rxfl |= REG_ICASE;
	    }
	    if((regex = regalloc(s->argv[2], rxfl)) == NULL) {
		flog(LOG_WARNING, "%s:%i: invalid regex for `header' match", s->file, s->lno);
		continue;
	    }
	    rule = newrule(pat);
	    rule->type = PAT_HEADER;
	    rule->header = sstrdup(s->argv[1]);
	    rule->pattern = regex;
	    if(s->argc >= 4) {
		if(strchr(s->argv[3], 's'))
		    rule->fl |= PATFL_MSS;
	    }
	} else if(!strcmp(s->argv[0], "all")) {
	    newrule(pat)->type = PAT_ALL;
	} else if(!strcmp(s->argv[0], "default")) {
	    newrule(pat)->type = PAT_ALL;
	    pat->prio = -10;
	} else if(!strcmp(s->argv[0], "order") || !strcmp(s->argv[0], "priority")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing specification for `%s' directive", s->file, s->lno, s->argv[0]);
		continue;
	    }
	    pat->prio = atoi(s->argv[1]);
	} else if(!strcmp(s->argv[0], "handler")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing child name for `handler' directive", s->file, s->lno);
		continue;
	    }
	    if(pat->childnm != NULL)
		free(pat->childnm);
	    pat->childnm = sstrdup(s->argv[1]);
	    pat->handler = HND_CHILD;
	} else if(!strcmp(s->argv[0], "reparse")) {
	    pat->handler = HND_REPARSE;
	} else if(!strcmp(s->argv[0], "restpat")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing pattern for `restpat' directive", s->file, s->lno);
		continue;
	    }
	    if(pat->restpat != NULL)
		free(pat->restpat);
	    pat->restpat = sstrdup(s->argv[1]);
	} else if(!strcmp(s->argv[0], "set") || !strcmp(s->argv[0], "xset")) {
	    if(s->argc < 3) {
		flog(LOG_WARNING, "%s:%i: missing header name or pattern for `%s' directive", s->file, s->lno, s->argv[0]);
		continue;
	    }
	    omalloc(head);
	    if(!strcmp(s->argv[0], "xset"))
		head->name = sprintf2("X-Ash-%s", s->argv[1]);
	    else
		head->name = sstrdup(s->argv[1]);
	    head->value = sstrdup(s->argv[2]);
	    head->next = pat->headers;
	    pat->headers = head;
	} else if(!strcmp(s->argv[0], "end") || !strcmp(s->argv[0], "eof")) {
	    break;
	} else {
	    flog(LOG_WARNING, "%s:%i: unknown directive `%s' in pattern declaration", s->file, s->lno, s->argv[0]);
	}
    }
    
    if(pat->rules[0] == NULL) {
	flog(LOG_WARNING, "%s:%i: missing rules in match declaration", s->file, sl);
	freepattern(pat);
	return(NULL);
    }
    if(pat->handler == 0) {
	flog(LOG_WARNING, "%s:%i: missing handler in match declaration", s->file, sl);
	freepattern(pat);
	return(NULL);
    }
    return(pat);
}

static struct config *readconfig(char *filename)
{
    struct cfstate *s;
    struct config *cf;
    struct child *ch;
    struct pattern *pat;
    FILE *in;
    
    if((in = fopen(filename, "r")) == NULL) {
	flog(LOG_WARNING, "%s: %s", filename, strerror(errno));
	return(NULL);
    }
    s = mkcfparser(in, filename);
    omalloc(cf);
    
    while(1) {
	getcfline(s);
	if((ch = parsechild(s)) != NULL) {
	    ch->next = cf->children;
	    cf->children = ch;
	} else if((pat = parsepattern(s)) != NULL) {
	    pat->next = cf->patterns;
	    cf->patterns = pat;
	} else if(!strcmp(s->argv[0], "eof")) {
	    break;
	} else {
	    flog(LOG_WARNING, "%s:%i: unknown directive `%s'", s->file, s->lno, s->argv[0]);
	}
    }
    
    freecfparser(s);
    fclose(in);
    return(cf);
}

static void exprestpat(struct hthead *req, struct pattern *pat, char **mstr)
{
    char *p, *p2, *hdr;
    int mc;
    struct charbuf buf;
    
    if(mstr == NULL)
	mc = 0;
    else
	for(mc = 0; mstr[mc]; mc++);
    bufinit(buf);
    for(p = pat->restpat; *p; ) {
	if(*p == '$') {
	    p++;
	    if((*p >= '0') && (*p <= '9')) {
		if(*p - '0' < mc)
		    bufcatstr(buf, mstr[*p - '0']);
		p++;
	    } else if(*p == '_') {
		bufcatstr(buf, req->rest);
		p++;
	    } else if(*p == '$') {
		bufadd(buf, '$');
		p++;
	    } else if(*p == '{') {
		if((p2 = strchr(p, '}')) == NULL) {
		    p++;
		} else {
		    hdr = getheader(req, sprintf3("%.*s", p2 - p - 1, p + 1));
		    if(hdr)
			bufcatstr(buf, hdr);
		}
	    } else if(!*p) {
	    }
	} else {
	    bufadd(buf, *(p++));
	}
    }
    bufadd(buf, 0);
    replrest(req, buf.b);
    buffree(buf);
}

static void qoffsets(char *buf, int *obuf, char *pstr, int unquote)
{
    int i, o, d1, d2;
    
    if(unquote) {
	i = o = 0;
	while(pstr[i]) {
	    obuf[o] = i;
	    if((pstr[i] == '%') && ((d1 = hexdigit(pstr[i + 1])) >= 0) && ((d2 = hexdigit(pstr[i + 2])) >= 0)) {
		buf[o] = (d1 << 4) | d2;
		i += 3;
	    } else {
		buf[o] = pstr[i];
		i++;
	    }
	    o++;
	}
	buf[o] = 0;
	obuf[o] = i;
    } else {
	for(i = 0; pstr[i]; i++) {
	    buf[i] = pstr[i];
	    obuf[i] = i;
	}
	buf[i] = 0;
	obuf[i] = i;
    }
}

struct match {
    struct pattern *pat;
    char **mstr;
    int rmo;
};

static void freematch(struct match *match)
{
    freeca(match->mstr);
    free(match);
}

static struct match *findmatch(struct config *cf, struct hthead *req, struct match *match)
{
    int i, o;
    struct pattern *pat;
    struct rule *rule;
    int rmo;
    regex_t *rx;
    char *pstr;
    char **mstr;
    regmatch_t gr[10];
    
    mstr = NULL;
    for(pat = cf->patterns; pat != NULL; pat = pat->next) {
	if(pat->disable || (match && (pat->prio <= match->pat->prio)))
	    continue;
	rmo = -1;
	for(i = 0; (rule = pat->rules[i]) != NULL; i++) {
	    rx = NULL;
	    if(rule->type == PAT_REST) {
		rx = rule->pattern;
		pstr = req->rest;
	    } else if(rule->type == PAT_URL) {
		rx = rule->pattern;
		pstr = req->url;
	    } else if(rule->type == PAT_METHOD) {
		rx = rule->pattern;
		pstr = req->method;
	    } else if(rule->type == PAT_HEADER) {
		rx = rule->pattern;
		if(!(pstr = getheader(req, rule->header)))
		    break;
	    }
	    if(rx != NULL) {
		char pbuf[strlen(pstr) + 1];
		int  obuf[strlen(pstr) + 1];
		qoffsets(pbuf, obuf, pstr, !!(rule->fl & PATFL_UNQ));
		if(regexec(rx, pbuf, 10, gr, 0))
		    break;
		else if(rule->type == PAT_REST)
		    rmo = obuf[gr[0].rm_eo];
		if(rule->fl & PATFL_MSS) {
		    if(mstr) {
			flog(LOG_WARNING, "two pattern rules marked with `s' flag found (for handler %s)", pat->childnm);
			freeca(mstr);
		    }
		    for(o = 0; o < 10; o++) {
			if(gr[o].rm_so < 0)
			    break;
		    }
		    mstr = szmalloc((o + 1) * sizeof(*mstr));
		    for(o = 0; o < 10; o++) {
			if(gr[o].rm_so < 0)
			    break;
			mstr[o] = smalloc(obuf[gr[o].rm_eo] - obuf[gr[o].rm_so] + 1);
			memcpy(mstr[o], pstr + obuf[gr[o].rm_so], obuf[gr[o].rm_eo] - obuf[gr[o].rm_so]);
			mstr[o][obuf[gr[o].rm_eo] - obuf[gr[o].rm_so]] = 0;
		    }
		}
	    } else if(rule->type == PAT_ALL) {
	    }
	}
	if(!rule) {
	    if(match)
		freematch(match);
	    omalloc(match);
	    match->pat = pat;
	    match->mstr = mstr;
	    match->rmo = rmo;
	}
    }
    return(match);
}

static void execmatch(struct hthead *req, struct match *match)
{
    struct headmod *head;
    
    if(match->pat->restpat)
	exprestpat(req, match->pat, match->mstr);
    else if(match->rmo != -1)
	replrest(req, req->rest + match->rmo);
    for(head = match->pat->headers; head != NULL; head = head->next) {
	headrmheader(req, head->name);
	headappheader(req, head->name, head->value);
    }
}

static void childerror(struct hthead *req, int fd)
{
    if(errno == EAGAIN)
	simpleerror(fd, 500, "Server Error", "The request handler is overloaded.");
    else
	simpleerror(fd, 500, "Server Error", "The request handler crashed.");
}

static void serve(struct hthead *req, int fd)
{
    struct match *match;
    struct child *ch;
    
    match = NULL;
    match = findmatch(lconfig, req, match);
    if(gconfig != NULL)
	match = findmatch(gconfig, req, match);
    if(match == NULL) {
	simpleerror(fd, 404, "Not Found", "The requested resource could not be found on this server.");
	return;
    }
    execmatch(req, match);
    switch(match->pat->handler) {
    case HND_CHILD:
	ch = NULL;
	if(ch == NULL)
	    ch = getchild(lconfig, match->pat->childnm);
	if((ch == NULL) && (gconfig != NULL))
	    ch = getchild(gconfig, match->pat->childnm);
	if(ch == NULL) {
	    flog(LOG_ERR, "child %s requested, but was not declared", match->pat->childnm);
	    simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", match->pat->childnm);
	    break;
	}
	if(childhandle(ch, req, fd, NULL, NULL))
	    childerror(req, fd);
	break;
    case HND_REPARSE:
	match->pat->disable = 1;
	serve(req, fd);
	match->pat->disable = 0;
	break;
    default:
	abort();
    }
    freematch(match);
}

static void reloadconf(char *nm)
{
    struct config *cf;
    
    if((cf = readconfig(nm)) == NULL) {
	flog(LOG_WARNING, "could not reload configuration file `%s'", nm);
	return;
    }
    mergechildren(cf->children, lconfig->children);
    freeconfig(lconfig);
    lconfig = cf;
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
    if(sig == SIGHUP)
	reload = 1;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: patplex [-hN] CONFIGFILE\n");
}

int main(int argc, char **argv)
{
    int c;
    int nodef;
    char *gcf, *lcf;
    struct hthead *req;
    int fd;
    
    nodef = 0;
    while((c = getopt(argc, argv, "hN")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'N':
	    nodef = 1;
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
	if((gcf = findstdconf("ashd/patplex.rc")) != NULL) {
	    gconfig = readconfig(gcf);
	    free(gcf);
	}
    }
    if((strchr(lcf = argv[optind], '/')) == NULL) {
	if((lcf = findstdconf(sprintf3("ashd/%s", lcf))) == NULL) {
	    flog(LOG_ERR, "could not find requested configuration file `%s'", argv[optind]);
	    exit(1);
	}
    }
    if((lconfig = readconfig(lcf)) == NULL) {
	flog(LOG_ERR, "could not read `%s'", lcf);
	exit(1);
    }
    signal(SIGCHLD, chldhandler);
    signal(SIGHUP, sighandler);
    signal(SIGPIPE, sighandler);
    while(1) {
	if(reload) {
	    reloadconf(lcf);
	    reload = 0;
	}
	if((fd = recvreq(0, &req)) < 0) {
	    if(errno == EINTR)
		continue;
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
