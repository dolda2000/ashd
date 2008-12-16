#ifndef _LIB_PROC_H
#define _LIB_PROC_H

int stdmkchild(char **argv);
int sendfd(int sock, int fd, char *data, size_t datalen);
int recvfd(int sock, char **data, size_t *datalen);

#endif
