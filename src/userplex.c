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
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <resp.h>

struct user {
    struct user *next, *prev;
    char *name;
    int fd;
};

static int ignore = 0;
static char *mgroup = NULL;
static char *dirname = NULL;
static char **childspec;
static uid_t minuid = 0;
static struct user *users = NULL;

static void login(struct passwd *pwd)
{
    int fd;
    
    if(getuid() == 0) {
	if(initgroups(pwd->pw_name, pwd->pw_gid)) {
	    flog(LOG_ERR, "could not init group list for %s: %s", pwd->pw_name, strerror(errno));
	    exit(1);
	}
	if(setgid(pwd->pw_gid)) {
	    flog(LOG_ERR, "could not switch group for %s: %s", pwd->pw_name, strerror(errno));
	    exit(1);
	}
	if(setuid(pwd->pw_uid)) {
	    flog(LOG_ERR, "could not switch user to %s: %s", pwd->pw_name, strerror(errno));
	    exit(1);
	}
    } else {
	if(getuid() != pwd->pw_uid)
	    exit(1);
    }
    if(chdir(pwd->pw_dir)) {
	flog(LOG_ERR, "could not change to home directory for %s: %s", pwd->pw_name, strerror(errno));
	exit(1);
    }
    putenv(sprintf2("HOME=%s", pwd->pw_dir));
    putenv(sprintf2("SHELL=%s", pwd->pw_shell));
    putenv(sprintf2("USER=%s", pwd->pw_name));
    putenv(sprintf2("LOGNAME", pwd->pw_name));
    /* There's whole load of other stuff one could want to do here --
     * getting Kerberos credentials, running PAM session modules, and
     * who knows what. I'll add them along as I find them useful. */
    if(((fd = open(".ashd/output", O_WRONLY | O_APPEND)) >= 0) ||
       ((fd = open("/dev/null", 0)) >= 0)) {
	dup2(fd, 1);
	close(fd);
    }
    if(((fd = open(".ashd/error", O_WRONLY | O_APPEND)) >= 0) ||
       ((fd = open("/dev/null", 0)) >= 0)) {
	dup2(fd, 2);
	close(fd);
    }
}

static void execchild(struct passwd *pwd)
{
    if(!ignore)
	execl(".ashd/handler", ".ashd/handler", NULL);
    if(dirname != NULL) {
	if(access(dirname, X_OK | R_OK))
	    return;
    }
    execvp(childspec[0], childspec);
}

static int forkchild(char *usrnm)
{
    struct passwd *pwd;
    pid_t pid;
    int i, fd[2];
    
    /* XXX: There should be a way for the child to report errors (like
     * 404 when htpub doesn't exist), but for now I don't bother with
     * that. I might return to it at some later time. */
    if(socketpair(PF_UNIX, SOCK_SEQPACKET, 0, fd))
	return(-1);
    if((pwd = getpwnam(usrnm)) == NULL) {
	flog(LOG_ERR, "already discovered user `%s' has disappeared", usrnm);
	return(-1);
    }
    if((pid = fork()) < 0)
	return(-1);
    if(pid == 0) {
	for(i = 3; i < FD_SETSIZE; i++) {
	    if(i != fd[0])
		close(i);
	}
	dup2(fd[0], 0);
	close(fd[0]);
	login(pwd);
	execchild(pwd);
	exit(127);
    }
    close(fd[0]);
    return(fd[1]);
}

static void serve2(struct user *usr, struct hthead *req, int fd)
{
    if(usr->fd < 0)
	usr->fd = forkchild(usr->name);
    if(sendreq(usr->fd, req, fd)) {
	if(errno == EPIPE) {
	    /* Assume that the child has crashed and restart it. */
	    close(usr->fd);
	    usr->fd = forkchild(usr->name);
	    if(!sendreq(usr->fd, req, fd))
		return;
	}
	flog(LOG_ERR, "could not pass on request to user `%s': %s", usr->name, strerror(errno));
	close(usr->fd);
	usr->fd = -1;
	simpleerror(fd, 500, "User Error", "The request handler for that user keeps crashing.");
    }
}

static void initnew(struct hthead *req, int fd, char *usrnm)
{
    struct user *usr;
    struct passwd *pwd;
    struct group *grp;
    int i, valid;

    pwd = getpwnam(usrnm);
    if(pwd == NULL) {
	simpleerror(fd, 404, "Not Found", "No such resource could be found.");
	return;
    }
    if(pwd->pw_uid < minuid) {
	simpleerror(fd, 404, "Not Found", "No such resource could be found.");
	return;
    }
    if(mgroup) {
	if((grp = getgrnam(mgroup)) == NULL) {
	    flog(LOG_ERR, "unknown group %s specified to userplex", mgroup);
	    simpleerror(fd, 500, "Configuration Error", "The server has been erroneously configured.");
	    return;
	}
	valid = 0;
	if(grp->gr_gid == pwd->pw_gid) {
	    valid = 1;
	} else {
	    for(i = 0; grp->gr_mem[i] != NULL; i++) {
		if(!strcmp(grp->gr_mem[i], usrnm)) {
		    valid = 1;
		    break;
		}
	    }
	}
	if(!valid) {
	    simpleerror(fd, 404, "Not Found", "No such resource could be found.");
	    return;
	}
    }
    omalloc(usr);
    usr->name = sstrdup(usrnm);
    usr->fd = -1;
    usr->next = users;
    usr->prev = NULL;
    if(users != NULL)
	users->prev = usr;
    users = usr;
    serve2(usr, req, fd);
}

static void serve(struct hthead *req, int fd)
{
    struct user *usr;
    char *usrnm, *p;
    
    if((p = strchr(req->rest, '/')) == NULL) {
	if(*req->rest)
	    stdredir(req, fd, 301, sprintf3("%s/", req->url));
	else
	    simpleerror(fd, 404, "Not Found", "No such resource could be found.");
	return;
    }
    *(p++) = 0;
    usrnm = sstrdup(req->rest);
    replrest(req, p);
    for(usr = users; usr != NULL; usr = usr->next) {
	if(!strcmp(usr->name, usrnm)) {
	    serve2(usr, req, fd);
	    goto out;
	}
    }
    initnew(req, fd, usrnm);
    
out:
    free(usrnm);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: userplex [-hI] [-g GROUP] [-m MIN-UID] [-d PUB-DIR] [PROGRAM ARGS...]\n");
}

int main(int argc, char **argv)
{
    struct hthead *req;
    int c;
    int fd;
    struct charvbuf csbuf;
    
    while((c = getopt(argc, argv, "+hIg:m:d:")) >= 0) {
	switch(c) {
	case 'I':
	    ignore = 1;
	    break;
	case 'm':
	    if((minuid = atoi(optarg)) < 1) {
		fprintf(stderr, "userplex: argument to -m must be greater than 0\n");
		exit(1);
	    }
	    break;
	case 'g':
	    mgroup = optarg;
	    break;
	case 'd':
	    dirname = optarg;
	    break;
	case 'h':
	    usage(stdout);
	    exit(0);
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(optind < argc) {
	childspec = argv + optind;
    } else {
	if(dirname == NULL)
	    dirname = "htpub";
	bufinit(csbuf);
	bufadd(csbuf, "dirplex");
	bufadd(csbuf, dirname);
	bufadd(csbuf, NULL);
	childspec = csbuf.b;
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
