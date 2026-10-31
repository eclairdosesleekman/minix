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

# Fix the old no-argument getenv declaration in already-fetched Texinfo trees.
if [ -f texinfo/lib/system.h ] &&
    grep -Fq 'extern char *getenv ();' texinfo/lib/system.h; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-glibc-getenv)
fi

# Add the standard allocation and abort declarations to already-fetched trees.
if [ -f texinfo/lib/xmalloc.c ] &&
    ! grep -Fq '#include <stdlib.h>' texinfo/lib/xmalloc.c; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-xmalloc-stdlib)
fi

# Correct the obsolete two-argument __gen_tempname declaration in old Texinfo.
if [ -f texinfo/lib/mkstemp.c ] &&
    grep -Fq 'int __gen_tempname ();' texinfo/lib/mkstemp.c; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-mkstemp-prototype)
fi

# Add the standard allocation/free declarations to already-fetched makeinfo.
if [ -f texinfo/makeinfo/cmds.c ] &&
    ! grep -Fq '#include <stdlib.h>' texinfo/makeinfo/cmds.c; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-makeinfo-cmds-stdlib)
fi

# Add the standard free() declaration to already-fetched makeinfo.
if [ -f texinfo/makeinfo/defun.c ] &&
    ! grep -Fq '#include <stdlib.h>' texinfo/makeinfo/defun.c; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-makeinfo-defun-stdlib)
fi

# Make command handler prototypes explicit for C23/GCC 15 and later.
if [ -f texinfo/makeinfo/cmds.h ] &&
    ! grep -Fq 'typedef void COMMAND_FUNCTION (int, int, int);' texinfo/makeinfo/cmds.h; then
	(cd texinfo && patch -p1 < ../../usr.bin/texinfo/patches/patch-zz-texinfo-c23-command-functions)
fi
