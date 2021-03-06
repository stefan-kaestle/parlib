#include "internal/parlib.h"
#include "waitfreelist.h"
#include "atomic.h"
#include <stdlib.h>
#include "parlib.h"
#include "export.h"

void wfl_init(struct wfl *list)
{
  list->size = 0;
  list->first.next = NULL;
  list->first.data = NULL;
  list->head = &list->first;
}

void wfl_cleanup(struct wfl *list)
{
  assert(list->first.data == NULL);
  struct wfl_slot *p = list->first.next; // don't free the first element
  while (p != NULL) {
    assert(p->data == NULL);
    struct wfl_slot *tmp = p;
    p = p->next;
    free(tmp);
  }
}

size_t wfl_capacity(struct wfl *list)
{
  size_t res = 0;
  for (struct wfl_slot *p = list->head; p != NULL; p = p->next)
    res++;
  return res;
}

size_t wfl_size(struct wfl *list)
{
  return list->size;
}

#if 0
struct wfl_slot *wfl_insert(struct wfl *list, void *data)
{
  struct wfl_slot *p = list->head; // list head is never null
  struct wfl_slot *new_slot = NULL;
  while (1) {
    if (p->data == NULL) {
      if (__sync_bool_compare_and_swap(&p->data, NULL, data)) {
        free(new_slot);
        return p;
      }
    }

    if (p->next != NULL) {
      p = p->next;
      continue;
    }

    if (new_slot == NULL) {
      new_slot = parlib_aligned_alloc(ARCH_CL_SIZE, sizeof(struct wfl_slot));
      if (new_slot == NULL)
        abort();
      new_slot->data = data;
      new_slot->next = NULL;
      wmb();
    }
 
    if (__sync_bool_compare_and_swap(&p->next, NULL, new_slot)) {
      __sync_fetch_and_add(&list->size, 1);
      return new_slot;
    }
    p = list->head;
  }
}
#endif

struct wfl_slot *wfl_insert(struct wfl *list, void *data)
{
  struct wfl_slot *p = list->head; // list head is never null
  while (1) {
    if (wfl_insert_into(list, p, data))
      return p;

    if (p->next == NULL)
      break;

    p = p->next;
  }

  struct wfl_slot *new_slot;
  new_slot = parlib_aligned_alloc(ARCH_CL_SIZE, sizeof(struct wfl_slot));
  if (new_slot == NULL)
    abort();
  new_slot->data = data;
  new_slot->next = NULL;

  wmb();

  struct wfl_slot *next;
  while ((next = __sync_val_compare_and_swap(&p->next, NULL, new_slot)))
    p = next;

  __sync_fetch_and_add(&list->size, 1);
  return new_slot;
}

bool wfl_insert_into(struct wfl *list, struct wfl_slot *slot, void *data)
{
  if (slot->data != NULL)
    return false;
  bool ret = __sync_bool_compare_and_swap(&slot->data, NULL, data);
  if (ret)
    __sync_fetch_and_add(&list->size, 1);
  return ret;
}

void *wfl_remove_from(struct wfl *list, struct wfl_slot *slot)
{
  if (slot->data == NULL)
    return NULL;
  void *data = atomic_swap_ptr(&slot->data, 0);
  if (data != NULL)
    __sync_fetch_and_add(&list->size, -1);
  return data;
}

void *wfl_remove(struct wfl *list)
{
  if (list->size == 0)
    return NULL;
  for (struct wfl_slot *p = list->head; p != NULL; p = p->next) {
    void *data = wfl_remove_from(list, p);
    if (data != NULL)
      return data;
  }
  return NULL;
}

size_t wfl_remove_all(struct wfl *list, void *data)
{
  size_t n = 0;
  for (struct wfl_slot *p = list->head; p != NULL; p = p->next) {
    if (p->data == data)
      n += __sync_bool_compare_and_swap(&p->data, data, NULL);
  }
  __sync_fetch_and_add(&list->size, -n);
  return n;
}

#undef wfl_init
#undef wfl_cleanup
#undef wfl_insert
#undef wfl_insert_into
#undef wfl_remove
#undef wfl_remove_from
#undef wfl_remove_all
#undef wfl_capacity
#undef wfl_size
EXPORT_ALIAS(INTERNAL(wfl_init), wfl_init)
EXPORT_ALIAS(INTERNAL(wfl_cleanup), wfl_cleanup)
EXPORT_ALIAS(INTERNAL(wfl_insert), wfl_insert)
EXPORT_ALIAS(INTERNAL(wfl_insert_into), wfl_insert_into)
EXPORT_ALIAS(INTERNAL(wfl_remove), wfl_remove)
EXPORT_ALIAS(INTERNAL(wfl_remove_from), wfl_remove_from)
EXPORT_ALIAS(INTERNAL(wfl_remove_all), wfl_remove_all)
EXPORT_ALIAS(INTERNAL(wfl_capacity), wfl_capacity)
EXPORT_ALIAS(INTERNAL(wfl_size), wfl_size)
