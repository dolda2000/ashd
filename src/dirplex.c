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
#include <fnmatch.h>
#include <time.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>
#include <cf.h>

#define PAT_BASENAME 0
#define PAT_PATHNAME 1
#define PAT_ALL 2
#define PAT_DEFAULT 3

#define PT_FILE 0
#define PT_DIR 1

struct config {
    struct config *next, *prev;
    char *path;
    time_t mtime, lastck;
    struct child *children;
    struct pattern *patterns;
    char **index;
};

struct rule {
    int type;
    char **patterns;
};

struct pattern {
    struct pattern *next;
    int type;
    char *childnm;
    char **fchild;
    struct rule **rules;
};

static struct config *cflist;
static struct config *gconfig, *lconfig;
static time_t now;

static void freerule(struct rule *rule)
{
    freeca(rule->patterns);
    free(rule);
}

static void freepattern(struct pattern *pat)
{
    struct rule **rule;
    
    for(rule = pat->rules; *rule; rule++)
	freerule(*rule);
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
    int sl;

    if(!strcmp(s->argv[0], "match")) {
	s->expstart = 1;
	pat = newpattern();
    } else {
	return(NULL);
    }
    
    if((s->argc > 1) && !strcmp(s->argv[1], "directory"))
	pat->type = PT_DIR;
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

static struct config *readconfig(char *file)
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
	    cf->index = NULL;
	    if(s->argc > 1)
		cf->index = cadup(s->argv + 1);
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

static struct config *getconfig(char *path)
{
    struct config *cf;
    struct stat sb;
    char *fn;
    time_t mtime;
    
    fn = sprintf3("%s/.htrc", path);
    for(cf = cflist; cf != NULL; cf = cf->next) {
	if(!strcmp(cf->path, path)) {
	    if(now - cf->lastck > 5) {
		if(stat(fn, &sb) || (sb.st_mtime != cf->mtime)) {
		    freeconfig(cf);
		    break;
		}
	    }
	    cf->lastck = now;
	    return(cf);
	}
    }
    if(access(fn, R_OK) || stat(fn, &sb)) {
	cf = emptyconfig();
	mtime = 0;
    } else {
	if((cf = readconfig(fn)) == NULL)
	    return(NULL);
	mtime = sb.st_mtime;
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

static struct config **getconfigs(char *file)
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

static struct child *findchild(char *file, char *name)
{
    int i;
    struct config **cfs;
    struct child *ch;
    
    cfs = getconfigs(file);
    for(i = 0; cfs[i] != NULL; i++) {
	if((ch = getchild(cfs[i], name)) != NULL)
	    break;
    }
    return(ch);
}

static struct pattern *findmatch(char *file, int trydefault, int dir)
{
    int i, o, c;
    char *bn;
    struct config **cfs;
    struct pattern *pat;
    struct rule *rule;
    
    if((bn = strrchr(file, '/')) != NULL)
	bn++;
    else
	bn = file;
    cfs = getconfigs(file);
    for(c = 0; cfs[c] != NULL; c++) {
	for(pat = cfs[c]->patterns; pat != NULL; pat = pat->next) {
	    if(!dir && (pat->type == PT_DIR))
		continue;
	    if(dir && (pat->type != PT_DIR))
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
			if(!fnmatch(rule->patterns[o], file, FNM_PATHNAME))
			    break;
		    }
		    if(rule->patterns[o] == NULL)
			break;
		} else if(rule->type == PAT_ALL) {
		} else if(rule->type == PAT_DEFAULT) {
		    if(!trydefault)
			break;
		}
	    }
	    if(!rule)
		return(pat);
	}
    }
    if(!trydefault)
	return(findmatch(file, 1, dir));
    return(NULL);
}

static void handle(struct hthead *req, int fd, char *path, struct pattern *pat)
{
    struct child *ch;

    headappheader(req, "X-Ash-File", path);
    if(pat->fchild) {
	stdforkserve(pat->fchild, req, fd);
    } else {
	if((ch = findchild(path, pat->childnm)) == NULL) {
	    flog(LOG_ERR, "child %s requested, but was not declared", pat->childnm);
	    simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", pat->childnm);
	    return;
	}
	if(childhandle(ch, req, fd))
	    simpleerror(fd, 500, "Server Error", "The request handler crashed.");
    }
}

static void handlefile(struct hthead *req, int fd, char *path)
{
    struct pattern *pat;

    if((pat = findmatch(path, 0, 0)) == NULL) {
	simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
	return;
    }
    handle(req, fd, path, pat);
}

