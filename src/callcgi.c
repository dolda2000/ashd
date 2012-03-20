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
#include <sys/poll.h>
#include <sys/wait.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>

static char **environ;

static int passdata(FILE *in, FILE *out)
{
    int ret;
    char buf[65536];
    struct pollfd pfds[2];
    
    while(!feof(in)) {
	memset(pfds, 0, sizeof(struct pollfd) * 2);
	pfds[0].fd = fileno(in);
	pfds[0].events = POLLIN;
	pfds[1].fd = fileno(out);
	pfds[1].events = POLLHUP;
	ret = poll(pfds, 2, -1);
	if(ret < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "callcgi: error in poll: %s", strerror(errno));
		return(1);
	    }
	}
	if(ret > 0) {
	    if(pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
		ret = fread(buf, 1, 65536, in);
		if(ferror(in)) {
		    flog(LOG_ERR, "callcgi: could not read input: %s", strerror(errno));
		    return(1);
		}
		if(fwrite(buf, 1, ret, out) != ret) {
		    flog(LOG_ERR, "callcgi: could not write output: %s", strerror(errno));
		    return(1);
		}
	    }
	    if(pfds[1].revents & POLLHUP)
		return(1);
	}
    }
    return(0);
}

static char *absolutify(char *file)
{
    char cwd[1024];
    
    if(*file != '/') {
	getcwd(cwd, sizeof(cwd));
	return(sprintf2("%s/%s", cwd, file));
    }
    return(sstrdup(file));
}

static pid_t forkchild(int inpath, char *prog, char *file, char *method, char *url, char *rest, int *infd, int *outfd)
{
    char *qp, **env, *name;
    int inp[2], outp[2];
    pid_t pid;
    char *pi;

    pipe(inp);
    pipe(outp);
    if((pid = fork()) < 0) {
	flog(LOG_ERR, "callcgi: could not fork");
	exit(1);
    }
    if(pid == 0) {
	dup2(inp[0], 0);
	dup2(outp[1], 1);
	close(inp[0]);
	close(inp[1]);
	close(outp[0]);
	close(outp[1]);
	if((qp = strchr(url, '?')) != NULL)
	    *(qp++) = 0;
	putenv(sprintf2("SERVER_SOFTWARE=ashd/%s", VERSION));
	putenv("GATEWAY_INTERFACE=CGI/1.1");
	if(getenv("HTTP_VERSION"))
	    putenv(sprintf2("SERVER_PROTOCOL=%s", getenv("HTTP_VERSION")));
	putenv(sprintf2("REQUEST_METHOD=%s", method));
	name = url;
	/* XXX: This is an ugly hack (I think), but though I can think
	 * of several alternatives, none seem to be better. */
	if(*rest && (strlen(url) >= strlen(rest)) &&
	   !strcmp(rest, url + strlen(url) - strlen(rest))) {
	    name = sprintf2("%.*s", (int)(strlen(url) - strlen(rest)), url);
	}
	if((pi = unquoteurl(rest)) == NULL)
	    pi = rest;
	if(!strcmp(name, "/")) {
	    /*
	     * Normal CGI behavior appears to be to always let
	     * PATH_INFO begin with a slash and never let SCRIPT_NAME
	     * end with one. That conflicts, however, with some
	     * behaviors, such as "mounting" CGI applications on a
	     * directory element of the URI space -- a handler
	     * responding to "/foo/" would not be able to tell that it
	     * is not called "/foo", which makes a large difference,
	     * not least in relation to URI reconstruction and
	     * redirections. A common practical case is CGI-handled
	     * index files in directories. Therefore, this only
	     * handles the nonconditional case of the root directory
	     * and leaves other decisions to the previous handler
	     * handing over the request to callcgi. It is unclear if
	     * there is a better way to handle the problem.
	     */
	    name[0] = 0;
	    pi = sprintf2("/%s", pi);
	}
	putenv(sprintf2("PATH_INFO=%s", pi));
	putenv(sprintf2("SCRIPT_NAME=%s", name));
	putenv(sprintf2("QUERY_STRING=%s", qp?qp:""));
	if(getenv("REQ_HOST"))
	    putenv(sprintf2("SERVER_NAME=%s", getenv("REQ_HOST")));
	if(getenv("REQ_X_ASH_SERVER_PORT"))
	    putenv(sprintf2("SERVER_PORT=%s", getenv("REQ_X_ASH_SERVER_PORT")));
	if(getenv("REQ_X_ASH_PROTOCOL") && !strcmp(getenv("REQ_X_ASH_PROTOCOL"), "https"))
	    putenv("HTTPS=on");
	if(getenv("REQ_X_ASH_ADDRESS"))
	    putenv(sprintf2("REMOTE_ADDR=%s", getenv("REQ_X_ASH_ADDRESS")));
	if(getenv("REQ_X_ASH_REMOTE_USER"))
	    putenv(sprintf2("REMOTE_USER=%s", getenv("REQ_X_ASH_REMOTE_USER")));
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
	putenv(sprintf2("SCRIPT_FILENAME=%s", absolutify(file)));
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
    return(pid);
}

static void trim(struct charbuf *buf)
{
    char *p;
    
    for(p = buf->b; (p - buf->b < buf->d) && isspace(*p); p++);
    memmove(buf->b, p, buf->d -= (p - buf->b));
    for(p = buf->b + buf->d - 1; (p > buf->b) && isspace(*p); p--, buf->d--);
}

static char **parsecgiheaders(FILE *s)
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

static char *defstatus(int code)
{
    if(code == 200)
	return("OK");
    else if(code == 201)
	return("Created");
    else if(code == 202)
	return("Accepted");
    else if(code == 204)
	return("No Content");
    else if(code == 300)
	return("Multiple Choices");
    else if(code == 301)
	return("Moved Permanently");
    else if(code == 302)
	return("Found");
    else if(code == 303)
	return("See Other");
    else if(code == 304)
	return("Not Modified");
    else if(code == 307)
	return("Moved Temporarily");
    else if(code == 400)
	return("Bad Request");
    else if(code == 401)
	return("Unauthorized");
    else if(code == 403)
	return("Forbidden");
    else if(code == 404)
	return("Not Found");
    else if(code == 500)
	return("Internal Server Error");
    else if(code == 501)
	return("Not Implemented");
    else if(code == 503)
	return("Service Unavailable");
    else
	return("Unknown status");
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
	    hp += 2;
	} else {
	    hp += 2;
	}
    }
    if(status) {
	if(strchr(status, ' '))
	    fprintf(out, "HTTP/1.1 %s\n", status);
	else
	    fprintf(out, "HTTP/1.1 %i %s\n", atoi(status), defstatus(atoi(status)));
    } else if(location) {
	fprintf(out, "HTTP/1.1 303 See Other\n");
    } else {
	fprintf(out, "HTTP/1.1 200 OK\n");
    }
}

