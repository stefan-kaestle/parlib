/* Copyright (C) 2005, 2008 Free Software Foundation, Inc.
   Contributed by Richard Henderson <rth@redhat.com>.

   This file is part of the GNU OpenMP Library (libgomp).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
   more details.

   You should have received a copy of the GNU Lesser General Public License 
   along with libgomp; see the file COPYING.LIB.  If not, write to the
   Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.  */

/* As a special exception, if you link this library with other files, some
   of which are compiled with GCC, to produce an executable, this library
   does not by itself cause the resulting executable to be covered by the
   GNU General Public License.  This exception does not however invalidate
   any other reasons why the executable file might be covered by the GNU
   General Public License.  */

/* This file contains routines to manage the work-share queue for a team
   of threads.  */

#include "libgomp.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


/* Allocate a new work share structure, preferably from current team's
   free gomp_work_share cache.  */

static struct gomp_work_share *
alloc_work_share (struct gomp_team *team)
{
  struct gomp_work_share *ws;
  unsigned int i;

  /* This is called in a critical section.  */
  if (team->work_share_list_alloc != NULL)
    {
      ws = team->work_share_list_alloc;
      team->work_share_list_alloc = ws->next_free;
      return ws;
    }

#ifdef HAVE_SYNC_BUILTINS
  ws = team->work_share_list_free;
  /* We need atomic read from work_share_list_free,
     as free_work_share can be called concurrently.  */
  __asm ("" : "+r" (ws));

  if (ws && ws->next_free)
    {
      struct gomp_work_share *next = ws->next_free;
      ws->next_free = NULL;
      team->work_share_list_alloc = next->next_free;
      return next;
    }
#else
  gomp_mutex_lock (&team->work_share_list_free_lock);
  ws = team->work_share_list_free;
  if (ws)
    {
      team->work_share_list_alloc = ws->next_free;
      team->work_share_list_free = NULL;
      gomp_mutex_unlock (&team->work_share_list_free_lock);
      return ws;
    }
  gomp_mutex_unlock (&team->work_share_list_free_lock);
#endif

  team->work_share_chunk *= 2;
  ws = gomp_malloc (team->work_share_chunk * sizeof (struct gomp_work_share));
  ws->next_alloc = team->work_shares[0].next_alloc;
  team->work_shares[0].next_alloc = ws;
  team->work_share_list_alloc = &ws[1];
  for (i = 1; i < team->work_share_chunk - 1; i++)
    ws[i].next_free = &ws[i + 1];
  ws[i].next_free = NULL;
  return ws;
}

/* Initialize an already allocated struct gomp_work_share.
   This shouldn't touch the next_alloc field.  */

void
gomp_init_work_share (struct gomp_work_share *ws, bool ordered,
		      unsigned nthreads)
{
  gomp_mutex_init (&ws->lock);
  if (__builtin_expect (ordered, 0))
    {
#define INLINE_ORDERED_TEAM_IDS_CNT \
  ((sizeof (struct gomp_work_share) \
    - offsetof (struct gomp_work_share, inline_ordered_team_ids)) \
   / sizeof (((struct gomp_work_share *) 0)->inline_ordered_team_ids[0]))

      if (nthreads > INLINE_ORDERED_TEAM_IDS_CNT)
	ws->ordered_team_ids
	  = gomp_malloc (nthreads * sizeof (*ws->ordered_team_ids));
      else
	ws->ordered_team_ids = ws->inline_ordered_team_ids;
      memset (ws->ordered_team_ids, '\0',
	      nthreads * sizeof (*ws->ordered_team_ids));
      ws->ordered_num_used = 0;
      ws->ordered_owner = -1;
      ws->ordered_cur = 0;
    }
  else
    ws->ordered_team_ids = NULL;
  gomp_ptrlock_init (&ws->next_ws, NULL);
  ws->threads_completed = 0;
}

/* Do any needed destruction of gomp_work_share fields before it
   is put back into free gomp_work_share cache or freed.  */

void
gomp_fini_work_share (struct gomp_work_share *ws)
{
  gomp_mutex_destroy (&ws->lock);
  if (ws->ordered_team_ids != ws->inline_ordered_team_ids)
    free (ws->ordered_team_ids);
  gomp_ptrlock_destroy (&ws->next_ws);
}

