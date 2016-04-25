#ifndef _LIB_BUFIO_H
#define _LIB_BUFIO_H

struct bufioops {
    ssize_t (*read)(void *pdata, void *buf, size_t len);
    ssize_t (*write)(void *pdata, const void *buf, size_t len);
    int (*close)(void *pdata);
};

struct bufio {
    struct charbuf rbuf, wbuf;
    size_t rh, wh, bufhint;
    int err, eof;
    void *pdata;
    struct bufioops *ops;
};

struct bufio *bioopen(void *pdata, struct bufioops *ops);
int bioclose(struct bufio *bio);

size_t biordata(struct bufio *bio);
size_t biorspace(struct bufio *bio);
int bioeof(struct bufio *bio);
ssize_t biorensure(struct bufio *bio, size_t bytes);
ssize_t biofillsome(struct bufio *bio);
int biogetc(struct bufio *bio);
ssize_t bioreadsome(struct bufio *bio, void *buf, size_t len);

size_t biowdata(struct bufio *bio);
size_t biowspace(struct bufio *bio);
int bioflush(struct bufio *bio);
int bioflushsome(struct bufio *bio);
ssize_t biowensure(struct bufio *bio, size_t bytes);
int bioputc(struct bufio *bio, int c);
ssize_t biowrite(struct bufio *bio, const void *data, size_t len);
ssize_t biowritesome(struct bufio *bio, const void *data, size_t len);
int bioprintf(struct bufio *bio, const char *format, ...);
ssize_t biocopysome(struct bufio *dst, struct bufio *src);

#endif
