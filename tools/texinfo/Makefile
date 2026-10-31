#	$NetBSD: Makefile,v 1.11 2013/06/14 16:10:02 tsutsui Exp $

MODULE=		texinfo

CONFIGURE_ARGS=	--program-prefix=${_TOOL_PREFIX}

.include "${.CURDIR}/../Makefile.gnuhost"

# The GNU source fetch hook is only enabled when building on MINIX itself.
# Cross-builds hosted on Linux must apply the same compatibility patch here,
# before configure/build can read the fetched Texinfo 4.8 sources.
TEXINFO_GLIBC_PATCH=${NETBSDSRCDIR}/gnu/usr.bin/texinfo/patches/patch-zz-glibc-strcasecmp
TEXINFO_SYSTEM_H=${GNUHOSTDIST}/lib/system.h

.if exists(${TEXINFO_SYSTEM_H}) && exists(${TEXINFO_GLIBC_PATCH})
.texinfo_glibc_patch_done: ${TEXINFO_SYSTEM_H} ${TEXINFO_GLIBC_PATCH}
	@if grep -Fq 'extern int strcasecmp ();' ${TEXINFO_SYSTEM_H}; then (cd ${GNUHOSTDIST} && patch -p1 < ${TEXINFO_GLIBC_PATCH}); fi
	@touch ${.TARGET}

.configure_done: .texinfo_glibc_patch_done
.endif

.if !empty(.MAKE.OS:M*CYGWIN*) && target(install)
.END: install-texinfo
install-texinfo: 
	${HOST_SH} ${NETBSDSRCDIR}/tools/binstall/mkmanifest ${TOOLDIR}/bin/${_TOOL_PREFIX}install-info.exe
.endif
