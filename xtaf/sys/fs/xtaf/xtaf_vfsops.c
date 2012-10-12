/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: msdosfs_vfsops.c,v 1.51 1997/11/17 15:36:58 ws Exp $	*/

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
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
/* #include <sys/mount.h> */ /* used by msdosfs */
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <sys/endian.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <fs/xtaf/bpb.h>
#include <fs/xtaf/direntry.h>
#include <fs/xtaf/denode.h>
#include <fs/xtaf/fat.h>
#include <fs/xtaf/xtafmount.h>

static const char xtaf_lock_msg[] = "xtaflk";

/* Mount options we support */
static const char *xtaf_opts[] = {
	"export", "from",
	"gid", "uid", "mask", "dirmask",
	"noclusterr", "noclusterw",
	NULL
};

#define ZERO_SIZE	4096
#define BYTES_PER_SEC	512

MALLOC_DEFINE(M_XTAFMNT, "XTAF_mount", "XTAF mount structure");
static MALLOC_DEFINE(M_XTAFFAT, "XTAF_fat", "XTAF file allocation table");

static int	update_mp(struct mount *mp, struct thread *td);
static int	mountxtaf(struct vnode *devvp, struct mount *mp);
static vfs_mount_t	xtaf_mount;
static vfs_root_t	xtaf_root;
static vfs_statfs_t	xtaf_statfs;
static vfs_unmount_t	xtaf_unmount;
static vfs_sync_t	xtaf_sync;
static vfs_fhtovp_t	xtaf_fhtovp;

static int
update_mp(struct mount *mp, struct thread *td)
{
	struct xtafmount *pmp = VFSTOXTAF(mp);
	int v;

	if (1 == vfs_scanopt(mp->mnt_optnew, "gid", "%d", &v))
		pmp->pm_gid = v;
	if (1 == vfs_scanopt(mp->mnt_optnew, "uid", "%d", &v))
		pmp->pm_uid = v;
	if (1 == vfs_scanopt(mp->mnt_optnew, "mask", "%o", &v))
		pmp->pm_mask = v & ALLPERMS;
	else
		pmp->pm_mask = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	if (1 == vfs_scanopt(mp->mnt_optnew, "dirmask", "%o", &v))
		pmp->pm_dirmask = v & ALLPERMS;
	else
		pmp->pm_dirmask = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH |
		    S_IXOTH;

	return (0);
}

/*
 * mp - path - addr in user space of mount point (ie /usr or whatever)
 * data - addr in user space of mount params including the name of the block
 * special file to treat as a filesystem.
 */
