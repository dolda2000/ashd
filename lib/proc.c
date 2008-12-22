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
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <ctype.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <proc.h>
#include <req.h>

int stdmkchild(char **argv)
{
    int i;
    pid_t pid;
    int fd[2];
    
    if(socketpair(PF_UNIX, SOCK_SEQPACKET, 0, fd))
	return(-1);
    if((pid = fork()) < 0)
	return(-1);
    if(pid == 0) {
	for(i = 3; i < FD_SETSIZE; i++) {
	    if(i != fd[0])
		close(i);
	}
	dup2(fd[0], 0);
	close(fd[0]);
	execvp(argv[0], argv);
	flog(LOG_WARNING, "could not exec child program %s: %s", argv[0], strerror(errno));
	exit(127);
    }
    close(fd[0]);
    return(fd[1]);
}

int sendfd(int sock, int fd, char *data, size_t datalen)
{
    struct msghdr msg;
    struct cmsghdr *cmsg;
    char cmbuf[CMSG_SPACE(sizeof(int))];
    struct iovec bufvec;
    
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &bufvec;
    msg.msg_iovlen = 1;
    bufvec.iov_base = data;
    bufvec.iov_len = datalen;
    
    msg.msg_control = cmbuf;
    msg.msg_controllen = sizeof(cmbuf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd;
    msg.msg_controllen = cmsg->cmsg_len;
    
    return(sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT));
}

int recvfd(int sock, char **data, size_t *datalen)
{
    int ret, fd;
    char *buf, cbuf[1024];;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec bufvec;
    
    buf = smalloc(65536);
    memset(&msg, 0, sizeof(msg));

    msg.msg_iov = &bufvec;
    msg.msg_iovlen = 1;
    bufvec.iov_base = buf;
    bufvec.iov_len = 65536;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    
    ret = recvmsg(sock, &msg, 0);
    if(ret <= 0) {
	free(buf);
	if(ret == 0)
	    errno = 0;
	return(-1);
    }
    
    fd = -1;
    for(cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
	if((cmsg->cmsg_level == SOL_SOCKET) && (cmsg->cmsg_type == SCM_RIGHTS)) {
	    fd = *((int *)CMSG_DATA(cmsg));
	}
    }
    if(fd < 0) {
	free(buf);
	errno = EPROTO;
	return(-1);
    }
    buf = realloc(buf, ret);
    *data = buf;
    *datalen = ret;
    return(fd);
}

pid_t stdforkserve(char **argv, struct hthead *req, int fd)
{
    int i;
    char *ebuf, *p;
    pid_t pid;
    struct charvbuf args;
    
    if((pid = fork()) < 0)
	return(-1);
    if(pid == 0) {
	dup2(fd, 0);
	dup2(fd, 1);
	for(i = 3; i < FD_SETSIZE; i++)
	    close(i);
	
	bufinit(args);
	for(i = 0; argv[i]; i++)
	    bufadd(args, argv[i]);
	bufadd(args, req->method);
	bufadd(args, req->url);
	bufadd(args, req->rest);
	bufadd(args, NULL);
	
	for(i = 0; i < req->noheaders; i++) {
	    ebuf = sstrdup(req->headers[i][0]);
	    for(p = ebuf; *p; p++) {
		if(isalnum(*p))
		    *p = toupper(*p);
		else
		    *p = '_';
	    }
	    putenv(sprintf2("REQ_%s=%s", ebuf, req->headers[i][1]));
	}
	putenv(sprintf2("HTTP_VERSION=%s", req->ver));
	
	execvp(args.b[0], args.b);
	flog(LOG_WARNING, "could not exec child program %s: %s", argv[0], strerror(errno));
	exit(127);
    }
    return(pid);
}
