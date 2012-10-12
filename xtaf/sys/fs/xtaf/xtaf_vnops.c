/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: msdosfs_vnops.c,v 1.68 1998/02/10 14:10:04 mrg Exp $	*/

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
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/clock.h>
#include <sys/dirent.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/malloc.h>
/* #include <sys/mount.h> */ /* used in msdosfs */
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <sys/endian.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#include <fs/xtaf/direntry.h>
#include <fs/xtaf/denode.h>
#include <fs/xtaf/fat.h>
#include <fs/xtaf/xtafmount.h>

#define	XTAF_FILESIZE_MAX	0xffffffff

/*
 * Prototypes for XTAF vnode operations
 */
static vop_open_t	xtaf_open;
static vop_close_t	xtaf_close;
static vop_access_t	xtaf_access;
static vop_getattr_t	xtaf_getattr;
static vop_inactive_t	xtaf_inactive;
static vop_read_t	xtaf_read;
static vop_readdir_t	xtaf_readdir;
static vop_bmap_t	xtaf_bmap;
static vop_cachedlookup_t	xtaf_lookup;
static vop_strategy_t	xtaf_strategy;
static vop_pathconf_t	xtaf_pathconf;
static vop_create_t	xtaf_create;
static vop_fsync_t	xtaf_fsync;
static vop_mkdir_t	xtaf_mkdir;
static vop_remove_t	xtaf_remove;
static vop_rename_t	xtaf_rename;
static vop_rmdir_t	xtaf_rmdir;
static vop_setattr_t	xtaf_setattr;
static vop_write_t	xtaf_write;
static vop_vptofh_t	xtaf_vptofh;

/*
 * Some general notes:
 *
 * In the ufs filesystem the inodes, superblocks, and indirect blocks are
 * read/written using the vnode for the filesystem. Blocks that represent
 * the contents of a file are read/written using the vnode for the file
 * (including directories when they are read/written as files). This
 * presents problems for the XTAF filesystem because data that should be in
 * an inode (if XTAF had them) resides in the directory itself.  Since we
 * must update directory entries without the benefit of having the vnode
 * for the directory we must use the vnode for the filesystem.  This means
 * that when a directory is actually read/written (via read, write, or
 * readdir, or seek) we must use the vnode for the filesystem instead of
 * the vnode for the directory as would happen in ufs. This is to insure we
 * retreive the correct block from the buffer cache since the hash value is
 * based upon the vnode address and the desired block number.
 */

static int
xtaf_open(struct vop_open_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);
	vnode_create_vobject(ap->a_vp, dep->de_FileSize, ap->a_td);
	return (0);
}

static int
xtaf_close(struct vop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);
	struct timespec ts;

	VI_LOCK(vp);
	if (vp->v_usecount > 1) {
		getnanotime(&ts);
		DETIMES(dep, &ts, &ts, &ts);
	}
	VI_UNLOCK(vp);
	return (0);
}

static int
xtaf_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(ap->a_vp);
	struct xtafmount *pmp = dep->de_pmp;
	mode_t file_mode;
	accmode_t accmode = ap->a_accmode;

	file_mode = (S_IXUSR|S_IXGRP|S_IXOTH) | (S_IRUSR|S_IRGRP|S_IROTH) |
	    ((dep->de_Attributes & ATTR_READONLY) ? 0 :
	    (S_IWUSR|S_IWGRP|S_IWOTH));
	file_mode &= (vp->v_type == VDIR ? pmp->pm_dirmask : pmp->pm_mask);

	/*
	 * Disallow writing to directories and regular files if the
	 * filesystem is read-only.
	 */
	if (accmode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}

	return (vaccess(vp->v_type, file_mode, pmp->pm_uid, pmp->pm_gid,
	    ap->a_accmode, ap->a_cred, NULL));
}

static int
xtaf_getattr(struct vop_getattr_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);
	struct xtafmount *pmp = dep->de_pmp;
	struct vattr *vap = ap->a_vap;
	mode_t mode;
	struct timespec ts;
	u_long dirsperblk = 512 / sizeof(struct direntry);
	uint32_t fileid;

	getnanotime(&ts);
	DETIMES(dep, &ts, &ts, &ts);
	vap->va_fsid = dev2udev(pmp->pm_dev);
#ifdef XTAF_DEBUG
	printf("xtaf_getattr: filename = %s\n", dep->de_Name);
#endif
	/*
	 * The following computation of the fileid must be the same as that
	 * used in xtaf_readdir() to compute d_fileno. If not, pwd
	 * doesn't work.
	 */
	if (dep->de_Attributes & ATTR_DIRECTORY) {
		if (dep->de_StartCluster == XTAFROOT) {
			fileid = 1;
#ifdef XTAF_DEBUG
			printf("xtaf_getattr(): dep->de_StartCluster == XTAFROOT, ok if in rootdir\n");
#endif
		} else
			fileid = (uint32_t)cntobn(pmp, dep->de_StartCluster) *
			    dirsperblk;
	} else {
		if (dep->de_dirclust == XTAFROOT)
			fileid = (uint32_t)roottobn(pmp, 0) * dirsperblk;
			/* empty file */
		else
			fileid = (uint32_t)cntobn(pmp, dep->de_dirclust) *
			    dirsperblk;
		fileid += (uint32_t)dep->de_diroffset / sizeof(struct direntry);
	}
	vap->va_fileid = (long)fileid;
#ifdef XTAF_DEBUG
	printf("xtaf_getattr(): fileid=%x\n", fileid);
#endif
	if (dep->de_Attributes & ATTR_READONLY)
		mode = S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH;
	else
		mode = S_IRWXU|S_IRWXG|S_IRWXO;
	vap->va_mode = mode &
	    (ap->a_vp->v_type == VDIR ? pmp->pm_dirmask : pmp->pm_mask);
	vap->va_uid = pmp->pm_uid;
	vap->va_gid = pmp->pm_gid;
	vap->va_nlink = 1;
	vap->va_rdev = NODEV;
	vap->va_size = dep->de_FileSize;

	fattime2timespec(dep->de_CDate, dep->de_CTime, 0, 0, &vap->va_birthtime);
	fattime2timespec(dep->de_ADate, dep->de_ATime, 0, 0, &vap->va_atime);
	fattime2timespec(dep->de_MDate, dep->de_MTime, 0, 0, &vap->va_mtime);
	vap->va_ctime = vap->va_mtime; /* XTAF has no inode change time */
	vap->va_flags = 0;
	if ((dep->de_Attributes & ATTR_DIRECTORY) ||
	    ((dep->de_Attributes & ATTR_ARCHIVE) == 0))
		vap->va_flags |= SF_ARCHIVED;
	if (dep->de_Attributes & ATTR_SYSTEM) {
#if XTAF_DEBUG
		printf("xtaf_getattr(): setting SF_IMMUTABLE for %s\n",dep->de_Name);
#endif
		vap->va_flags |= SF_IMMUTABLE; /* best flag match ? */
	}
	/* We're not going to support the hidden flag, since that would require
	 * to prefix the filename with a dot.  The volume flag will not be
	 * supported either, since it has no UNIX equivalence and XTAF does not
	 * use it anyway.  The file /name.txt contains the media name in 
	 * big endian UTF-16.
	 */

	vap->va_gen = 0;
	vap->va_blocksize = pmp->pm_bpcluster;
	vap->va_bytes =
	    (dep->de_FileSize + pmp->pm_crbomask) & ~pmp->pm_crbomask;
	vap->va_type = ap->a_vp->v_type;
	vap->va_filerev = dep->de_modrev;
	return (0);
}

int
xtaf_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);
	struct thread *td = ap->a_td;
	int error = 0;

	/*
	 * Ignore denodes related to stale file handles.
	 */
	if (dep->de_Length == LEN_DELETED)
		goto out;

	/*
	 * If the file has been deleted and it is on a read/write
	 * filesystem, then truncate the file, and mark the directory slot
	 * as empty.
	 */
	if (dep->de_refcnt <= 0 && (vp->v_mount->mnt_flag & MNT_RDONLY) == 0) {
		error = xtaf_detrunc(dep, (u_long) 0, 0, NOCRED, td);
		dep->de_flag |= DE_UPDATE;
		dep->de_Length = LEN_DELETED;
	}
	xtaf_deupdat(dep, 0);

