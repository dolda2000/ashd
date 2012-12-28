#ifndef _LIB_MTIO_H
#define _LIB_MTIO_H

#include <stdio.h>

#define EV_READ 1
#define EV_WRITE 2

int block(int fd, int ev, time_t to);
int ioloop(void);
void exitioloop(int status);
FILE *mtstdopen(int fd, int issock, int timeout, char *mode);

#endif
