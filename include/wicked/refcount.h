/*
 *	refcount -- reference couting utils
 *
 *	Copyright (C) 2022 SUSE LLC
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *	Authors:
 *		Marius Tomaschewski
 *		Clemens Famulla-Conrad
 */
#ifndef WICKED_REFCOUNT_H
#define WICKED_REFCOUNT_H

#include <wicked/types.h>

typedef unsigned int	ni_refcount_t;

extern ni_bool_t	ni_refcount_init(ni_refcount_t *refcount);
extern ni_bool_t	ni_refcount_increment(ni_refcount_t *refcount);
extern ni_bool_t	ni_refcount_decrement(ni_refcount_t *refcount);


#define 		ni_refcounted_declare_new(prefix,args...)	\
	prefix##_t *		prefix##_new(args)

#define			ni_refcounted_declare_ref(prefix)		\
	prefix##_t *		prefix##_ref(prefix##_t *)

#define			ni_refcounted_declare_free(prefix)		\
	void			prefix##_free(prefix##_t *)

#define			ni_refcounted_declare_hold(prefix)		\
	ni_bool_t		prefix##_hold(prefix##_t **, prefix##_t *)

#define			ni_refcounted_declare_drop(prefix)		\
	ni_bool_t		prefix##_drop(prefix##_t **)

#define			ni_refcounted_declare_move(prefix)		\
	ni_bool_t		prefix##_move(prefix##_t **, prefix##_t **)


#define			ni_refcounted_define_new_xargs(prefix,		\
					func_args, init_args)		\
	prefix##_t *							\
	prefix##_new func_args						\
	{								\
		prefix##_t *obj = malloc(sizeof(*obj));			\
		if (obj) {						\
			if (!prefix##_init init_args) {			\
				free(obj);				\
				return NULL;				\
			}						\
			if (!ni_refcount_init(&obj->refcount)) {	\
				prefix##_destroy(obj);			\
				free(obj);				\
				return NULL;				\
			}						\
		}							\
		return obj;						\
	}
#define			ni_refcounted_define_new0(prefix)		\
			ni_refcounted_define_new_xargs(prefix,		\
					(void), (obj))
#define			ni_refcounted_define_new1(prefix, _1)		\
			ni_refcounted_define_new_xargs(prefix,		\
					(_1 a), (obj, a))
#define			ni_refcounted_define_new2(prefix, _1, _2)	\
			ni_refcounted_define_new_xargs(prefix,		\
					(_1 a, _2 b), (obj, a, b))
#define			ni_refcounted_define_new3(prefix, _1, _2, _3)	\
			ni_refcounted_define_new_xargs(prefix,		\
					(_1 a, _2 b, _3 c), (obj, a, b, c))
#define			ni_refcounted_macro(_1, _2, _3, _4, NAME, ...) NAME

#define			ni_refcounted_define_new(args...)		\
			ni_refcounted_macro(args,			\
				ni_refcounted_define_new3,		\
				ni_refcounted_define_new2,		\
				ni_refcounted_define_new1,		\
				ni_refcounted_define_new0)(args)

#define			ni_refcounted_define_ref(prefix)		\
	prefix##_t *							\
	prefix##_ref(prefix##_t *ref)					\
	{								\
		if (ref && ni_refcount_increment(&ref->refcount))	\
			return ref;					\
		return NULL;						\
	}

#define 		ni_refcounted_define_free(prefix)		\
	void								\
	prefix##_free(prefix##_t *ref)					\
	{								\
		if (ref && ni_refcount_decrement(&ref->refcount)) {	\
			prefix##_destroy(ref);				\
			free(ref);					\
		}							\
	}

#define 		ni_refcounted_define_hold(prefix)		\
	ni_bool_t							\
	prefix##_hold(prefix##_t **tie, prefix##_t *ref)		\
	{								\
		prefix##_t *old;					\
		if (tie && ref) {					\
			old = *tie;					\
			*tie = prefix##_ref(ref);			\
			prefix##_free(old);				\
			return TRUE;					\
		}							\
		return FALSE;						\
	}

#define 		ni_refcounted_define_drop(prefix)		\
	ni_bool_t							\
	prefix##_drop(prefix##_t **tie)					\
	{								\
		prefix##_t *old;					\
		if (tie) {						\
			old = *tie;					\
			*tie = NULL;					\
			prefix##_free(old);				\
			return TRUE;					\
		}							\
		return FALSE;						\
	}

#define 		ni_refcounted_define_move(prefix)		\
	ni_bool_t							\
	prefix##_move(prefix##_t **dst, prefix##_t **src)		\
	{								\
		if (src && prefix##_hold(dst, *src))			\
			return prefix##_drop(src);			\
		return FALSE;						\
	}

#endif /* WICKED_REFCOUNT_H */
