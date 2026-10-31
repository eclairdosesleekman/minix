#!/bin/sh

# Make sure we're in our directory (i.e., where this shell script is)
echo $0
cd `dirname $0`

FETCH=ftp
which curl >/dev/null
if [ $? -eq 0 ]; then
	FETCH="curl -O -f"
fi

# Configure fetch method - TEXINFO
URL="http://www.minix3.org/pkgsrc/distfiles/minix/3.4.0/texinfo-4.8.tar.bz2"
BACKUP_URL="ftp://ftp.gnu.org/gnu/texinfo/texinfo-4.8.tar.bz2"

# Fetch sources if not available
if [ ! -d texinfo ];
then
	if [ ! -f texinfo-4.8.tar.bz2 ]; then
		$FETCH $URL
		if [ $? -ne 0 ]; then
			$FETCH $BACKUP_URL
		fi
	fi

	tar -xjf texinfo-4.8.tar.bz2 && \
	cd texinfo-4.8 && \
	cat ../../usr.bin/texinfo/patches/* | patch -p1 && \
	cd - && \
	mv texinfo-4.8 texinfo
fi

# Apply the glibc compatibility patch to an already-fetched source tree too.
# The extraction block above applies patches only when texinfo is first fetched.
# This makes the fix effective for existing worktrees without requiring a
# manual CLEANFETCHED=yes or deletion of the downloaded source directory.
if [ -f texinfo/lib/system.h ] &&
    grep -Fq 'extern int strcasecmp ();' texinfo/lib/system.h; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-glibc-strcasecmp)
fi

# Add the missing standard-library declaration to an already-fetched tree too.
if [ -f texinfo/lib/xexit.c ] &&
    ! grep -Fq '#include <stdlib.h>' texinfo/lib/xexit.c; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-xexit-stdlib)
fi
