CC?=cc
CFLAGS?=-O2
CFLAGS+=-std=c99 -Wall -Wextra -Werror -I../../include -I../../fs/mfs

TEST=mfs4_journal_test
CORE=../../fs/mfs/mfs4_journal_core.c

all: ${TEST}

${TEST}: mfs4_journal_test.c ${CORE} ../../fs/mfs/mfs4_journal.h
	${CC} ${CFLAGS} -o $@ mfs4_journal_test.c ${CORE}

test: ${TEST}
	./${TEST}

clean:
	rm -f ${TEST}

.PHONY: all test clean
