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
#include <fnmatch.h>
#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <cf.h>
#include <resp.h>

#include "dirplex.h"

static struct config *cflist;
struct config *gconfig, *lconfig;

static void freerule(struct rule *rule)
{
    freeca(rule->patterns);
    free(rule);
}

static void freepattern(struct pattern *pat)
{
    struct rule **rule;
    struct headmod *head;
    
    for(rule = pat->rules; *rule; rule++)
	freerule(*rule);
    while((head = pat->headers) != NULL) {
	pat->headers = head->next;
	free(head->name);
	free(head->value);
	free(head);
    }
    if(pat->childnm != NULL)
	free(pat->childnm);
    freeca(pat->fchild);
    free(pat);
}

static void freeconfig(struct config *cf)
{
    struct child *ch, *nch;
    struct pattern *pat, *npat;
    
    if(cf->prev != NULL)
	cf->prev->next = cf->next;
    if(cf->next != NULL)
	cf->next->prev = cf->prev;
    if(cf == cflist)
	cflist = cf->next;
    if(cf->path != NULL)
	free(cf->path);
    for(ch = cf->children; ch != NULL; ch = nch) {
	nch = ch->next;
	freechild(ch);
    }
    for(pat = cf->patterns; pat != NULL; pat = npat) {
	npat = pat->next;
	freepattern(pat);
    }
    freeca(cf->index);
    freeca(cf->dotallow);
    if(cf->capture != NULL)
	free(cf->capture);
    if(cf->reparse != NULL)
	free(cf->reparse);
    free(cf);
}

struct child *getchild(struct config *cf, char *name)
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

static char **cadup(char **w)
{
    char **ret;
    int i, l;
    
    l = calen(w);
    ret = smalloc(sizeof(*ret) * (l + 1));
    for(i = 0; i < l; i++)
	ret[i] = sstrdup(w[i]);
    ret[i] = NULL;
    return(ret);
}

static struct pattern *parsepattern(struct cfstate *s)
{
    struct pattern *pat;
    struct rule *rule;
    struct headmod *head;
    int sl;

    if(!strcmp(s->argv[0], "match")) {
	s->expstart = 1;
	pat = newpattern();
    } else {
	return(NULL);
    }
    
