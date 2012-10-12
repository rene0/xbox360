/* XXX legal */
/* $FreeBSD: */
/*	$NetBSD: direntry.h,v 1.14 1997/11/17 15:36:32 ws Exp $	*/

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

#ifndef _XTAF_DIRENTRY_H_
#define _XTAF_DIRENTRY_H_

/*
 * Structure of an XTAF directory entry.
 */
struct direntry {
	u_int8_t	deLength;	/* filename length, <= 42 */
#define	LEN_DELETED	0xe5		/* length for deleted files */
	u_int8_t	deAttributes;	/* file attributes */
#define	ATTR_READONLY	0x01		/* file is readonly */
#define	ATTR_HIDDEN	0x02		/* file is hidden */
#define	ATTR_SYSTEM	0x04		/* file is a system file */
#define	ATTR_VOLUME	0x08		/* entry is a volume label */
#define	ATTR_DIRECTORY	0x10		/* entry is a directory name */
#define	ATTR_ARCHIVE	0x20		/* file is new or modified */
	u_int8_t	deName[42];	/* filename, 0x00 or 0xff filled */
#define	SLOT_EMPTY	0xff		/* slot has never been used */
	u_int8_t	deStartCluster[4]; /* starting cluster of file */
	u_int8_t	deFileSize[4];	/* size of file in bytes */
	u_int8_t	deCDate[2];	/* create date */
	u_int8_t	deCTime[2];	/* create time */
	u_int8_t	deADate[2];	/* access date */
	u_int8_t	deATime[2];	/* access time */
	u_int8_t	deMDate[2];	/* last update date */
	u_int8_t	deMTime[2];	/* last update time */
};

#ifdef _KERNEL
int	xtaf2unixfn(u_char dn[42], u_char *un);
int	unix2xtaffn(const u_char *un, u_char dn[43], size_t unlen, u_char pad);
#endif	/* _KERNEL */

#endif	/* _XTAF_DIRENTRY_H_ */
