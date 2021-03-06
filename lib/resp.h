#ifndef _LIB_HTRESP_H
#define _LIB_HTRESP_H

#include "req.h"

char *urlquote(char *text);
char *htmlquote(char *text);
void simpleerror(int fd, int code, char *msg, char *fmt, ...);
void simpleerror2(FILE *out, int code, char *msg, char *fmt, ...);
void stdredir(struct hthead *req, int fd, int code, char *dst);
char *fmthttpdate(time_t time);
time_t parsehttpdate(char *date);
char *httpdefstatus(int code);

#endif
