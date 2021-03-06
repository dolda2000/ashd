#ifndef _UTILS_H
#define _UTILS_H

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

#define max(a, b) (((b) > (a))?(b):(a))
#define min(a, b) (((b) < (a))?(b):(a))

#define smalloc(size) ({void *__result__; ((__result__ = malloc(size)) == NULL)?({abort(); (void *)0;}):__result__;})
#define srealloc(ptr, size) ({void *__result__; ((__result__ = realloc((ptr), (size))) == NULL)?({abort(); (void *)0;}):__result__;})
#define szmalloc(size) memset(smalloc(size), 0, size)
#define sstrdup(str) ({const char *__strbuf__ = (str); strcpy(smalloc(strlen(__strbuf__) + 1), __strbuf__);})
#define omalloc(o) ((o) = szmalloc(sizeof(*(o))))

#define bufinit(buf) memset(&(buf), 0, sizeof(buf))
#define buffree(buf) do { if((buf).b != NULL) {free((buf).b);} bufinit(buf); } while(0)
#define sizebuf(buf, wanted) (_sizebuf((struct buffer *)&(buf), (wanted), sizeof(*((buf).b))))
#define bufdel(buf, i) (memmove((buf).b + (i), (buf).b + (i) + 1, (--((buf).d) - (i)) * sizeof(*((buf).b))))
#define bufadd(buf, new) \
do { \
    _sizebuf((struct buffer *)&(buf), (buf).d + 1, sizeof(*((buf).b))); \
    (buf).b[(buf).d++] = (new); \
} while(0)
#define bufcat(buf, new, size) \
do { \
    size_t __bufcat_size__; \
    __bufcat_size__ = (size); \
    _sizebuf((struct buffer *)&(buf), (buf).d + __bufcat_size__, sizeof((buf).b)); \
    memcpy((buf).b + (buf).d, (new), (__bufcat_size__) * sizeof(*((buf).b))); \
    (buf).d += __bufcat_size__; \
} while(0)
#define bufcatstr(buf, str) \
do { \
    char *__buf__; \
    __buf__ = (str); \
    bufcat((buf), __buf__, strlen(__buf__)); \
} while(0)
#define bufcatstr2(buf, str) \
do { \
    char *__buf__; \
    __buf__ = (str); \
    bufcat((buf), __buf__, strlen(__buf__) + 1); \
} while(0)
#define bufeat(buf, len) memmove((buf).b, (buf).b + (len), (buf).d -= (len))

struct buffer {
    void *b;
    size_t s, d;
};

#define typedbuf(type) struct {type *b; size_t s, d;}

struct charbuf {
    char *b;
    size_t s, d;
};

struct charvbuf {
    char **b;
    size_t s, d;
};

struct btree {
    struct btree *l, *r;
    int h;
    void *d;
};

void _sizebuf(struct buffer *buf, size_t wanted, size_t el);
char *decstr(char **p, size_t *len);
char *vsprintf2(char *format, va_list al);
char *sprintf2(char *format, ...);
char *sprintf3(char *format, ...);
off_t atoo(char *n);
char **tokenize(char *src);
void freeca(char **ca);
int calen(char **a);
void bvprintf(struct charbuf *buf, char *format, va_list al);
void bprintf(struct charbuf *buf, char *format, ...);
void replstr(char **p, char *n);
char *base64encode(char *data, size_t datalen);
char *base64decode(char *data, size_t *datalen);
int hexdigit(char c);
int bbtreedel(struct btree **tree, void *item, int (*cmp)(void *, void *));
void freebtree(struct btree **tree, void (*ffunc)(void *));
int bbtreeput(struct btree **tree, void *item, int (*cmp)(void *, void *));
void *btreeget(struct btree *tree, void *key, int (*cmp)(void *, void *));
FILE *funstdio(void *pdata,
	       ssize_t (*read)(void *pdata, void *buf, size_t len),
	       ssize_t (*write)(void *pdata, const void *buf, size_t len),
	       off_t (*seek)(void *pdata, off_t offset, int whence),
	       int (*close)(void *pdata));

#endif
