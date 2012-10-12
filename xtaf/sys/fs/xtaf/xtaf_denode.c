/* XXX legal*/
/* $FreeBSD: */
/*	$NetBSD: msdosfs_denode.c,v 1.28 1998/02/10 14:10:00 mrg Exp $	*/

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
#include <sys/clock.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
/* #include <sys/mount.h> */ /* used by msdosfs */
#include <sys/vnode.h>

#include <sys/endian.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#include <fs/xtaf/direntry.h>
#include <fs/xtaf/denode.h>
#include <fs/xtaf/fat.h>
#include <fs/xtaf/xtafmount.h>

static MALLOC_DEFINE(M_XTAFNODE, "XTAF_node", "XTAF vnode private part");

static int
de_vncmpf(struct vnode *vp, void *arg)
{
	struct denode *dep;
	uint32_t *a;

	a = arg;
	dep = VTODE(vp);
	return (dep->de_inode != *a);
}

/*
 * If xtaf_deget() succeeds it returns with the gotten denode locked().
 *
 * pmp	     - address of xtafmount structure of the filesystem containing
 *	       the denode of interest.  The address of the xtafmount structure
 *	       is used.
 * dirclust  - which cluster bp contains, if dirclust is 0 (root directory)
 *	       diroffset is relative to the beginning of the root directory,
 *	       otherwise it is cluster relative.  This is the cluster this
	       directory entry came from.
 * diroffset - offset past begin of cluster of denode we want
 * depp	     - returns the address of the gotten denode.
 */
int
xtaf_deget(struct xtafmount *pmp, u_long dirclust, u_long diroffset,
	    struct denode **depp)
{
	int error;
	uint32_t inode;
	struct mount *mntp = pmp->pm_mountp;
	struct direntry *ep;
	struct denode *ldep;
	struct vnode *nvp, *xvp;
	struct buf *bp;

#ifdef XTAF_DEBUG
	printf("xtaf_deget(pmp %p, dirclust %lx, diroffset %lx, depp %p)\n",
	    pmp, dirclust, diroffset, depp);
#endif

	/*
	 * See if the denode is in the denode cache. Use the location of
	 * the directory entry to compute the hash value. For subdir use
	 * address of the first dir entry. For root dir use cluster XTAFROOT,
	 * offset XTAFROOT_OFS
	 */
	inode = (uint32_t)pmp->pm_bpcluster * dirclust + diroffset;

	error = vfs_hash_get(mntp, inode, LK_EXCLUSIVE, curthread, &nvp,
	    de_vncmpf, &inode);
	if (error)
		return (error);
	if (nvp != NULL) {
		*depp = VTODE(nvp);
		KASSERT((*depp)->de_dirclust == dirclust, ("wrong dirclust"));
		KASSERT((*depp)->de_diroffset == diroffset, ("wrong diroffset"));
		return (0);
	}

	/*
	 * Do the malloc before the getnewvnode since doing so afterward
	 * might cause a bogus v_data pointer to get dereferenced
	 * elsewhere if malloc should block.
	 */
	ldep = malloc(sizeof(struct denode), M_XTAFNODE, M_WAITOK | M_ZERO);

	/*
	 * Directory entry was not in cache, have to create a vnode and
	 * copy it from the passed disk buffer.
	 */
	/* getnewvnode() does a VREF() on the vnode */
	error = getnewvnode("xtaf", mntp, &xtaf_vnodeops, &nvp);
	if (error) {
		*depp = NULL;
		free(ldep, M_XTAFNODE);
		return error;
	}
	nvp->v_data = ldep;
	ldep->de_vnode = nvp;
	ldep->de_flag = 0;
	ldep->de_dirclust = dirclust;
	ldep->de_diroffset = diroffset;
	ldep->de_inode = inode;

