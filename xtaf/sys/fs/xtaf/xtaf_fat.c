/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: msdosfs_fat.c,v 1.28 1997/11/17 15:36:49 ws Exp $	*/

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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
/* #include <sys/mount.h> */ /* used in msdosfs */
#include <sys/vnode.h>

#include <sys/endian.h>

#include <fs/xtaf/xtafmount.h>
#include <fs/xtaf/direntry.h>
#include <fs/xtaf/denode.h>
#include <fs/xtaf/fat.h>

static void	fatblock(struct xtafmount *pmp, u_long ofs, u_long *bnp,
		    u_long *sizep, u_long *bop);
static void	fc_lookup(struct denode *dep, u_long findcn, u_long *frcnp,
		    u_long *fsrcnp);
static __inline void
		usemap_free(struct xtafmount *pmp, u_long cn);
static int	xtaf_clusteralloc1(struct xtafmount *pmp, u_long start,
		    u_long count, u_long fillwith, u_long *retcluster,
		    u_long *got);

static int	chainalloc(struct xtafmount *pmp, u_long start,
		    u_long count, u_long fillwith, u_long *retcluster,
		    u_long *got);
static int	chainlength(struct xtafmount *pmp, u_long start,
		    u_long count);
static int	fatchain(struct xtafmount *pmp, u_long start, u_long count,
		    u_long fillwith);
static void	updatefat(struct xtafmount *pmp, struct buf *bp,
		    u_long fatbn);
static __inline void
		usemap_alloc(struct xtafmount *pmp, u_long cn);

static void
fatblock(struct xtafmount *pmp, u_long ofs, u_long *bnp, u_long *sizep,
	    u_long *bop)
{
	u_long bn, size;

	bn = ofs / pmp->pm_fatblocksize * pmp->pm_fatblocksec;
	size = min(pmp->pm_fatblocksec, pmp->pm_FATsecs - bn) * DEV_BSIZE;
	bn += pmp->pm_fatblk;

	if (bnp)
		*bnp = bn;
	if (sizep)
		*sizep = size;
	if (bop)
		*bop = ofs % pmp->pm_fatblocksize;
}

/*
 * Map the logical cluster number of a file into a physical disk sector
 * that is filesystem relative.
 *
 * dep	  - address of denode representing the file of interest
 * findcn - file relative cluster whose filesystem relative cluster number
 *	    and/or block number are/is to be found
 * bnp	  - address of where to place the filesystem relative block number.
 *	    If this pointer is null then don't return this quantity.
 * cnp	  - address of where to place the filesystem relative cluster number.
 *	    If this pointer is null then don't return this quantity.
 * sp	  - returned block size
 *
 * NOTE: Either bnp or cnp must be non-NULL.
 * This function has one side effect.  If the requested file relative cluster
 * is beyond the end of file, then the actual number of clusters in the file
 * is returned in *cnp.  This is useful for determining how long a directory is.
 *  If cnp is null, nothing is returned.
 */
