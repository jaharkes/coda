/* BLURB lgpl

                           Coda File System
                              Release 5

          Copyright (c) 1987-1999 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the  terms of the  GNU  Library General Public Licence  Version 2,  as
shown in the file LICENSE. The technical and financial contributors to
Coda are listed in the file CREDITS.

                        Additional copyrights
                           none currently

#*/

#ifndef _DLLIST_H_
#define _DLLIST_H_

/*
 * doubly linked list implementation -- based on linux 
 * kernel code lists.
 *
 */

struct dllist_head {
	struct dllist_head *next, *prev;
};
#define dllist_chain dllist_head

#define DLLIST_HEAD(x) struct dllist_head x = { &x, &x }

#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

#ifdef __cplusplus
extern "C" {
#endif

int list_empty(struct dllist_head *head);
void list_del(struct dllist_head *entry);
void list_add(struct dllist_head *entry, struct dllist_head *head);
void list_head_init(struct dllist_head *ptr);

#ifdef __cplusplus
}
#endif

#endif /* _DLLIST_H_ */

