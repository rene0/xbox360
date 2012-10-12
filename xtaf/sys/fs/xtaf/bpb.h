/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: bpb.h,v 1.7 1997/11/17 15:36:24 ws Exp $	*/

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

#ifndef _XTAF_BPB_H_
#define _XTAF_BPB_H_

/*
 * BPB for XTAF (based on FATX description and images)
 */
struct bpb_xtaf {
	u_int8_t	bpbIdent[4];	/* XTAF identification string */
	u_int32_t	bpbVolId;	/* volume id */
	u_int32_t	bpbSecPerClust;	/* (512 byte) sectors per cluster */
	u_int32_t	bpbFATs;	/* number of FATs, always 1 */
	u_int16_t	bpbFill;	/* unknown, 0 */
					/* ignore the 4078 byte filler */
};

/*
 * The following structures represent how the bpb's look on disk.  shorts
 * and longs are just character arrays of the appropriate length.  This is
 * because the compiler forces shorts and longs to align on word or
 * halfword boundaries.
 */

/*
 * BPB for XTAF (byte version)
 */
struct byte_bpb_xtaf {
	u_int8_t	bpbIdent[4];	/* XTAF identification string */
	u_int8_t	bpbVolId[4];	/* volume id */
	u_int8_t	bpbSecPerClust[4];	/* (512 byte) sectors per cluster */
	u_int8_t	bpbFATs[4];	/* number of FATs always 1 */
	u_int8_t	bpbFill[2];	/* unknown, 0 */
					/* ignore the 4078 byte filler */
};

#endif /* _XTAF_BPB_H_ */