int
xtaf_pcbmap(struct denode *dep, u_long findcn, daddr_t *bnp, u_long *cnp,
	    int *sp)
{
	int error;
	u_long i;
	u_long cn;
	u_long prevcn = 0;
	u_long byteoffset;
	u_long bn;
	u_long bo;
	struct buf *bp = NULL;
	u_long bp_bn = -1;
	struct xtafmount *pmp = dep->de_pmp;
	u_long bsize;

	KASSERT(bnp != NULL || cnp != NULL || sp != NULL,
	    ("pcbmap: extra call"));
	ASSERT_VOP_ELOCKED(DETOV(dep), "pcbmap");

	cn = dep->de_StartCluster;
	/*
	 * The "file" that makes up the root directory is contiguous,
	 * permanently allocated, of fixed size, and is not made up of
	 * clusters.  If the cluster number is beyond the end of the root
	 * directory, then return the number of clusters in the file.
	 */
	if (cn == XTAFROOT) {
		if (dep->de_Attributes & ATTR_DIRECTORY) {
			if (de_cn2off(pmp, findcn) >= dep->de_FileSize) {
				if (cnp)
					*cnp = de_bn2cn(pmp, pmp->pm_rootdirsize);
				return (E2BIG);
			}
			if (bnp)
				*bnp = pmp->pm_rootdirblk + de_cn2bn(pmp, findcn);
			if (cnp)
				*cnp = XTAFROOT;
			if (sp)
				*sp = min(pmp->pm_bpcluster,
				    dep->de_FileSize - de_cn2off(pmp, findcn));
			return (0);
		} else {		/* just an empty file */
			if (cnp)
				*cnp = 0;
			return (E2BIG);
		}
	}

	/*
	 * All other files do I/O in cluster sized blocks
	 */
	if (sp)
		*sp = pmp->pm_bpcluster;

	/*
	 * Rummage around in the fat cache, maybe we can avoid tromping
	 * thru every fat entry for the file. And, keep track of how far
	 * off the cache was from where we wanted to be.
	 */
	i = 0;
	fc_lookup(dep, findcn, &i, &cn);

	/*
	 * Handle all other files or directories the normal way.
	 */
	for (; i < findcn; i++) {
		/*
		 * Stop with all special clusters, not just with EOF.
		 */
		if ((cn | ~pmp->pm_fatmask) >= CLUST_BAD)
			goto hiteof;
		byteoffset = FATOFS(pmp, cn);
		fatblock(pmp, byteoffset, &bn, &bsize, &bo);
		if (bn != bp_bn) {
			if (bp)
				brelse(bp);
			error = bread(pmp->pm_devvp, bn, bsize, NOCRED, &bp);
			if (error) {
				brelse(bp);
				return (error);
			}
			bp_bn = bn;
		}
		prevcn = cn;
		if (bo >= bsize) {
			if (bp)
				brelse(bp);
			return (EIO);
		}
		if (FAT32(pmp))
			cn = be32dec(&bp->b_data[bo]);
		else
			cn = be16dec(&bp->b_data[bo]);
		cn &= pmp->pm_fatmask;

		/*
		 * Force the special cluster numbers
		 * to be the same for all cluster sizes
		 * to let the rest of xtaf handle
		 * all cases the same.
		 */
		if ((cn | ~pmp->pm_fatmask) >= CLUST_BAD)
			cn |= ~pmp->pm_fatmask;
	}

	if (!XTAFEOF(pmp, cn)) {
		if (bp)
			brelse(bp);
		if (bnp)
			*bnp = cntobn(pmp, cn);
		if (cnp)
			*cnp = cn;
		fc_setcache(dep, FC_LASTMAP, i, cn);
		return (0);
	}

hiteof:
	if (cnp)
		*cnp = i;
	if (bp)
		brelse(bp);
	/* update last file cluster entry in the fat cache */
	fc_setcache(dep, FC_LASTFC, i - 1, prevcn);
	return (E2BIG);
}

/*
 * Find the closest entry in the fat cache to the cluster we are looking
 * for.
 */
static void
fc_lookup(struct denode *dep, u_long findcn, u_long *frcnp, u_long *fsrcnp)
{
	int i;
	u_long cn;
	struct fatcache *closest = 0;

	ASSERT_VOP_LOCKED(DETOV(dep), "fc_lookup");

	for (i = 0; i < FC_SIZE; i++) {
		cn = dep->de_fc[i].fc_frcn;
		if (cn != FCE_EMPTY && cn <= findcn) {
			if (closest == 0 || cn > closest->fc_frcn)
				closest = &dep->de_fc[i];
		}
	}
	if (closest) {
		*frcnp = closest->fc_frcn;
		*fsrcnp = closest->fc_fsrcn;
	}
}

/*
 * Purge the fat cache in denode dep of all entries relating to file
 * relative cluster frcn and beyond.
 */
void
xtaf_fc_purge(struct denode *dep, u_int frcn)
{
	int i;
	struct fatcache *fcp;

	ASSERT_VOP_LOCKED(DETOV(dep), "fc_purge");

	fcp = dep->de_fc;
	for (i = 0; i < FC_SIZE; i++, fcp++) {
		if (fcp->fc_frcn >= frcn)
			fcp->fc_frcn = FCE_EMPTY;
	}
}

