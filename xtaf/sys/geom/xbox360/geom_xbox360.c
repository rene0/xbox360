/*-
 * Copyright (c) 2006-2008 Rene Ladan <rene@freebsd.org>
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <geom/geom.h>
#include <geom/geom_slice.h>

#define XBOX360_CLASS_NAME "XBOX360"

#define MD_64_SIZE	0x03e20000ULL
#define MD_256_SIZE	0x0f880000ULL /* XXX estimated */
#define MD_512_SIZE	0x1e200000ULL
/*
 * www.free60.org says there are both 18.63 GB (Samsung) and 20 GB
 * (Travelstar/Seagate) hard disks.
 *
 * It turns out that the Samsung and the Travelstar hd have the same capicity.
 * The Travelstar capicity is from www.prodimex.ch/partage/f_tech/23435.pdf
 */
#define	HD_20_SIZE	0x04a8530000ULL
#define HD_60_SIZE	0x0df8f90000ULL /* XXX estimated */
#define HD_120_SIZE	0x1bf1f20000ULL /* XXX estimated */

static off_t md_table[2] = {
	 0x0ULL,	/* cache */
	 0x7ff000ULL, 	/* user */
};

static off_t hd_table[5][2] = {
	{ 0ULL,		0, },	/* header (not a fs) */
	{ 0x80000ULL,	1, },	/* cache */
	{ 0x80080000ULL,  0, },	/* unknown (empty or fake fs with real sig) */
	{ 0x120eb0000ULL, 1, },	/* xbox 1 compatibility */
	{ 0x130eb0000ULL, 1, },	/* user */
};

static int
check_xtaf_sig(struct g_consumer *cp, u_int sectorsize, off_t off)
{
	u_char *buf;
	int ret;

	buf = g_read_data(cp, off, sectorsize, NULL);
	if (buf == NULL)
		return (1);
	ret = bcmp(buf, "XTAF", 4);
	g_free(buf);
	return (ret);
}

static struct g_geom *
g_xbox360_taste(struct g_class *mp, struct g_provider *pp, int insist)
{
	struct g_geom *gp;
	struct g_consumer *cp;
	int numslices;
	u_int sectorsize;
	unsigned long long size;
	off_t end, offset;
	int i, ret;

	g_trace(G_T_TOPOLOGY, "xbox360_taste(%s,%s)", mp->name, pp->name);
	g_topology_assert();
	if (!strcmp(pp->geom->class->name, XBOX360_CLASS_NAME))
		return (NULL);

	/* find out number of slices, skip invalid values */
	size = (unsigned long long)pp->mediasize;
	numslices = 0;
	if (size == MD_64_SIZE || size == MD_256_SIZE || size == MD_512_SIZE)
		numslices = 2;
	else if (size == HD_20_SIZE || size == HD_60_SIZE ||
	    size == HD_120_SIZE)
		numslices = 5;
	if (numslices == 0)
		return (NULL);

	gp = g_slice_new(mp, numslices, pp, &cp, NULL, 0, 0);
	if (gp == NULL)
		return (NULL);
	g_topology_unlock();
	sectorsize = cp->provider->sectorsize;
	if (sectorsize < DEV_BSIZE) {
		g_topology_lock();
		goto done;
	}

	/* look for the XTAF signatures */
	ret = 0;
	for (i = 0; i < numslices; i++)
		if (size == MD_64_SIZE || size == MD_256_SIZE ||
		    size == MD_512_SIZE)
			ret += check_xtaf_sig(cp, sectorsize,
			    md_table[i]);
		else if ((size == HD_20_SIZE || size == HD_60_SIZE ||
		    size == HD_120_SIZE) && hd_table[i][1])
			ret += check_xtaf_sig(cp, sectorsize,
			    hd_table[i][0]);
	g_topology_lock();
	if (ret > 0)
		goto done;

	/* fill the slices */
	offset = end = 0;
	for (i = 0; i < numslices; i++) {
		if (size == MD_64_SIZE || size == MD_256_SIZE ||
		    size == MD_512_SIZE) {
			offset = md_table[i];
			end = (i == numslices - 1 ? size :
			    md_table[i + 1]);
		} else if (size == HD_20_SIZE || size == HD_60_SIZE ||
			    size == HD_120_SIZE) {
			offset = hd_table[i][0];
			end = (i == numslices - 1 ? size :
			    hd_table[i + 1][0]);
		}
		g_slice_config(gp, i, G_SLICE_CONFIG_SET, offset,
		    end - offset, sectorsize, "%ss%d", gp->name, i + 1);
	}
done:
	g_access(cp, -1, 0, 0);
	if (LIST_EMPTY(&gp->provider)) {
		g_slice_spoiled(cp);
		return (NULL);
	}
	return (gp);
}

static struct g_class g_xbox360_class = {
	.name = XBOX360_CLASS_NAME,
	.version = G_VERSION,
	.taste = g_xbox360_taste,
};

DECLARE_GEOM_CLASS(g_xbox360_class, g_xbox360);
