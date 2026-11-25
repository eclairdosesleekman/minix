FSCK=   ${NETBSDSRCDIR}/sbin/fsck
PROG=	fsck_mfs
.PATH:	${NETBSDSRCDIR}/minix/fs/mfs
SRCS=	fsck.c fsck_mfs4.c \
	mfs4_fsck_core.c \
	mfs4_journal_core.c \
	mfs4_orphan_core.c \
	mfs4_xattr_core.c
CPPFLAGS+= -I${NETBSDSRCDIR}/minix/fs -I${NETBSDSRCDIR}/minix/fs/mfs -I${FSCK}
BINDIR=	/sbin
MAN=	fsck.mfs.1

.include <bsd.prog.mk>