out:
	/*
	 * If we are done with the denode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (dep->de_Length == LEN_DELETED)
		vrecycle(vp, td);
	return (error);
}

static int
xtaf_read(struct vop_read_args *ap)
{
	int error = 0;
	int blsize;
	int isadir;
	int orig_resid;
	u_int n;
	u_long diff;
	u_long on;
	daddr_t lbn;
	daddr_t rablock;
	int rasize;
	int seqcount;
	struct buf *bp;
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);
	struct xtafmount *pmp = dep->de_pmp;
	struct uio *uio = ap->a_uio;

	/*
	 * If they didn't ask for any data, then we are done.
	 */
	orig_resid = uio->uio_resid;
	if (orig_resid == 0)
		return (0);

	/*
	 * The caller is supposed to ensure that
	 * uio->uio_offset >= 0 and uio->uio_resid >= 0.
	 * We don't need to check for large offsets as in ffs because
	 * dep->de_FileSize <= XTAF_FILESIZE_MAX < OFF_MAX, so large
	 * offsets cannot cause overflow even in theory.
	 */

	seqcount = ap->a_ioflag >> IO_SEQSHIFT;

	isadir = dep->de_Attributes & ATTR_DIRECTORY;
#ifdef XTAF_DEBUG
	printf("xtaf_read(): isadir = %i\n", isadir);
#endif
	do {
		if (uio->uio_offset >= dep->de_FileSize)
			break;
		lbn = de_cluster(pmp, uio->uio_offset);
		rablock = lbn + 1;
		blsize = pmp->pm_bpcluster;
		on = uio->uio_offset & pmp->pm_crbomask;
		/*
		 * If we are operating on a directory file then be sure to
		 * do i/o with the vnode for the filesystem instead of the
		 * vnode for the directory.
		 */
		if (isadir) { /* XXX notreached? */
			/* convert cluster # to block # */
#ifdef XTAF_DEBUG
			printf("xtaf_read(): lbn (in) = %x\n", (uint32_t)lbn);
#endif
			error = xtaf_pcbmap(dep, lbn, &lbn, NULL, &blsize);
#ifdef XTAF_DEBUG
			printf("xtaf_read(): lbn (out) = %x error=%i\n", (uint32_t)lbn, error);
#endif
			if (error == E2BIG)
				error = EINVAL;
			if (error)
				break;
			error = bread(pmp->pm_devvp, lbn, blsize, NOCRED, &bp);
		} else if (de_cn2off(pmp, rablock) >= dep->de_FileSize) {
			error = bread(vp, lbn, blsize, NOCRED, &bp);
		} else if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERR) == 0) {
			error = cluster_read(vp, dep->de_FileSize, lbn, blsize,
			    NOCRED, on + uio->uio_resid, seqcount, &bp);
		} else if (seqcount > 1) {
			rasize = blsize;
			error = breadn(vp, lbn, blsize, &rablock, &rasize, 1,
			    NOCRED, &bp);
		} else {
				error = bread(vp, lbn, blsize, NOCRED, &bp);
		}
		if (error) {
			brelse(bp);
			break;
		}
		on = uio->uio_offset & pmp->pm_crbomask;
		diff = pmp->pm_bpcluster - on;
		n = diff > uio->uio_resid ? uio->uio_resid : diff;
		diff = dep->de_FileSize - uio->uio_offset;
		if (diff < n)
			n = diff;
		diff = blsize - bp->b_resid;
		if (diff < n)
			n = diff;
		error = uiomove(bp->b_data + on, (int) n, uio);
		brelse(bp);
	} while (error == 0 && uio->uio_resid > 0 && n != 0);
	if (!isadir && (error == 0 || uio->uio_resid != orig_resid) &&
	    (vp->v_mount->mnt_flag & MNT_NOATIME) == 0)
		dep->de_flag |= DE_ACCESS;
	return (error);
}

/*
 * Create a regular file. On entry the directory to contain the file being
 * created is locked.  We must release before we return. We must also free
 * the pathname buffer pointed at by cnp->cn_pnbuf, always on error, or
 * only if the SAVESTART bit in cn_flags is clear on success.
 */
static int
xtaf_create(struct vop_create_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	struct denode nde;
	struct denode *dep;
	struct denode *pdep = VTODE(ap->a_dvp);
	struct timespec ts;
	int error;

#ifdef XTAF_DEBUG
	printf("xtaf_create(cnp %p, vap %p\n", cnp, ap->a_vap);
#endif

	/*
	 * If this is the root directory and there is no space left we
	 * can't do anything.  This is because the root directory can not
	 * change size.
	 */
	if (pdep->de_StartCluster == XTAFROOT
	    && pdep->de_fndoffset >= pdep->de_FileSize) {
		error = ENOSPC;
		goto bad;
	}

	/*
	 * Create a directory entry for the file, then call xtaf_createde()
	 * to have it installed.  We use the absence of the owner write bit
	 * to make the file readonly.
	 */
#ifdef DIAGNOSTIC
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("xtaf_create: no name");
#endif
	bzero(&nde, sizeof(nde));
	error = uniqxtafname(cnp, nde.de_Name);
	if (error)
		goto bad;

	nde.de_Attributes = (ap->a_vap->va_mode & VWRITE) ?
				ATTR_ARCHIVE : ATTR_ARCHIVE | ATTR_READONLY;
	nde.de_StartCluster = 0;
	nde.de_FileSize = 0;
	nde.de_pmp = pdep->de_pmp;
	nde.de_flag = DE_ACCESS | DE_CREATE | DE_UPDATE;
	getnanotime(&ts);
	DETIMES(&nde, &ts, &ts, &ts);
	error = xtaf_createde(&nde, pdep, &dep, cnp);
	if (error)
		goto bad;
	*ap->a_vpp = DETOV(dep);
	return (0);
bad:
	return (error);
}

static int
xtaf_setattr(struct vop_setattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(ap->a_vp);
	struct xtafmount *pmp = dep->de_pmp;
	struct vattr *vap = ap->a_vap;
	struct ucred *cred = ap->a_cred;
	struct thread *td = curthread;
	int error = 0;

#ifdef XTAF_DEBUG
	printf("xtaf_setattr(): vp %p, vap %p, cred %p\n",
	    ap->a_vp, vap, cred);
#endif

	/*
	 * Check for unsettable attributes.
	 */
	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    (vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL)) {
#ifdef XTAF_DEBUG
		printf("xtaf_setattr(): returning EINVAL\n");
		printf("    va_type %d, va_nlink %x, va_fsid %x, va_fileid %lx\n",
		    vap->va_type, vap->va_nlink, vap->va_fsid, vap->va_fileid);
		printf("    va_blocksize %lx, va_rdev %x, va_gen %lx\n",
		    vap->va_blocksize, vap->va_rdev, vap->va_gen);
		printf("    va_uid %x, va_gid %x\n",
		    vap->va_uid, vap->va_gid);
#endif
		return (EINVAL);
	}
	if (vap->va_flags != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (cred->cr_uid != pmp->pm_uid) {
			error = priv_check_cred(cred, PRIV_VFS_ADMIN, 0);
			if (error)
				return (error);
		}
		/*
		 * We are very inconsistent about handling unsupported
		 * attributes.  We ignored the access time and the
		 * read and execute bits.  We were strict for the other
		 * attributes.
		 *
		 * Here we are strict, stricter than ufs in not allowing
		 * users to attempt to set SF_SETTABLE bits or anyone to
		 * set unsupported bits.  However, we ignore attempts to
		 * set ATTR_ARCHIVE for directories `cp -pr' from a more
		 * sensible filesystem attempts it a lot.
		 */
		if (vap->va_flags & SF_SETTABLE) {
			error = priv_check_cred(cred, PRIV_VFS_SYSFLAGS, 0);
			if (error)
			    return (error);
		}
		/* bde 2006-12-05 */
		if (dep->de_Attributes & ATTR_DIRECTORY) {
			if (vap->va_flags & SF_ARCHIVED)
				return (EOPNOTSUPP);
		} else {
			if (vap->va_flags & SF_ARCHIVED)
				dep->de_Attributes &= ~ATTR_ARCHIVE;
			else
				dep->de_Attributes |= ATTR_ARCHIVE;
			dep->de_flag |= DE_MODIFIED;
		}
	}

	if (vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL) {
		uid_t uid;
		gid_t gid;

		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		uid = vap->va_uid;
		if (uid == (uid_t)VNOVAL)
			uid = pmp->pm_uid;
		gid = vap->va_gid;
		if (gid == (gid_t)VNOVAL)
			gid = pmp->pm_gid;
		if (cred->cr_uid != pmp->pm_uid || uid != pmp->pm_uid ||
		    (gid != pmp->pm_gid && !groupmember(gid, cred))) {
			error = priv_check_cred(cred, PRIV_VFS_CHOWN, 0);
			if (error)
			    return (error);
		}
		if (uid != pmp->pm_uid || gid != pmp->pm_gid)
			return EINVAL;
	}

	if (vap->va_size != VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VREG:
			/*
			 * Truncation is only supported for regular files,
			 * disallow it if the filesystem is read-only.
			 */
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			/*
			 * According to POSIX, the result is unspecified
			 * for file types other than regular files,
			 * directories and shared memory objects.  We
			 * don't support any file types except regular
			 * files and directories in this file system, so
			 * this (default) case is unreachable and can do
			 * anything.  Keep falling through to xtaf_detrunc()
			 * for now.
			 */
			break;
		}
		error = xtaf_detrunc(dep, vap->va_size, 0, cred, td);
		if (error)
			return (error);
	}
	if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (vap->va_vaflags & VA_UTIMES_NULL) {
			error = VOP_ACCESS(vp, VADMIN, cred, td);
			if (error)
				error = VOP_ACCESS(vp, VWRITE, cred, td);
		} else
			error = VOP_ACCESS(vp, VADMIN, cred, td);
		if (vp->v_type != VDIR) {
			if (vap->va_atime.tv_sec != VNOVAL) {
				dep->de_flag &= ~DE_ACCESS;
				timespec2fattime(&vap->va_atime, 0,
				    &dep->de_ADate, &dep->de_ATime, NULL);
			}
			if (vap->va_mtime.tv_sec != VNOVAL) {
				dep->de_flag &= ~DE_UPDATE;
				timespec2fattime(&vap->va_mtime, 0,
				    &dep->de_MDate, &dep->de_MTime, NULL);
			}
			dep->de_Attributes |= ATTR_ARCHIVE;
			dep->de_flag |= DE_MODIFIED;
		}
	}
	/*
	 * XTAF files only have the ability to have their writability
	 * attribute set, so we use the owner write bit to set the readonly
	 * attribute.
	 */
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (cred->cr_uid != pmp->pm_uid) {
			error = priv_check_cred(cred, PRIV_VFS_ADMIN, 0);
			if (error)
				return (error);
		}
		if (vp->v_type != VDIR) {
			/* We ignore the read and execute bits. */
			if (vap->va_mode & VWRITE)
				dep->de_Attributes &= ~ATTR_READONLY;
			else
				dep->de_Attributes |= ATTR_READONLY;
			dep->de_Attributes |= ATTR_ARCHIVE;
			dep->de_flag |= DE_MODIFIED;
		}
	}
	return (xtaf_deupdat(dep, 0));
}

