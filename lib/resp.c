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
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <regex.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <resp.h>

static char safechars[128] = {
 /* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xa xb xc xd xe xf */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0,
};

char *urlquote(char *text)
{
    static char *ret = NULL;
    struct charbuf buf;
    unsigned char c;
    
    if(ret != NULL)
	free(ret);
    bufinit(buf);
    for(; *text; text++) {
	c = *text;
	if((c < 128) && safechars[(int)c])
	    bufadd(buf, *text);
	else
	    bprintf(&buf, "%%%02X", (int)c);
    }
    bufadd(buf, 0);
    return(ret = buf.b);
}

char *htmlquote(char *text)
{
    static char *ret = NULL;
    struct charbuf buf;
    
    if(ret != NULL)
	free(ret);
    bufinit(buf);
    for(; *text; text++) {
	if(*text == '<')
	    bufcatstr(buf, "&lt;");
	else if(*text == '>')
	    bufcatstr(buf, "&gt;");
	else if(*text == '&')
	    bufcatstr(buf, "&amp;");
	else if(*text == '\"')
	    bufcatstr(buf, "&quot;");
	else
	    bufadd(buf, *text);
    }
    bufadd(buf, 0);
    return(ret = buf.b);
}

static void simpleerror2v(FILE *out, int code, char *msg, char *fmt, va_list args)
{
    struct charbuf buf;
    char *tmp;
    
    tmp = vsprintf2(fmt, args);
    bufinit(buf);
    bufcatstr(buf, "<?xml version=\"1.0\" encoding=\"US-ASCII\"?>\r\n");
    bufcatstr(buf, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\r\n");
    bufcatstr(buf, "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en-US\" xml:lang=\"en-US\">\r\n");
    bufcatstr(buf, "<head>\r\n");
    bprintf(&buf, "<title>%s</title>\r\n", msg);
    bufcatstr(buf, "</head>\r\n");
    bufcatstr(buf, "<body>\r\n");
    bprintf(&buf, "<h1>%s</h1>\r\n", msg);
    bprintf(&buf, "<p>%s</p>\r\n", htmlquote(tmp));
    bufcatstr(buf, "</body>\r\n");
    bufcatstr(buf, "</html>\r\n");
    fprintf(out, "HTTP/1.1 %i %s\n", code, msg);
    fprintf(out, "Content-Type: text/html\n");
    fprintf(out, "Content-Length: %zi\n", buf.d);
    fprintf(out, "\n");
    fwrite(buf.b, 1, buf.d, out);
    buffree(buf);
    free(tmp);
}

void simpleerror2(FILE *out, int code, char *msg, char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    simpleerror2v(out, code, msg, fmt, args);
    va_end(args);
}

void simpleerror(int fd, int code, char *msg, char *fmt, ...)
{
    va_list args;
    FILE *out;

    va_start(args, fmt);
    out = fdopen(dup(fd), "w");
    simpleerror2v(out, code, msg, fmt, args);
    fclose(out);
    va_end(args);
}

void stdredir(struct hthead *req, int fd, int code, char *dst)
{
    FILE *out;
    char *sp, *cp, *ep, *qs, *path, *url, *adst, *proto, *host;
    
    sp = strchr(dst, '/');
    cp = strchr(dst, ':');
    if(cp && (!sp || (cp < sp))) {
	adst = sstrdup(dst);
    } else {
	proto = getheader(req, "X-Ash-Protocol");
	host = getheader(req, "Host");
	if((proto == NULL) || (host == NULL)) {
	    /* Not compliant, but there isn't a whole lot to be done
	     * about it. */
	    adst = sstrdup(dst);
	} else {
	    if(*dst == '/') {
		path = sstrdup(dst + 1);
	    } else {
		if((*(url = req->url)) == '/')
		    url++;
		if((ep = strchr(url, '?')) == NULL) {
		    ep = url + strlen(url);
		    qs = "";
		} else {
		    qs = ep;
		}
		for(; (ep > url) && (ep[-1] != '/'); ep--);
		path = sprintf2("%.*s%s%s", ep - url, url, dst, qs);
	    }
	    adst = sprintf2("%s://%s/%s", proto, host, path);
	    free(path);
	}
    }
    out = fdopen(dup(fd), "w");
    fprintf(out, "HTTP/1.1 %i Redirection\n", code);
    fprintf(out, "Content-Length: 0\n");
    fprintf(out, "Location: %s\n", adst);
    fprintf(out, "\n");
    fclose(out);
    free(adst);
}

char *fmthttpdate(time_t time)
{
    /* I avoid using strftime, since it depends on locale settings. */
    static char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm *tm;
    
    tm = gmtime(&time);
    return(sprintf3("%s, %i %s %i %02i:%02i:%02i GMT", days[(tm->tm_wday + 6) % 7], tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec));
}

static int gtoi(char *bstr, regmatch_t g)
{
    int i, n;

    for(i = g.rm_so, n = 0; i < g.rm_eo; i++)
	n = (n * 10) + (bstr[i] - '0');
    return(n);
}

static int gstrcmp(char *bstr, regmatch_t g, char *str)
{
    if(g.rm_eo - g.rm_so != strlen(str))
	return(1);
    return(strncasecmp(bstr + g.rm_so, str, g.rm_eo - g.rm_so));
}

time_t parsehttpdate(char *date)
{
    static regex_t *spec = NULL;
    static char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int i;
    regmatch_t g[11];
    struct tm tm;
    int tz;
    
    if(spec == NULL) {
	omalloc(spec);
	if(regcomp(spec, "^[A-Z]{3}, +([0-9]+) +([A-Z]{3}) +([0-9]+) +([0-9]{2}):([0-9]{2}):([0-9]{2}) +(([A-Z]+)|[+-]([0-9]{2})([0-9]{2}))$", REG_EXTENDED | REG_ICASE)) {
	    free(spec);
	    spec = NULL;
	    return(0);
	}
    }
    if(regexec(spec, date, 11, g, 0))
	return(0);
    tm.tm_mday = gtoi(date, g[1]);
    tm.tm_year = gtoi(date, g[3]) - 1900;
    tm.tm_hour = gtoi(date, g[4]);
    tm.tm_min = gtoi(date, g[5]);
    tm.tm_sec = gtoi(date, g[6]);
    
    tm.tm_mon = -1;
    for(i = 0; i < 12; i++) {
	if(!gstrcmp(date, g[2], months[i])) {
	    tm.tm_mon = i;
	    break;
	}
    }
    if(tm.tm_mon < 0)
	return(0);
    
    if(g[8].rm_so > 0) {
	if(!gstrcmp(date, g[8], "GMT"))
	    tz = 0;
	else
	    return(0);
    } else if((g[9].rm_so > 0) && (g[10].rm_so > 0)) {
	tz = gtoi(date, g[9]) * 3600 + gtoi(date, g[10]) * 60;
	if(date[g[7].rm_so] == '-')
	    tz = -tz;
    } else {
	return(0);
    }
    
    return(timegm(&tm) - tz);
}

char *httpdefstatus(int code)
{
    switch(code) {
    case 200:
	return("OK");
    case 201:
	return("Created");
    case 202:
	return("Accepted");
    case 204:
	return("No Content");
    case 300:
	return("Multiple Choices");
    case 301:
	return("Moved Permanently");
    case 302:
	return("Found");
    case 303:
	return("See Other");
    case 304:
	return("Not Modified");
    case 307:
	return("Moved Temporarily");
    case 400:
	return("Bad Request");
    case 401:
	return("Unauthorized");
    case 403:
	return("Forbidden");
    case 404:
	return("Not Found");
    case 500:
	return("Internal Server Error");
    case 501:
	return("Not Implemented");
    case 503:
	return("Service Unavailable");
    default:
	return("Unknown status");
    }
}
