#	$NetBSD: Makefile,v 1.11 2013/06/14 16:10:02 tsutsui Exp $

MODULE=		texinfo

CONFIGURE_ARGS=	--program-prefix=${_TOOL_PREFIX}

.include "${.CURDIR}/../Makefile.gnuhost"

# Apply host compatibility patches before configuring the fetched
# Texinfo 4.8 source. This is required for Linux-hosted cross-builds too.
TEXINFO_GLIBC_PATCH=${NETBSDSRCDIR}/gnu/usr.bin/texinfo/patches/patch-zz-glibc-strcasecmp
TEXINFO_XEXIT_PATCH=${NETBSDSRCDIR}/gnu/usr.bin/texinfo/patches/patch-zz-xexit-stdlib
TEXINFO_SYSTEM_H=${GNUHOSTDIST}/lib/system.h
TEXINFO_XEXIT_C=${GNUHOSTDIST}/lib/xexit.c

.if exists(${TEXINFO_SYSTEM_H}) && exists(${TEXINFO_GLIBC_PATCH}) && \
    exists(${TEXINFO_XEXIT_C}) && exists(${TEXINFO_XEXIT_PATCH})
.texinfo_host_compat_done: ${TEXINFO_SYSTEM_H} ${TEXINFO_XEXIT_C} \
    ${TEXINFO_GLIBC_PATCH} ${TEXINFO_XEXIT_PATCH}
	@if grep -Fq 'extern int strcasecmp ();' ${TEXINFO_SYSTEM_H}; then (cd ${GNUHOSTDIST} && patch -p1 < ${TEXINFO_GLIBC_PATCH}); fi
	@if ! grep -Fq '#include <stdlib.h>' ${TEXINFO_XEXIT_C}; then (cd ${GNUHOSTDIST} && patch -p1 < ${TEXINFO_XEXIT_PATCH}); fi
	@touch ${.TARGET}

.configure_done: .texinfo_glibc_patch_done
.endif

.if !empty(.MAKE.OS:M*CYGWIN*) && target(install)
.END: install-texinfo
install-texinfo: 
	${HOST_SH} ${NETBSDSRCDIR}/tools/binstall/mkmanifest ${TOOLDIR}/bin/${_TOOL_PREFIX}install-info.exe
.endif