    if((s->argc > 1) && !strcmp(s->argv[1], "directory"))
	pat->type = PT_DIR;
    else if((s->argc > 1) && !strcmp(s->argv[1], "notfound"))
	pat->type = PT_NOTFOUND;
    else
	pat->type = PT_FILE;
    sl = s->lno;
    while(1) {
	getcfline(s);
	if(!strcmp(s->argv[0], "filename")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing pattern for `filename' match", s->file, s->lno);
		continue;
	    }
	    rule = newrule(pat);
	    rule->type = PAT_BASENAME;
	    rule->patterns = cadup(s->argv + 1);
	} else if(!strcmp(s->argv[0], "pathname")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing pattern for `pathname' match", s->file, s->lno);
		continue;
	    }
	    rule = newrule(pat);
	    rule->type = PAT_PATHNAME;
	    rule->patterns = cadup(s->argv + 1);
	} else if(!strcmp(s->argv[0], "all")) {
	    newrule(pat)->type = PAT_ALL;
	} else if(!strcmp(s->argv[0], "default")) {
	    newrule(pat)->type = PAT_DEFAULT;
	} else if(!strcmp(s->argv[0], "local")) {
	    newrule(pat)->type = PAT_LOCAL;
	} else if(!strcmp(s->argv[0], "handler")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing child name for `handler' directive", s->file, s->lno);
		continue;
	    }
	    if(pat->childnm != NULL)
		free(pat->childnm);
	    pat->childnm = sstrdup(s->argv[1]);
	} else if(!strcmp(s->argv[0], "fork")) {
	    pat->fchild = cadup(s->argv + 1);
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
    if((pat->childnm == NULL) && (pat->fchild == NULL)) {
	flog(LOG_WARNING, "%s:%i: missing handler in match declaration", s->file, sl);
	freepattern(pat);
	return(NULL);
    }
    return(pat);
}

static struct config *emptyconfig(void)
{
    struct config *cf;
    
    omalloc(cf);
    return(cf);
}

struct config *readconfig(char *file)
{
    struct cfstate *s;
    FILE *in;
    struct config *cf;
    struct child *child;
    struct pattern *pat;
    
    if((in = fopen(file, "r")) == NULL) {
	flog(LOG_WARNING, "%s: %s", file, strerror(errno));
	return(NULL);
    }
    s = mkcfparser(in, file);
    cf = emptyconfig();
    
    while(1) {
	getcfline(s);
	if((child = parsechild(s)) != NULL) {
	    child->next = cf->children;
	    cf->children = child;
	} else if((pat = parsepattern(s)) != NULL) {
	    pat->next = cf->patterns;
	    cf->patterns = pat;
	} else if(!strcmp(s->argv[0], "index-file")) {
	    freeca(cf->index);
	    cf->index = cadup(s->argv + 1);
	} else if(!strcmp(s->argv[0], "dot-allow")) {
	    freeca(cf->dotallow);
	    cf->dotallow = cadup(s->argv + 1);
	} else if(!strcmp(s->argv[0], "capture")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing argument to capture declaration", s->file, s->lno);
		continue;
	    }
	    if(cf->capture != NULL)
		free(cf->capture);
	    cf->capture = sstrdup(s->argv[1]);
	    cf->caproot = 0;
	    if((s->argc > 2) && strchr(s->argv[2], 'D'))
		cf->caproot = 1;
	} else if(!strcmp(s->argv[0], "reparse")) {
	    if(s->argc < 2) {
		flog(LOG_WARNING, "%s:%i: missing argument to reparse declaration", s->file, s->lno);
		continue;
	    }
	    if(cf->reparse != NULL)
		free(cf->reparse);
	    cf->reparse = sstrdup(s->argv[1]);
	    cf->parsecomb = 0;
	    if((s->argc > 2) && strchr(s->argv[2], 'c'))
		cf->parsecomb = 1;
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

struct config *getconfig(char *path)
{
    struct config *cf, *ocf;
    struct stat sb;
    char *fn;
    time_t mtime;
    
    fn = sprintf3("%s/.htrc", path);
    for(cf = cflist; cf != NULL; cf = cf->next) {
	if(!strcmp(cf->path, path)) {
	    if(now - cf->lastck > 5) {
		cf->lastck = now;
		if(stat(fn, &sb) || (sb.st_mtime != cf->mtime))
		    break;
	    }
	    return(cf);
	}
    }
    ocf = cf;
    if(access(fn, R_OK) || stat(fn, &sb)) {
	cf = emptyconfig();
	mtime = 0;
    } else {
	if((cf = readconfig(fn)) == NULL)
	    return(NULL);
	mtime = sb.st_mtime;
    }
    if(ocf != NULL) {
	mergechildren(cf->children, ocf->children);
	freeconfig(ocf);
    }
    cf->path = sstrdup(path);
    cf->mtime = mtime;
    cf->lastck = now;
    cf->next = cflist;
    cf->prev = NULL;
    if(cflist != NULL)
	cflist->prev = cf;
    cflist = cf;
    return(cf);
}

struct config **getconfigs(char *file)
{
    static struct config **ret = NULL;
    struct {
	struct config **b;
	size_t s, d;
    } buf;
    struct config *cf;
    char *tmp, *p;
    
    if(ret != NULL)
	free(ret);
    bufinit(buf);
    if(!strncmp(file, "./", 2))
	file += 2;
    tmp = sstrdup(file);
    while(1) {
	if((p = strrchr(tmp, '/')) == NULL)
	    break;
	*p = 0;
	if((cf = getconfig(tmp)) != NULL)
	    bufadd(buf, cf);
    }
    free(tmp);
    if((cf = getconfig(".")) != NULL)
	bufadd(buf, cf);
    if(lconfig != NULL)
	bufadd(buf, lconfig);
    if(gconfig != NULL)
	bufadd(buf, gconfig);
    bufadd(buf, NULL);
    return(ret = buf.b);
}

struct child *findchild(char *file, char *name, struct config **cf)
{
    int i;
    struct config **cfs;
    struct child *ch;
    
    if(cf != NULL)
	*cf = NULL;
    cfs = getconfigs(file);
    for(i = 0; cfs[i] != NULL; i++) {
	if((ch = getchild(cfs[i], name)) != NULL) {
	    if(cf != NULL)
		*cf = cfs[i];
	    return(ch);
	}
    }
    if(!strcmp(name, ".notfound"))
	return(notfound);
    return(NULL);
}

struct pattern *findmatch(char *file, int trydefault, int type)
{
    int i, o, c;
    char *bn, *ln;
    struct config **cfs;
    struct pattern *pat;
    struct rule *rule;
    size_t pl;
    
    if((bn = strrchr(file, '/')) != NULL)
	bn++;
    else
	bn = file;
    cfs = getconfigs(file);
    for(c = 0; cfs[c] != NULL; c++) {
	if(cfs[c]->path == NULL) {
	    ln = file;
	} else {
	    pl = strlen(cfs[c]->path);
	    if((strlen(file) > pl) && !strncmp(file, cfs[c]->path, pl) && (file[pl] == '/'))
		ln = file + pl + 1;
	    else
		ln = file;	/* This should only happen in the base directory. */
	}
	for(pat = cfs[c]->patterns; pat != NULL; pat = pat->next) {
	    if(pat->type != type)
		continue;
	    for(i = 0; (rule = pat->rules[i]) != NULL; i++) {
		if(rule->type == PAT_BASENAME) {
		    for(o = 0; rule->patterns[o] != NULL; o++) {
			if(!fnmatch(rule->patterns[o], bn, 0))
			    break;
		    }
		    if(rule->patterns[o] == NULL)
			break;
		} else if(rule->type == PAT_PATHNAME) {
		    for(o = 0; rule->patterns[o] != NULL; o++) {
			if(!fnmatch(rule->patterns[o], ln, FNM_PATHNAME))
			    break;
		    }
		    if(rule->patterns[o] == NULL)
			break;
		} else if(rule->type == PAT_ALL) {
		} else if(rule->type == PAT_DEFAULT) {
		    if(!trydefault)
			break;
		} else if(rule->type == PAT_LOCAL) {
		    if(strchr(ln, '/'))
			break;
		}
	    }
	    if(!rule)
		return(pat);
	}
    }
    if(!trydefault)
	return(findmatch(file, 1, type));
    return(NULL);
}

static int donotfound(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *idata)
{
    simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
    return(0);
}

static struct chandler i_notfound = {
    .handle = donotfound,
};

static struct child s_notfound = {
    .name = ".notfound",
    .iface = &i_notfound,
};
struct child *notfound = &s_notfound;