static void handledir(struct hthead *req, int fd, char *path)
{
    struct config **cfs;
    int i, o;
    struct stat sb;
    char *inm, *ipath, *p, *cpath;
    DIR *dir;
    struct dirent *dent;
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
		
		ipath = NULL;
		if(!strchr(inm, '.') && ((dir = opendir(path)) != NULL)) {
		    while((dent = readdir(dir)) != NULL) {
			if((p = strchr(dent->d_name, '.')) == NULL)
			    continue;
			if(strncmp(dent->d_name, inm, strlen(inm)))
			    continue;
			ipath = sprintf2("%s/%s", path, dent->d_name);
			if(stat(ipath, &sb) || !S_ISREG(sb.st_mode)) {
			    free(ipath);
			    ipath = NULL;
			    continue;
			}
			break;
		    }
		    closedir(dir);
		}
		if(ipath != NULL) {
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

static int checkdir(struct hthead *req, int fd, char *path)
{
    return(0);
}

static void serve(struct hthead *req, int fd)
{
    char *p, *p2, *path, *tmp, *buf, *p3, *nm;
    struct stat sb;
    DIR *dir;
    struct dirent *dent;
    
    now = time(NULL);
    nm = req->rest;
    path = sstrdup(".");
    p = nm;
    while(1) {
	if((p2 = strchr(p, '/')) == NULL) {
	} else {
	    *(p2++) = 0;
	}
	if((tmp = unquoteurl(p)) == NULL) {
	    simpleerror(fd, 400, "Bad Request", "The requested URL contains an invalid escape sequence.");
	    goto fail;
	}
	strcpy(p, tmp);
	free(tmp);
	
	if(!*p) {
	    if(p2 == NULL) {
		if(stat(path, &sb)) {
		    flog(LOG_WARNING, "failed to stat previously stated directory %s: %s", path, strerror(errno));
		    simpleerror(fd, 500, "Internal Server Error", "The server encountered an unexpected condition.");
		    goto fail;
		}
		break;
	    } else {
		simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
		goto fail;
	    }
	}
	if(*p == '.') {
	    simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
	    goto fail;
	}
	
	getconfig(path);
	
	/*
	 * First, check the name verbatimely:
	 */
	buf = sprintf3("%s/%s", path, p);
	if(!stat(buf, &sb)) {
	    if(S_ISDIR(sb.st_mode)) {
		tmp = path;
		if(!strcmp(path, "."))
		    path = sstrdup(p);
		else
		    path = sprintf2("%s/%s", path, p);
		free(tmp);
		if(p2 == NULL) {
		    stdredir(req, fd, 301, sprintf3("%s/", p));
		    goto out;
		}
		if(checkdir(req, fd, path))
		    break;
		goto next;
	    }
	    if(S_ISREG(sb.st_mode)) {
		tmp = path;
		path = sprintf2("%s/%s", path, p);
		free(tmp);
		break;
	    }
	    simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
	    goto fail;
	}

	/*
	 * Check the file extensionlessly:
	 */
	if(!strchr(p, '.') && ((dir = opendir(path)) != NULL)) {
	    while((dent = readdir(dir)) != NULL) {
		buf = sprintf3("%s/%s", path, dent->d_name);
		if((p3 = strchr(dent->d_name, '.')) != NULL)
		    *p3 = 0;
		if(strcmp(dent->d_name, p))
		    continue;
		if(stat(buf, &sb))
		    continue;
		if(!S_ISREG(sb.st_mode))
		    continue;
		tmp = path;
		path = sstrdup(buf);
		free(tmp);
		break;
	    }
	    closedir(dir);
	    if(dent != NULL)
		break;
	}
	
	simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
	goto fail;
	
    next:
	if(p2 == NULL)
	    break;
	p = p2;
    }
    if(p2 == NULL)
	replrest(req, "");
    else
	replrest(req, p2);
    if(!strncmp(path, "./", 2))
	memmove(path, path + 2, strlen(path + 2) + 1);
    if(S_ISDIR(sb.st_mode)) {
	handledir(req, fd, path);
    } else if(S_ISREG(sb.st_mode)) {
	handlefile(req, fd, path);
    } else {
	simpleerror(fd, 404, "Not Found", "The requested URL has no corresponding resource.");
	goto fail;
    }
    goto out;
    
fail:
    /* No special handling, for now at least. */
out:
    free(path);
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
