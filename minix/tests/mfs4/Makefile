CC?=cc
CFLAGS?=-O2
CFLAGS+=-std=c99 -Wall -Wextra -Werror -I../../include -I../../fs/mfs

TESTS=mfs4_journal_test mfs4_orphan_test mfs4_xattr_test mfs4_acl_test mfs4_fsck_test
JOURNAL_CORE=../../fs/mfs/mfs4_journal_core.c
ORPHAN_CORE=../../fs/mfs/mfs4_orphan_core.c
XATTR_CORE=../../fs/mfs/mfs4_xattr_core.c

all: ${TESTS}

mfs4_journal_test: mfs4_journal_test.c ${JOURNAL_CORE} ../../fs/mfs/mfs4_journal.h
	${CC} ${CFLAGS} -o $@ mfs4_journal_test.c ${JOURNAL_CORE}

mfs4_orphan_test: mfs4_orphan_test.c ${ORPHAN_CORE} ../../fs/mfs/mfs4_orphan_core.h ../../include/minix/mfs4_disk.h
	${CC} ${CFLAGS} -o $@ mfs4_orphan_test.c ${ORPHAN_CORE}

mfs4_xattr_test: mfs4_xattr_test.c ${XATTR_CORE} ../../fs/mfs/mfs4_xattr_core.h ../../include/minix/mfs4_disk.h
	${CC} ${CFLAGS} -o $@ mfs4_xattr_test.c ${XATTR_CORE}

mfs4_acl_test: mfs4_acl_test.c ../../include/minix/posix_acl.h
	${CC} ${CFLAGS} -o $@ mfs4_acl_test.c

mfs4_fsck_test: mfs4_fsck_test.c ../../fs/mfs/mfs4_fsck_core.c ../../fs/mfs/mfs4_journal_core.c ../../fs/mfs/mfs4_orphan_core.c ../../fs/mfs/mfs4_xattr_core.c
	${CC} ${CFLAGS} -o $@ mfs4_fsck_test.c ../../fs/mfs/mfs4_fsck_core.c ../../fs/mfs/mfs4_journal_core.c ../../fs/mfs/mfs4_orphan_core.c ../../fs/mfs/mfs4_xattr_core.c

test: ${TESTS}
	./mfs4_journal_test
	./mfs4_orphan_test
	./mfs4_xattr_test
	./mfs4_acl_test
	./mfs4_fsck_test

clean:
	rm -f ${TESTS}

.PHONY: all test clean