static int
xtaf_mount(struct mount *mp)
{
	struct vnode *devvp;		/* vnode for blk device to mount */
	struct thread *td;
	struct xtafmount *pmp = NULL;	/* xtaf specific mount control block */
	struct nameidata ndp;
	int error, flags;
	accmode_t accmode;
	char *from;

	td = curthread;
	if (vfs_filteropt(mp->mnt_optnew, xtaf_opts))
		return (EINVAL);

	/*
	 * If updating, check whether changing from read-only to
	 * read/write; if there is no device name, that's all we do.
	 */
	if (mp->mnt_flag & MNT_UPDATE) {
		pmp = VFSTOXTAF(mp);
		if (vfs_flagopt(mp->mnt_optnew, "export", NULL, 0)) {
			/* Process export requests. */
			return (0);
		}
		if (!(pmp->pm_flags & XTAFMNT_RONLY) &&
		    vfs_flagopt(mp->mnt_optnew, "ro", NULL, 0)) {
			error = VFS_SYNC(mp, MNT_WAIT);
			if (error)
				return (error);
			flags = WRITECLOSE;
			if (mp->mnt_flag & MNT_FORCE)
				flags |= FORCECLOSE;
			error = vflush(mp, 0, flags, td);
			if (error)
				return (error);
			DROP_GIANT();
			g_topology_lock();
			error = g_access(pmp->pm_cp, 0, -1, 0);
			g_topology_unlock();
			PICKUP_GIANT();
			if (error)
				return (error);
		} else if ((pmp->pm_flags & XTAFMNT_RONLY) &&
		    !vfs_flagopt(mp->mnt_optnew, "ro", NULL, 0)) {
			/*
			 * If upgrade to read-write by non-root, then verify
			 * that user has necessary permissions on the device.
			 */
			devvp = pmp->pm_devvp;
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			error = VOP_ACCESS(devvp, VREAD | VWRITE,
			    td->td_ucred, td);
			if (error)
				error = priv_check(td, PRIV_VFS_MOUNT_PERM);
			if (error) {
				VOP_UNLOCK(devvp, 0);
				return (error);
			}
			VOP_UNLOCK(devvp, 0);
			DROP_GIANT();
			g_topology_lock();
			error = g_access(pmp->pm_cp, 0, 1, 0);
			g_topology_unlock();
			PICKUP_GIANT();
			if (error)
				return (error);

		}
		vfs_flagopt(mp->mnt_optnew, "ro",
		    &mp->mnt_flag, MNT_RDONLY);
	}
	/*
	 * Not an update, or updating the name: look up the name
	 * and verify that it refers to a sensible device.
	 */
	if (vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL))
		return (EINVAL);
	NDINIT(&ndp, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from, td);
	error = namei(&ndp);
	if (error)
		return (error);
	devvp = ndp.ni_vp;
	NDFREE(&ndp, NDF_ONLY_PNBUF);

	if (!vn_isdisk(devvp, &error)) {
		vput(devvp);
		return (error);
	}
	/*
	 * If mount by non-root, then verify that user has necessary
	 * permissions on the device.
	 */
	accmode = VREAD;
	if ((mp->mnt_flag & MNT_RDONLY) == 0)
		accmode |= VWRITE;
	error = VOP_ACCESS(devvp, accmode, td->td_ucred, td);
	if (error)
		error = priv_check(td, PRIV_VFS_MOUNT_PERM);
	if (error) {
		vput(devvp);
		return (error);
	}
	if ((mp->mnt_flag & MNT_UPDATE) == 0)
		error = mountxtaf(devvp, mp);
	else {
		if (devvp != pmp->pm_devvp)
			error = EINVAL;	/* XXX needs translation */
		else
			vput(devvp);
	}
	if (error) {
		vrele(devvp);
		return (error);
	}

	error = update_mp(mp, td);
	if (error) {
		if ((mp->mnt_flag & MNT_UPDATE) == 0)
			xtaf_unmount(mp, MNT_FORCE);
		return error;
	}

	vfs_mountedfrom(mp, from);
	return (0);
}

/* Format of a 'boot' sector.  This is the first sector on a memory card,
 * but not the first sector on a hard disk.  Hard disks have a 512 kB
 * header.
 */

struct bootsector_xtaf {
	u_int8_t	bsBPB[18]; /* BIOS parameter block */
};

