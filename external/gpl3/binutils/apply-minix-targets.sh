#!/bin/sh
#
# Configure the MINIX x86_64 target in the fetched GNU binutils source tree.
# Do not use patch(1) hunks here: the upstream target tables move between
# binutils releases and downstream archives.
#

set -eu

srcdir=${1:?usage: apply-minix-targets.sh binutils-source-directory}
bfd=${srcdir}/bfd/config.bfd
gas=${srcdir}/gas/configure.tgt
ld=${srcdir}/ld/configure.tgt

for file in "$bfd" "$gas" "$ld"; do
	if [ ! -f "$file" ]; then
		echo "binutils MINIX target setup: missing $file" >&2
		exit 1
	fi
done

# Insert a target case immediately before a stable generic x86_64/i386
# target entry.  The match is textual, not tied to a source line number or
# to the exact set of neighbouring OS patterns.
insert_target()
{
	file=$1
	marker=$2
	kind=$3
	present=$4

	if grep -Fq "$present" "$file"; then
		return 0
	fi

	tmp=${file}.minix-targets.$$
	trap 'rm -f "$tmp"' EXIT HUP INT TERM

	awk -v marker="$marker" -v kind="$kind" '
		!done && index($0, marker) {
			if (kind == "bfd") {
				print "      x86_64-*-minix*)"
				print "        targ_defvec=x86_64_elf64_vec"
				print "        targ_selvecs=\"i386_elf32_vec iamcu_elf32_vec x86_64_elf32_vec\""
				print "        want64=true"
				print "        ;;"
			} else if (kind == "gas") {
				print "      i386-*-minix*) fmt=elf ;;"
			} else if (kind == "ld") {
				print "    x86_64-*-minix*)"
				print "            targ_emul=elf_x86_64"
				print "            targ_extra_emuls=\"elf32_x86_64 elf_i386 elf_iamcu\""
				print "            targ_extra_libpath=\"elf_i386 elf32_x86_64\""
				print "            tdir_elf_i386=`echo ${targ_alias} | sed -e '\''s/x86_64/i386/'\''`"
				print "            ;;"
			}
			done=1
		}
		{ print }
		END {
			if (!done)
				exit 1
		}
	' "$file" > "$tmp" || {
		echo "binutils MINIX target setup: cannot find anchor '$marker' in $file" >&2
		rm -f "$tmp"
		trap - EXIT HUP INT TERM
		exit 1
	}

	mv "$tmp" "$file"
	trap - EXIT HUP INT TERM
}

insert_target "$bfd" 'x86_64-*-elf*' bfd 'x86_64-*-minix*)'
insert_target "$gas" 'i386-*-gnu*' gas 'i386-*-minix*)'
insert_target "$ld" 'x86_64-*-elf*' ld 'x86_64-*-minix*)'

# Fail early if any insertion did not take effect.
grep -Fq 'x86_64-*-minix*)' "$bfd"
grep -Fq 'i386-*-minix*)' "$gas"
grep -Fq 'x86_64-*-minix*)' "$ld"

exit 0
