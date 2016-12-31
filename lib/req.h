#ifndef _LIB_HTREQ_H
#define _LIB_HTREQ_H

#include <stdio.h>

struct bufio;

struct hthead {
    char *method, *url, *ver, *msg;
    int code;
    char *rest;
    char ***headers;
    int noheaders;
};

struct hthead *mkreq(char *method, char *url, char *ver);
struct hthead *mkresp(int code, char *msg, char *ver);
void freehthead(struct hthead *head);
char *getheader(struct hthead *head, char *name);
void headpreheader(struct hthead *head, const char *name, const char *val);
void headappheader(struct hthead *head, const char *name, const char *val);
void headrmheader(struct hthead *head, const char *name);
int sendreq2(int sock, struct hthead *req, int fd, int flags);
int sendreq(int sock, struct hthead *req, int fd);
int recvreq(int sock, struct hthead **reqp);
void replrest(struct hthead *head, char *rest);
int parseheaders(struct hthead *head, FILE *in);
int parseheadersb(struct hthead *head, struct bufio *in);
struct hthead *parseresponse(FILE *in);
struct hthead *parseresponseb(struct bufio *in);
int writeresp(FILE *out, struct hthead *resp);
int writerespb(struct bufio *out, struct hthead *resp);
char *unquoteurl(char *in);

#endif