static int
mountxtaf(struct vnode *devvp, struct  mount *mp)
{
	struct xtafmount *pmp;
	struct buf *bp;
	struct cdev *dev;
	struct bootsector_xtaf *bs_xtaf;
	struct byte_bpb_xtaf *bxtaf;
	u_int32_t SecPerClust;
	u_long clusters;
	int ronly, error;
	int zero;
	struct g_consumer *cp;
	struct bufobj *bo;

	bp = NULL;
	pmp = NULL;
	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	dev = devvp->v_rdev;
	dev_ref(dev);
	DROP_GIANT();
	g_topology_lock();
	error = g_vfs_open(devvp, &cp, "xtaf", ronly ? 0 : 1);
	g_topology_unlock();
	PICKUP_GIANT();
	VOP_UNLOCK(devvp, 0);
	if (error)
		goto error_exit;

	bo = &devvp->v_bufobj;

	/*
	 * Check if we can read the medium to prevent panicing when trying
	 * to mount an audio CD.  From sys/ufs/ffs/ffs_vfsops.c rev 1.316
	 */
	if ((PAGE_SIZE % cp->provider->sectorsize) != 0) {
		error = EINVAL;
		vfs_mount_error(mp,
		    "Invalid sectorsize %d for superblock size %d",
		    cp->provider->sectorsize, PAGE_SIZE);
		goto error_exit;
	}

	/*
	 * Read the boot sector of the filesystem, and then check the
	 * boot signature.  If not an XTAF boot sector then error out.
	 *
	 * NOTE: 8192 is a magic size that works for ffs.
	 */
	error = bread(devvp, 0, 8192, NOCRED, &bp);
	if (error)
		goto error_exit;
	bp->b_flags |= B_AGE;
	bs_xtaf = (struct bootsector_xtaf *)bp->b_data;
	bxtaf = (struct byte_bpb_xtaf *)bs_xtaf->bsBPB;

	/*
	 * Release the bootsector buffer to make room for the FAT test buffer.
	 */
	brelse(bp);
	bp = NULL;

	pmp = malloc(sizeof *pmp, M_XTAFMNT, M_WAITOK | M_ZERO);
	pmp->pm_mountp = mp;
	pmp->pm_cp = cp;
	pmp->pm_bo = bo;

	lockinit(&pmp->pm_fatlock, 0, xtaf_lock_msg, 0, 0);

	/*
	 * Initialize ownerships and permissions, since nothing else will
	 * initialize them iff we are mounting root.
	 */
	pmp->pm_uid = UID_ROOT;
	pmp->pm_gid = GID_WHEEL;
	pmp->pm_mask = pmp->pm_dirmask = S_IXUSR | S_IXGRP | S_IXOTH |
	    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR;

	/*
	 * Compute several useful quantities from the bpb in the
	 * bootsector.
	 */

	SecPerClust = be32dec(bxtaf->bpbSecPerClust);
	pmp->pm_FATs = be32dec(bxtaf->bpbFATs);

	/* calculate the ratio of sector size to DEV_BSIZE */
	pmp->pm_BlkPerSec = BYTES_PER_SEC / DEV_BSIZE;

#ifdef XTAF_DEBUG
	printf("ident = %c%c%c%c\n", bxtaf->bpbIdent[0], bxtaf->bpbIdent[1], bxtaf->bpbIdent[2], bxtaf->bpbIdent[3]);
	printf("serial = %x\n", be32dec(bxtaf->bpbVolId));
	printf("SecPerClust (before) = %x\n", SecPerClust);
	printf("pm_FATs = %x\n", pmp->pm_FATs);
	printf("fill = %x\n", be16dec(bxtaf->bpbFill));
	printf("mediasize = %llx\n", (unsigned long long)cp->provider->mediasize);
#endif
	/* (rene) We could check bpbFill */
	if (!SecPerClust || pmp->pm_FATs != 1 ||
	    bcmp(bxtaf->bpbIdent, "XTAF", 4)) {
		error = EINVAL;
		goto error_exit;
	}

	/* cp->provider->mediasize has type off_t and is in bytes */
	pmp->pm_FATsecs = (unsigned long long)cp->provider->mediasize /
	    (BYTES_PER_SEC * SecPerClust);
#ifdef XTAF_DEBUG
	printf("pm_FATsecs (before) = %lx\n", pmp->pm_FATsecs);
#endif
	if (pmp->pm_FATsecs >= 0xfff4) {
		pmp->pm_fatmask = FAT32_MASK;
		pmp->pm_fatmult = 4;
	} else {
		pmp->pm_fatmask = FAT16_MASK;
		pmp->pm_fatmult = 2;
	}
	pmp->pm_FATsecs *= pmp->pm_fatmult;
	/* round up to nearest block size if number would be fraction */
	if (pmp->pm_FATsecs % PAGE_SIZE)
		pmp->pm_FATsecs = ((pmp->pm_FATsecs / PAGE_SIZE) + 1) *
		    PAGE_SIZE;
	pmp->pm_FATsecs /= DEV_BSIZE; /* convert to blocks */
#ifdef XTAF_DEBUG
	printf("pm_FATsecs (after) = %lx\n", pmp->pm_FATsecs);
#endif

	/*
	 * Check a few values (could do some more):
	 * - logical sector size: power of 2, >= block size
	 * - sectors per cluster: power of 2, >= 1
	 * - number of sectors:   >= 1, <= size of partition
	 * - number of FAT sectors: >= 1
	 */
	if ((SecPerClust == 0) || (SecPerClust & (SecPerClust - 1)) ||
	    (pmp->pm_FATsecs == 0)) {
		error = EINVAL;
		goto error_exit;
	}

	pmp->pm_FATsecs *= pmp->pm_BlkPerSec;
	SecPerClust     *= pmp->pm_BlkPerSec;

	pmp->pm_fatblk   = 8 * pmp->pm_BlkPerSec; /* FAT starts at 8 blocks */
	pmp->pm_rootdirblk = pmp->pm_FATsecs + pmp->pm_fatblk;

	/*
	 * Quirk: partition 0 and 1 of the hard disk have a 4 kB hole
	 * filled with 0 bytes after the FAT.  Test if this is the case for
	 * this partition and adjust pm_rootdirblk accordingly.
	 */
	error = bread(devvp, pmp->pm_rootdirblk, ZERO_SIZE, NOCRED, &bp);
	if (error)
		goto error_exit;
	for (zero = 0; zero < ZERO_SIZE && bp->b_data[zero] == 0; zero++)
		;
#ifdef XTAF_DEBUG
	printf("zero = %x\n", zero);
#endif
	if (zero == ZERO_SIZE)
		pmp->pm_rootdirblk += (ZERO_SIZE / DEV_BSIZE);
	/*
	 * Release the FAT test buffer.
	 */
	brelse(bp);
	bp = NULL;

	/*
	 * The root directory can hold 8 * SecPerClust entries, in contrast
	 * to the Xbox 1 root directory which always held 256 entries.
	 * Round up to the nearest integer if we have a fraction.
	 */
	pmp->pm_rootdirsize = (8 * SecPerClust * sizeof(struct direntry) +
	    DEV_BSIZE - 1) / DEV_BSIZE; /* in blocks */
	pmp->pm_firstcluster = pmp->pm_rootdirblk + pmp->pm_rootdirsize;
	pmp->pm_maxcluster = ((unsigned long long)cp->provider->mediasize /
	    BYTES_PER_SEC - pmp->pm_firstcluster) / SecPerClust + 1;
	pmp->pm_fatsize = pmp->pm_FATsecs * DEV_BSIZE;

	clusters = pmp->pm_fatsize / pmp->pm_fatmult;
	if (pmp->pm_maxcluster >= clusters) {
		printf("Warning: number of clusters (%lx) exceeds FAT "
		    "capacity (%ld)\n", pmp->pm_maxcluster + 1, clusters);
		pmp->pm_maxcluster = clusters - 1;
	}

	pmp->pm_fatblocksize = roundup(PAGE_SIZE, 512);
	pmp->pm_fatblocksec = pmp->pm_fatblocksize / DEV_BSIZE;
	pmp->pm_bnshift = ffs(DEV_BSIZE) - 1;
#ifdef XTAF_DEBUG
	printf("SecPerClust (after) = %x\n", SecPerClust);
	printf("rootdirblk, rootdirsize = %lx, %lx\n", pmp->pm_rootdirblk,
	    pmp->pm_rootdirsize);
	printf("firstcluster, maxcluster = %lx, %lx\n", pmp->pm_firstcluster,
	    pmp->pm_maxcluster);
	printf("fatsize, clusters = %lx, %lx\n", pmp->pm_fatsize, clusters);
#endif
	/*
	 * Compute mask and shift value for isolating cluster relative byte
	 * offsets and cluster numbers from a file offset.
	 */
	pmp->pm_bpcluster = SecPerClust * DEV_BSIZE;
	pmp->pm_crbomask = pmp->pm_bpcluster - 1;
	pmp->pm_cnshift = ffs(pmp->pm_bpcluster) - 1;

	/*
	 * Check for valid cluster size
	 * must be a power of 2
	 */
	if (pmp->pm_bpcluster ^ (1 << pmp->pm_cnshift)) {
		error = EINVAL;
		goto error_exit;
	}

	/*
	 * Allocate memory for the bitmap of allocated clusters, and then
	 * fill it in.
	 */
	pmp->pm_inusemap = malloc(howmany(pmp->pm_maxcluster + 1, N_INUSEBITS) *
			    sizeof(*pmp->pm_inusemap), M_XTAFFAT, M_WAITOK);
#ifdef XTAF_DEBUG
	printf("inusemap size = %lx\n", howmany(pmp->pm_maxcluster + 1,
	    N_INUSEBITS) * sizeof(*pmp->pm_inusemap));
#endif

	/*
	 * xtaf_fillinusemap() needs pm_devvp.
	 */
	pmp->pm_devvp = devvp;
	pmp->pm_dev = dev;

	/*
	 * Have the inuse map filled in.
	 */
	XTAF_LOCK_MP(pmp);
	error = xtaf_fillinusemap(pmp);
	XTAF_UNLOCK_MP(pmp);
	if (error != 0)
		goto error_exit;
#ifdef XTAF_DEBUG
	if (FAT32(pmp))
		printf("In FAT32 mode.\n");
#endif
	/*
	 * If they want fat updates to be synchronous then let them suffer
	 * the performance degradation in exchange for the on disk copy of
	 * the fat being correct just about all the time.  I suppose this
	 * would be a good thing to turn on if the kernel is still flakey.
	 */
	if (mp->mnt_flag & MNT_SYNCHRONOUS)
		pmp->pm_flags |= XTAFMNT_WAITONFAT;

	/*
	 * Initalize the dot lookup table and add an entry for the root.
	 */
	init_dot_lookup_table(pmp);
	add_dot_entry(pmp, XTAFROOT, XTAFROOT);
#ifdef XTAF_DEBUG
	printf("added (XTAFROOT, XTAFROOT)\n");
#endif

	/*
	 * Finish up.
	 */
	if (ronly)
		pmp->pm_flags |= XTAFMNT_RONLY;
	else
		pmp->pm_fmod = 1;
	mp->mnt_data = pmp;
	mp->mnt_stat.f_fsid.val[0] = dev2udev(dev);
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_MPSAFE;
	MNT_IUNLOCK(mp);

	return (0);

error_exit:
	if (bp)
		brelse(bp);
	if (cp != NULL) {
		DROP_GIANT();
		g_topology_lock();
		g_vfs_close(cp);
		g_topology_unlock();
		PICKUP_GIANT();
	}
	lockdestroy(&pmp->pm_fatlock);
	if (pmp) {
		if (pmp->pm_inusemap)
			free(pmp->pm_inusemap, M_XTAFFAT);
		free(pmp, M_XTAFMNT);
		mp->mnt_data = NULL;
		dev_rel(dev);
	}
	return (error);
}