/* Free a work share struct, if not orphaned, put it into current
   team's free gomp_work_share cache.  */

static inline void
free_work_share (struct gomp_team *team, struct gomp_work_share *ws)
{
  gomp_fini_work_share (ws);
  if (__builtin_expect (team == NULL, 0))
    free (ws);
  else
    {
      struct gomp_work_share *next_ws;
#ifdef HAVE_SYNC_BUILTINS
      do
	{
	  next_ws = team->work_share_list_free;
	  ws->next_free = next_ws;
	}
      while (!__sync_bool_compare_and_swap (&team->work_share_list_free,
					    next_ws, ws));
#else
      gomp_mutex_lock (&team->work_share_list_free_lock);
      next_ws = team->work_share_list_free;
      ws->next_free = next_ws;
      team->work_share_list_free = ws;
      gomp_mutex_unlock (&team->work_share_list_free_lock);
#endif
    }
}

/* The current thread is ready to begin the next work sharing construct.
   In all cases, thr->ts.work_share is updated to point to the new
   structure.  In all cases the work_share lock is locked.  Return true
   if this was the first thread to reach this point.  */

bool
gomp_work_share_start (bool ordered)
{
  struct gomp_thread *thr = gomp_thread ();
  struct gomp_team *team = thr->ts.team;
  struct gomp_work_share *ws;

  /* Work sharing constructs can be orphaned.  */
  if (team == NULL)
    {
      ws = gomp_malloc (sizeof (*ws));
      gomp_init_work_share (ws, ordered, 1);
      thr->ts.work_share = ws;
      return ws;
    }

  ws = thr->ts.work_share;
  thr->ts.last_work_share = ws;
  ws = gomp_ptrlock_get (&ws->next_ws);
  if (ws == NULL)
    {
      /* This thread encountered a new ws first.  */
      struct gomp_work_share *ws = alloc_work_share (team);
      gomp_init_work_share (ws, ordered, team->nthreads);
      thr->ts.work_share = ws;
      return true;
    }
  else
    {
      thr->ts.work_share = ws;
      return false;
    }
}

/* The current thread is done with its current work sharing construct.
   This version does imply a barrier at the end of the work-share.  */

void
gomp_work_share_end (void)
{
  struct gomp_thread *thr = gomp_thread ();
  struct gomp_team *team = thr->ts.team;
  gomp_barrier_state_t bstate;

  /* Work sharing constructs can be orphaned.  */
  if (team == NULL)
    {
      free_work_share (NULL, thr->ts.work_share);
      thr->ts.work_share = NULL;
      return;
    }

  bstate = gomp_barrier_wait_start (&team->barrier);

  if (gomp_barrier_last_thread (bstate))
    {
      if (__builtin_expect (thr->ts.last_work_share != NULL, 1))
	free_work_share (team, thr->ts.last_work_share);
    }

  gomp_team_barrier_wait_end (&team->barrier, bstate);
  thr->ts.last_work_share = NULL;
}

/* The current thread is done with its current work sharing construct.
   This version does NOT imply a barrier at the end of the work-share.  */

void
gomp_work_share_end_nowait (void)
{
  struct gomp_thread *thr = gomp_thread ();
  struct gomp_team *team = thr->ts.team;
  struct gomp_work_share *ws = thr->ts.work_share;
  unsigned completed;

  /* Work sharing constructs can be orphaned.  */
  if (team == NULL)
    {
      free_work_share (NULL, ws);
      thr->ts.work_share = NULL;
      return;
    }

  if (__builtin_expect (thr->ts.last_work_share == NULL, 0))
    return;

#ifdef HAVE_SYNC_BUILTINS
  completed = __sync_add_and_fetch (&ws->threads_completed, 1);
#else
  gomp_mutex_lock (&ws->lock);
  completed = ++ws->threads_completed;
  gomp_mutex_unlock (&ws->lock);
#endif

  if (completed == team->nthreads)
    free_work_share (team, thr->ts.last_work_share);
  thr->ts.last_work_share = NULL;
}