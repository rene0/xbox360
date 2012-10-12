/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: msdosfsmount.h,v 1.17 1997/11/17 15:37:07 ws Exp $	*/

/*-
 * Copyright (C) 1994, 1995, 1997 Wolfgang Solfrank.
 * Copyright (C) 1994, 1995, 1997 TooLs GmbH.
 * All rights reserved.
 * Original code by Paul Popelka (paulp@uts.amdahl.com) (see below).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Written by Paul Popelka (paulp@uts.amdahl.com)
 *
 * You can do anything you want with this software, just don't say you wrote
 * it, and don't remove this notice.
 *
 * This software is provided "as is".
 *
 * The author supplies this software to be publicly redistributed on the
 * understanding that the author is not responsible for the correct
 * functioning of this software in any circumstances and is not liable for
 * any damages caused by this software.
 *
 * October 1992
 */

#ifndef _XTAF_XTAFMOUNT_H_
#define	_XTAF_XTAFMOUNT_H_

#ifdef _KERNEL

#include <sys/types.h>
#include <sys/lock.h>
#include <sys/lockmgr.h>

#include <sys/queue.h>
#include <sys/mount.h>

#include <fs/xtaf/bpb.h>

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_XTAFMNT);
#endif

struct dot_entry;

/*
 * Layout of the mount control block for an XTAF filesystem.
 */
struct xtafmount {
	struct mount *pm_mountp;/* vfs mount struct for this fs */
	struct g_consumer *pm_cp;
	struct bufobj *pm_bo;
	uid_t pm_uid;		/* uid to set as owner of the files */
	gid_t pm_gid;		/* gid to set as owner of the files */
	mode_t pm_mask;		/* mask to and with file protection bits
				   for files */
	mode_t pm_dirmask;	/* mask to and with file protection bits
				   for directories */
	struct vnode *pm_devvp;	/* vnode for block device mounted */
	struct cdev *pm_dev;	/* character device mounted */
	struct bpb_xtaf pm_bpb; /* BIOS parameter block for this filesystem */
	u_long pm_BlkPerSec;	/* How many DEV_BSIZE blocks fit inside a physical sector */
	u_long pm_FATs;		/* number of FATs specified in XTAF header */
	u_long pm_FATsecs;	/* actual number of fat sectors */
	u_long pm_fatblk;	/* block # of FAT */
	u_long pm_rootdirblk;	/* block # of root directory number */
	u_long pm_rootdirsize;	/* size in blocks (not clusters) */
	u_long pm_firstcluster;	/* block number of first cluster */
	u_long pm_maxcluster;	/* maximum cluster number */
	u_long pm_freeclustercount;	/* number of free clusters */
	u_long pm_cnshift;	/* shift file offset right this amount to get a cluster number */
	u_long pm_crbomask;	/* and a file offset with this mask to get cluster rel offset */
	u_long pm_bnshift;	/* shift file offset right this amount to get a block number */
	u_long pm_bpcluster;	/* bytes per cluster */
	u_long pm_fmod;		/* ~0 if fs is modified, this can rollover to 0	*/
	u_long pm_fatblocksize;	/* size of fat blocks in bytes */
	u_long pm_fatblocksec;	/* size of fat blocks in sectors */
	u_long pm_fatsize;	/* size of fat in bytes */
	u_int32_t pm_fatmask;	/* mask to use for fat numbers */
	u_long pm_nxtfree;	/* next place to search for a free cluster */
	u_int pm_fatmult;	/* 2 or 4 depending on FAT bitsize */
	u_int *pm_inusemap;	/* ptr to bitmap of in-use clusters */
	u_int pm_flags;		/* see below */
	struct lock pm_fatlock; /* lockmgr protecting allocations */
	SLIST_HEAD(dot_head, dot_entry) dot_lookup_table;
};

/*
 * Dot lookup table
 */
#define DOT_NOT_FOUND 0xfffffff0

