#ifndef _LIB_HTREQ_H
#define _LIB_HTREQ_H

struct htreq {
    char *method, *url, *ver;
    char *restbuf, *rest;
    char ***headers;
    int noheaders;
};

struct htreq *mkreq(char *method, char *url, char *ver);
void freereq(struct htreq *req);
void reqpreheader(struct htreq *req, char *name, char *val);
void reqappheader(struct htreq *req, char *name, char *val);

#endif