static __inline void
usemap_free(struct xtafmount *pmp, u_long cn)
{
	XTAF_ASSERT_MP_LOCKED(pmp);
	pmp->pm_freeclustercount++;
	KASSERT((pmp->pm_inusemap[cn / N_INUSEBITS] &
	    (1 << (cn % N_INUSEBITS))) != 0,
	    ("Freeing unused sector %ld %ld %x", cn, cn % N_INUSEBITS,
	    (unsigned)pmp->pm_inusemap[cn / N_INUSEBITS]));
	pmp->pm_inusemap[cn / N_INUSEBITS] &= ~(1 << (cn % N_INUSEBITS));
}

/*
 * Get or Set or 'Get and Set' the cluster'th entry in the fat.
 *
 * function	- whether to get or set a fat entry
 * pmp		- address of the xtafmount structure for the filesystem
 *		  whose fat is to be manipulated.
 * cn		- which cluster is of interest
 * oldcontents	- address of a word that is to receive the contents of the
 *		  cluster'th entry if this is a get function
 * newcontents	- the new value to be written into the cluster'th element of
 *		  the fat if this is a set function.
 *
 * This function can also be used to free a cluster by setting the fat entry
 * for a cluster to 0.
 *
 * All copies of the fat are updated if this is a set function. NOTE: If
 * xtaf_fatentry() marks a cluster as free it does not update the inusemap
 * in the xtafmount structure. This is left to the caller.
 */