struct dot_entry {
	SLIST_ENTRY(dot_entry) next;
	u_long startcluster;
	u_long dot;
};

void init_dot_lookup_table(struct xtafmount *);
void remove_dot_lookup_table(struct xtafmount *);
u_long find_dot_entry(struct xtafmount *, u_long);
void add_dot_entry(struct xtafmount *, u_long, u_long);

/* Byte offset in FAT on filesystem pmp, cluster cn */
#define	FATOFS(pmp, cn)	((cn) * (pmp)->pm_fatmult)

#define	VFSTOXTAF(mp)	((struct xtafmount *)mp->mnt_data)

/* Number of bits in one pm_inusemap item: */
#define	N_INUSEBITS	(8 * sizeof(u_int))

/*
 * Shorthand for fields in the bpb contained in the xtafmount structure.
 */
#define	pm_SecPerClust	pm_bpb.bpbSecPerClust
#define	pm_FATs		pm_bpb.bpbFATs
#define	pm_Fill		pm_bpb.bpbFill

/*
 * Convert pointer to buffer -> pointer to direntry
 */
#define	bptoep(pmp, bp, dirofs) \
	((struct direntry *)(((bp)->b_data)	\
	 + ((dirofs) & (pmp)->pm_crbomask)))

/*
 * Convert block number to cluster number
 */
#define	de_bn2cn(pmp, bn) \
	((bn) >> ((pmp)->pm_cnshift - (pmp)->pm_bnshift))

/*
 * Convert cluster number to block number
 */
#define	de_cn2bn(pmp, cn) \
	((cn) << ((pmp)->pm_cnshift - (pmp)->pm_bnshift))

/*
 * Convert file offset to cluster number
 */
#define de_cluster(pmp, off) \
	((off) >> (pmp)->pm_cnshift)

/*
 * Clusters required to hold size bytes
 */
#define	de_clcount(pmp, size) \
	(((size) + (pmp)->pm_bpcluster - 1) >> (pmp)->pm_cnshift)

/*
 * Convert file offset to block number
 */
#define de_blk(pmp, off) \
	(de_cn2bn(pmp, de_cluster((pmp), (off))))

/*
 * Convert cluster number to file offset
 */
#define	de_cn2off(pmp, cn) \
	((cn) << (pmp)->pm_cnshift)

/*
 * Convert block number to file offset
 */
#define	de_bn2off(pmp, bn) \
	((bn) << (pmp)->pm_bnshift)
/*
 * Map a cluster number into a filesystem relative block number.
 */
#define	cntobn(pmp, cn) \
	(de_cn2bn((pmp), (cn)-CLUST_FIRST) + (pmp)->pm_firstcluster)

/*
 * Calculate block number for directory entry in root dir, offset dirofs
 */
#define	roottobn(pmp, dirofs) \
	(de_blk((pmp), (dirofs)) + (pmp)->pm_rootdirblk)

/*
 * Calculate block number for directory entry at cluster dirclu, offset
 * dirofs
 */
#define	detobn(pmp, dirclu, dirofs) \
	((dirclu) == XTAFROOT \
	 ? roottobn((pmp), (dirofs)) \
	 : cntobn((pmp), (dirclu)))

#define XTAF_LOCK_MP(pmp) \
	lockmgr(&(pmp)->pm_fatlock, LK_EXCLUSIVE, NULL)
#define XTAF_UNLOCK_MP(pmp) \
	lockmgr(&(pmp)->pm_fatlock, LK_RELEASE, NULL)
#define XTAF_ASSERT_MP_LOCKED(pmp) \
	lockmgr_assert(&(pmp)->pm_fatlock, KA_XLOCKED)

#endif /* _KERNEL */

/*
 * XTAF mount options for pm_flags:
 */
#define	XTAFMNT_RONLY	0x80000000	/* mounted read-only	*/
#define	XTAFMNT_WAITONFAT	0x40000000	/* mounted synchronous	*/

#endif /* !_XTAF_XTAFMOUNT_H_ */
