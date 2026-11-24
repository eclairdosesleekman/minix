CC?=cc
CFLAGS?=-O2
CFLAGS+=-std=c99 -Wall -Wextra -Werror -I../../include -I../../fs/mfs

TESTS=mfs4_journal_test mfs4_orphan_test
JOURNAL_CORE=../../fs/mfs/mfs4_journal_core.c
ORPHAN_CORE=../../fs/mfs/mfs4_orphan_core.c

all: ${TESTS}

mfs4_journal_test: mfs4_journal_test.c ${JOURNAL_CORE} ../../fs/mfs/mfs4_journal.h
	${CC} ${CFLAGS} -o $@ mfs4_journal_test.c ${JOURNAL_CORE}

mfs4_orphan_test: mfs4_orphan_test.c ${ORPHAN_CORE} ../../fs/mfs/mfs4_orphan_core.h ../../include/minix/mfs4_disk.h
	${CC} ${CFLAGS} -o $@ mfs4_orphan_test.c ${ORPHAN_CORE}

test: ${TESTS}
	./mfs4_journal_test
	./mfs4_orphan_test

clean:
	rm -f ${TESTS}

.PHONY: all test clean