	lockmgr(nvp->v_vnlock, LK_EXCLUSIVE, NULL);
	xtaf_fc_purge(ldep, 0);	/* init the fat cache for this denode */
	error = insmntque(nvp, mntp);
	if (error != 0) {
		free(ldep, M_XTAFNODE);
		*depp = NULL;
		return (error);
	}
	error = vfs_hash_insert(nvp, inode, LK_EXCLUSIVE, curthread, &xvp,
	    de_vncmpf, &inode);
	if (error) {
		*depp = NULL;
		return (error);
	}
	if (xvp != NULL) {
		*depp = xvp->v_data;
		return (0);
	}

	ldep->de_pmp = pmp;
	ldep->de_refcnt = 1;
#ifdef XTAF_DEBUG
	printf("xtaf_deget(): dirclust=%lx diroffset=%lx\n", dirclust, diroffset);
#endif
	/*
	 * Copy the directory entry into the denode area of the vnode.
	 */
	if (dirclust == XTAFROOT && diroffset == XTAFROOT_OFS) {
		/*
		 * Directory entry for the root directory. There isn't one,
		 * so we manufacture one.
		 */
		nvp->v_vflag |= VV_ROOT;
		FAKEDIR(ldep, pmp->pm_rootdirsize, XTAFROOT);
#ifdef XTAF_DEBUG
		printf("xtaf_deget(): FAKEDIR root\n");
#endif
	} else {
		error = xtaf_readep(pmp, dirclust, diroffset, &bp, &ep);
		if (error) {
			/*
			 * The denode does not contain anything useful, so
			 * it would be wrong to leave it on its hash chain.
			 * Arrange for vput() to just forget about it.
			 */
			ldep->de_Length = LEN_DELETED;

			vput(nvp);
			*depp = NULL;
			return (error);
		}
		DE_INTERNALIZE(ldep, ep);
		brelse(bp);
	}

	/*
	 * Fill in a few fields of the vnode and finish filling in the
	 * denode.  Then return the address of the found denode.
	 */
	if (ldep->de_Attributes & ATTR_DIRECTORY) {
		/*
		 * Since XTAF directory entries that describe directories
		 * have 0 in the filesize field, we take this opportunity
		 * to find out the length of the directory and plug it into
		 * the denode structure.
		 */
		u_long size;

#ifdef XTAF_DEBUG
		/* FIXME something goes wrong here, StartCluster shouldn't
		 * be 0xffffffff ?   Filename also gets blanked here and
		 * SF_IMMUTABLE set
		 */
		printf("ldep->de_StartCluster = %lx dirclust = %lx\n", ldep->de_StartCluster, dirclust);
#endif
		nvp->v_type = VDIR;
		if (ldep->de_StartCluster != XTAFROOT) {
			error = xtaf_pcbmap(ldep, 0xffff, 0, &size, 0);
			if (error == E2BIG) {
				ldep->de_FileSize = de_cn2off(pmp, size);
				error = 0;
			} else
				printf("xtaf_deget(): xtaf_pcbmap returned "
				       "%d\n", error);
		}
	} else
		nvp->v_type = VREG;
	ldep->de_modrev = init_va_filerev();
	*depp = ldep;
	return (0);
}

int
xtaf_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);

#ifdef XTAF_DEBUG
	printf("xtaf_reclaim(): dep %p, file %s, refcnt %ld\n",
	    dep, dep->de_Name, dep->de_refcnt);
#endif

	/*
	 * Destroy the vm object and flush associated pages.
	 */
	vnode_destroy_vobject(vp);
	/*
	 * Remove the denode from its hash chain.
	 */
	vfs_hash_remove(vp);
	/*
	 * Purge old data structures associated with the denode.
	 */
	free(dep, M_XTAFNODE);
	vp->v_data = NULL;

	return (0);
}

