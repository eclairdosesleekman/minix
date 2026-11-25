FSCK=   ${NETBSDSRCDIR}/sbin/fsck
PROG=	fsck_mfs
SRCS=	fsck.c fsck_mfs4.c \
	${NETBSDSRCDIR}/minix/fs/mfs/mfs4_fsck_core.c \
	${NETBSDSRCDIR}/minix/fs/mfs/mfs4_journal_core.c \
	${NETBSDSRCDIR}/minix/fs/mfs/mfs4_orphan_core.c \
	${NETBSDSRCDIR}/minix/fs/mfs/mfs4_xattr_core.c
CPPFLAGS+= -I${NETBSDSRCDIR}/minix/fs -I${NETBSDSRCDIR}/minix/fs/mfs -I${FSCK}
BINDIR=	/sbin
MAN=	fsck.mfs.1

.include <bsd.prog.mk>
