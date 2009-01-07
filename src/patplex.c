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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>

#define CH_SOCKET 0
#define CH_FORK 1

#define PAT_REST 0
#define PAT_URL 1
#define PAT_METHOD 2
#define PAT_HEADER 3
#define PAT_ALL 4
#define PAT_DEFAULT 5

#define PATFL_MSS 1

struct config {
    struct child *children;
    struct pattern *patterns;
};

struct child {
    struct child *next;
    char *name;
    int type;
    char **argv;
    int fd;
};

struct rule {
    int type;
    int fl;
    char *header;
    regex_t *pattern;
};

struct pattern {
    struct pattern *next;
    char *childnm;
    struct rule **rules;
    char *restpat;
};

static struct config *config;

static void freepattern(struct pattern *pat)
{
    struct rule **rule;
    
    for(rule = pat->rules; *rule; rule++) {
	if((*rule)->header != NULL)
	    free((*rule)->header);
	if((*rule)->pattern != NULL) {
	    regfree((*rule)->pattern);
	    free((*rule)->pattern);
	}
	free(*rule);
    }
    if(pat->childnm != NULL)
	free(pat->childnm);
    free(pat);
}

static void freechild(struct child *ch)
{
    if(ch->fd != -1)
	close(ch->fd);
    if(ch->name != NULL)
	free(ch->name);
    if(ch->argv != NULL)
	freeca(ch->argv);
    free(ch);
}

static struct child *newchild(char *name, int type)
{
    struct child *ch;
    
    omalloc(ch);
    ch->name = sstrdup(name);
    ch->type = type;
    ch->fd = -1;
    return(ch);
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

static struct config *readconfig(char *filename)
{
    int i;
    struct config *cf;
    FILE *s;
    char line[1024];
    char *p, **w;
    int ind, eof;
    int lno;
    int state;
    int rv;
    int argc;
    struct child *child;
    struct pattern *pat;
    struct rule *rule;
    regex_t *regex;
    int rxfl;
    