/*
 * Write data to a file or directory.
 */
static int
xtaf_write(struct vop_write_args *ap)
{
	int n;
	int croffset;
	int resid;
	u_long osize;
	int error = 0;
	u_long count;
	int seqcount;
	daddr_t bn, lastcn;
	struct buf *bp;
	int ioflag = ap->a_ioflag;
	struct uio *uio = ap->a_uio;
	struct thread *td = uio->uio_td;
	struct vnode *vp = ap->a_vp;
	struct vnode *thisvp;
	struct denode *dep = VTODE(vp);
	struct xtafmount *pmp = dep->de_pmp;
	struct ucred *cred = ap->a_cred;

#ifdef XTAF_DEBUG
	printf("xtaf_write(vp %p, uio %p, ioflag %x, cred %p\n",
	    vp, uio, ioflag, cred);
	printf("xtaf_write(): diroff %lx, dirclust %lx, startcluster %lx\n",
	    dep->de_diroffset, dep->de_dirclust, dep->de_StartCluster);
#endif

	switch (vp->v_type) {
	case VREG:
		if (ioflag & IO_APPEND)
			uio->uio_offset = dep->de_FileSize;
		thisvp = vp;
		break;
	case VDIR:
		return EISDIR;
	default:
		panic("xtaf_write(): bad file type");
	}

	/*
	 * This is needed (unlike in ffs_write()) because we extend the
	 * file outside of the loop but we don't want to extend the file
	 * for writes of 0 bytes.
	 */
	if (uio->uio_resid == 0)
		return (0);

	/*
	 * The caller is supposed to ensure that
	 * uio->uio_offset >= 0 and uio->uio_resid >= 0.
	 */
	if ((uoff_t)uio->uio_offset + uio->uio_resid > XTAF_FILESIZE_MAX)
		return (EFBIG);

	/*
	 * If they've exceeded their filesize limit, tell them about it.
	 */
	if (td != NULL) {
		PROC_LOCK(td->td_proc);
		if ((uoff_t)uio->uio_offset + uio->uio_resid >
		    lim_cur(td->td_proc, RLIMIT_FSIZE)) {
			psignal(td->td_proc, SIGXFSZ);
			PROC_UNLOCK(td->td_proc);
			return (EFBIG);
		}
		PROC_UNLOCK(td->td_proc);
	}

	/*
	 * If the offset we are starting the write at is beyond the end of
	 * the file, then they've done a seek.  Unix filesystems allow
	 * files with holes in them, XTAF doesn't so we must fill the hole
	 * with zeroed blocks.
	 */
	if (uio->uio_offset > dep->de_FileSize) {
		error = xtaf_deextend(dep, uio->uio_offset, cred);
		if (error)
			return (error);
	}

	/*
	 * Remember some values in case the write fails.
	 */
	resid = uio->uio_resid;
	osize = dep->de_FileSize;

	/*
	 * If we write beyond the end of the file, extend it to its ultimate
	 * size ahead of the time to hopefully get a contiguous area.
	 */
	if (uio->uio_offset + resid > osize) {
		count = de_clcount(pmp, uio->uio_offset + resid) -
			de_clcount(pmp, osize);
		error = xtaf_extendfile(dep, count, NULL, NULL, 0);
		if (error &&  (error != ENOSPC || (ioflag & IO_UNIT)))
			goto errexit;
		lastcn = dep->de_fc[FC_LASTFC].fc_frcn;
	} else
		lastcn = de_clcount(pmp, osize) - 1;

	seqcount = ioflag >> IO_SEQSHIFT;
	do {
		if (de_cluster(pmp, uio->uio_offset) > lastcn) {
			error = ENOSPC;
			break;
		}

		croffset = uio->uio_offset & pmp->pm_crbomask;
		n = min(uio->uio_resid, pmp->pm_bpcluster - croffset);
		if (uio->uio_offset + n > dep->de_FileSize) {
			dep->de_FileSize = uio->uio_offset + n;
			/* The object size needs to be set before buffer is
			 * allocated */
			vnode_pager_setsize(vp, dep->de_FileSize);
		}

		bn = de_cluster(pmp, uio->uio_offset);
		if ((uio->uio_offset & pmp->pm_crbomask) == 0
		    && (de_cluster(pmp, uio->uio_offset + uio->uio_resid)
			> de_cluster(pmp, uio->uio_offset)
			|| uio->uio_offset + uio->uio_resid >= dep->de_FileSize)) {
			/*
			 * If either the whole cluster gets written,
			 * or we write the cluster from its start beyond EOF,
			 * then no need to read data from disk.
			 */
			bp = getblk(thisvp, bn, pmp->pm_bpcluster, 0, 0, 0);
			vfs_bio_clrbuf(bp);
			/*
			 * Do the bmap now, since xtaf_pcbmap needs buffers
			 * for the fat table. (see xtaf_strategy)
			 */
			if (bp->b_blkno == bp->b_lblkno) {
				error = xtaf_pcbmap(dep, bp->b_lblkno, &bn, 0,
				    0);
				if (error)
					bp->b_blkno = -1;
				else
					bp->b_blkno = bn;
			}
			if (bp->b_blkno == -1) {
				brelse(bp);
				if (!error)
					error = EIO;		/* XXX */
				break;
			}
		} else {
			/*
			 * The block we need to write into exists,
			 * so read it in.
			 */
			error = bread(thisvp, bn, pmp->pm_bpcluster, cred, &bp);
			if (error) {
				brelse(bp);
				break;
			}
		}

		/*
		 * Should these vnode_pager_* functions be done on dir
		 * files?
		 */

		/*
		 * Copy the data from user space into the buf header.
		 */
		error = uiomove(bp->b_data + croffset, n, uio);
		if (error) {
			brelse(bp);
			break;
		}

		/* Prepare for clustered writes in some else clauses. */
		if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERW) == 0)
			bp->b_flags |= B_CLUSTEROK;

		/*
		 * If IO_SYNC, then each buffer is written synchronously.
		 * Otherwise, if we have a severe page deficiency then
		 * write the buffer asynchronously.  Otherwise, if on a
		 * cluster boundary then write the buffer asynchronously,
		 * combining it with contiguous clusters if permitted and
		 * possible, since we don't expect more writes into this
		 * buffer soon.  Otherwise, do a delayed write because we
		 * expect more writes into this buffer soon.
		 */
		if (ioflag & IO_SYNC)
			(void) bwrite(bp);
		else if (vm_page_count_severe() || buf_dirty_count_severe())
			bawrite(bp);
		else if (n + croffset == pmp->pm_bpcluster) {
			if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERW) == 0)
				cluster_write(vp, bp, dep->de_FileSize,
				    seqcount);
			else
				bawrite(bp);
		} else
			bdwrite(bp);
		dep->de_flag |= DE_UPDATE;
	} while (error == 0 && uio->uio_resid > 0);

	/*
	 * If the write failed and they want us to, truncate the file back
	 * to the size it was before the write was attempted.
	 */
