/* XXX legal */

// TODO ?? r204474  about msdosfs_deget_dotdot()
/* $FreeBSD: */
/*	$NetBSD: msdosfs_lookup.c,v 1.37 1997/11/17 15:36:54 ws Exp $	*/

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
#include <sys/namei.h>
#include <sys/vnode.h>

#include <sys/endian.h>

#include <fs/xtaf/direntry.h>
#include <fs/xtaf/denode.h>
#include <fs/xtaf/fat.h>
#include <fs/xtaf/xtafmount.h>

/*
 * Be sure a directory is empty even for "." and "..". Return 1 if empty,
 * return 0 if not empty or error.
 */
int
xtafdirempty(struct denode *dep)
{
	int blsize;
	int error;
	u_long cn;
	daddr_t bn;
	struct buf *bp;
	struct xtafmount *pmp = dep->de_pmp;
	struct direntry *dentp;

	/*
	 * Since the filesize field in directory entries for a directory is
	 * zero, we just have to feel our way through the directory until
	 * we hit the end of the directory.
	 */
	for (cn = 0;; cn++) {
		if ((error = xtaf_pcbmap(dep, cn, &bn, 0, &blsize)) != 0) {
			if (error == E2BIG)
				return (1);	/* it's empty */
			return (0);
		}
		error = bread(pmp->pm_devvp, bn, blsize, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return (0);
		}
		for (dentp = (struct direntry *)bp->b_data;
		     (char *)dentp < bp->b_data + blsize;
		     dentp++) {
			if (dentp->deLength != LEN_DELETED) {
				/*
				 * In XTAF directories an entry whose
				 * length is SLOT_EMPTY (0xff) starts the
				 * beginning of the unused part of the
				 * directory, so we can just return that it
				 * is empty.
				 */
				if (dentp->deLength == SLOT_EMPTY) {
					brelse(bp);
					return (1);
				}
				/*
				 * Any names in a directory mean
				 * it is not empty.
				 */
				if (dentp->deLength > 0) {
					brelse(bp);
#ifdef XTAF_DEBUG
					printf("xtafdirempty(): entry found %x (%x)\n",
					    dentp->deLength, dentp->deName[0]);
#endif
					return (0);	/* not empty */
				}
			}
		}
		brelse(bp);
	}
	/* NOTREACHED */
}

/*
 * Read in the disk block containing the directory entry (dirclu, dirofs)
 * and return the address of the buf header, and the address of the
 * directory entry within the block.
 */
int
xtaf_readep(struct xtafmount *pmp, u_long dirclust, u_long diroffset,
	    struct buf **bpp, struct direntry **epp)
{
	int error;
	daddr_t bn;
	int blsize;

	blsize = pmp->pm_bpcluster;
	if (dirclust == XTAFROOT
	    && de_blk(pmp, diroffset + blsize) > pmp->pm_rootdirsize)
		blsize = de_bn2off(pmp, pmp->pm_rootdirsize) & pmp->pm_crbomask;
	bn = detobn(pmp, dirclust, diroffset);
#ifdef XTAF_DEBUG
	printf("xtaf_readep(): dirclust=%lx diroffset=%lx --> (bn, byte) (direntry) = (%x,%x)\n", dirclust, diroffset, (uint32_t)bn, (uint32_t)bn*DEV_BSIZE);
#endif
	if ((error = bread(pmp->pm_devvp, bn, blsize, NOCRED, bpp)) != 0) {
		brelse(*bpp);
		*bpp = NULL;
		return (error);
	}
	if (epp)
		*epp = bptoep(pmp, *bpp, diroffset);
	return (0);
}

/*
 * Read in the disk block containing the directory entry dep came from and
 * return the address of the buf header, and the address of the directory
 * entry within the block.
 */
int
xtaf_readde(struct denode *dep, struct buf **bpp, struct direntry **epp)
{

	return (xtaf_readep(dep->de_pmp, dep->de_dirclust, dep->de_diroffset,
	    bpp, epp));
}

