/*
 * Copyright (c) 2011 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu>
 *
 * This file is part of Parlib.
 * 
 * Parlib is free software: you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Parlib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 * 
 * See COPYING.LESSER for details on the GNU Lesser General Public License.
 * See COPYING for details on the GNU General Public License.
 */

#include <errno.h>

#include "parlib.h"
#include "vcore.h"
#include "uthread.h"
#include "atomic.h"
#include "arch.h"
#include "tls.h"

#define printd(...)

/* Which operations we'll call for the 2LS.  Will change a bit with Lithe.  For
 * now, there are no defaults.  2LSs can override sched_ops. */
struct schedule_ops default_2ls_ops = {0};
struct schedule_ops *sched_ops __attribute__((weak)) = &default_2ls_ops;

__thread struct uthread *current_uthread = 0;

#ifndef PARLIB_NO_UTHREAD_TLS
/* static helpers: */
static int __uthread_allocate_tls(struct uthread *uthread);
static int __uthread_reinit_tls(struct uthread *uthread);
static void __uthread_free_tls(struct uthread *uthread);
#endif

/* The real 2LS calls this, passing in a uthread representing thread0.  When it
 * returns, you're in _M mode, still running thread0, on vcore0 */
int uthread_lib_init(struct uthread* uthread)
{
	/* Only do this initialization once, every time after, just return 0 */
	static bool first = TRUE;
	if (!first) 
		return 0;
	first = FALSE;

	/* Init the vcore system */
	assert(!vcore_lib_init());

	/* Set current_uthread to the uthread passed in, so we have a place to
	 * save the main thread's context when yielding */
	current_uthread = uthread;

#ifndef PARLIB_NO_UTHREAD_TLS
	/* Associate the main thread's tls with the current tls as well */
	current_uthread->tls_desc = current_tls_desc;
#endif

	/* Finally, switch to vcore 0's tls and set current_uthread to be the main
	 * thread, so when vcore 0 comes up it will resume the main thread.
	 * Note: there is no need to restore the original tls here, since we
	 * are right about to transition onto vcore 0 anyway... */
	set_tls_desc(vcore_tls_descs[0], 0);
	safe_set_tls_var(current_uthread, uthread);

	/* Request some cores ! */
	while (num_vcores() < 1) {
		/* Ask for a core -- this will transition the main thread onto
                 * vcore 0 once successful */
		vcore_request(1);
		cpu_relax();
	}
	/* We are now running on vcore 0 */

	return 0;
}

void __attribute__((noreturn)) uthread_vcore_entry(void);
void vcore_entry() {
	if(vcore_saved_ucontext) {
		assert(current_uthread);
		memcpy(&current_uthread->uc, vcore_saved_ucontext, sizeof(struct ucontext));
#ifndef PARLIB_NO_UTHREAD_TLS
		current_uthread->tls_desc = vcore_saved_tls_desc;
#endif
	}
	uthread_vcore_entry();
}

/* 2LSs shouldn't call uthread_vcore_entry directly */
void __attribute__((noreturn)) uthread_vcore_entry(void)
{
	assert(in_vcore_context());
	assert(sched_ops->sched_entry);
	sched_ops->sched_entry();
	/* 2LS sched_entry should never return */
	assert(0);
}

void uthread_init(struct uthread *uthread)
{
#ifndef PARLIB_NO_UTHREAD_TLS
	/* Make sure we are not in vcore context */
	// TODO: Need to revisit this since in ROS this assert is still there...
//	assert(!in_vcore_context());

	/* If a tls_desc is already set for this thread, reinit it... */
	if (uthread->tls_desc)
		assert(!__uthread_reinit_tls(uthread));
	/* Otherwise get a TLS for the new thread */
	else
		assert(!__uthread_allocate_tls(uthread));

	/* Set the thread's internal tls variable for current_uthread to itself */
	uthread_set_tls_var(uthread, current_uthread, uthread);
#endif
}

void uthread_cleanup(struct uthread *uthread)
{
#ifndef PARLIB_NO_UTHREAD_TLS
	printd("[U] thread %08p on vcore %d is DYING!\n", uthread, vcore_id());
	/* Free the uthread's tls descriptor */
	assert(uthread->tls_desc);
	__uthread_free_tls(uthread);
#endif
}

void uthread_runnable(struct uthread *uthread)
{
	/* Allow the 2LS to make the thread runnable, and do whatever. */
	assert(sched_ops->thread_runnable);
	sched_ops->thread_runnable(uthread);
}

/* Need to have this as a separate, non-inlined function since we clobber the
 * stack pointer before calling it, and don't want the compiler to play games
 * with my hart. */
static void __attribute__((noinline, noreturn)) 
__uthread_yield(void)
{
	assert(in_vcore_context());
	assert(sched_ops->thread_yield);

	struct uthread *uthread = current_uthread;
	/* 2LS will save the thread somewhere for restarting.  Later on,
	 * we'll probably have a generic function for all sorts of waiting.
	 */
	sched_ops->thread_yield(uthread);

	/* Leave the current vcore completely */
	current_uthread = NULL;

	/* Go back to the entry point, where we can handle notifications or
	 * reschedule someone. */
	uthread_vcore_entry();
}

/* Calling thread yields.  TODO: combine similar code with uthread_exit() (done
 * like this to ease the transition to the 2LS-ops */