errexit:
	if (error) {
		if (ioflag & IO_UNIT) {
			xtaf_detrunc(dep, osize, ioflag & IO_SYNC, NOCRED, NULL);
			uio->uio_offset -= resid - uio->uio_resid;
			uio->uio_resid = resid;
		} else {
			xtaf_detrunc(dep, dep->de_FileSize, ioflag & IO_SYNC, NOCRED, NULL);
			if (uio->uio_resid != resid)
				error = 0;
		}
	} else if (ioflag & IO_SYNC)
		error = xtaf_deupdat(dep, 1);
	return (error);
}

/*
 * Flush the blocks of a file to disk.
 *
 * This function is worthless for vnodes that represent directories. Maybe we
 * could just do a sync if they try an fsync on a directory file.
 */
static int
xtaf_fsync(struct vop_fsync_args *ap)
{
	vop_stdfsync(ap);

	return (xtaf_deupdat(VTODE(ap->a_vp), ap->a_waitfor == MNT_WAIT));
}

static int
xtaf_remove(struct vop_remove_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);
	struct denode *ddep = VTODE(ap->a_dvp);
	int error;

	if (ap->a_vp->v_type == VDIR)
		error = EPERM;
	else
		error = xtaf_removede(ddep, dep);
#ifdef XTAF_DEBUG
	printf("xtaf_remove(), dep %p, v_usecount %d\n", dep, ap->a_vp->v_usecount);
#endif
	return (error);
}

/*
 * Renames on files require moving the denode to a new hash queue since the
 * denode's location is used to compute which hash queue to put the file
 * in. Unless it is a rename in place.  For example "mv a b".
 *
 * What follows is the basic algorithm:
 *
 * if (file move) {
 *	if (dest file exists) {
 *		remove dest file
 *	}
 *	if (dest and src in same directory) {
 *		rewrite name in existing directory slot
 *	} else {
 *		write new entry in dest directory
 *		update offset and dirclust in denode
 *		move denode to new hash chain
 *		clear old directory entry
 *	}
 * } else {
 *	directory move
 *	if (dest directory exists) {
 *		if (dest is not empty) {
 *			return ENOTEMPTY
 *		}
 *		remove dest directory
 *	}
 *	if (dest and src in same directory) {
 *		rewrite name in existing entry
 *	} else {
 *		be sure dest is not a child of src directory
 *		write entry in dest directory
 *		update "." and ".." in moved directory
 *		clear old directory entry for moved directory
 *	}
 * }
 *
 * On entry:
 *	source's parent directory is unlocked
 *	source file or directory is unlocked
 *	destination's parent directory is locked
 *	destination file or directory is locked if it exists
 *
 * On exit:
 *	all denodes should be released
 */
static int
xtaf_rename(struct vop_rename_args *ap)
{
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *tvp = ap->a_tvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct denode *ip, *xp, *dp, *zp;
	u_char toname[42], oldname[42];
	u_long from_diroffset, to_diroffset;
	u_char to_count;
	int doingdirectory = 0, newparent = 0;
	int error;
	struct denode *fddep;	/* from file's parent directory	 */
	struct xtafmount *pmp;

	fddep = VTODE(ap->a_fdvp);
	pmp = fddep->de_pmp;

	pmp = VFSTOXTAF(fdvp->v_mount);

#ifdef DIAGNOSTIC
	if ((tcnp->cn_flags & HASBUF) == 0 ||
	    (fcnp->cn_flags & HASBUF) == 0)
		panic("xtaf_rename: no name");
#endif
	/*
	 * Check for cross-device rename.
	 */
	if (fvp->v_mount != tdvp->v_mount ||
	    (tvp && fvp->v_mount != tvp->v_mount)) {
		error = EXDEV;
abortit:
		if (tdvp == tvp)
			vrele(tdvp);
		else
			vput(tdvp);
		if (tvp)
			vput(tvp);
		vrele(fdvp);
		vrele(fvp);
		return (error);
	}

	/*
	 * If source and dest are the same, do nothing.
	 */
	if (tvp == fvp) {
		error = 0;
		goto abortit;
	}

	error = vn_lock(fvp, LK_EXCLUSIVE);
	if (error)
		goto abortit;
	dp = VTODE(fdvp);
	ip = VTODE(fvp);

	/*
	 * Be sure we are not renaming ".", "..", or an alias of ".". This
	 * leads to a crippled directory tree.  It's pretty tough to do a
	 * "ls" or "pwd" with the "." directory entry missing, and "cd .."
	 * doesn't work if the ".." entry is missing.
	 */
	if (ip->de_Attributes & ATTR_DIRECTORY) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
		    dp == ip ||
		    (fcnp->cn_flags & ISDOTDOT) ||
		    (tcnp->cn_flags & ISDOTDOT) ||
		    (ip->de_flag & DE_RENAME)) {
			VOP_UNLOCK(fvp, 0);
			error = EINVAL;
			goto abortit;
		}
		ip->de_flag |= DE_RENAME;
		doingdirectory++;
	}

	/*
	 * When the target exists, both the directory
	 * and target vnodes are returned locked.
	 */
	dp = VTODE(tdvp);
	xp = tvp ? VTODE(tvp) : NULL;
	/*
	 * Remember direntry place to use for destination
	 */
	to_diroffset = dp->de_fndoffset;
	to_count = dp->de_fndcnt;

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory hierarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..". We must repeat the call
	 * to namei, as the parent directory is unlocked by the
	 * call to xtafcheckpath().
	 */
	error = VOP_ACCESS(fvp, VWRITE, tcnp->cn_cred, tcnp->cn_thread);
	VOP_UNLOCK(fvp, 0);
	if (VTODE(fdvp)->de_StartCluster != VTODE(tdvp)->de_StartCluster)
		newparent = 1;
	if (doingdirectory && newparent) {
		if (error)	/* write access check above */
			goto bad;
		if (xp != NULL)
			vput(tvp);
		/*
		 * xtafcheckpath() vput()'s dp,
		 * so we have to do a relookup afterwards
		 */
		error = xtafcheckpath(ip, dp);
		if (error)
			goto out;
		if ((tcnp->cn_flags & SAVESTART) == 0)
			panic("xtaf_rename: lost to startdir");
		error = relookup(tdvp, &tvp, tcnp);
		if (error)
			goto out;
		dp = VTODE(tdvp);
		xp = tvp ? VTODE(tvp) : NULL;
	}

	if (xp != NULL) {
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if (xp->de_Attributes & ATTR_DIRECTORY) {
			if (!xtafdirempty(xp)) {
				error = ENOTEMPTY;
				goto bad;
			}
			if (!doingdirectory) {
				error = ENOTDIR;
				goto bad;
			}
			cache_purge(tdvp);
		} else if (doingdirectory) {
			error = EISDIR;
			goto bad;
		}
		error = xtaf_removede(dp, xp);
		if (error)
			goto bad;
		vput(tvp);
		xp = NULL;
	}

	/*
	 * Convert the filename in tcnp into a XTAF filename. We copy this
	 * into the denode and directory entry for the destination
	 * file/directory.
	 */
	error = uniqxtafname(tcnp, toname);
	if (error)
		goto abortit;

	/*
	 * Since from wasn't locked at various places above,
	 * have to do a relookup here.
	 */
	fcnp->cn_flags &= ~MODMASK;
	fcnp->cn_flags |= LOCKPARENT | LOCKLEAF;
	if ((fcnp->cn_flags & SAVESTART) == 0)
		panic("xtaf_rename: lost from startdir");
	if (!newparent)
		VOP_UNLOCK(tdvp, 0);
	if (relookup(fdvp, &fvp, fcnp) == 0)
		vrele(fdvp);
	if (fvp == NULL) {
		/*
		 * From name has disappeared.
		 */
		if (doingdirectory)
			panic("xtaf_rename: lost dir entry");
		if (newparent)
			VOP_UNLOCK(tdvp, 0);
		vrele(tdvp);
		vrele(ap->a_fvp);
		return 0;
	}
	xp = VTODE(fvp);
	zp = VTODE(fdvp);
	from_diroffset = zp->de_fndoffset;

	/*
	 * Ensure that the directory entry still exists and has not
	 * changed till now. If the source is a file the entry may
	 * have been unlinked or renamed. In either case there is
	 * no further work to be done. If the source is a directory
	 * then it cannot have been rmdir'ed or renamed; this is
	 * prohibited by the DE_RENAME flag.
	 */
	if (xp != ip) {
		if (doingdirectory)
			panic("xtaf_rename: lost dir entry");
		VOP_UNLOCK(fvp, 0);
		if (newparent)
			VOP_UNLOCK(fdvp, 0);
		vrele(ap->a_fvp);
		xp = NULL;
	} else {
		vrele(fvp);
		xp = NULL;

		/*
		 * First write a new entry in the destination
		 * directory and mark the entry in the source directory
		 * as deleted.  Then move the denode to the correct hash
		 * chain for its new location in the filesystem.  And, if
		 * we moved a directory, then update its .. entry to point
		 * to the new parent directory.
		 */
		bcopy(ip->de_Name, oldname, 42);
		bcopy(toname, ip->de_Name, 42);	/* update denode */
		dp->de_fndoffset = to_diroffset;
		dp->de_fndcnt = to_count;
		error = xtaf_createde(ip, dp, (struct denode **)0, tcnp);
		if (error) {
			bcopy(oldname, ip->de_Name, 42);
			if (newparent)
				VOP_UNLOCK(fdvp, 0);
			VOP_UNLOCK(fvp, 0);
			goto bad;
		}
		ip->de_refcnt++;
		zp->de_fndoffset = from_diroffset;
		error = xtaf_removede(zp, ip);
		if (error) {
			/* XXX should downgrade to ro here, fs is corrupt */
			if (newparent)
				VOP_UNLOCK(fdvp, 0);
			VOP_UNLOCK(fvp, 0);
			goto bad;
		}
		if (!doingdirectory) {
			error = xtaf_pcbmap(dp, de_cluster(pmp, to_diroffset), 0,
				       &ip->de_dirclust, 0);
			if (error) {
				/* XXX should downgrade to ro here, fs is corrupt */
				if (newparent)
					VOP_UNLOCK(fdvp, 0);
				VOP_UNLOCK(fvp, 0);
				goto bad;
			}
			if (ip->de_dirclust == XTAFROOT)
				ip->de_diroffset = to_diroffset;
			else
				ip->de_diroffset = to_diroffset & pmp->pm_crbomask;
		}
		xtaf_reinsert(ip);
		if (newparent)
			VOP_UNLOCK(fdvp, 0);
	}

	cache_purge(fvp);
	VOP_UNLOCK(fvp, 0);
