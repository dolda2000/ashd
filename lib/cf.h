#ifndef _ASHCONF_H
#define _ASHCONF_H

#include <req.h>

struct cfstate {
    struct muth *pf;
    int expstart;
    char **res, **argv;
    int argc;
    int lno;
    char *file;
};

struct child {
    struct child *next;
    char *name;
    struct chandler *iface;
    void *pdata;
};

struct chandler {
    int (*handle)(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *idata);
    void (*merge)(struct child *dst, struct child *src);
    void (*destroy)(struct child *ch);
};

void skipcfblock(struct cfstate *s);
struct cfstate *mkcfparser(FILE *in, char *name);
void freecfparser(struct cfstate *s);
char **getcfline(struct cfstate *s);
char *findstdconf(char *name);

struct child *newchild(char *name, struct chandler *iface, void *pdata);
void freechild(struct child *ch);
void mergechildren(struct child *dst, struct child *src);
struct child *parsechild(struct cfstate *s);
int childhandle(struct child *ch, struct hthead *req, int fd, void (*chinit)(void *), void *idata);

#endif