void uthread_yield(bool save_state)
{
	assert(!in_vcore_context());

	struct uthread *uthread = current_uthread;
	uint32_t vcoreid = vcore_id();
	printd("[U] Uthread %p is yielding on vcore %d\n", uthread, vcoreid);

	volatile bool yielding = TRUE; /* signal to short circuit when restarting */
	cmb();
	/* Take the current state and save it into uthread->uc when this pthread
	 * restarts, it will continue from right after this, see yielding is false,
	 * and short circuit the function. */
	if(save_state) {
		int ret = getcontext(&uthread->uc);
		assert(ret == 0);
	}
	if (!yielding)
		goto yield_return_path;
	yielding = FALSE; /* for when it starts back up */
	/* Change to the transition context (both TLS and stack). */
#ifndef PARLIB_NO_UTHREAD_TLS
	set_tls_desc(vcore_tls_descs[vcoreid], vcoreid);
#else
	extern __thread bool __in_vcore_context;
    __in_vcore_context = true;
#endif
	assert(current_uthread == uthread);	
	assert(in_vcore_context());	/* technically, we aren't fully in vcore context */
	/* After this, make sure you don't use local variables. */
	set_stack_pointer(vcore_context.uc_stack.ss_sp + vcore_context.uc_stack.ss_size);
	cmb();
	/* Finish yielding in another function. */
	__uthread_yield();
	/* Should never get here */
	assert(0);
	/* Will jump here when the uthread's trapframe is restarted/popped. */
yield_return_path:
	assert(current_uthread == uthread);	
	printd("[U] Uthread %p returning from a yield on vcore %d with tls %p!\n", 
           current_uthread, vcore_id(), get_tls_desc(vcore_id()));
}

/* Saves the state of the current uthread from the point at which it is called */
void save_current_uthread(struct uthread *uthread)
{
	int ret = getcontext(&uthread->uc);
	assert(ret == 0);
}

/* Simply sets current uthread to be whatever the value of uthread is.  This
 * can be called from outside of sched_entry() to highjack the current context,
 * and make sure that the new uthread struct is used to store this context upon
 * yielding, etc. USE WITH EXTREME CAUTION!
*/
void highjack_current_uthread(struct uthread *uthread)
{
	assert(uthread != current_uthread);
	vcore_set_tls_var(current_uthread, uthread);

#ifndef PARLIB_NO_UTHREAD_TLS
	assert(uthread->tls_desc);
	set_tls_desc(uthread->tls_desc, vcore_id());
#else
	extern __thread bool __in_vcore_context;
    __in_vcore_context = false;
#endif
}

/* Runs whatever thread is vcore's current_uthread */
void run_current_uthread(void)
{
	assert(current_uthread);

	uint32_t vcoreid = vcore_id();
	struct ucontext *uc = &current_uthread->uc;
#ifndef PARLIB_NO_UTHREAD_TLS
	set_tls_desc(current_uthread->tls_desc, vcoreid);
#else
	extern __thread bool __in_vcore_context;
    __in_vcore_context = false;
#endif
	setcontext(uc);
	assert(0);
}

/* Launches the uthread on the vcore.  Don't call this on current_uthread. */
void run_uthread(struct uthread *uthread)
{
	highjack_current_uthread(uthread);
	setcontext(&uthread->uc);
	assert(0);
}

/* Swaps the currently running uthread for a new one, saving the state of the
 * current uthread in the process */
void swap_uthreads(struct uthread *__old, struct uthread *__new)
{
  volatile bool swap = true;
#ifndef PARLIB_NO_UTHREAD_TLS
  void *tls_desc = get_tls_desc(vcore_id());
#endif
  ucontext_t uc;
  getcontext(&uc);
  cmb();
  if(swap) {
    swap = false;
    memcpy(&__old->uc, &uc, sizeof(ucontext_t));
    run_uthread(__new);
  }
  vcore_set_tls_var(current_uthread, __old);
#ifndef PARLIB_NO_UTHREAD_TLS
  set_tls_desc(tls_desc, vcore_id());
#endif
}

/* Deals with a pending preemption (checks, responds).  If the 2LS registered a
 * function, it will get run.  Returns true if you got preempted.  Called
 * 'check' instead of 'handle', since this isn't an event handler.  It's the "Oh
 * shit a preempt is on its way ASAP".  While it is isn't too involved with
 * uthreads, it is tied in to sched_ops. */
bool check_preempt_pending(uint32_t vcoreid)
{
	bool retval = FALSE;
//	if (__procinfo.vcoremap[vcoreid].preempt_pending) {
//		retval = TRUE;
//		if (sched_ops->preempt_pending)
//			sched_ops->preempt_pending();
//		/* this tries to yield, but will pop back up if this was a spurious
//		 * preempt_pending. */
//		sys_yield(TRUE);
//	}
	return retval;
}

#ifndef PARLIB_NO_UTHREAD_TLS
/* TLS helpers */
static int __uthread_allocate_tls(struct uthread *uthread)
{
	uthread->tls_desc = allocate_tls();
	if (!uthread->tls_desc) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

static int __uthread_reinit_tls(struct uthread *uthread)
{
    uthread->tls_desc = reinit_tls(uthread->tls_desc);
    if (!uthread->tls_desc) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static void __uthread_free_tls(struct uthread *uthread)
{
	free_tls(uthread->tls_desc);
	uthread->tls_desc = NULL;
}
#endif

