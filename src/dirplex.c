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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>

#define CH_SOCKET 0
#define CH_FORK 1

#define PAT_BASENAME 0
#define PAT_PATHNAME 1
#define PAT_ALL 2
#define PAT_DEFAULT 3

struct config {
    struct config *next, *prev;
    char *path;
    time_t mtime;
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
    char *pattern;
};

struct pattern {
    struct pattern *next;
    char *childnm;
    struct rule **rules;
};

struct config *cflist;

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

static void freepattern(struct pattern *pat)
{
    struct rule **rule;
    
    for(rule = pat->rules; *rule; rule++) {
	if((*rule)->pattern != NULL)
	    free((*rule)->pattern);
	free(*rule);
    }
    if(pat->childnm != NULL)
	free(pat->childnm);
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
    free(cf->path);
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

static struct config *readconfig(char *path)
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
    struct stat sb;
    
    p = sprintf3("%s/.htrc", path);
    if(stat(p, &sb))
	return(NULL);
    if((s = fopen(p, "r")) == NULL)
	return(NULL);
    omalloc(cf);
    cf->mtime = sb.st_mtime;
    cf->path = sstrdup(path);
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
		flog(LOG_WARNING, "%s%i: unexpected line indentation in global scope", path, lno);
		goto next;
	    } else {
		if(!w[0]) {
		} else if(!strcmp(w[0], "child")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing name in child declaration", path, lno);
			goto next;
		    }
		    child = newchild(w[1], CH_SOCKET);
		    state = 1;
		} else if(!strcmp(w[0], "fchild")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing name in child declaration", path, lno);
			goto next;
		    }
		    child = newchild(w[1], CH_FORK);
		    state = 1;
		} else if(!strcmp(w[0], "match")) {
		    pat = newpattern();
		    state = 2;
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", path, lno, w[0]);
		}
	    }
	} else if(state == 1) {
	    if(ind) {
		if(!w[0]) {
		} else if(!strcmp(w[0], "exec")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: too few parameters to `exec'", path, lno);
			goto next;
		    }
		    child->argv = szmalloc(sizeof(*child->argv) * argc);
		    for(i = 0; i < argc - 1; i++)
			child->argv[i] = sstrdup(w[i + 1]);
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", path, lno, w[0]);
		}
	    } else {
		state = 0;
		if(child->argv == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing `exec' in child declaration %s", path, lno, child->name);
		    freechild(child);
		    goto retry;
		}
		child->next = cf->children;
		cf->children = child;
		goto retry;
	    }
	} else if(state == 2) {
	    if(ind) {
		if(!w[0]) {
		} else if(!strcmp(w[0], "filename")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing pattern for `filename' match", path, lno);
			goto next;
		    }
		    rule = newrule(pat);
		    rule->type = PAT_BASENAME;
		    rule->pattern = sstrdup(w[1]);
		} else if(!strcmp(w[0], "pathname")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing pattern for `pathname' match", path, lno);
			goto next;
		    }
		    rule = newrule(pat);
		    rule->type = PAT_PATHNAME;
		    rule->pattern = sstrdup(w[1]);
		} else if(!strcmp(w[0], "all")) {
		    newrule(pat)->type = PAT_ALL;
		} else if(!strcmp(w[0], "default")) {
		    newrule(pat)->type = PAT_DEFAULT;
		} else if(!strcmp(w[0], "handler")) {
		    if(argc < 2) {
			flog(LOG_WARNING, "%s:%i: missing child name for `handler' directive", path, lno);
			goto next;
		    }
		    if(pat->childnm != NULL)
			free(pat->childnm);
		    pat->childnm = sstrdup(w[1]);
		} else {
		    flog(LOG_WARNING, "%s:%i: unknown directive %s", path, lno, w[0]);
		}
	    } else {
		state = 0;
		if(pat->rules[0] == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing rules in match declaration", path, lno);
		    freepattern(pat);
		    goto retry;
		}
		if(pat->childnm == NULL) {
		    flog(LOG_WARNING, "%s:%i: missing handler in match declaration", path, lno);
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

static struct config *getconfig(char *path)
{
    struct config *cf;
    struct stat sb;
    
    for(cf = cflist; cf != NULL; cf = cf->next) {
	if(!strcmp(cf->path, path)) {
	    if(stat(sprintf3("%s/.htrc", path), &sb))
		return(NULL);
	    if(sb.st_mtime != cf->mtime) {
		freeconfig(cf);
		break;
	    }
	    return(cf);
	}
    }
    if((cf = readconfig(path)) != NULL) {
	cf->next = cflist;
	cflist = cf;
    }
    return(cf);
}

static struct child *findchild(char *file, char *name)
{
    char *buf, *p;
    struct config *cf;
    struct child *ch;

    buf = sstrdup(file);
    while(1) {
	ch = NULL;
	if(!strcmp(buf, "."))
	    break;
	if((p = strrchr(buf, '/')) != NULL)
	    *p = 0;
	else
	    strcpy(buf, ".");
	cf = getconfig(buf);
	if(cf == NULL)
	    continue;
	if((ch = getchild(cf, name)) != NULL)
	    break;
    }
    free(buf);
    return(ch);
}

static struct pattern *findmatch(char *file, int trydefault)
{
    int i;
    char *buf, *p, *bn;
    struct config *cf;
    struct pattern *pat;
    struct rule *rule;
    
    if((bn = strrchr(file, '/')) != NULL)
	bn++;
    else
	bn = file;
    buf = sstrdup(file);
    while(1) {
	pat = NULL;
	if(!strcmp(buf, "."))
	    break;
	if((p = strrchr(buf, '/')) != NULL)
	    *p = 0;
	else
	    strcpy(buf, ".");
	cf = getconfig(buf);
	if(cf == NULL)
	    continue;
	for(pat = cf->patterns; pat != NULL; pat = pat->next) {
	    for(i = 0; (rule = pat->rules[i]) != NULL; i++) {
		if(rule->type == PAT_BASENAME) {
		    if(fnmatch(rule->pattern, bn, 0))
			break;
		} else if(rule->type == PAT_PATHNAME) {
		    if(fnmatch(rule->pattern, file, FNM_PATHNAME))
			break;
		} else if(rule->type == PAT_ALL) {
		} else if(rule->type == PAT_DEFAULT) {
		    if(!trydefault)
			break;
		}
	    }
	    if(!rule)
		goto out;
	}
    }

out:
    free(buf);
    return(pat);
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
	simpleerror(fd, 500, "Server Error", "The request handler crashed.");
    }
}

static void handlefile(struct hthead *req, int fd, char *path)
{
    struct pattern *pat;
    struct child *ch;

    headappheader(req, "X-Ash-File", path);
    if(((pat = findmatch(path, 0)) == NULL) && ((pat = findmatch(path, 1)) == NULL)) {
	/* XXX: Send a 500 error? 404? */
	return;
    }
    if((ch = findchild(path, pat->childnm)) == NULL) {
	flog(LOG_ERR, "child %s requested, but was not declared", pat->childnm);
	simpleerror(fd, 500, "Configuration Error", "The server is erroneously configured. Handler %s was requested, but not declared.", pat->childnm);
	return;
    }
    
    if(ch->type == CH_SOCKET) {
	passreq(ch, req, fd);
    } else if(ch->type == CH_FORK) {
	stdforkserve(ch->argv, req, fd);
    }
}

static void handledir(struct hthead *req, int fd, char *path)
{
    /* XXX: Todo */
    simpleerror(fd, 403, "Not Authorized", "Will not send directory listings or indices yet");
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
    
    nm = req->rest;
    path = sstrdup(".");
    p = nm;
    while(1) {
	if((p2 = strchr(p, '/')) == NULL) {
	} else {
	    *(p2++) = 0;
	}
	
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

int main(int argc, char **argv)
{
    struct hthead *req;
    int fd;
    
    if(argc < 2) {
	flog(LOG_ERR, "usage: dirplex DIR");
	exit(1);
    }
    if(chdir(argv[1])) {
	flog(LOG_ERR, "could not change directory to %s: %s", argv[1], strerror(errno));
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
