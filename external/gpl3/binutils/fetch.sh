#!/bin/sh

# Make sure we're in our directory (i.e., where this shell script is).
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

# Configure fetch method.
URL="http://ftp.gnu.org/gnu/binutils/binutils-2.34.tar.bz2"
BACKUP_URL="https://sourceware.org/pub/binutils/releases/binutils-2.34.tar.bz2"
FETCH=ftp
if command -v curl >/dev/null 2>&1; then
	FETCH="curl -O -f"
fi

fetch_archive()
{
	if [ -f binutils-2.34.tar.bz2 ]; then
		return 0
	fi

	if ! $FETCH "$URL"; then
		$FETCH "$BACKUP_URL"
	fi
}

# Fetch and prepare the sources if they are not available.
if [ ! -d dist ]; then
	fetch_archive
	tar -oxjf binutils-2.34.tar.bz2
	mv binutils-2.34 dist

	(
		cd dist
		# Keep libiberty/testsuite: libiberty/configure generates
		# testsuite/Makefile from testsuite/Makefile.in even when tests
		# are not run.  Removing it makes config.status fail.
		find . -type d -name testsuite \
		    ! -path './libiberty/testsuite' -prune -exec rm -fr {} +
		find . -name .gitignore -delete
		cat ../patches/* | patch -p1
	)
fi

# Older fetch.sh versions removed every testsuite directory. Restore the
# one required by libiberty from the original archive for already-fetched
# source trees too.
if [ ! -f dist/libiberty/testsuite/Makefile.in ]; then
	fetch_archive
	mkdir -p dist/libiberty
	tar -oxjf binutils-2.34.tar.bz2 -C dist --strip-components=2 \
	    binutils-2.34/libiberty/testsuite
fi

if [ ! -f dist/libiberty/testsuite/Makefile.in ]; then
	echo "binutils fetch: libiberty/testsuite/Makefile.in is missing" >&2
	exit 1
fi

exit 0