bad:
	if (xp)
		vput(tvp);
	vput(tdvp);
out:
	ip->de_flag &= ~DE_RENAME;
	vrele(fdvp);
	vrele(fvp);
	return (error);
}

static int
xtaf_mkdir(struct vop_mkdir_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	struct denode *dep;
	struct denode *pdep = VTODE(ap->a_dvp);
	struct xtafmount *pmp = pdep->de_pmp;
	u_long newcluster;
	int error;
	struct denode nde;
	struct timespec ts;

	/*
	 * If this is the root directory and there is no space left we
	 * can't do anything.  This is because the root directory can not
	 * change size.
	 */
	if (pdep->de_StartCluster == XTAFROOT
	    && pdep->de_fndoffset >= pdep->de_FileSize) {
		error = ENOSPC;
		goto bad2;
	}

	/*
	 * Allocate a cluster to hold the about to be created directory.
	 */
	error = xtaf_clusteralloc(pmp, 0, 1, CLUST_EOFE, &newcluster, NULL);
	if (error)
		goto bad2;

	bzero(&nde, sizeof(nde));
	nde.de_pmp = pmp;
	nde.de_flag = DE_ACCESS | DE_CREATE | DE_UPDATE;
	getnanotime(&ts);
	DETIMES(&nde, &ts, &ts, &ts);
	/*
	 * Now build up a directory entry pointing to the newly allocated
	 * cluster.  This will be written to an empty slot in the parent
	 * directory.
	 */
#ifdef DIAGNOSTIC
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("xtaf_mkdir: no name");
#endif
	error = uniqxtafname(cnp, nde.de_Name);
	if (error)
		goto bad;

	nde.de_Attributes = ATTR_DIRECTORY;
	nde.de_StartCluster = newcluster;
	nde.de_FileSize = 0;
	error = xtaf_createde(&nde, pdep, &dep, cnp);
	if (error)
		goto bad;
	*ap->a_vpp = DETOV(dep);
	return (0);
bad:
	xtaf_clusterfree(pmp, newcluster, NULL);
bad2:
	return (error);
}

static int
xtaf_rmdir(struct vop_rmdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct denode *ip, *dp;
	struct thread *td = cnp->cn_thread;
	int error;
	ip = VTODE(vp);
	dp = VTODE(dvp);

	/*
	 * Verify the directory is empty (and valid).
	 * (Rmdir ".." won't be valid since ".." will contain a reference to
	 *  the current directory and thus be non-empty.)
	 */
	error = 0;
	if (!xtafdirempty(ip) || ip->de_flag & DE_RENAME) {
		error = ENOTEMPTY;
		goto out;
	}
	/*
	 * Delete the entry from the directory.  For XTAF filesystems this
	 * gets rid of the directory entry on disk, the in memory copy
	 * still exists but the de_refcnt is <= 0.  This prevents it from
	 * being found by xtaf_deget().  When the vput() on dep is done we
	 * give up access and eventually xtaf_reclaim() will be called
	 * which will remove it from the denode cache.
	 */
	error = xtaf_removede(dp, ip);
	if (error)
		goto out;
	/*
	 * This is where we decrement the link count in the parent
	 * directory.  Since XTAF filesystems don't do this we just purge
	 * the name cache.
	 */
	cache_purge(dvp);
	/*
	 * Truncate the directory that is being deleted.
	 */
	error = xtaf_detrunc(ip, (u_long)0, IO_SYNC, cnp->cn_cred, td);
	cache_purge(vp);

out:
	return (error);
}