/*
 * Unmount the filesystem described by mp.
 */
static int
xtaf_unmount(struct mount *mp, int mntflags)
{
	struct xtafmount *pmp;
	int error, flags;

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, 0, flags, curthread);
	if (error && error != ENXIO)
		return error;
	pmp = VFSTOXTAF(mp);

	remove_dot_lookup_table(pmp);

	DROP_GIANT();
	g_topology_lock();
	g_vfs_close(pmp->pm_cp);
	g_topology_unlock();
	PICKUP_GIANT();
	vrele(pmp->pm_devvp);
	dev_rel(pmp->pm_dev);
	free(pmp->pm_inusemap, M_XTAFFAT);
	lockdestroy(&pmp->pm_fatlock);
	free(pmp, M_XTAFMNT);
	mp->mnt_data = NULL;
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);
	return (error);
}

static int
xtaf_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct xtafmount *pmp = VFSTOXTAF(mp);
	struct denode *ndep;
	int error;

	error = xtaf_deget(pmp, XTAFROOT, XTAFROOT_OFS, &ndep);
	if (error)
		return (error);
	*vpp = DETOV(ndep);
	return (0);
}

static int
xtaf_statfs(struct mount *mp, struct statfs *sbp)
{
	struct xtafmount *pmp;

	pmp = VFSTOXTAF(mp);
	sbp->f_bsize = pmp->pm_bpcluster;
	sbp->f_iosize = pmp->pm_bpcluster;
	sbp->f_blocks = pmp->pm_maxcluster + 1;
	sbp->f_bfree = pmp->pm_freeclustercount;
	sbp->f_bavail = pmp->pm_freeclustercount;
	sbp->f_files = 256;
	sbp->f_ffree = 0;	/* what to put in here? */
	return (0);
}

