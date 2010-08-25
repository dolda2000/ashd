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
    int type;
    char **argv;
    int fd;
};

void skipcfblock(struct cfstate *s);
struct cfstate *mkcfparser(FILE *in, char *name);
void freecfparser(struct cfstate *s);
char **getcfline(struct cfstate *s);
char *findstdconf(char *name);

void freechild(struct child *ch);
struct child *parsechild(struct cfstate *s);
int childhandle(struct child *ch, struct hthead *req, int fd);

#endif