/*
 * dep  - directory entry to copy into the directory
 * ddep - directory to add to
 * depp - return the address of the denode for the created directory entry
 *	  if depp != 0
 */
int
xtaf_createde(struct denode *dep, struct denode *ddep, struct denode **depp,
	    struct componentname *cnp)
{
	int error;
	u_long dirclust, diroffset;
	struct direntry *ne;
	struct xtafmount *pmp = ddep->de_pmp;
	struct buf *bp;
	daddr_t bn;
	int blsize;

#ifdef XTAF_DEBUG
	printf("xtaf_createde(dep %p, ddep %p, depp %p, cnp %p)\n",
	    dep, ddep, depp, cnp);
#endif

	/*
	 * If no space left in the directory then allocate another cluster
	 * and chain it onto the end of the file.  There is one exception
	 * to this.  That is, if the root directory has no more space it
	 * can NOT be expanded.  xtaf_extendfile() checks for and fails
	 * attempts to extend the root directory.  We just return an error
	 * in that case.
	 */
	if (ddep->de_fndoffset >= ddep->de_FileSize) {
		diroffset = ddep->de_fndoffset + sizeof(struct direntry)
		    - ddep->de_FileSize;
		dirclust = de_clcount(pmp, diroffset);
		error = xtaf_extendfile(ddep, dirclust, 0, 0, DE_CLEAR);
		if (error) {
			(void)xtaf_detrunc(ddep, ddep->de_FileSize, 0, NOCRED,
			    NULL);
			return error;
		}

		/*
		 * Update the size of the directory
		 */
		ddep->de_FileSize += de_cn2off(pmp, dirclust);
	}

	/*
	 * We just read in the cluster with space.  Copy the new directory
	 * entry in.  Then write it to disk. NOTE:  XTAF directories
	 * do not get smaller as clusters are emptied.
	 */
	error = xtaf_pcbmap(ddep, de_cluster(pmp, ddep->de_fndoffset),
		       &bn, &dirclust, &blsize);
	if (error)
		return error;
	diroffset = ddep->de_fndoffset;
	if (dirclust != XTAFROOT)
		diroffset &= pmp->pm_crbomask;
	if ((error = bread(pmp->pm_devvp, bn, blsize, NOCRED, &bp)) != 0) {
		brelse(bp);
		return error;
	}
	ne = bptoep(pmp, bp, ddep->de_fndoffset);

	DE_EXTERNALIZE(ne, dep);

	if (DETOV(ddep)->v_mount->mnt_flag & MNT_ASYNC)
		bdwrite(bp);
	else if ((error = bwrite(bp)) != 0)
		return error;

	/*
	 * If they want us to return with the denode gotten.
	 */
	if (depp) {
		if (dep->de_Attributes & ATTR_DIRECTORY) {
			dirclust = dep->de_StartCluster;
			if (dirclust == XTAFROOT)
				diroffset = XTAFROOT_OFS;
			else
				diroffset = 0;
		}
		return xtaf_deget(pmp, dirclust, diroffset, depp);
	}

	return (0);
}

/*
 * Check to see if the directory described by target is in some
 * subdirectory of source.  This prevents something like the following from
 * succeeding and leaving a bunch or files and directories orphaned. mv
 * /a/b/c /a/b/c/d/e/f Where c and f are directories.
 *
 * source - the inode for /a/b/c
 * target - the inode for /a/b/c/d/e/f
 *
 * Returns 0 if target is NOT a subdirectory of source.
 * Otherwise returns a non-zero error number.
 * The target inode is always unlocked on return.
 */
