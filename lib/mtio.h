#ifndef _LIB_MTIO_H
#define _LIB_MTIO_H

#include <stdio.h>

#define EV_READ 1
#define EV_WRITE 2

struct stdiofd {
    int fd;
    int sock;
    int timeout;
    int rights, sendrights;
};

int block(int fd, int ev, time_t to);
int ioloop(void);
void exitioloop(int status);
FILE *mtstdopen(int fd, int issock, int timeout, char *mode, struct stdiofd **infop);
struct bufio *mtbioopen(int fd, int issock, int timeout, char *mode, struct stdiofd **infop);
void mtiopipe(FILE **read, FILE **write);

#endif