int
xtaf_deupdat(struct denode *dep, int waitfor)
{
	int error;
	struct buf *bp;
	struct direntry *ep;
	struct timespec ts;

	if (DETOV(dep)->v_mount->mnt_flag & MNT_RDONLY)
		return (0);
	getnanotime(&ts);
	DETIMES(dep, &ts, &ts, &ts);
	if ((dep->de_flag & DE_MODIFIED) == 0)
		return (0);
	dep->de_flag &= ~DE_MODIFIED;
	if (dep->de_Attributes & ATTR_DIRECTORY)
		return (0);
	/*
	 * NOTE: The check for de_refcnt > 0 below ensures the denode being
	 * examined does not represent an unlinked but still open file.
	 * These files are not to be accessible even when the directory
	 * entry that represented the file happens to be reused while the
	 * deleted file is still open.
	*/
	if (dep->de_refcnt <= 0)
		return (0);
	error = xtaf_readde(dep, &bp, &ep);
	if (error)
		return (error);
	DE_EXTERNALIZE(ep, dep);
	if (waitfor)
		return (bwrite(bp));
	else {
		bdwrite(bp);
		return (0);
	}
}

/*
 * Truncate the file described by dep to the length specified by length.
 */
int
xtaf_detrunc(struct denode *dep, u_long length, int flags, struct ucred *cred,
	   struct thread *td)
{
	int error;
	int allerror;
	u_long eofentry;
	u_long chaintofree;
	daddr_t bn;
	int boff;
	int isadir = dep->de_Attributes & ATTR_DIRECTORY;
	struct buf *bp;
	struct xtafmount *pmp = dep->de_pmp;

#ifdef XTAF_DEBUG
	printf("xtaf_detrunc(): file %s, length %lu, flags %x\n", dep->de_Name,
	    length, flags);
#endif

	/*
	 * Disallow attempts to truncate the root directory since it is of
	 * fixed size.  That's just the way XTAF filesystems are.  We use
	 * the VROOT bit in the vnode because checking for the directory
	 * bit and a startcluster of 0 in the denode is not adequate to
	 * recognize the root directory at this point in a file or
	 * directory's life.
	 */
	if (DETOV(dep)->v_vflag & VV_ROOT) {
		printf("xtaf_detrunc(): can't truncate root directory, "
		    "clust %ld, offset %ld\n",
		    dep->de_dirclust, dep->de_diroffset);
		return (EINVAL);
	}

	if (dep->de_FileSize < length) {
		vnode_pager_setsize(DETOV(dep), length);
		return xtaf_deextend(dep, length, cred);
	}

	/*
	 * If the desired length is 0 then remember the starting cluster of
	 * the file and set the StartCluster field in the directory entry
	 * to 0.  If the desired length is not zero, then get the number of
	 * the last cluster in the shortened file.  Then get the number of
	 * the first cluster in the part of the file that is to be freed.
	 * Then set the next cluster pointer in the last cluster of the
	 * file to CLUST_EOFE.
	 */
	if (length == 0) {
		chaintofree = dep->de_StartCluster;
		dep->de_StartCluster = 0;
		eofentry = ~0;
	} else {
		error = xtaf_pcbmap(dep, de_clcount(pmp, length) - 1, 0,
			       &eofentry, 0);
		if (error) {
#ifdef XTAF_DEBUG
			printf("xtaf_detrunc(): xtaf_pcbmap fails %d\n", error);
#endif
			return (error);
		}
	}

	xtaf_fc_purge(dep, de_clcount(pmp, length));

	/*
	 * If the new length is not a multiple of the cluster size then we
	 * must zero the tail end of the new last cluster in case it
	 * becomes part of the file again because of a seek.
	 */
	if ((boff = length & pmp->pm_crbomask) != 0) {
		if (isadir) {
			bn = cntobn(pmp, eofentry);
			error = bread(pmp->pm_devvp, bn, pmp->pm_bpcluster,
			    NOCRED, &bp);
			if (error) {
				brelse(bp);
#ifdef XTAF_DEBUG
				printf("xtaf_detrunc(): bread fails %d\n",
				    error);
#endif
				return (error);
			}
			bzero(bp->b_data + boff, pmp->pm_bpcluster - boff);
			if (flags & IO_SYNC)
				bwrite(bp);
			else
				bdwrite(bp);
		}
	}

