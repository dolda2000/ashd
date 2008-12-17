#ifndef _LIB_HTREQ_H
#define _LIB_HTREQ_H

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
int sendreq(int sock, struct hthead *req);
int recvreq(int sock, struct hthead **reqp);
void replrest(struct hthead *head, char *rest);

#endif