static int
xtaf_readdir(struct vop_readdir_args *ap)
{
	int error = 0;
	int diff;
	long n;
	int blsize;
	long on;
	u_long cn, dot;
	u_long dirsperblk;
	long bias = 0;
	daddr_t bn, lbn;
	struct buf *bp;
	struct denode *dep = VTODE(ap->a_vp);
	struct xtafmount *pmp = dep->de_pmp;
	struct direntry *ep;
	struct dirent dirbuf;
	struct uio *uio = ap->a_uio;
	u_long *cookies = NULL;
	int ncookies = 0;
	off_t offset, off;
	uint32_t fileno;

#ifdef XTAF_DEBUG
	printf("xtaf_readdir(): vp %p, uio %p, cred %p, eofflagp %p\n",
	    ap->a_vp, uio, ap->a_cred, ap->a_eofflag);
#endif

	/*
	 * xtaf_readdir() won't operate properly on regular files since
	 * it does i/o only with the the filesystem vnode, and hence can
	 * retrieve the wrong block from the buffer cache for a plain file.
	 * So, fail attempts to readdir() on a plain file.
	 */
	if ((dep->de_Attributes & ATTR_DIRECTORY) == 0)
		return (ENOTDIR);

	/*
	 * To be safe, initialize dirbuf
	 */
	bzero(dirbuf.d_name, sizeof(dirbuf.d_name));

	/*
	 * If the user buffer is smaller than the size of one XTAF directory
	 * entry or the file offset is not a multiple of the size of a
	 * directory entry, then we fail the read.
	 */
	off = offset = uio->uio_offset;
	if (uio->uio_resid < sizeof(struct direntry) ||
	    (offset & (sizeof(struct direntry) - 1)))
		return (EINVAL);
	dirsperblk = 512 / sizeof(struct direntry);

	if (ap->a_ncookies) {
		ncookies = uio->uio_resid / 16;
		cookies = malloc(ncookies * sizeof(u_long), M_TEMP, M_WAITOK);
		*ap->a_cookies = cookies;
		*ap->a_ncookies = ncookies;
	}
#ifdef XTAF_DEBUG
	printf("xtaf_readdir(): dep->de_StartCluster = %lx\n", dep->de_StartCluster);
#endif

	/*
	 * If they are reading from a directory then we simulate
	 * the . and .. entries since these don't exist in any
	 * directory.  We also set the offset bias to make up for having to
	 * simulate these entries. By this I mean that at file offset 128 we
	 * read the first entry in the directory that lives on disk.
	 */
	bias = 2 * sizeof(struct direntry);
	if (offset < bias) {
#ifdef XTAF_DEBUG
		printf("xtaf_readdir(): going after . or .., offset = %x, bias = %lx\n", (int)offset, bias);
#endif
		dot = find_dot_entry(pmp, dep->de_StartCluster);
		for (n = (int)offset / sizeof(struct direntry);
		     n < 2; n++) {
			dirbuf.d_namlen = n + 1;
			if (n == 0) {
				strcpy(dirbuf.d_name, ".");
				dirbuf.d_fileno = (dep->de_StartCluster == XTAFROOT ? 1 :
				    dot);
			} else {
				strcpy(dirbuf.d_name, "..");
				dirbuf.d_fileno = (dep->de_StartCluster == XTAFROOT ? 1 :
				    find_dot_entry(pmp, dot));
			}
#ifdef XTAF_DEBUG
			printf("xtaf_readdir(): n = %lx, fileno = %lx\n", n, (long unsigned)dirbuf.d_fileno);
#endif
			dirbuf.d_type = DT_DIR;
			dirbuf.d_reclen = GENERIC_DIRSIZ(&dirbuf);
			if (uio->uio_resid < dirbuf.d_reclen)
				goto out;
			error = uiomove(&dirbuf, dirbuf.d_reclen, uio);
			if (error)
				goto out;
			offset += sizeof(struct direntry);
			off = offset;
			if (cookies) {
				*cookies++ = offset;
				if (--ncookies <= 0)
					goto out;
			}
		}
	}
	off = offset;
	while (uio->uio_resid > 0) {
		lbn = de_cluster(pmp, offset - bias);
		on = (offset - bias) & pmp->pm_crbomask;
		n = min(pmp->pm_bpcluster - on, uio->uio_resid);
		diff = dep->de_FileSize - (offset - bias);
		if (diff <= 0)
			break;
		n = min(n, diff);
		error = xtaf_pcbmap(dep, lbn, &bn, &cn, &blsize);
		if (error)
			break;
		error = bread(pmp->pm_devvp, bn, blsize, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
		n = min(n, blsize - bp->b_resid);
		if (n == 0) {
			brelse(bp);
			return (EIO);
		}

		/*
		 * Convert from XTAF directory entries to fs-independent
		 * directory entries.
		 */
		for (ep = (struct direntry *)(bp->b_data + on);
		     (char *)ep < bp->b_data + on + n;
		     ep++, offset += sizeof(struct direntry)) {
#ifdef XTAF_DEBUG
			unsigned char name_c;
#endif
			/*
			 * If this is an unused entry, we can stop.
			 */
			if (ep->deLength == SLOT_EMPTY) {
				brelse(bp);
				goto out;
			}
			/*
			 * Skip deleted entries.
			 */
			if (ep->deLength == LEN_DELETED)
				continue;

#ifdef XTAF_DEBUG
			printf("rd: deLength %x attr %x startclust %x filesize %x mdate %x mtime %x adate %x atime %x cdate %x ctime %x name ",
			    ep->deLength, ep->deAttributes, be32dec(ep->deStartCluster), be32dec(ep->deFileSize), be16dec(ep->deMDate), be16dec(ep->deMTime), be16dec(ep->deADate), be16dec(ep->deATime), be16dec(ep->deCDate), be16dec(ep->deCTime));
			for (name_c = 0; name_c < 42; name_c++)
				printf("%c", ep->deName[name_c]);
			printf("\n");
#endif

			/* Sanity check the entry, not sure what to do if it is
			 * incorrect. */
			if (ep->deLength < 1 || ep->deLength > 42 ||
			    be32dec(ep->deStartCluster) < CLUST_FIRST ||
			    be32dec(ep->deStartCluster) > pmp->pm_maxcluster) {
				printf("xtaf_readdir: directory entry incorrect"
				    " at offset %x\n: deLength=%x"
				    " deStartCluster=%x", (uint32_t)offset,
				    ep->deLength, be32dec(ep->deStartCluster));
				continue; /* XXX lame */
			}

			/* add [ep->deStartCluster, dep->de_StartCluster]
			 * to dot list if ep->deStartCluster is used and
			 * if it is a directory
			 */
			if ((ep->deStartCluster != XTAFROOT) &&
			    (ep->deAttributes & ATTR_DIRECTORY)) {
				add_dot_entry(pmp, be32dec(ep->deStartCluster),
				    dep->de_StartCluster);
#ifdef XTAF_DEBUG
				printf("added (%x, %lx)\n",
				    be32dec(ep->deStartCluster),
				    dep->de_StartCluster);
#endif
			}

			/*
			 * This computation of d_fileno must match
			 * the computation of va_fileid in xtaf_getattr.
			 */
			if (ep->deAttributes & ATTR_DIRECTORY) {
				fileno = be32dec(ep->deStartCluster);
				dirbuf.d_fileno = (fileno == XTAFROOT) ? 1 :
				    (uint32_t)cntobn(pmp, fileno) * dirsperblk;
				dirbuf.d_type = DT_DIR;
			} else {
				dirbuf.d_fileno = (uint32_t)offset / sizeof(struct direntry);
				dirbuf.d_type = DT_REG;
			}

			dirbuf.d_namlen = xtaf2unixfn(ep->deName,
			    (u_char *)dirbuf.d_name);
			dirbuf.d_reclen = GENERIC_DIRSIZ(&dirbuf);
			if (uio->uio_resid < dirbuf.d_reclen) {
				brelse(bp);
				goto out;
			}
			error = uiomove(&dirbuf, dirbuf.d_reclen, uio);
			if (error) {
				brelse(bp);
				goto out;
			}
			if (cookies) {
				*cookies++ = offset + sizeof(struct direntry);
				if (--ncookies <= 0) {
					brelse(bp);
					goto out;
				}
			}
			off = offset + sizeof(struct direntry);
		}
		brelse(bp);
	}
out:
	/* Subtract unused cookies */
	if (ap->a_ncookies)
		*ap->a_ncookies -= ncookies;

	uio->uio_offset = off;

	/*
	 * Set the eofflag (NFS uses it)
	 */
	if (ap->a_eofflag) {
		if (dep->de_FileSize - (offset - bias) <= 0)
			*ap->a_eofflag = 1;
		else
			*ap->a_eofflag = 0;
	}
	return (error);
}

/*
 * a_vp   - pointer to the file's vnode
 * a_bn   - logical block number within the file (cluster number for us)
 * a_bop  - where to return the bufobj of the special file containing the fs
 * a_bnp  - where to return the "physical" block number corresponding to a_bn
 * 	(relative to the special file; units are blocks of size DEV_BSIZE)
 * a_runp - where to return the "run past" a_bn.  This is the conut of logical
 * 	blocks whose physical blocks (together with a_bn's physical block)
 * 	are contiguous.
 * a_runb - where to return the "run before" a_bn.
 */
static int
xtaf_bmap(struct vop_bmap_args *ap)
{
	struct denode *dep;
	struct mount *mp;
	struct xtafmount *pmp;
	struct vnode *vp;
	daddr_t runbn;
	u_long cn;
	int bnpercn, error, maxio, maxrun, run;

	vp = ap->a_vp;
	dep = VTODE(vp);
	pmp = dep->de_pmp;
	if (ap->a_bop != NULL)
		*ap->a_bop = &pmp->pm_devvp->v_bufobj;
	if (ap->a_bnp == NULL)
		return (0);
	if (ap->a_runp != NULL)
		*ap->a_runp = 0;
	if (ap->a_runb != NULL)
		*ap->a_runb = 0;
	cn = ap->a_bn;
	if (cn != ap->a_bn)
		return (EFBIG);
	error = xtaf_pcbmap(dep, cn, ap->a_bnp, NULL, NULL);
	if (error != 0 || (ap->a_runp == NULL && ap->a_runb == NULL))
		return (error);

	mp = vp->v_mount;
	maxio = mp->mnt_iosize_max / mp->mnt_stat.f_iosize;
	bnpercn = de_cn2bn(pmp, 1);
	if (ap->a_runp != NULL) {
		maxrun = ulmin(maxio - 1, pmp->pm_maxcluster - cn);
		for (run = 1; run <= maxrun; run++) {
			if (xtaf_pcbmap(dep, cn + run, &runbn, NULL, NULL) != 0 ||
			    runbn != *ap->a_bnp + run * bnpercn)
				break;
		}
		*ap->a_runp = run - 1;
	}
	if (ap->a_runb != NULL) {
		maxrun = ulmin(maxio - 1, cn);
		for (run = 1; run < maxrun; run++) {
			if (xtaf_pcbmap(dep, cn - run, &runbn, NULL, NULL) != 0 ||
			    runbn != *ap->a_bnp - run * bnpercn)
				break;
		}
		*ap->a_runb = run - 1;
	}
	return (0);
}

/*
 * When we search a directory the blocks containing directory entries are
 * read and examined.  The directory entries contain information that would
 * normally be in the inode of a unix filesystem.  This means that some of
 * a directory's contents may also be in memory resident denodes (sort of
 * an inode).  This can cause problems if we are searching while some other
 * process is modifying a directory.  To prevent one process from accessing
 * incompletely modified directory information we depend upon being the
 * sole owner of a directory block.  bread/brelse provide this service.
 * This being the case, when a process modifies a directory it must first
 * acquire the disk block that contains the directory entry to be modified.
 * Then update the disk block and the denode, and then write the disk block
 * out to disk.  This way disk blocks containing directory entries and in
 * memory denode's will be in synch.
 */
int
xtaf_lookup(struct vop_cachedlookup_args *ap)
{
	struct vnode *vdp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	daddr_t bn;
	int error;
	int slotcount;
	int slotoffset = 0;
	int frcn;
	u_long cluster;
	int blkoff = 0;		/* silent gcc */
	int diroff;
	int blsize;
	int isadir;		/* ~0 if found direntry is a directory	 */
	u_long scn;		/* starting cluster number		 */
	struct vnode *pdp;
	struct denode *dep;
	struct denode *tdep;
	struct xtafmount *pmp;
	struct buf *bp = NULL;
	struct direntry *ep = NULL;
	/*
	 * File names on the harddisk are padded with either 0x00 or 0xff,
	 * so look for both.
	 */
	u_char xtaffilename_00[43];
	u_char xtaffilename_ff[43];
	int flags = cnp->cn_flags;
#ifdef XTAF_DEBUG
	int nameiop = cnp->cn_nameiop;
#endif

	dep = VTODE(vdp);
	pmp = dep->de_pmp;
	*vpp = NULL;
#ifdef XTAF_DEBUG
	printf("xtaf_lookup(): vdp %p, dep %p, Attr %x, StartCluster %lx, "
	    "namelen %li, name %s\n", vdp, dep, dep->de_Attributes,
	    dep->de_StartCluster, cnp->cn_namelen, cnp->cn_nameptr);
#endif

	if (unix2xtaffn((const u_char *)cnp->cn_nameptr, xtaffilename_00,
	    cnp->cn_namelen, 0x00) == 0 ||
	    unix2xtaffn((const u_char *)cnp->cn_nameptr, xtaffilename_ff,
	    cnp->cn_namelen, 0xff) == 0)
		return (EINVAL);

	/*
	 * Suppress search for slots unless creating
	 * file and at end of pathname, in which case
	 * we watch for a place to put the new file in
	 * case it doesn't already exist.
	 */
	slotcount = 1;
	if ((nameiop == CREATE || nameiop == RENAME) &&
	    (flags & ISLASTCN))
		slotcount = 0;
	/*
	 * Search the directory pointed at by vdp for the name pointed at
	 * by cnp->cn_nameptr.
	 */
	tdep = NULL;
	/*
	 * The outer loop ranges over the clusters that make up the
	 * directory.  Note that the root directory is different from all
	 * other directories.  It has a fixed number of blocks that are not
	 * part of the pool of allocatable clusters.  So, we treat it a
	 * little differently. The root directory starts at "cluster" 0.
	 */
	diroff = 0;
	for (frcn = 0;; frcn++) {
		error = xtaf_pcbmap(dep, frcn, &bn, &cluster, &blsize);
#ifdef XTAF_DEBUG
		printf("xtaf_lookup: error=%d bn=%x cluster=%lx blsize=%x\n",
		    error, (uint32_t)bn, cluster, blsize);
#endif
		if (error) {
			if (error == E2BIG)
				break;
			return (error);
		}
		error = bread(pmp->pm_devvp, bn, blsize, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
		for (blkoff = 0; blkoff < blsize;
		     blkoff += sizeof(struct direntry),
		     diroff += sizeof(struct direntry)) {
			ep = (struct direntry *)(bp->b_data + blkoff);
			/*
			 * If the slot is empty and we are still looking
			 * for an empty then remember this one.  If the
			 * slot is not empty then check to see if it
			 * matches what we are looking for.  If the slot
			 * has never been filled with anything, then the
			 * remainder of the directory has never been used,
			 * so there is no point in searching it.
			 */
			if (ep->deLength == SLOT_EMPTY ||
			    ep->deLength == LEN_DELETED) {
				/*
				 * Drop memory of previous long matches
				 */

				if (slotcount < 1) {
					slotcount++;
					slotoffset = diroff;
				}
				if (ep->deLength == SLOT_EMPTY) {
					brelse(bp);
					goto notfound;
				}
			} else {
				/*
				 * Check for a name match
				 */
				if (bcmp(xtaffilename_00, ep->deName, 42) && 
				    bcmp(xtaffilename_ff, ep->deName, 42))
					continue; /* name not found */
#ifdef XTAF_DEBUG
				printf("xtaf_lookup(): match blkoff %x, diroff %x\n",
				    blkoff, diroff);
#endif
				/*
				 * Remember where this directory
				 * entry came from for whoever did
				 * this lookup.
				 */
				dep->de_fndoffset = diroff;
#ifdef XTAF_DEBUG
				{
					unsigned char name_c;
					printf("xtaf_lookup(): ep: length=%x attr=%x startCluster=%x fsize=%x ", ep->deLength, ep->deAttributes, be32dec(ep->deStartCluster), be32dec(ep->deFileSize));
					for (name_c = 0; name_c < 42; name_c++)
						printf("%c", ep->deName[name_c]);
					printf("\n");
				}
#endif
				goto found;
			}
		}	/* for (blkoff = 0; .... */
		/*
		 * Release the buffer holding the directory cluster just
		 * searched.
		 */
		brelse(bp);
	}	/* for (frcn = 0; ; frcn++) */

notfound:
	/*
	 * We hold no disk buffers at this point.
	 *
	 * Fixup the slot description to point to the place where
	 * we might put the new XTAF direntry
	 */
	if (!slotcount) {
		slotcount = 1;
		slotoffset = diroff;
	}

	/*
	 * If we get here we didn't find the entry we were looking for. But
	 * that's ok if we are creating or renaming and are at the end of
	 * the pathname and the directory hasn't been removed.
	 */
#ifdef XTAF_DEBUG
	printf("xtaf_lookup(): entry notfound, op %d, refcnt %ld\n",
	    nameiop, dep->de_refcnt);
	printf("               slotcount %d, slotoffset %d\n",
	       slotcount, slotoffset);
#endif
	if ((nameiop == CREATE || nameiop == RENAME) &&
	    (flags & ISLASTCN) && dep->de_refcnt != 0) {
		/*
		 * Access for write is interpreted as allowing
		 * creation of files in the directory.
		 */
		error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred, cnp->cn_thread);
		if (error)
			return (error);
		/*
		 * Return an indication of where the new directory
		 * entry should be put.
		 */
		dep->de_fndoffset = slotoffset;

		/*
		 * We return with the directory locked, so that
		 * the parameters we set up above will still be
		 * valid if we actually decide to do a direnter().
		 * We return ni_vp == NULL to indicate that the entry
		 * does not currently exist; we leave a pointer to
		 * the (locked) directory inode in ndp->ni_dvp.
		 * The pathname buffer is saved so that the name
		 * can be obtained later.
		 *
		 * NB - if the directory is unlocked, then this
		 * information cannot be used.
		 */
		cnp->cn_flags |= SAVENAME;
		return (EJUSTRETURN);
	}
	return (ENOENT);

found:
	/*
	 * NOTE:  We still have the buffer with matched directory entry at
	 * this point.
	 */
	isadir = ep->deAttributes & ATTR_DIRECTORY;
	scn = be32dec(ep->deStartCluster);
	/*
	 * Now release buf to allow xtaf_deget() to read the entry again.
	 * Reserving it here and giving it to xtaf_deget() could result
	 * in a deadlock.
	 */
	brelse(bp);
	bp = NULL;
	if (isadir)
		cluster = scn;
	else if (cluster == XTAFROOT)
		blkoff = diroff; /* empty file */
	/*
	 * If deleting, and at end of pathname, return
	 * parameters which can be used to remove file.
	 */
	if (nameiop == DELETE && (flags & ISLASTCN)) {
		/*
		 * Don't allow deleting the root.
		 */
		if (blkoff == XTAFROOT_OFS)
			return (EOPNOTSUPP);

		/*
		 * Write access to directory required to delete files.
		 */
		error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred, cnp->cn_thread);
		if (error)
			return (error);

		/*
		 * Return pointer to current entry in dep->i_offset.
		 * Save directory inode pointer in ndp->ni_dvp for dirremove().
		 */
		if (isadir && cnp->cn_namelen == 1 &&
		    !strcmp(cnp->cn_nameptr, ".")) {
			VREF(vdp);
			*vpp = vdp;
			return (0);
		}
		error = xtaf_deget(pmp, cluster, blkoff, &tdep);
		if (error)
			return (error);
		*vpp = DETOV(tdep);
		return (0);
	}

	/*
	 * If rewriting (RENAME), return the inode and the
	 * information required to rewrite the present directory
	 * Must get inode of directory entry to verify it's a
	 * regular file, or empty directory.
	 */
	if (nameiop == RENAME && (flags & ISLASTCN)) {
		if (blkoff == XTAFROOT_OFS)
			return (EOPNOTSUPP);

		error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred, cnp->cn_thread);
		if (error)
			return (error);

		/*
		 * Careful about locking second inode.
		 * This can only occur if the target is ".".
		 */
		if (isadir && cnp->cn_namelen == 1 &&
		    !strcmp(cnp->cn_nameptr, "."))
			return (EISDIR);

		if ((error = xtaf_deget(pmp, cluster, blkoff, &tdep)) != 0)
			return (error);
		*vpp = DETOV(tdep);
		cnp->cn_flags |= SAVENAME;
		return (0);
	}

	/*
	 * Step through the translation in the name.  We do not `vput' the
	 * directory because we may need it again if a symbolic link
	 * is relative to the current directory.  Instead we save it
	 * unlocked as "pdp".  We must get the target inode before unlocking
	 * the directory to insure that the inode will not be removed
	 * before we get it.  We prevent deadlock by always fetching
	 * inodes from the root, moving down the directory tree. Thus
	 * when following backward pointers ".." we must unlock the
	 * parent directory before getting the requested directory.
	 * There is a potential race condition here if both the current
	 * and parent directories are removed before the VFS_VGET for the
	 * inode associated with ".." returns.  We hope that this occurs
	 * infrequently since we cannot avoid this race condition without
	 * implementing a sophisticated deadlock detection algorithm.
	 * Note also that this simple deadlock detection scheme will not
	 * work if the filesystem has any hard links other than ".."
	 * that point backwards in the directory structure.
	 */
	pdp = vdp;
	if (flags & ISDOTDOT) {
#ifdef XTAF_DEBUG
		printf("xtaf_lookup(): isdotdot\n");
#endif
		VOP_UNLOCK(pdp, 0);
		error = xtaf_deget(pmp, cluster, blkoff,  &tdep);
		vn_lock(pdp, LK_EXCLUSIVE | LK_RETRY);
		if (error)
			return (error);
		*vpp = DETOV(tdep);
	} else if (isadir && cnp->cn_namelen == 1 &&
	    !strcmp(cnp->cn_nameptr, ".")) {
#ifdef XTAF_DEBUG
		printf("xtaf_lookup(): isdot, scn==%lx\n", dep->de_StartCluster);
#endif
		VREF(vdp);	/* we want ourself, ie "." */
		*vpp = vdp;
	} else {
#ifdef XTAF_DEBUG
		printf("xtaf_lookup(): searching cluster %lx, blkoff %x\n",
		    cluster, blkoff);
#endif
		if ((error = xtaf_deget(pmp, cluster, blkoff, &tdep)) != 0)
			return (error);
		*vpp = DETOV(tdep);
	}

	/*
	 * Insert name into cache if appropriate.
	 */
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(vdp, *vpp, cnp);
	return (0);
}


