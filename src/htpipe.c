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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

static void usage(FILE *out)
{
    fprintf(out, "usge: htpipe [-h] [-CS] SOCKET-PATH [CHILD ARGS...]\n");
}

static int clconnect(char *path)
{
    int sk;
    struct sockaddr_un unm;
    
    if((sk = socket(AF_UNIX, SOCK_SEQPACKET, 0)) < 0)
	return(-1);
    memset(&unm, 0, sizeof(unm));
    unm.sun_family = AF_UNIX;
    strcpy(unm.sun_path, path);
    if(connect(sk, (struct sockaddr *)&unm, sizeof(unm))) {
	close(sk);
	return(-1);
    }
    return(sk);
}

static int mklisten(char *path)
{
    int sk;
    struct sockaddr_un unm;
    struct stat sb;
    
    if(!stat(path, &sb) && S_ISSOCK(sb.st_mode))
	unlink(path);
    if((sk = socket(AF_UNIX, SOCK_SEQPACKET, 0)) < 0)
	return(-1);
    memset(&unm, 0, sizeof(unm));
    unm.sun_family = AF_UNIX;
    strcpy(unm.sun_path, path);
    if(bind(sk, (struct sockaddr *)&unm, sizeof(unm)) || listen(sk, 128)) {
	close(sk);
	return(-1);
    }
    return(sk);
}

static void runclient(int sk)
{
    int fd;
    struct hthead *req;
    
    while(1) {
	if((fd = recvreq(0, &req)) < 0) {
	    if(errno == 0)
		break;
	    flog(LOG_ERR, "htpipe: error in recvreq: %s", strerror(errno));
	    exit(1);
	}
	if(sendreq(sk, req, fd)) {
	    flog(LOG_ERR, "htpipe: could not pass request across pipe: %s", strerror(errno));
	    exit(1);
	}
	freehthead(req);
	close(fd);
    }
}

static void runserver(int lsk, int ch)
{
    int i, o, ret, rfd, ncl, *cl, acl;
    struct hthead *req;
    
    ncl = 0;
    while(1) {
	struct pollfd pfd[ncl + 1];
	for(i = 0; i < ncl; i++) {
	    pfd[i].fd = cl[i];
	    pfd[i].events= POLLIN;
	}
	pfd[i].fd = lsk;
	pfd[i].events = POLLIN;
	if((ret = poll(pfd, ncl + 1, -1)) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "htpipe: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	for(i = 0; i < ncl; i++) {
	    if(pfd[i].revents & POLLIN) {
		if((rfd = recvreq(cl[i], &req)) < 0) {
		    if(errno != 0)
			flog(LOG_ERR, "htpipe: error from client: %s", strerror(errno));
		    cl[i] = -1;
		} else {
		    if(sendreq(ch, req, rfd)) {
			flog(LOG_ERR, "htpipe: could not pass request to child: %s", strerror(errno));
			exit(1);
		    }
		    freehthead(req);
		    close(rfd);
		}
	    }
	}
	if(pfd[i].revents & POLLIN) {
	    if((acl = accept(lsk, NULL, 0)) < 0) {
		flog(LOG_ERR, "htpipe: error in accept: %s", strerror(errno));
	    } else {
		cl = srealloc(cl, sizeof(*cl) * (ncl + 1));
		cl[ncl++] = acl;
	    }
	}
	for(i = o = 0; i < ncl; i++) {
	    if(cl[i] >= 0)
		cl[o++] = cl[i];
	}
	ncl = o;
    }
}

int main(int argc, char **argv)
{
    int c, cli, srv, sk, ch, sst;
    pid_t sproc;
    char *path, **chspec;
    
    cli = srv = 0;
    while((c = getopt(argc, argv, "+hCS")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'C':
	    cli = 1;
	    break;
	case 'S':
	    srv = 1;
	    break;
	}
    }
    if(argc - optind < 1) {
	usage(stderr);
	exit(1);
    }
    path = argv[optind++];
    chspec = argv + optind;
    if(cli) {
	if((sk = clconnect(path)) < 0) {
	    flog(LOG_ERR, "htpipe: %s: %s", path, strerror(errno));
	    exit(1);
	}
	runclient(sk);
    } else if(srv) {
	if(!*chspec) {
	    usage(stderr);
	    exit(1);
	}
	if((sk = mklisten(path)) < 0) {
	    flog(LOG_ERR, "htpipe: %s: %s", path, strerror(errno));
	    exit(1);
	}
	if((ch = stdmkchild(chspec, NULL, NULL)) < 0) {
	    flog(LOG_ERR, "htpipe: could not fork child: %s", strerror(errno));
	    exit(1);
	}
	runserver(sk, ch);
    } else {
	if(!*chspec) {
	    usage(stderr);
	    exit(1);
	}
	if((sk = clconnect(path)) < 0) {
	    if((sproc = fork()) < 0)
		err(1, "fork");
	    if(sproc == 0) {
		if((sk = mklisten(path)) < 0) {
		    flog(LOG_ERR, "htpipe: %s: %s", path, strerror(errno));
		    exit(1);
		}
		if((ch = stdmkchild(chspec, NULL, NULL)) < 0) {
		    flog(LOG_ERR, "htpipe: could not fork child: %s", strerror(errno));
		    exit(1);
		}
		daemon(0, 1);
		runserver(sk, ch);
		abort();
	    }
	    if((waitpid(sproc, &sst, 0)) != sproc) {
		flog(LOG_ERR, "htpipe: could not wait for server process: %s", strerror(errno));
		exit(1);
	    }
	    if((sk = clconnect(path)) < 0) {
		flog(LOG_ERR, "htpipe: could not connect to newly forked server: %s", strerror(errno));
		exit(1);
	    }
	}
	runclient(sk);
    }
    return(0);
}