static void sendheaders(char **headers, FILE *out)
{
    while(*headers) {
	if(**headers)
	    fprintf(out, "%s: %s\n", headers[0], headers[1]);
	headers += 2;
    }
}

static void usage(void)
{
    flog(LOG_ERR, "usage: callcgi [-c] [-p PROGRAM] METHOD URL REST");
}

int main(int argc, char **argv, char **envp)
{
    int c;
    char *file, *prog, *sp;
    int inpath, cd;
    int infd, outfd;
    FILE *in, *out;
    char **headers;
    pid_t child;
    int estat;
    
    environ = envp;
    signal(SIGPIPE, SIG_IGN);
    
    prog = NULL;
    inpath = 0;
    cd = 0;
    while((c = getopt(argc, argv, "cp:")) >= 0) {
	switch(c) {
	case 'c':
	    cd = 1;
	    break;
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
    
    if(cd) {
	/* This behavior is encouraged by the CGI specification (RFC 3875, 7.2),
	 * but not strictly required, and I get the feeling it might break some
	 * relative paths here or there, so it's not the default for now. */
	if((sp = strrchr(file, '/')) != NULL) {
	    *sp = 0;
	    if(chdir(file)) {
		*sp = '/';
	    } else {
		file = sp + 1;
	    }
	}
    }
    
    if(prog == NULL)
	prog = file;
    child = forkchild(inpath, prog, file, argv[optind], argv[optind + 1], argv[optind + 2], &infd, &outfd);
    in = fdopen(infd, "w");
    passdata(stdin, in);	/* Ignore errors, perhaps? */
    fclose(in);
    out = fdopen(outfd, "r");
    if((headers = parsecgiheaders(out)) == NULL) {
	flog(LOG_WARNING, "CGI handler returned invalid headers");
	exit(1);
    }
    sendstatus(headers, stdout);
    sendheaders(headers, stdout);
    printf("\n");
    if(passdata(out, stdout))
	kill(child, SIGINT);
    if(waitpid(child, &estat, 0) == child) {
	if(WCOREDUMP(estat))
	    flog(LOG_WARNING, "CGI handler `%s' dumped core", prog);
	if(WIFEXITED(estat) && !WEXITSTATUS(estat))
	    return(0);
	else
	    return(1);
    }
    flog(LOG_WARNING, "could not wait for CGI handler: %s", strerror(errno));
    return(1);
}
