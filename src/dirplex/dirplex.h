#ifndef _ASH_DIRPLEX_H
#define _ASH_DIRPLEX_H

#define PAT_BASENAME 0
#define PAT_PATHNAME 1
#define PAT_ALL 2
#define PAT_DEFAULT 3
#define PAT_LOCAL 4

#define PT_FILE 0
#define PT_DIR 1

struct config {
    struct config *next, *prev;
    char *path;
    time_t mtime, lastck;
    struct child *children;
    struct pattern *patterns;
    char **index;
    char *capture;
    int caproot;
};

struct rule {
    int type;
    char **patterns;
};

struct headmod {
    struct headmod *next;
    char *name, *value;
};

struct pattern {
    struct pattern *next;
    int type;
    struct headmod *headers;
    char *childnm;
    char **fchild;
    struct rule **rules;
};

struct child *getchild(struct config *cf, char *name);
struct config *readconfig(char *file);
struct config *getconfig(char *path);
struct config **getconfigs(char *file);
struct child *findchild(char *file, char *name, struct config **cf);
struct pattern *findmatch(char *file, int trydefault, int dir);
void modheaders(struct hthead *req, struct pattern *pat);

extern time_t now;
extern struct child *notfound;
extern struct config *gconfig, *lconfig;

#endif