int
xtaf_fatentry(int function, struct xtafmount *pmp, u_long cn,
	    u_long *oldcontents, u_long newcontents)
{
	int error;
	u_long readcn;
	u_long bn, bo, bsize, byteoffset;
	struct buf *bp;

#ifdef	XTAF_DEBUG
	printf("xtaf_fatentry(func %d, pmp %p, clust %lx, oldcon %p, newcon %lx)\n",
	    function, pmp, cn, oldcontents, newcontents);
#endif

#ifdef DIAGNOSTIC
	/*
	 * Be sure they asked us to do something.
	 */
	if ((function & (FAT_SET | FAT_GET)) == 0) {
		printf("xtaf_fatentry(): function code doesn't specify get or set\n");
		return (EINVAL);
	}

	/*
	 * If they asked us to return a cluster number but didn't tell us
	 * where to put it, give them an error.
	 */
	if ((function & FAT_GET) && oldcontents == NULL) {
		printf("xtaf_fatentry(): get function with no place to put result\n");
		return (EINVAL);
	}
#endif

	/*
	 * Be sure the requested cluster is in the filesystem.
	 */
	if (cn < CLUST_FIRST || cn > pmp->pm_maxcluster)
		return (EINVAL);

	byteoffset = FATOFS(pmp, cn);
	fatblock(pmp, byteoffset, &bn, &bsize, &bo);
	error = bread(pmp->pm_devvp, bn, bsize, NOCRED, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	if (function & FAT_GET) {
		if (FAT32(pmp))
			readcn = be32dec(&bp->b_data[bo]);
		else
			readcn = be16dec(&bp->b_data[bo]);
		readcn &= pmp->pm_fatmask;
		/* map special fat entries to same values for all fats */
		if ((readcn | ~pmp->pm_fatmask) >= CLUST_BAD)
			readcn |= ~pmp->pm_fatmask;
		*oldcontents = readcn;
	}
	if (function & FAT_SET) {
		switch (pmp->pm_fatmask) {
		case FAT16_MASK:
			be16enc(&bp->b_data[bo], newcontents);
			break;
		case FAT32_MASK:
			/*
			 * According to spec we have to retain the
			 * high order bits of the fat entry.
			 */
			readcn = be32dec(&bp->b_data[bo]);
			readcn &= ~FAT32_MASK;
			readcn |= newcontents & FAT32_MASK;
			be32enc(&bp->b_data[bo], readcn);
			break;
		}
		updatefat(pmp, bp, bn);
		bp = NULL;
		pmp->pm_fmod = 1;
	}
	if (bp)
		brelse(bp);
	return (0);
}

/*
 * Read in fat blocks looking for free clusters. For every free cluster
 * found turn off its corresponding bit in the pm_inusemap.
 */
int
xtaf_fillinusemap(struct xtafmount *pmp)
{
	struct buf *bp = NULL;
	u_long cn, readcn;
	int error;
	u_long bn, bo, bsize, byteoffset;

	XTAF_ASSERT_MP_LOCKED(pmp);

	/*
	 * Mark all clusters in use, we mark the free ones in the fat scan
	 * loop further down.
	 */
	for (cn = 0; cn < (pmp->pm_maxcluster + N_INUSEBITS) / N_INUSEBITS; cn++)
		pmp->pm_inusemap[cn] = (u_int)-1;

	/*
	 * Figure how many free clusters are in the filesystem by ripping
	 * through the fat counting the number of entries whose content is
	 * zero.  These represent free clusters.
	 */
	pmp->pm_freeclustercount = 0;
	for (cn = CLUST_FIRST; cn <= pmp->pm_maxcluster; cn++) {
		byteoffset = FATOFS(pmp, cn);
		bo = byteoffset % pmp->pm_fatblocksize;
		if (!bo || !bp) {
			/* Read new FAT block */
			if (bp)
				brelse(bp);
			fatblock(pmp, byteoffset, &bn, &bsize, NULL);
			error = bread(pmp->pm_devvp, bn, bsize, NOCRED, &bp);
			if (error) {
				brelse(bp);
				return (error);
			}
		}
		if (FAT32(pmp))
			readcn = be32dec(&bp->b_data[bo]);
		else
			readcn = be16dec(&bp->b_data[bo]);
		readcn &= pmp->pm_fatmask;
		if (readcn == 0)
			usemap_free(pmp, cn);
	}
	if (bp != NULL)
		brelse(bp);
	return (0);
}

/*
 * Update the fat.
 *
 * pmp	 - xtafmount structure for filesystem to update
 * bp	 - addr of modified fat block
 * fatbn - block number relative to begin of filesystem of the modified fat block.
 */
static void
updatefat(struct xtafmount *pmp, struct buf *bp, u_long fatbn)
{
#ifdef XTAF_DEBUG
	printf("updatefat(pmp %p, bp %p, fatbn %lx)\n", pmp, bp, fatbn);
#endif

	/*
	 * Write out the fat.
	 */
	if (pmp->pm_flags & XTAFMNT_WAITONFAT)
		bwrite(bp);
	else
		bdwrite(bp);
}

static __inline void
usemap_alloc(struct xtafmount *pmp, u_long cn)
{
	XTAF_ASSERT_MP_LOCKED(pmp);
	KASSERT((pmp->pm_inusemap[cn / N_INUSEBITS] &
	    (1 << (cn % N_INUSEBITS))) == 0,
	    ("Allocating used sector %ld %ld %x", cn, cn % N_INUSEBITS,
	    (unsigned)pmp->pm_inusemap[cn / N_INUSEBITS]));
	pmp->pm_inusemap[cn / N_INUSEBITS] |= 1 << (cn % N_INUSEBITS);
	KASSERT(pmp->pm_freeclustercount > 0, ("usemap_alloc: too little"));
	pmp->pm_freeclustercount--;
}

int
xtaf_clusterfree(struct xtafmount *pmp, u_long cluster, u_long *oldcnp)
{
	int error;
	u_long oldcn;

	usemap_free(pmp, cluster);
	error = xtaf_fatentry(FAT_GET_AND_SET, pmp, cluster, &oldcn, XTAFFREE);
	if (error)
		return (error);

	/*
	 * If the cluster was successfully marked free, then update
	 * the count of free clusters, and turn off the "allocated"
	 * bit in the "in use" cluster bit map.
	 */
	XTAF_LOCK_MP(pmp);
	usemap_free(pmp, cluster);
	XTAF_UNLOCK_MP(pmp);
	if (oldcnp)
		*oldcnp = oldcn;
	return (0);
}

/*
 * Update a contiguous cluster chain
 *
 * pmp	    - mount point
 * start    - first cluster of chain
 * count    - number of clusters in chain
 * fillwith - what to write into fat entry of last cluster
 */
static int
fatchain(struct xtafmount *pmp, u_long start, u_long count, u_long fillwith)
{
	int error;
	u_long bn, bo, bsize, byteoffset, readcn, newc;
	struct buf *bp;

#ifdef XTAF_DEBUG
	printf("fatchain(pmp %p, start %lx, count %lx, fillwith %lx)\n",
	    pmp, start, count, fillwith);
#endif
	/*
	 * Be sure the clusters are in the filesystem.
	 */
	if (start < CLUST_FIRST || start + count - 1 > pmp->pm_maxcluster)
		return (EINVAL);

	while (count > 0) {
		byteoffset = FATOFS(pmp, start);
		fatblock(pmp, byteoffset, &bn, &bsize, &bo);
		error = bread(pmp->pm_devvp, bn, bsize, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
		while (count > 0) {
			start++;
			newc = --count > 0 ? start : fillwith;
			switch (pmp->pm_fatmask) {
			case FAT16_MASK:
				be16enc(&bp->b_data[bo], newc);
				bo += 2;
				break;
			case FAT32_MASK:
				readcn = be32dec(&bp->b_data[bo]);
				readcn &= ~pmp->pm_fatmask;
				readcn |= newc & pmp->pm_fatmask;
				be32enc(&bp->b_data[bo], readcn);
				bo += 4;
				break;
			}
			if (bo >= bsize)
				break;
		}
		updatefat(pmp, bp, bn);
	}
	pmp->pm_fmod = 1;
	return (0);
}

/*
 * Check the length of a free cluster chain starting at start.
 *
 * pmp	 - mount point
 * start - start of chain
 * count - maximum interesting length
 */
static int
chainlength(struct xtafmount *pmp, u_long start, u_long count)
{
	u_long idx, max_idx;
	u_int map;
	u_long len;

	XTAF_ASSERT_MP_LOCKED(pmp);

	max_idx = pmp->pm_maxcluster / N_INUSEBITS;
	idx = start / N_INUSEBITS;
	start %= N_INUSEBITS;
	map = pmp->pm_inusemap[idx];
	map &= ~((1 << start) - 1);
	if (map) {
		len = ffs(map) - 1 - start;
		return (len > count ? count : len);
	}
	len = N_INUSEBITS - start;
	if (len >= count)
		return (count);
	while (++idx <= max_idx) {
		if (len >= count)
			break;
		map = pmp->pm_inusemap[idx];
		if (map) {
			len += ffs(map) - 1;
			break;
		}
		len += N_INUSEBITS;
	}
	return (len > count ? count : len);
}

/*
 * Allocate contigous free clusters.
 *
 * pmp	      - mount point.
 * start      - start of cluster chain.
 * count      - number of clusters to allocate.
 * fillwith   - put this value into the fat entry for the
 *		last allocated cluster.
 * retcluster - put the first allocated cluster's number here.
 * got	      - how many clusters were actually allocated.
 */
static int
chainalloc(struct xtafmount *pmp, u_long start, u_long count, u_long fillwith,
	    u_long *retcluster, u_long *got)
{
	int error;
	u_long cl, n;

	XTAF_ASSERT_MP_LOCKED(pmp);

	for (cl = start, n = count; n-- > 0;)
		usemap_alloc(pmp, cl++);

	error = fatchain(pmp, start, count, fillwith);
	if (error != 0)
		return (error);
#ifdef XTAF_DEBUG
	printf("xtaf_chainalloc(): allocated cluster chain at %lx (%lx clusters)\n",
	    start, count);
#endif
	if (retcluster)
		*retcluster = start;
	if (got)
		*got = count;
	pmp->pm_nxtfree = start + count;
	if (pmp->pm_nxtfree > pmp->pm_maxcluster)
		pmp->pm_nxtfree = CLUST_FIRST;
	return (0);
}

/*
 * Allocate contiguous free clusters.
 *
 * pmp	      - mount point.
 * start      - preferred start of cluster chain.
 * count      - number of clusters requested.
 * fillwith   - put this value into the fat entry for the
 *		last allocated cluster.
 * retcluster - put the first allocated cluster's number here.
 * got	      - how many clusters were actually allocated.
 */
int
xtaf_clusteralloc(struct xtafmount *pmp, u_long start, u_long count,
	    u_long fillwith, u_long *retcluster, u_long *got)
{
	int error;

	XTAF_LOCK_MP(pmp);
	error = xtaf_clusteralloc1(pmp, start, count, fillwith, retcluster, got);
	XTAF_UNLOCK_MP(pmp);
	return (error);
}

int
xtaf_clusteralloc1(struct xtafmount *pmp, u_long start, u_long count,
	    u_long fillwith, u_long *retcluster, u_long *got)
{
	u_long idx;
	u_long len, newst, foundl, cn, l;
	u_long foundcn = 0;
	u_int map;

	XTAF_ASSERT_MP_LOCKED(pmp);

#ifdef XTAF_DEBUG
	printf("xtaf_clusteralloc1(): find %lx clusters\n", count);
#endif
	if (start) {
		if ((len = chainlength(pmp, start, count)) >= count)
			return (chainalloc(pmp, start, count, fillwith,
			    retcluster, got));
	} else
		len = 0;

	newst = pmp->pm_nxtfree;
	foundl = 0;

	for (cn = newst; cn <= pmp->pm_maxcluster;) {
		idx = cn / N_INUSEBITS;
		map = pmp->pm_inusemap[idx];
		map |= (1 << (cn % N_INUSEBITS)) - 1;
		if (map != (u_int)-1) {
			cn = idx * N_INUSEBITS + ffs(map^(u_int)-1) - 1;
			if ((l = chainlength(pmp, cn, count)) >= count)
				return (chainalloc(pmp, cn, count, fillwith,
				    retcluster, got));
			if (l > foundl) {
				foundcn = cn;
				foundl = l;
			}
			cn += l + 1;
			continue;
		}
		cn += N_INUSEBITS - cn % N_INUSEBITS;
	}
	for (cn = 0; cn < newst; ) {
		idx = cn / N_INUSEBITS;
		map = pmp->pm_inusemap[idx];
		map |= (1 << (cn % N_INUSEBITS)) - 1;
		if (map != (u_int)-1) {
			cn = idx * N_INUSEBITS + ffs(map ^ (u_int)-1) - 1;
			if ((l = chainlength(pmp, cn, count)) >= count)
				return (chainalloc(pmp, cn, count, fillwith,
				    retcluster, got));
			if (l > foundl) {
				foundcn = cn;
				foundl = l;
			}
			cn += l + 1;
			continue;
		}
		cn += N_INUSEBITS - cn % N_INUSEBITS;
	}

	if (!foundl)
		return (ENOSPC);

	if (len)
		return (chainalloc(pmp, start, len, fillwith, retcluster, got));
	else
		return (chainalloc(pmp, foundcn, foundl, fillwith, retcluster,
			    got));
}

/*
 * Free a chain of clusters.
 *
 * pmp		- address of the xtaf mount structure for the filesystem
 *		  containing the cluster chain to be freed.
 * startcluster - number of the 1st cluster in the chain of clusters to be
 *		  freed.
 */
int
xtaf_freeclusterchain(struct xtafmount *pmp, u_long cluster)
{
	int error;
	struct buf *bp = NULL;
	u_long bn, bo, bsize, byteoffset;
	u_long lbn = -1;

	XTAF_LOCK_MP(pmp);

	while (cluster >= CLUST_FIRST && cluster <= pmp->pm_maxcluster) {
		byteoffset = FATOFS(pmp, cluster);
		fatblock(pmp, byteoffset, &bn, &bsize, &bo);
		if (lbn != bn) {
			if (bp)
				updatefat(pmp, bp, lbn);
			error = bread(pmp->pm_devvp, bn, bsize, NOCRED, &bp);
			if (error) {
				brelse(bp);
				XTAF_UNLOCK_MP(pmp);
				return (error);
			}
			lbn = bn;
		}
		usemap_free(pmp, cluster);
		switch (pmp->pm_fatmask) {
		case FAT16_MASK:
			cluster = be16dec(&bp->b_data[bo]);
			be16enc(&bp->b_data[bo], XTAFFREE);
			break;
		case FAT32_MASK:
			cluster = be32dec(&bp->b_data[bo]);
			be32enc(&bp->b_data[bo], (XTAFFREE & FAT32_MASK) |
			    (cluster & ~FAT32_MASK));
			break;
		}
		cluster &= pmp->pm_fatmask;
		if ((cluster | ~pmp->pm_fatmask) >= CLUST_BAD)
			cluster |= pmp->pm_fatmask;
	}
	if (bp)
		updatefat(pmp, bp, bn);
	XTAF_UNLOCK_MP(pmp);
	return (0);
}

/*
 * Allocate a new cluster and chain it onto the end of the file.
 *
 * dep	 - the file to extend
 * count - number of clusters to allocate
 * bpp	 - where to return the address of the buf header for the first new
 *	   file block
 * ncp	 - where to put cluster number of the first newly allocated cluster
 *	   If this pointer is 0, do not return the cluster number.
 * flags - see fat.h
 *
 * NOTE: This function is not responsible for turning on the DE_UPDATE bit of
 * the de_flag field of the denode and it does not change the de_FileSize
 * field.  This is left for the caller to do.
 */
int
xtaf_extendfile(struct denode *dep, u_long count, struct buf **bpp, u_long *ncp,
	    int flags)
{
	int error;
	u_long frcn;
	u_long cn, got;
	struct xtafmount *pmp = dep->de_pmp;
	struct buf *bp;
	daddr_t blkno;

	/*
	 * Don't try to extend the root directory
	 */
	if (dep->de_StartCluster == XTAFROOT &&
	    (dep->de_Attributes & ATTR_DIRECTORY)) {
		printf("xtaf_extendfile(): attempt to extend root directory\n");
		return (ENOSPC);
	}

	/*
	 * If the "file's last cluster" cache entry is empty, and the file
	 * is not empty, then fill the cache entry by calling xtaf_pcbmap().
	 */
	if (dep->de_fc[FC_LASTFC].fc_frcn == FCE_EMPTY &&
	    dep->de_StartCluster != 0) {
		error = xtaf_pcbmap(dep, 0xffff, NULL, &cn, NULL);
		/* we expect it to return E2BIG */
		if (error != E2BIG)
			return (error);
	}

	dep->de_fc[FC_NEXTTOLASTFC].fc_frcn =
	    dep->de_fc[FC_LASTFC].fc_frcn;
	dep->de_fc[FC_NEXTTOLASTFC].fc_fsrcn =
	    dep->de_fc[FC_LASTFC].fc_fsrcn;
	while (count > 0) {
		/*
		 * Allocate a new cluster chain and cat onto the end of the
		 * file.  If the file is empty we make de_StartCluster point
		 * to the new block.  Note that de_StartCluster being 0 is
		 * sufficient to be sure the file is empty since we exclude
		 * attempts to extend the root directory above, and the root
		 * dir is the only file with a startcluster of 0 that has
		 * blocks allocated (sort of).
		 */
		if (dep->de_StartCluster == 0)
			cn = 0;
		else
			cn = dep->de_fc[FC_LASTFC].fc_fsrcn + 1;
		error = xtaf_clusteralloc(pmp, cn, count, CLUST_EOFE, &cn, &got);
		if (error)
			return (error);

		count -= got;

		/*
		 * Give them the filesystem relative cluster number if they want
		 * it.
		 */
		if (ncp) {
			*ncp = cn;
			ncp = NULL;
		}

		if (dep->de_StartCluster == 0) {
			dep->de_StartCluster = cn;
			frcn = 0;
		} else {
			error = xtaf_fatentry(FAT_SET, pmp,
					 dep->de_fc[FC_LASTFC].fc_fsrcn, 0, cn);
			if (error) {
				xtaf_clusterfree(pmp, cn, NULL);
				return (error);
			}
			frcn = dep->de_fc[FC_LASTFC].fc_frcn + 1;
		}

		/*
		 * Update the "last cluster of the file" entry in the denode's
		 * fat cache.
		 */
		fc_setcache(dep, FC_LASTFC, frcn + got - 1, cn + got - 1);

		if (flags & DE_CLEAR) {
			while (got-- > 0) {
				/*
				 * Get the buf header for the new block of the
				 * file.
				 */
				if (dep->de_Attributes & ATTR_DIRECTORY)
					bp = getblk(pmp->pm_devvp,
					    cntobn(pmp, cn++),
					    pmp->pm_bpcluster, 0, 0, 0);
				else {
					bp = getblk(DETOV(dep),
					    frcn++,
					    pmp->pm_bpcluster, 0, 0, 0);
					/*
					 * Do the bmap now, as in xtaf_write
					 */
					if (xtaf_pcbmap(dep,
					    bp->b_lblkno,
					    &blkno, 0, 0))
						bp->b_blkno = -1;
					if (bp->b_blkno == -1)
						panic("xtaf_extendfile: xtaf_pcbmap");
					else
						bp->b_blkno = blkno;
				}
				vfs_bio_clrbuf(bp);
				if (bpp) {
					*bpp = bp;
					bpp = NULL;
				} else
					bdwrite(bp);
			}
		}
	}

	return (0);
}
