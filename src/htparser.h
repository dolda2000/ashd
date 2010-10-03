#ifndef _ASH_HTPARSER_H
#define _ASH_HTPARSER_H

struct conn {
    int (*initreq)(struct conn *, struct hthead *);
    void *pdata;
};

void serve(FILE *in, struct conn *conn);

int listensock4(int port);
int listensock6(int port);
char *formathaddress(struct sockaddr *name, socklen_t namelen);
void handleplain(int argc, char **argp, char **argv);
#ifdef HAVE_GNUTLS
void handlegnussl(int argc, char **argp, char **argv);
#endif

#endif
