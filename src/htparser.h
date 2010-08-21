#ifndef _ASH_HTPARSER_H
#define _ASH_HTPARSER_H

struct conn {
    int (*initreq)(struct conn *, struct hthead *);
    void *pdata;
};

void serve(FILE *in, struct conn *conn);

void handleplain(int argc, char **argp, char **argv);

#endif
