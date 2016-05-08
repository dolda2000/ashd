#ifndef _ASH_HTPARSER_H
#define _ASH_HTPARSER_H

struct conn {
    int (*initreq)(struct conn *, struct hthead *);
    void *pdata;
};

struct mtbuf {
    struct muth **b;
    size_t s, d;
};

void serve(struct bufio *in, struct conn *conn);

int listensock4(int port);
int listensock6(int port);
char *formathaddress(struct sockaddr *name, socklen_t namelen);
void handleplain(int argc, char **argp, char **argv);
#ifdef HAVE_GNUTLS
void handlegnussl(int argc, char **argp, char **argv);
#endif

extern struct mtbuf listeners;

#endif