	/*
	 * Write out the updated directory entry.  Even if the update fails
	 * we free the trailing clusters.
	 */
	dep->de_FileSize = length;
	if (!isadir)
		dep->de_flag |= DE_UPDATE | DE_MODIFIED;
	allerror = vtruncbuf(DETOV(dep), cred, td, length, pmp->pm_bpcluster);
#ifdef XTAF_DEBUG
	if (allerror)
		printf("xtaf_detrunc(): vtruncbuf error %d\n", allerror);
#endif
	error = xtaf_deupdat(dep, !(DETOV(dep)->v_mount->mnt_flag & MNT_ASYNC));
	if (error != 0 && allerror == 0)
		allerror = error;
#ifdef XTAF_DEBUG
	printf("xtaf_detrunc(): allerror %d, eofentry %lu\n",
	       allerror, eofentry);
#endif

	/*
	 * If we need to break the cluster chain for the file then do it
	 * now.
	 */
	if (eofentry != ~0) {
		error = xtaf_fatentry(FAT_GET_AND_SET, pmp, eofentry,
				 &chaintofree, CLUST_EOFE);
		if (error) {
#ifdef XTAF_DEBUG
			printf("xtaf_detrunc(): xtaf_fatentry errors %d\n",
			    error);
#endif
			return (error);
		}
		fc_setcache(dep, FC_LASTFC, de_cluster(pmp, length - 1),
			    eofentry);
	}

	/*
	 * Now free the clusters removed from the file because of the
	 * truncation.
	 */
	if (chaintofree != 0 && !XTAFEOF(pmp, chaintofree))
		xtaf_freeclusterchain(pmp, chaintofree);

	return (allerror);
}

/*
 * Extend the file described by dep to length specified by length.
 */
int
xtaf_deextend(struct denode *dep, u_long length, struct ucred *cred)
{
	struct xtafmount *pmp = dep->de_pmp;
	u_long count;
	int error;

	/*
	 * The root of a XTAF filesystem cannot be extended.
	 */
	if (DETOV(dep)->v_vflag & VV_ROOT)
		return (EINVAL);

	/*
	 * Directories cannot be extended.
	 */
	if (dep->de_Attributes & ATTR_DIRECTORY)
		return (EISDIR);

	if (length <= dep->de_FileSize)
		panic("xtaf_deextend: file too large");

	/*
	 * Compute the number of clusters to allocate.
	 */
	count = de_clcount(pmp, length) - de_clcount(pmp, dep->de_FileSize);
	if (count > 0) {
		if (count > pmp->pm_freeclustercount)
			return (ENOSPC);
		error = xtaf_extendfile(dep, count, NULL, NULL, DE_CLEAR);
		if (error) {
			/* truncate the added clusters away again */
			(void) xtaf_detrunc(dep, dep->de_FileSize, 0, cred, NULL);
			return (error);
		}
	}
	dep->de_FileSize = length;
	dep->de_flag |= DE_UPDATE | DE_MODIFIED;
	return (xtaf_deupdat(dep, !(DETOV(dep)->v_mount->mnt_flag & MNT_ASYNC)));
}

/*
 * Move a denode to its correct hash queue after the file it represents has
 * been moved to a new directory.
 */
void
xtaf_reinsert(struct denode *dep)
{
	struct vnode *vp;

	/*
	 * Fix up the denode cache.  If the denode is for a directory,
	 * there is nothing to do since the hash is based on the starting
	 * cluster of the directory file and that hasn't changed.  If for a
	 * file the hash is based on the location of the directory entry,
	 * so we must remove it from the cache and re-enter it with the
	 * hash based on the new location of the directory entry.
	 */
	if (dep->de_Attributes & ATTR_DIRECTORY)
		return;
	vp = DETOV(dep);
	dep->de_inode = (uint32_t)dep->de_pmp->pm_bpcluster * dep->de_dirclust +
	    dep->de_diroffset;
	vfs_hash_rehash(vp, dep->de_inode);
}
