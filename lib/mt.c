/*
    ashd - A Sane HTTP Daemon
    Copyright (C) 2008  Fredrik Tolf <fredrik@dolda2000.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>

#ifdef HAVE_VALGRIND
#include <valgrind/memcheck.h>
#endif

struct muth *current = NULL;
static ucontext_t mainctxt;

static void freemt(struct muth *muth)
{
    if(muth->running)
	abort();
#ifdef VALGRIND_STACK_DEREGISTER
    VALGRIND_STACK_DEREGISTER(muth->vgid);
#endif
    free(muth->stack);
    free(muth);
}

static void muboot(void)
{
    struct muth *muth;
    
    muth = current;
    muth->running = 1;
    muth->entry(muth, *muth->arglist);
    muth->running = 0;
    swapcontext(&muth->ctxt, muth->last);
}

struct muth *mustart(void (*fn)(struct muth *muth, va_list args), ...)
{
    struct muth *muth, *last;
    va_list args;
    
    omalloc(muth);
    getcontext(&muth->ctxt);
    muth->ctxt.uc_link = &mainctxt;
    muth->ctxt.uc_stack.ss_size = 65536;
    muth->ctxt.uc_stack.ss_sp = muth->stack = smalloc(muth->ctxt.uc_stack.ss_size);
#ifdef VALGRIND_STACK_REGISTER
    muth->vgid = VALGRIND_STACK_REGISTER(muth->stack, muth->stack + 65536);
#endif
    va_start(args, fn);
    muth->entry = fn;
    muth->arglist = &args;
    makecontext(&muth->ctxt, muboot, 0);
    if(current == NULL)
	muth->last = &mainctxt;
    else
	muth->last = &current->ctxt;
    last = current;
    current = muth;
    swapcontext(muth->last, &muth->ctxt);
    current = last;
    va_end(args);
    if(!muth->running)
	freemt(muth);
    return(muth);
}

int yield(void)
{
    ucontext_t *ret;
    
    if((current == NULL) || (current->last == NULL))
	abort();
    ret = current->last;
    current->last = NULL;
    swapcontext(&current->ctxt, ret);
    return(current->yr);
}

void resume(struct muth *muth, int ret)
{
    struct muth *last;
    
    if(muth->last != NULL)
	abort();
    if(current == NULL)
	muth->last = &mainctxt;
    else
	muth->last = &current->ctxt;
    last = current;
    current = muth;
    muth->yr = ret;
    swapcontext(muth->last, &current->ctxt);
    current = last;
    if(!muth->running)
	freemt(muth);
}
