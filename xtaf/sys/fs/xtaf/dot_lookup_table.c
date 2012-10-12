/*-
 * Copyright (c) 2006, 2007 Rene Ladan <r.c.ladan@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Since Microsoft 'forgot' to include the .. and more importantly the .
 * directory entries, we keep a lookup table for them ourselves.
 *
 * Note that .. can be retrieved by a double lookup.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: $");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/kernel.h>

#include <fs/xtaf/xtafmount.h>

static MALLOC_DEFINE(M_XTAFDOT, "XTAF_dot", "XTAF dot lookup table");

/*
 * Initialize the dot lookup list.
 */
void
init_dot_lookup_table(struct xtafmount *pmp)
{
	SLIST_INIT(&(pmp->dot_lookup_table));
}

/*
 * Find an entry in the dot lookup list.
 */
u_long
find_dot_entry(struct xtafmount *pmp, u_long startcluster)
{
	struct dot_entry *dot;

	SLIST_FOREACH(dot, &(pmp->dot_lookup_table), next)
		if (dot->startcluster == startcluster)
			return (dot->dot);
	return (DOT_NOT_FOUND);
}

/*
 * Drop the dot lookup list.
 */
void
remove_dot_lookup_table(struct xtafmount *pmp)
{
	struct dot_entry *dot;

	while (!SLIST_EMPTY(&(pmp->dot_lookup_table))) {
		dot = SLIST_FIRST(&(pmp->dot_lookup_table));
		SLIST_REMOVE_HEAD(&(pmp->dot_lookup_table), next);
		free(dot, M_XTAFDOT);
	}
}

/*
 * Add an entry to the dot lookup list.
 */
void
add_dot_entry(struct xtafmount *pmp, u_long cluster, u_long dot)
{
	struct dot_entry *newdot;
	/*
	 * Do a lookup in advance to conserve kernel memory.  This makes
	 * the function O(n) instead of O(1).
	 */
	if (find_dot_entry(pmp, cluster) == DOT_NOT_FOUND) {
		newdot = malloc(sizeof(struct dot_entry), M_XTAFDOT, M_WAITOK);
		newdot->startcluster = cluster;
		newdot->dot          = dot;
		SLIST_INSERT_HEAD(&(pmp->dot_lookup_table), newdot, next);
	}
}