static int
xtaf_sync(struct mount *mp, int waitfor)
{
	struct vnode *vp, *nvp;
	struct thread *td;
	struct denode *dep;
	struct xtafmount *pmp = VFSTOXTAF(mp);
	int error;
	int allerror = 0;

	td = curthread;
	if (pmp->pm_fmod != 0) {
		if (pmp->pm_flags & XTAFMNT_RONLY)
			panic("xtaf_sync: rofs mod");
	}
	/*
	 * Write back each (modified) denode.
	 */
	MNT_ILOCK(mp);
loop:
	MNT_VNODE_FOREACH(vp, mp, nvp) {
		VI_LOCK(vp);
		if (vp->v_type == VNON || (vp->v_iflag & VI_DOOMED)) {
			VI_UNLOCK(vp);
			continue;
		}
		MNT_IUNLOCK(mp);
		dep = VTODE(vp);
		if ((dep->de_flag &
		    (DE_ACCESS | DE_CREATE | DE_UPDATE | DE_MODIFIED)) == 0 &&
		    (vp->v_bufobj.bo_dirty.bv_cnt == 0 ||
		    waitfor == MNT_LAZY)) {
			VI_UNLOCK(vp);
			MNT_ILOCK(mp);
			continue;
		}
		error = vget(vp, LK_EXCLUSIVE | LK_NOWAIT | LK_INTERLOCK, td);
		if (error) {
			MNT_ILOCK(mp);
			if (error == ENOENT)
				goto loop;
			continue;
		}
		error = VOP_FSYNC(vp, waitfor, td);
		if (error)
			allerror = error;
		VOP_UNLOCK(vp, 0);
		vrele(vp);
		MNT_ILOCK(mp);
	}
	MNT_IUNLOCK(mp);

	/*
	 * Flush filesystem control info.
	 */
	if (waitfor != MNT_LAZY) {
		vn_lock(pmp->pm_devvp, LK_EXCLUSIVE | LK_RETRY);
		error = VOP_FSYNC(pmp->pm_devvp, waitfor, td);
		if (error)
			allerror = error;
		VOP_UNLOCK(pmp->pm_devvp, 0);
	}
	return (allerror);
}

static int
xtaf_fhtovp(struct mount *mp, struct fid *fhp, int flags, struct vnode **vpp)
{
	struct xtafmount *pmp = VFSTOXTAF(mp);
	struct defid *defhp = (struct defid *) fhp;
	struct denode *dep;
	int error;

	error = xtaf_deget(pmp, defhp->defid_dirclust, defhp->defid_dirofs, &dep);
	if (error) {
		*vpp = NULLVP;
		return (error);
	}
	*vpp = DETOV(dep);
	vnode_create_vobject(*vpp, dep->de_FileSize, curthread);
	return (0);
}

static struct vfsops xtaf_vfsops = {
	.vfs_mount =		xtaf_mount,
	.vfs_root =		xtaf_root,
	.vfs_statfs =		xtaf_statfs,
	.vfs_unmount =		xtaf_unmount,
	.vfs_sync =		xtaf_sync,
	.vfs_fhtovp =		xtaf_fhtovp,
};

VFS_SET(xtaf_vfsops, xtaf, 0);
MODULE_VERSION(xtaf, 1);
