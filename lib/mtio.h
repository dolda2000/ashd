#ifndef _LIB_MTIO_H
#define _LIB_MTIO_H

#define EV_READ 1
#define EV_WRITE 2

int block(int fd, int ev, time_t to);
void ioloop(void);
FILE *mtstdopen(int fd, int issock, int timeout, char *mode);

#endif
