#ifndef _UTILS_H
#define _UTILS_H

#define smalloc(size) ({void *__result__; ((__result__ = malloc(size)) == NULL)?({exit(-1); (void *)0;}):__result__;})
#define srealloc(ptr, size) ({void *__result__; ((__result__ = realloc((ptr), (size))) == NULL)?({exit(-1); (void *)0;}):__result__;})
#define szmalloc(size) memset(smalloc(size), 0, size)
#define sstrdup(str) ({char *__strbuf__ = (str); strcpy(smalloc(strlen(__strbuf__) + 1), __strbuf__);})
#define omalloc(o) ((o) = szmalloc(sizeof(*(o))))

#define bufinit(buf) memset(&(buf), 0, sizeof(buf))
#define buffree(buf) do { if((buf).b != NULL) {free((buf).b);} } while(0)
#define sizebuf(buf, wanted) (_sizebuf((struct buffer *)&(buf), (wanted), sizeof(*((buf).b))))
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
#define bufcatstr2(buf, str) \
do { \
    char *__buf__; \
    __buf__ = (str); \
    bufcat((buf), __buf__, strlen(__buf__)); \
} while(0)

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

void _sizebuf(struct buffer *buf, size_t wanted, size_t el);

#endif