static int
xtaf_strategy(struct vop_strategy_args *ap)
{
	struct buf *bp = ap->a_bp;
	struct denode *dep = VTODE(ap->a_vp);
	struct bufobj *bo;
	int error = 0;
	daddr_t blkno;

	/*
	 * If we don't already know the filesystem relative block number
	 * then get it using xtaf_pcbmap().  If xtaf_pcbmap() returns the block
	 * number as -1 then we've got a hole in the file.  XTAF filesystems
	 * don't allow files with holes, so we shouldn't ever see this.
	 */
	if (bp->b_blkno == bp->b_lblkno) {
		error = xtaf_pcbmap(dep, bp->b_lblkno, &blkno, 0, 0);
		bp->b_blkno = blkno;
		if (error) {
			bp->b_error = error;
			bp->b_ioflags |= BIO_ERROR;
			bufdone(bp);
			return (0);
		}
		if ((long)bp->b_blkno == -1)
			vfs_bio_clrbuf(bp);
	}
	if (bp->b_blkno == -1) {
		bufdone(bp);
		return (0);
	}
	/*
	 * Read/write the block from/to the disk that contains the desired
	 * file block.
	 */
	bp->b_iooffset = dbtob(bp->b_blkno);
	bo = dep->de_pmp->pm_bo;
	BO_STRATEGY(bo, bp);
	return (0);
}

