/*XXX legal*/
/* $FreeBSD: */
/*	$NetBSD: msdosfs_conv.c,v 1.25 1997/11/17 15:36:40 ws Exp $	*/

/*-
 * Copyright (C) 1995, 1997 Wolfgang Solfrank.
 * Copyright (C) 1995, 1997 TooLs GmbH.
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

#include <fs/xtaf/direntry.h>

static u_char xtaf2unixchr(const u_char **, size_t *);
static u_char unix2xtafchr(const u_char **, size_t *);

/*
 * 1 - 'reserved' character, should be replaced by '_' in XTAF file name.
       " * + , / : ; < = > ? \ |
 * 2 - unproved legal character.
       ! # % & ' ( ) - / @ [ ] ^ ` { } ~ DEL
 */
static u_char
unix2xtaf[96] = {
	0x20, 2,    1,    2,    0x24, 2,    2,    2,	/* 20-27 */
	2,    2,    1,    1,    1,    2,    0x2e, 1,	/* 28-2f */
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,	/* 30-37 */
	0x38, 0x39, 1,    1,    1,    1,    1,    1,	/* 38-3f */
	2,    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,	/* 40-47 */
	0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,	/* 48-4f */
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,	/* 50-57 */
	0x58, 0x59, 0x5a, 2,    1,    2,    2,    0x5f, /* 58-5f */
	2,    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,	/* 60-67 */
	0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,	/* 68-6f */
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x57,	/* 70-77 */
	0x78, 0x79, 0x7a, 2,    1,    2,    2,    2,   	/* 78-7f */
};

/*
 * XTAF filenames are made of 1 part with a maximum length of 42
 * characters.  It may contain trailing blanks if it is not long
 * enough to fill its respective field.
 */

/*
 * Convert a XTAF filename to a unix filename. And, return the number of
 * characters in the resulting unix filename excluding the terminating
 * null.
 */
int
xtaf2unixfn(u_char dn[42], u_char *un)
{
	size_t i;
	int thislong = 0;

	/*
	 * Copy the name portion into the unix filename string.
	 */
	for (i = 42; i > 0 && *dn != 0x00 && *dn != 0xff;) {
		*un++ = xtaf2unixchr((const u_char **)&dn, &i);
		thislong++;
	}
	*un++ = 0;

	return (thislong);
}

/*
 * Convert a unix filename to a XTAF filename.
 * Returns
 *	0 if name couldn't be converted
 *	1 if the converted name is the same as the original
 *	2 if conversion was successful
 */
int
unix2xtaffn(const u_char *un, u_char dn[43], size_t unlen, u_char pad)
{
	ssize_t i, j;
	const u_char *cp;

	/*
	 * Fill the XTAF filename string with blanks. These are XTAF's pad
	 * characters.
	 */
	for (i = 0; i < 42; i++)
		dn[i] = pad;
	dn[42] = 0;

	/*
	 * The filenames "." and ".." are handled specially, since they
	 * don't follow XTAF filename rules.
	 */
	if (un[0] == '.' && unlen == 1) {
		dn[0] = '.';
		return (1);
	}
	if (un[0] == '.' && un[1] == '.' && unlen == 2) {
		dn[0] = '.';
		dn[1] = '.';
		return (1);
	}

	/*
	 * Filenames with only blanks and dots are not allowed !
	 */
	for (cp = un, i = unlen; --i >= 0; cp++)
		if (*cp != 0x00 && *cp != 0xff && *cp != '.')
			break;
	if (i < 0)
		return (0);


	/*
	 * Filenames with some characters are not allowed !
	 */
	for (cp = un, i = unlen; i > 0; )
		if (unix2xtafchr(&cp, (size_t *)&i) == 0)
			return (0);

	/*
	 * Now convert the name
	 */
	for (i = unlen, j = 0; i > 0 && j < 42; j++) {
		dn[j] = unix2xtafchr(&un, &i);
		if (dn[j] == 1 || dn[j] == 2)
			dn[j] = '_';
	}
	/*
	 * If we didn't have any chars in filename,
	 * generate a default
	 */
	if (!j)
		dn[0] = '_';

	return (2);
}

/*
 * Convert XTAF char to Local char
 */
static u_char
xtaf2unixchr(const u_char **instr, size_t *ilen)
{
	u_char c;

	(*ilen)--;
	c = *(*instr)++;
	return (c > 0x1f && c < 0x80 ? c : 0x3f); /* 0x3f == '?' in ISO 8859-1 */
}

/*
 * Convert Local char to XTAF char
 */
static u_char
unix2xtafchr(const u_char **instr, size_t *ilen)
{
	u_char c;

	(*ilen)--;
	c = *(*instr)++;
	return (c > 0x1f && c < 0x80 ? unix2xtaf[c - 0x20] : 0);
}
