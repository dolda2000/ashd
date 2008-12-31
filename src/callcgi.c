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
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>

static char **environ;

static void passdata(FILE *in, FILE *out)
{
    int ret;
    char *buf;
    
    buf = smalloc(65536);
    while(!feof(in)) {
	ret = fread(buf, 1, 65536, in);
	if(ferror(in)) {
	    flog(LOG_ERR, "sendfile: could not read input: %s", strerror(errno));
	    break;
	}
	if(fwrite(buf, 1, ret, out) != ret) {
	    flog(LOG_ERR, "sendfile: could not write output: %s", strerror(errno));
	    break;
	}
    }
    free(buf);
}

static void forkchild(int inpath, char *prog, char *file, char *method, char *url, char *rest, int *infd, int *outfd)
{
    int i;
    char *qp, **env;
    int inp[2], outp[2];
    pid_t pid;

    pipe(inp);
    pipe(outp);
    if((pid = fork()) < 0) {
	flog(LOG_ERR, "callcgi: could not fork");
	exit(1);
    }
    if(pid == 0) {
	close(inp[1]);
	close(outp[0]);
	dup2(inp[0], 0);
	dup2(outp[1], 1);
	for(i = 3; i < FD_SETSIZE; i++)
	    close(i);
	if((qp = strchr(url, '?')) != NULL)
	    *(qp++) = 0;
	/*
	 * XXX: Currently missing:
	 *  SERVER_NAME (Partially)
	 *  SERVER_PORT
	 */
	putenv(sprintf2("SERVER_SOFTWARE=ashd/%s", VERSION));
	putenv("GATEWAY_INTERFACE=CGI/1.1");
	if(getenv("HTTP_VERSION"))
	    putenv(sprintf2("SERVER_PROTOCOL=HTTP/%s", getenv("HTTP_VERSION")));
	putenv(sprintf2("REQUEST_METHOD=%s", method));
	putenv(sprintf2("PATH_INFO=%s", rest));
	putenv(sprintf2("SCRIPT_NAME=%s", url));
	putenv(sprintf2("QUERY_STRING=%s", qp?qp:""));
	if(getenv("REQ_HOST"))
	    putenv(sprintf2("SERVER_NAME=%s", getenv("REQ_HOST")));
	if(getenv("REQ_X_ASH_ADDRESS"))
	    putenv(sprintf2("REMOTE_ADDR=%s", getenv("REQ_X_ASH_ADDRESS")));
	if(getenv("REQ_CONTENT_TYPE"))
	    putenv(sprintf2("CONTENT_TYPE=%s", getenv("REQ_CONTENT_TYPE")));
	if(getenv("REQ_CONTENT_LENGTH"))
	    putenv(sprintf2("CONTENT_LENGTH=%s", getenv("REQ_CONTENT_LENGTH")));
	for(env = environ; *env; env++) {
	    if(!strncmp(*env, "REQ_", 4))
		putenv(sprintf2("HTTP_%s", (*env) + 4));
	}
	/*
	 * This is (understandably) missing from the CGI
	 * specification, but PHP seems to require it.
	 */
	putenv(sprintf2("SCRIPT_FILENAME=%s", file));
	if(inpath)
	    execlp(prog, prog, file, NULL);
	else
	    execl(prog, prog, file, NULL);
	exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    *infd = inp[1];
    *outfd = outp[0];
}

static void trim(struct charbuf *buf)
{
    char *p;
    
    for(p = buf->b; (p - buf->b < buf->d) && isspace(*p); p++);
    memmove(buf->b, p, buf->d -= (p - buf->b));
    for(p = buf->b + buf->d - 1; (p > buf->b) && isspace(*p); p--, buf->d--);
}

static char **parseheaders(FILE *s)
{
    int c, state;
    struct charvbuf hbuf;
    struct charbuf buf;
    
    bufinit(hbuf);
    bufinit(buf);
    state = 0;
    while(1) {
	c = fgetc(s);
    again:
	if(state == 0) {
	    if(c == '\r') {
	    } else if(c == '\n') {
		break;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		state = 1;
		goto again;
	    }
	} else if(state == 1) {
	    if(c == ':') {
		trim(&buf);
		bufadd(buf, 0);
		bufadd(hbuf, buf.b);
		bufinit(buf);
		state = 2;
	    } else if(c == '\r') {
	    } else if(c == '\n') {
		goto fail;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		bufadd(buf, c);
	    }
	} else if(state == 2) {
	    if(c == '\r') {
	    } else if(c == '\n') {
		trim(&buf);
		bufadd(buf, 0);
		bufadd(hbuf, buf.b);
		bufinit(buf);
		state = 0;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		bufadd(buf, c);
	    }
	}
    }
    bufadd(hbuf, NULL);
    return(hbuf.b);
    
fail:
    buffree(hbuf);
    buffree(buf);
    return(NULL);
}

static void sendstatus(char **headers, FILE *out)
{
    char **hp;
    char *status, *location;
    
    hp = headers;
    status = location = NULL;
    while(*hp) {
	if(!strcasecmp(hp[0], "status")) {
	    status = hp[1];
	    /* Clear this header, so that it is not transmitted by sendheaders. */
	    **hp = 0;
	} else if(!strcasecmp(hp[0], "location")) {
	    location = hp[1];
	} else {
	    hp += 2;
	}
    }
    if(status) {
	fprintf(out, "HTTP/1.1 %s\r\n", status);
    } else if(location) {
	fprintf(out, "HTTP/1.1 303 See Other\r\n");
    } else {
	fprintf(out, "HTTP/1.1 200 OK\r\n");
    }
}

static void sendheaders(char **headers, FILE *out)
{
    while(*headers) {
	if(**headers)
	    fprintf(out, "%s: %s\r\n", headers[0], headers[1]);
	headers += 2;
    }
}

static void usage(void)
{
    flog(LOG_ERR, "usage: callcgi [-p PROGRAM] METHOD URL REST");
}

int main(int argc, char **argv, char **envp)
{
    int c;
    char *file, *prog;
    int inpath;
    int infd, outfd;
    FILE *in, *out;
    char **headers;
    
    environ = envp;
    signal(SIGPIPE, SIG_IGN);
    
    prog = NULL;
    inpath = 0;
    while((c = getopt(argc, argv, "p:")) >= 0) {
	switch(c) {
	case 'p':
	    prog = optarg;
	    inpath = 1;
	    break;
	default:
	    usage();
	    exit(1);
	}
    }
    
    if(argc - optind < 3) {
	usage();
	exit(1);
    }
    if((file = getenv("REQ_X_ASH_FILE")) == NULL) {
	flog(LOG_ERR, "callcgi: needs to be called with the X-Ash-File header");
	exit(1);
    }
    if(prog == NULL)
	prog = file;
    forkchild(inpath, prog, file, argv[optind], argv[optind + 1], argv[optind + 2], &infd, &outfd);
    in = fdopen(infd, "w");
    passdata(stdin, in);
    fclose(in);
    out = fdopen(outfd, "r");
    if((headers = parseheaders(out)) == NULL) {
	flog(LOG_WARNING, "CGI handler returned invalid headers");
	exit(1);
    }
    sendstatus(headers, stdout);
    sendheaders(headers, stdout);
    printf("\r\n");
    passdata(out, stdout);
    return(0);
}
