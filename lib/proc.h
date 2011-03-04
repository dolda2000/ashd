#ifndef _LIB_PROC_H
#define _LIB_PROC_H

#include "req.h"

int stdmkchild(char **argv, void (*chinit)(void *), void *idata);
int sendfd(int sock, int fd, char *data, size_t datalen);
int recvfd(int sock, char **data, size_t *datalen);
pid_t stdforkserve(char **argv, struct hthead *req, int fd, void (*chinit)(void *), void *idata);

#endif
