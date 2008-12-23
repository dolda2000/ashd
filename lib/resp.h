#ifndef _LIB_HTRESP_H
#define _LIB_HTRESP_H

void simpleerror(int fd, int code, char *msg, char *fmt, ...);
char *fmthttpdate(time_t time);
time_t parsehttpdate(char *date);

#endif
