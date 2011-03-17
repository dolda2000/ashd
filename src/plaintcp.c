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
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <req.h>
#include <mt.h>
#include <mtio.h>
#include <log.h>

#include "htparser.h"

struct tcpport {
    int fd;
    int sport;
};

struct tcpconn {
    struct sockaddr_storage name;
    struct tcpport *port;
    int fd;
};

int listensock4(int port)
{
    struct sockaddr_in name;
    int fd;
    int valbuf;
    
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    if((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	return(-1);
    valbuf = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &valbuf, sizeof(valbuf));
    if(bind(fd, (struct sockaddr *)&name, sizeof(name))) {
	close(fd);
	return(-1);
    }
    if(listen(fd, 128) < 0) {
	close(fd);
	return(-1);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return(fd);
}

int listensock6(int port)
{
    struct sockaddr_in6 name;
    int fd;
    int valbuf;
    
    memset(&name, 0, sizeof(name));
    name.sin6_family = AF_INET6;
    name.sin6_port = htons(port);
    if((fd = socket(PF_INET6, SOCK_STREAM, 0)) < 0)
	return(-1);
    valbuf = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &valbuf, sizeof(valbuf));
    if(bind(fd, (struct sockaddr *)&name, sizeof(name))) {
	close(fd);
	return(-1);
    }
    if(listen(fd, 128) < 0) {
	close(fd);
	return(-1);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return(fd);
}

char *formathaddress(struct sockaddr *name, socklen_t namelen)
{
    static char buf[128];
    struct sockaddr_in *v4;
    struct sockaddr_in6 *v6;

    switch(name->sa_family) {
    case AF_INET:
	v4 = (struct sockaddr_in *)name;
	if(!inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf)))
	    return(NULL);
	return(buf);
    case AF_INET6:
	v6 = (struct sockaddr_in6 *)name;
	if(IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
	    if(!inet_ntop(AF_INET, ((char *)&v6->sin6_addr) + 12, buf, sizeof(buf)))
		return(NULL);
	} else {
	    if(!inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf)))
		return(NULL);
	}
	return(buf);
    default:
	errno = EPFNOSUPPORT;
	return(NULL);
    }
}

static int initreq(struct conn *conn, struct hthead *req)
{
    struct tcpconn *tcp = conn->pdata;
    struct sockaddr_storage sa;
    socklen_t salen;
    
    headappheader(req, "X-Ash-Address", formathaddress((struct sockaddr *)&tcp->name, sizeof(sa)));
    if(tcp->name.ss_family == AF_INET)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)&tcp->name)->sin_port)));
    else if(tcp->name.ss_family == AF_INET6)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)&tcp->name)->sin6_port)));
    salen = sizeof(sa);
    if(!getsockname(tcp->fd, (struct sockaddr *)&sa, &salen))
	headappheader(req, "X-Ash-Server-Address", formathaddress((struct sockaddr *)&sa, sizeof(sa)));
    headappheader(req, "X-Ash-Server-Port", sprintf3("%i", tcp->port->sport));
    headappheader(req, "X-Ash-Protocol", "http");
    return(0);
}

void servetcp(struct muth *muth, va_list args)
{
    vavar(int, fd);
    vavar(struct sockaddr_storage, name);
    vavar(struct tcpport *, stcp);
    FILE *in;
    struct conn conn;
    struct tcpconn tcp;
    
    memset(&conn, 0, sizeof(conn));
    memset(&tcp, 0, sizeof(tcp));
    in = mtstdopen(fd, 1, 60, "r+");
    conn.pdata = &tcp;
    conn.initreq = initreq;
    tcp.fd = fd;
    tcp.name = name;
    tcp.port = stcp;
    serve(in, &conn);
}

static void listenloop(struct muth *muth, va_list args)
{
    vavar(struct tcpport *, tcp);
    int ns;
    struct sockaddr_storage name;
    socklen_t namelen;
    
    while(1) {
	namelen = sizeof(name);
	block(tcp->fd, EV_READ, 0);
	ns = accept(tcp->fd, (struct sockaddr *)&name, &namelen);
	if(ns < 0) {
	    flog(LOG_ERR, "accept: %s", strerror(errno));
	    goto out;
	}
	mustart(servetcp, ns, name, tcp);
    }
    
out:
    close(tcp->fd);
    free(tcp);
}

void handleplain(int argc, char **argp, char **argv)
{
    int port, fd;
    int i;
    struct tcpport *tcp;
    
    port = 80;
    for(i = 0; i < argc; i++) {
	if(!strcmp(argp[i], "help")) {
	    printf("plain handler parameters:\n");
	    printf("\tport=TCP-PORT   [80]\n");
	    printf("\t\tThe TCP port to listen on.\n");
	    exit(0);
	} else if(!strcmp(argp[i], "port")) {
	    port = atoi(argv[i]);
	} else {
	    flog(LOG_ERR, "unknown parameter `%s' to plain handler", argp[i]);
	    exit(1);
	}
    }
    if((fd = listensock6(port)) < 0) {
	flog(LOG_ERR, "could not listen on IPv6 (port %i): %s", port, strerror(errno));
	exit(1);
    }
    omalloc(tcp);
    tcp->fd = fd;
    tcp->sport = port;
    mustart(listenloop, tcp);
    if((fd = listensock4(port)) < 0) {
	if(errno != EADDRINUSE) {
	    flog(LOG_ERR, "could not listen on IPv4 (port %i): %s", port, strerror(errno));
	    exit(1);
	}
    } else {
	omalloc(tcp);
	tcp->fd = fd;
	tcp->sport = port;
	mustart(listenloop, tcp);
    }
}