static int
xtaf_pathconf(struct vop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 1;
		return (0);
	case _PC_NAME_MAX:
		*ap->a_retval = 42;
		return (0);
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return (0);
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return (0);
	case _PC_NO_TRUNC:
		*ap->a_retval = 0;
		return (0);
	default:
		return (EINVAL);
	}
	/* NOTREACHED */
}

static int
xtaf_vptofh(struct vop_vptofh_args *ap)
{
	struct denode *dep;
	struct defid *defhp;

	dep = VTODE(ap->a_vp);
	defhp = (struct defid *)ap->a_fhp;
	defhp->defid_len = sizeof(struct defid);
	defhp->defid_dirclust = dep->de_dirclust;
	defhp->defid_dirofs = dep->de_diroffset;
	return (0);
}

/* Global vfs data structures for xtaf */
struct vop_vector xtaf_vnodeops = {
	.vop_default =		&default_vnodeops,
	.vop_strategy =		xtaf_strategy,
	.vop_access =		xtaf_access,
	.vop_bmap =		xtaf_bmap,
	.vop_cachedlookup =	xtaf_lookup,
	.vop_open =		xtaf_open,
	.vop_close =		xtaf_close,
	.vop_getattr =		xtaf_getattr,
	.vop_inactive =		xtaf_inactive,
	.vop_lookup =		vfs_cache_lookup,
	.vop_pathconf =		xtaf_pathconf,
	.vop_read =		xtaf_read,
	.vop_readdir =		xtaf_readdir,
	.vop_reclaim =		xtaf_reclaim,
	.vop_create =		xtaf_create,
	.vop_fsync =		xtaf_fsync,
	.vop_mkdir =		xtaf_mkdir,
	.vop_remove =		xtaf_remove,
	.vop_rename =		xtaf_rename,
	.vop_rmdir =		xtaf_rmdir,
	.vop_setattr =		xtaf_setattr,
	.vop_write =		xtaf_write,
	.vop_vptofh =		xtaf_vptofh,
};
