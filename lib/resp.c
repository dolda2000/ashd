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
#include <stdarg.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <resp.h>

char *htmlquote(char *text)
{
    struct charbuf buf;
    
    bufinit(buf);
    for(; *text; text++) {
	if(*text == '<')
	    bufcatstr(buf, "&lt;");
	else if(*text == '>')
	    bufcatstr(buf, "&gt;");
	else if(*text == '&')
	    bufcatstr(buf, "&amp;");
	else
	    bufadd(buf, *text);
    }
    bufadd(buf, 0);
    return(buf.b);
}

void simpleerror(int fd, int code, char *msg, char *fmt, ...)
{
    struct charbuf buf;
    char *tmp1, *tmp2;
    va_list args;
    FILE *out;
    
    va_start(args, fmt);
    tmp1 = vsprintf2(fmt, args);
    va_end(args);
    tmp2 = htmlquote(tmp1);
    free(tmp1);
    bufinit(buf);
    bufcatstr(buf, "<?xml version=\"1.0\" encoding=\"US-ASCII\"?>\r\n");
    bufcatstr(buf, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\r\n");
    bufcatstr(buf, "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en-US\" xml:lang=\"en-US\">\r\n");
    bufcatstr(buf, "<head>\r\n");
    bprintf(&buf, "<title>%s</title>\r\n", msg);
    bufcatstr(buf, "</head>\r\n");
    bufcatstr(buf, "<body>\r\n");
    bprintf(&buf, "<h1>%s</h1>\r\n", msg);
    bprintf(&buf, "<p>%s</p>\r\n", tmp2);
    bufcatstr(buf, "</body>\r\n");
    bufcatstr(buf, "</html>\r\n");
    free(tmp2);
    out = fdopen(fd, "w");
    fprintf(out, "HTTP/1.1 %i %s\r\n", code, msg);
    fprintf(out, "Content-Type: text/html\r\n");
    fprintf(out, "Content-Length: %i\r\n", buf.d);
    fprintf(out, "\r\n");
    fwrite(buf.b, 1, buf.d, out);
    fclose(out);
    buffree(buf);
}