int
xtafcheckpath(struct denode *source, struct denode *target)
{
	daddr_t scn;
	struct xtafmount *pmp;
	struct direntry *ep;
	struct denode *dep;
	struct buf *bp = NULL;
	int error = 0;

	dep = target;
	if ((target->de_Attributes & ATTR_DIRECTORY) == 0 ||
	    (source->de_Attributes & ATTR_DIRECTORY) == 0) {
		error = ENOTDIR;
		goto out;
	}
	if (dep->de_StartCluster == source->de_StartCluster) {
		error = EEXIST;
		goto out;
	}
	if (dep->de_StartCluster == XTAFROOT)
		goto out;
	pmp = dep->de_pmp;
#ifdef	DIAGNOSTIC
	if (pmp != source->de_pmp)
		panic("xtafcheckpath: source and target on different filesystems");
#endif

	for (;;) {
		if ((dep->de_Attributes & ATTR_DIRECTORY) == 0) {
			error = ENOTDIR;
			break;
		}
		scn = dep->de_StartCluster;
		error = bread(pmp->pm_devvp, cntobn(pmp, scn),
			      pmp->pm_bpcluster, NOCRED, &bp);
		if (error)
			break;

		ep = (struct direntry *) bp->b_data + 1; /* XXX wrong */
		scn = be32dec(ep->deStartCluster); /* XXX wrong */

		if (scn == source->de_StartCluster) {
			error = EINVAL;
			break;
		}
		if (scn == XTAFROOT)
			break;

		vput(DETOV(dep));
		brelse(bp);
		bp = NULL;
		/* NOTE: xtaf_deget() clears dep on error */
		if ((error = xtaf_deget(pmp, scn, 0, &dep)) != 0)
			break;
	}
out:;
	if (bp)
		brelse(bp);
	if (dep != NULL)
		vput(DETOV(dep));
	return (error);
}

/*
 * Remove a directory entry. At this point the file represented by the
 * directory entry to be removed is still full length until noone has it
 * open.  When the file no longer being used xtaf_inactive() is called
 * and will truncate the file to 0 length.  When the vnode containing the
 * denode is needed for some other purpose by VFS it will call
 * xtaf_reclaim() which will remove the denode from the denode cache.
 *
 * pdep : directory where the entry is removed
 * dep  : file to be removed
 */
int
xtaf_removede(struct denode *pdep, struct denode *dep)
{
	int error;
	struct direntry *ep;
	struct buf *bp;
	daddr_t bn;
	int blsize;
	struct xtafmount *pmp = pdep->de_pmp;
	u_long offset = pdep->de_fndoffset;

#ifdef XTAF_DEBUG
	printf("xtaf_removede(): filename %s, dep %p, offset %lx\n",
	    dep->de_Name, dep, offset);
#endif

	dep->de_refcnt--;
	offset += sizeof(struct direntry);
	do {
		offset -= sizeof(struct direntry);
		error = xtaf_pcbmap(pdep, de_cluster(pmp, offset), &bn, 0,
		    &blsize);
		if (error)
			return error;
		error = bread(pmp->pm_devvp, bn, blsize, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return error;
		}
		ep = bptoep(pmp, bp, offset);
		/*
		 * XXX Check whether, if we came here the second time, i.e.
		 * when underflowing into the previous block, the last
		 * entry in this block is a longfilename entry, too.
		 */
		if (offset != pdep->de_fndoffset) {
			brelse(bp);
			break;
		}
		ep--->deLength = LEN_DELETED;
		if (DETOV(pdep)->v_mount->mnt_flag & MNT_ASYNC)
			bdwrite(bp);
		else if ((error = bwrite(bp)) != 0)
			return error;
	} while (!(offset & pmp->pm_crbomask) && offset);
	return 0;
}

/*
 * Create a unique XTAF name in dvp
 */
int
uniqxtafname(struct componentname *cnp, u_char *cp)
{
	return (unix2xtaffn((const u_char *)cnp->cn_nameptr, cp,
	    cnp->cn_namelen, 0xff) ? 0 : EINVAL);
}
