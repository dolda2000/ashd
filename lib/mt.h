#ifndef _MUTHREAD_H
#define _MUTHREAD_H

#include <ucontext.h>
#include <stdarg.h>

#define vavar(type, name) type name = va_arg(args, type)

struct muth {
    ucontext_t ctxt, *last;
    void *stack;
    void (*entry)(struct muth *muth, va_list args);
    va_list *arglist;
    int running;
    int yr;
    int freeme;
    int vgid;
};

struct muth *mustart(void (*fn)(struct muth *muth, va_list args), ...);
void resume(struct muth *muth, int ret);
int yield(void);

extern struct muth *current;

#endif