    if((s = fopen(filename, "r")) == NULL)
	return(NULL);
    omalloc(cf);
    eof = 0;
    state = 0;
    w = NULL;
    lno = 0;
    do {
	if(fgets(line, sizeof(line), s) == NULL) {
	    eof = 1;
	    line[0] = 0;
	}
	lno++;
	for(p = line; *p; p++) {
	    if(*p == '#')
		continue;
	    if(!isspace(*p))
		break;
	}
	ind = isspace(line[0]);
	w = tokenize(line);
	argc = calen(w);
	
    retry:
	if(state == 0) {
	    if(ind) {
		flog(LOG_WARNING, "%s%i: unexpected line indentation in global scope", filename, lno);
		goto next;
	    } else {
		if(!w[0]) {
		} else if(!strcmp(w[0], "child")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing name in child declaration", filename, lno);
			goto next;
		    }
		    child = newchild(w[1], CH_SOCKET);
		    state = 1;
		} else if(!strcmp(w[0], "fchild")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing name in child declaration", filename, lno);
			goto next;
		    }
		    child = newchild(w[1], CH_FORK);
		    state = 1;
		} else if(!strcmp(w[0], "match")) {
		    pat = newpattern();
		    state = 2;
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", filename, lno, w[0]);
		}
	    }
	} else if(state == 1) {
	    if(ind) {
		if(!w[0]) {
		} else if(!strcmp(w[0], "exec")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: too few parameters to `exec'", filename, lno);
			goto next;
		    }
		    child->argv = szmalloc(sizeof(*child->argv) * argc);
		    for(i = 0; i < argc - 1; i++)
			child->argv[i] = sstrdup(w[i + 1]);
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", filename, lno, w[0]);
		}
	    } else {
		state = 0;
		if(child->argv == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing `exec' in child declaration %s", filename, lno, child->name);
		    freechild(child);
		    goto retry;
		}
		child->next = cf->children;
		cf->children = child;
		goto retry;
	    }
	} else if(state == 2) {
	    if(ind) {
		rxfl = 0;
		if(!w[0]) {
		} else if(!strcmp(w[0], "point") ||
			  !strcmp(w[0], "url") ||
			  !strcmp(w[0], "method")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing pattern for `%s' match", w[0], filename, lno);
			goto next;
		    }
		    if(argc >= 3) {
			if(strchr(w[2], 'i'))
			    rxfl |= REG_ICASE;
		    }
		    if((regex = regalloc(w[1], rxfl)) == NULL) {
			flog(LOG_WARNING, "%s:%i: invalid regex for `%s' match", w[0], filename, lno);
			goto next;
		    }
		    rule = newrule(pat);
		    if(!strcmp(w[0], "point"))
			rule->type = PAT_REST;
		    else if(!strcmp(w[0], "url"))
			rule->type = PAT_URL;
		    else if(!strcmp(w[0], "method"))
			rule->type = PAT_METHOD;
		    rule->pattern = regex;
		    if(argc >= 3) {
			if(strchr(w[2], 's'))
			    rule->fl |= PATFL_MSS;
		    }
		} else if(!strcmp(w[0], "header")) {
		    if(argc < 3) {
			flog(LOG_WARNING, "%s:%i: missing header name or pattern for `header' match", filename, lno);
			goto next;
		    }
		    if(argc >= 4) {
			if(strchr(w[3], 'i'))
			    rxfl |= REG_ICASE;
		    }
		    if((regex = regalloc(w[2], rxfl)) == NULL) {
			flog(LOG_WARNING, "%s:%i: invalid regex for `header' match", filename, lno);
			goto next;
		    }
		    rule = newrule(pat);
		    rule->type = PAT_HEADER;
		    rule->header = sstrdup(w[1]);
		    rule->pattern = regex;
		    if(argc >= 4) {
			if(strchr(w[3], 's'))
			    rule->fl |= PATFL_MSS;
		    }
		} else if(!strcmp(w[0], "all")) {
		    newrule(pat)->type = PAT_ALL;
		} else if(!strcmp(w[0], "default")) {
		    newrule(pat)->type = PAT_DEFAULT;
		} else if(!strcmp(w[0], "handler")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing child name for `handler' directive", filename, lno);
			goto next;
		    }
		    if(pat->childnm != NULL)
			free(pat->childnm);
		    pat->childnm = sstrdup(w[1]);
		} else if(!strcmp(w[0], "restpat")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing pattern for `restpat' directive", filename, lno);
			goto next;
		    }
		    if(pat->restpat != NULL)
			free(pat->restpat);
		    pat->restpat = sstrdup(w[1]);
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", filename, lno, w[0]);
		}
	    } else {
		state = 0;
		if(pat->rules[0] == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing rules in match declaration", filename, lno);
		    freepattern(pat);
		    goto retry;
		}
		if(pat->childnm == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing handler in match declaration", filename, lno);
		    freepattern(pat);
		    goto retry;
		}
		pat->next = cf->patterns;
		cf->patterns = pat;
		goto retry;
	    }
	}
	
    next:
	freeca(w);
	w = NULL;
    } while(!eof);
    rv = 0;
    
    if(w != NULL)
	freeca(w);
    fclose(s);
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
		if((p2 = strchr(p, '{')) == NULL) {
		    p++;
		} else {
		    hdr = getheader(req, sprintf3("$.*s", p2 - p - 1, p + 1));
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

static char *findmatch(struct config *cf, struct hthead *req, int trydefault)
{
    int i, o;
    struct pattern *pat;
    struct rule *rule;
    int rmo, matched;
    char *pstr;
    char **mstr;
    regmatch_t gr[10];
    
    mstr = NULL;
    for(pat = cf->patterns; pat != NULL; pat = pat->next) {
	rmo = -1;
	for(i = 0; (rule = pat->rules[i]) != NULL; i++) {
	    matched = 0;
	    if(rule->type == PAT_REST) {
		if((matched = !regexec(rule->pattern, pstr = req->rest, 10, gr, 0)))
		    rmo = gr[0].rm_eo;
		else
		    break;
	    } else if(rule->type == PAT_URL) {
		if(!(matched = !regexec(rule->pattern, pstr = req->url, 10, gr, 0)))
		    break;
	    } else if(rule->type == PAT_METHOD) {
		if(!(matched = !regexec(rule->pattern, pstr = req->method, 10, gr, 0)))
		    break;
	    } else if(rule->type == PAT_HEADER) {
		if(!(pstr = getheader(req, rule->header)))
		    break;
		if(!(matched = !regexec(rule->pattern, pstr, 10, gr, 0)))
		    break;
	    } else if(rule->type == PAT_ALL) {
	    } else if(rule->type == PAT_DEFAULT) {
		if(!trydefault)
		    break;
	    }
	    if(matched && (rule->fl & PATFL_MSS)) {
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
		    mstr[o] = smalloc(gr[o].rm_eo - gr[o].rm_so + 1);
		    memcpy(mstr[o], pstr + gr[o].rm_so, gr[o].rm_eo - gr[o].rm_so);
		    mstr[o][gr[o].rm_eo - gr[o].rm_so] = 0;
		}
	    }
	}
	if(!rule) {
	    if(pat->restpat) {
		exprestpat(req, pat, mstr);
	    } else if(rmo != -1) {
		replrest(req, req->rest + rmo);
	    }
	    if(mstr)
		freeca(mstr);
	    return(pat->childnm);
	}
	if(mstr) {
	    freeca(mstr);
	    mstr = NULL;
	}
    }
    return(NULL);
}

static void forkchild(struct child *ch)
{
    ch->fd = stdmkchild(ch->argv);
}

static void passreq(struct child *ch, struct hthead *req, int fd)
{
    if(ch->fd < 0)
	forkchild(ch);
    if(sendreq(ch->fd, req, fd)) {
	if(errno == EPIPE) {
	    /* Assume that the child has crashed and restart it. */
	    forkchild(ch);
	    if(!sendreq(ch->fd, req, fd))
		return;
	}
	flog(LOG_ERR, "could not pass on request to child %s: %s", ch->name, strerror(errno));
	close(ch->fd);
	ch->fd = -1;
    }
}

static void serve(struct hthead *req, int fd)
{
    char *chnm;
    struct child *ch;
    
    if(((chnm = findmatch(config, req, 0)) == NULL) && ((chnm = findmatch(config, req, 1)) == NULL)) {
	simpleerror(fd, 404, "Not Found", "The requested resource could not be found on this server.");
	return;
    }
    if((ch = getchild(config, chnm)) == NULL) {
	flog(LOG_ERR, "child %s requested, but was not declared", chnm);
	simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", chnm);
	return;
    }
    
    if(ch->type == CH_SOCKET) {
	passreq(ch, req, fd);
    } else if(ch->type == CH_FORK) {
	stdforkserve(ch->argv, req, fd);
    }
}

int main(int argc, char **argv)
{
    struct hthead *req;
    int fd;

    if(argc < 2) {
	flog(LOG_ERR, "usage: patplex CONFIGFILE");
	exit(1);
    }
    config = readconfig(argv[1]);
    signal(SIGCHLD, SIG_IGN);
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