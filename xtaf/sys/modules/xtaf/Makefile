# $FreeBSD: $

.PATH: ${.CURDIR}/../../fs/xtaf

CFLAGS+=	-DXTAF_DEBUG
KMOD=	xtaf
SRCS=	opt_xtaf.h vnode_if.h \
	xtaf_conv.c xtaf_denode.c xtaf_fat.c xtaf_lookup.c \
	xtaf_vfsops.c xtaf_vnops.c dot_lookup_table.c

.include <bsd.kmod.mk>
