#!/bin/sh

# -s ~/src/linux -j 8 -d /

set -eu

progname=${0##*/}

srcdir=$(pwd)
config=
arch=
flavor=mainline
release=0
jobs=
objdir=build
destdir=
stage=all
priv=
splitdbg=0
strip=1
epoch=
interactive=0
runhooks=1
counter=
buildno=
ccache=auto
ccachebin=

usage() {
	cat <<EOF
Usage: $progname [options]

  -s DIR    kernel source tree            (default: cwd, currently $srcdir)
  -c FILE   config to start from          (default: /boot/config-\$(uname -r))
  -a ARCH   kernel ARCH                   (default: derived from uname -m)
  -f NAME   flavor, becomes part of the   (default: $flavor)
            localversion: -RELEASE-NAME
  -r NUM    release number for the above  (default: $release)
  -j N      parallel jobs                 (default: online CPU count)
  -o DIR    object dir, relative to -s    (default: $objdir)
  -d DIR    install destdir; unset means  (default: none, install skipped)
            do not install. Use -d / to
            install onto the live system.
  -t STAGE  prepare | config | build | install | all   (default: $stage)
  -u CMD    privilege escalation command used only for the
            install stage       (default: doas, else sudo, else none)
  -e SECS   SOURCE_DATE_EPOCH for a reproducible build (default: unset)
  -C MODE   ccache: auto | on | off       (default: auto)
  -D        keep split debug info + System.map (default: off, saves ~15G)
  -S        do not strip modules
  -i        answer new Kconfig symbols by hand instead of taking defaults
  -H        skip running the kernel.d hooks after installing to /
  -h        this help
EOF
}

die() {
	echo "$progname: $*" >&2
	exit 1
}

while getopts s:c:a:f:r:j:o:d:t:e:u:C:DSiHh opt; do
	case $opt in
	s) srcdir=$OPTARG ;;
	c) config=$OPTARG ;;
	a) arch=$OPTARG ;;
	f) flavor=$OPTARG ;;
	r) release=$OPTARG ;;
	j) jobs=$OPTARG ;;
	o) objdir=$OPTARG ;;
	d) destdir=$OPTARG ;;
	t) stage=$OPTARG ;;
	e) epoch=$OPTARG ;;
	u) priv=$OPTARG ;;
	C) ccache=$OPTARG ;;
	D) splitdbg=1 ;;
	S) strip=0 ;;
	i) interactive=1 ;;
	H) runhooks=0 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

case $stage in
prepare|config|build|install|all) ;;
*) die "unknown stage '$stage'" ;;
esac

case $ccache in
auto|on|off) ;;
*) die "unknown ccache mode '$ccache', want auto, on or off" ;;
esac

command -v chimera-buildkernel >/dev/null 2>&1 ||
	die "chimera-buildkernel not found; run: apk add base-kernel-devel"

[ -d "$srcdir" ] || die "source tree '$srcdir' does not exist"
srcdir=$(realpath "$srcdir")
[ -f "$srcdir/Kconfig" ] || die "'$srcdir' is not a kernel source tree"

if [ -z "$jobs" ]; then
	jobs=$(ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | wc -l)
	[ "$jobs" -ge 1 ] 2>/dev/null || jobs=1
fi

if [ -z "$config" ]; then
	config=/boot/config-$(uname -r)
	[ -r "$config" ] ||
		die "no -c given and '$config' is unreadable; point -c at a config"
fi
config=$(realpath "$config")

seed=$srcdir/.config-seed-$flavor
counter=$srcdir/.build-version

patch_bsd_date() {
	f=$srcdir/usr/gen_initramfs.sh
	[ -f "$f" ] || return 0
	if grep -q 'date -j -f' "$f"; then
		echo "=> usr/gen_initramfs.sh already carries the BSD date fix"
		return 0
	fi
	grep -q 'date -d' "$f" || {
		echo "=> usr/gen_initramfs.sh no longer uses date -d so nothing to do"
		return 0
	}
	echo "=> patching usr/gen_initramfs.sh for BSD date"
	gsed -i \
		's|date -d"$1" +%s|date -j -f "%a %b %e %H:%M:%S UTC %Y" "$1" +%s|' \
		"$f"
	grep -q 'date -j -f' "$f" || die "BSD date patch did not apply"
}

patch_tools_makeoverrides() {
	f=$srcdir/Makefile
	if grep -q 'tools_silent' "$f"; then
		echo "=> Makefile already carries the cports MAKEFLAGS revert"
		return 0
	fi
	if grep -q 'tools/%: MAKEOVERRIDES' "$f"; then
		echo "=> Makefile already isolates the tools/ submake"
		return 0
	fi
	if ! grep -q '^tools/: FORCE$' "$f"; then
		echo "=> WARNING: no 'tools/: FORCE' rule found; skipping tools/ fix" >&2
		return 0
	fi
	echo "=> isolating tools/ submake from command-line CFLAGS"
	awk '/^tools\/: FORCE$/ && !ins { print "tools/ tools/%: MAKEOVERRIDES ="; print ""; ins=1 } { print }' \
		"$f" > "$f.cbk.new" || die "failed to rewrite Makefile"
	mv "$f.cbk.new" "$f" || die "failed to replace Makefile"
	grep -q 'tools/%: MAKEOVERRIDES' "$f" || die "tools/ fix did not apply"
}

patch_asm_headers_race() {
	f=$srcdir/scripts/Makefile.asm-headers
	[ -f "$f" ] || return 0
	if grep -q "! -name '\.\*'" "$f"; then
		echo "=> asm-headers already skips transient dotfiles"
		return 0
	fi
	grep -q -- "-name \*\.h)" "$f" || {
		echo "=> asm-headers scan looks different so skipping race fix" >&2
		return 0
	}
	echo "=> excluding temp dotfiles from the asm-headers stale scan"
	gsed -i "s@-name \*\.h)@-name '*.h' ! -name '.*')@" "$f"
	grep -q "! -name '\.\*'" "$f" || die "asm-headers race fix did not apply"
}

add_wrapper() {
	_name=$1
	_impl=$2
	_prep=$srcdir/.chimera_prepare_done
	[ -r "$_prep" ] || return 0
	_wrap=$(cat "$_prep")/wrappers
	[ -d "$_wrap" ] || return 0
	[ -e "$_wrap/$_name" ] && return 0
	_path=$(command -v "$_impl" 2>/dev/null) || {
		echo "=> $_impl not installed; not wrapping $_name (apk add $_impl)" >&2
		return 0
	}
	ln -sf "$_path" "$_wrap/$_name" &&
		echo "=> wrapped $_impl as $_name"
}

add_shim() {
	_name=$1
	_prep=$srcdir/.chimera_prepare_done
	[ -r "$_prep" ] || return 0
	_wrap=$(cat "$_prep")/wrappers
	[ -d "$_wrap" ] || return 0
	_real=$(command -v "$_name" 2>/dev/null) || {
		echo "=> no $_name in PATH; not shimming it" >&2
		return 0
	}
	case $_real in
	"$_wrap"/*) return 0 ;;
	esac
	"shim_$_name" "$_real" >"$_wrap/$_name.new" ||
		die "could not generate the $_name shim"
	chmod 0755 "$_wrap/$_name.new" || die "could not chmod the $_name shim"
	mv "$_wrap/$_name.new" "$_wrap/$_name" || die "could not install the $_name shim"
	echo "=> shimmed $_name for GNU callers (real one: $_real)"
}

shim_stat() {
	cat <<EOF
#!/bin/sh
real_stat='$1'
EOF
	cat <<'EOF'

fatal() {
	echo "stat (chimera build shim): $*" >&2
	exit 64
}

_gnu=0
for _a in "$@"; do
	case $_a in
	-c|-c?*|--format|--format=*|--printf|--printf=*) _gnu=1 ;;
	esac
done
[ "$_gnu" = 1 ] || exec "$real_stat" "$@"

_fmt=
_opts=
_argc=$#
while [ "$_argc" -gt 0 ]; do
	_arg=$1
	shift
	_argc=$((_argc - 1))
	case $_arg in
	-c|--format)
		[ "$_argc" -gt 0 ] || fatal "$_arg needs a format argument"
		_fmt=$1
		shift
		_argc=$((_argc - 1))
		;;
	-c?*) _fmt=${_arg#-c} ;;
	--format=*) _fmt=${_arg#--format=} ;;
	--printf|--printf=*)
		fatal "--printf has no BSD equivalent (cannot drop the trailing newline)"
		;;
	-L|--dereference) _opts="$_opts -L" ;;
	--)
		while [ "$_argc" -gt 0 ]; do
			set -- "$@" "$1"
			shift
			_argc=$((_argc - 1))
		done
		;;
	-*) fatal "no translation for GNU option '$_arg'" ;;
	*) set -- "$@" "$_arg" ;;
	esac
done

_bsd=
while [ -n "$_fmt" ]; do
	_c=${_fmt%"${_fmt#?}"}
	_fmt=${_fmt#?}
	if [ "$_c" != "%" ]; then
		_bsd=$_bsd$_c
		continue
	fi
	[ -n "$_fmt" ] || fatal "format ends with a bare %"
	_s=${_fmt%"${_fmt#?}"}
	_fmt=${_fmt#?}
	case $_s in
	%) _bsd="$_bsd%%" ;;
	n) _bsd="$_bsd%N" ;;
	N) _bsd="$_bsd%N%SY" ;;
	s) _bsd="$_bsd%z" ;;
	b) _bsd="$_bsd%b" ;;
	B) _bsd="${_bsd}512" ;;
	o) _bsd="$_bsd%k" ;;
	d) _bsd="$_bsd%d" ;;
	i) _bsd="$_bsd%i" ;;
	h) _bsd="$_bsd%l" ;;
	u) _bsd="$_bsd%u" ;;
	U) _bsd="$_bsd%Su" ;;
	g) _bsd="$_bsd%g" ;;
	G) _bsd="$_bsd%Sg" ;;
	a) _bsd="$_bsd%OLp" ;;
	A) _bsd="$_bsd%Sp" ;;
	f) _bsd="$_bsd%Xp" ;;
	F) _bsd="$_bsd%HT" ;;
	t) _bsd="$_bsd%XHr" ;;
	T) _bsd="$_bsd%XLr" ;;
	X) _bsd="$_bsd%a" ;;
	Y) _bsd="$_bsd%m" ;;
	Z) _bsd="$_bsd%c" ;;
	W) _bsd="$_bsd%B" ;;
	x) _bsd="$_bsd%Sa" ;;
	y) _bsd="$_bsd%Sm" ;;
	z) _bsd="$_bsd%Sc" ;;
	*) fatal "no BSD equivalent for GNU format %$_s" ;;
	esac
done

exec "$real_stat" $_opts -f "$_bsd" -- "$@"
EOF
}

extra_wrappers() {
	add_wrapper grep ggrep
	add_shim stat
}

ccache_targets() {
	_out=clang
	_p=$srcdir/.chimera_prepare_done
	if [ -r "$_p" ]; then
		_d=$(cat "$_p")
		for _f in cc hostcc; do
			[ -r "$_d/$_f" ] || continue
			_v=$(cat "$_d/$_f")
			case $_v in
			''|*/*|*' '*) continue ;;
			esac
			case " $_out " in
			*" $_v "*) ;;
			*) _out="$_out $_v" ;;
			esac
		done
	fi
	echo "$_out"
}

setup_ccache() {
	[ "$ccache" = off ] && return 0

	_cc=$(command -v ccache 2>/dev/null) || {
		[ "$ccache" = on ] &&
			die "ccache requested but not installed (apk add ccache)"
		return 0
	}

	_p=$srcdir/.chimera_prepare_done
	if [ -r "$_p" ] && [ -s "$(cat "$_p")/cross" ]; then
		echo "=> cross build, leaving ccache out of PATH" >&2
		return 0
	fi

	_names=$(ccache_targets)
	ccachebin=$srcdir/.ccache-bin
	mkdir -p "$ccachebin" || die "could not create $ccachebin"
	for _n in $_names; do
		ln -sf "$_cc" "$ccachebin/$_n" ||
			die "could not link $_n in $ccachebin"
	done

	PATH=$ccachebin:$PATH
	export PATH
	echo "=> ccache enabled ($_cc), intercepting: $_names"
}

ccache_counts() {
	[ -n "$ccachebin" ] || return 0
	ccache --print-stats 2>/dev/null | awk '
		$1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" ||
		$1 == "remote_cache_hit" || $1 == "cache_hit_direct" ||
		$1 == "cache_hit_preprocessed" { h += $2; seen = 1 }
		$1 == "cache_miss" { m += $2; seen = 1 }
		END { if (seen) print h + 0, m + 0 }'
}

ccache_report() {
	[ -n "$1" ] && [ -n "$2" ] || return 0
	set -- $1 $2
	echo "=> ccache: $(($3 - $1)) hits, $(($4 - $2)) misses this build"
}

do_prepare() {
	patch_bsd_date
	patch_tools_makeoverrides
	patch_asm_headers_race

	cp "$config" "$seed"

	set -- \
		"CONFIG_FILE=$seed" \
		"OBJDIR=$objdir" \
		"FLAVOR=$flavor" \
		"RELEASE=$release" \
		"JOBS=$jobs" \
		"SPLIT_DBG=$splitdbg" \
		"STRIP=$strip"
	[ -n "$arch" ] && set -- "$@" "ARCH=$arch"
	[ -n "$epoch" ] && set -- "$@" "EPOCH=$epoch"

	echo "=> prepare: $srcdir -> $objdir (jobs=$jobs, flavor=$flavor)"

	if [ "$interactive" = 1 ]; then
		chimera-buildkernel prepare "$@"
	else
		yes '' | chimera-buildkernel prepare "$@"
	fi

	extra_wrappers
}

do_config() {
	echo "=> menuconfig"
	chimera-buildkernel config menuconfig
}

next_buildno() {
	if [ "${KBUILD_BUILD_VERSION+set}" = set ]; then
		echo "=> build #$KBUILD_BUILD_VERSION (from the environment)"
		return 0
	fi

	if [ -n "$epoch" ]; then
		export KBUILD_BUILD_VERSION=1
		echo "=> build #1 (pinned for the reproducible build)"
		return 0
	fi

	_n=$(cat "$counter" 2>/dev/null) || _n=
	case $_n in
	''|*[!0-9]*) _n=0 ;;
	esac

	buildno=$((_n + 1))
	export KBUILD_BUILD_VERSION=$buildno
	echo "=> build #$buildno"
}

save_buildno() {
	[ -n "$buildno" ] || return 0
	echo "$buildno" >"$counter" ||
		echo "=> WARNING: could not record the build number in $counter" >&2
}

objdir_buildno() {
	_f=$srcdir/$objdir/include/generated/utsversion.h
	[ -r "$_f" ] || return 0
	sed -n 's/.*UTS_VERSION "\(#[0-9]*\).*/\1/p' "$_f"
}

do_build() {
	extra_wrappers
	next_buildno
	echo "=> build"
	_ccbefore=$(ccache_counts)
	chimera-buildkernel build
	save_buildno
	ccache_report "$_ccbefore" "$(ccache_counts)"
}

kernver() {
	cat "$srcdir/$objdir/include/config/kernel.release"
}

refresh_initramfs() {
	_kv=$1

	echo "=> dropping stale initramfs for $_kv"
	$priv rm -f "/boot/initrd.img-$_kv" "/boot/initramfs-$_kv.img" ||
		die "could not remove the old initramfs for $_kv"

	echo "=> running kernel.d hooks (depmod, initramfs, bootloader)"
	$priv /usr/lib/base-kernel/run-kernel-d || die "kernel.d hooks failed"

	if [ ! -f "/boot/initrd.img-$_kv" ]; then
		echo "=> hooks made no initramfs; calling update-initramfs" >&2
		command -v update-initramfs >/dev/null 2>&1 ||
			die "no initramfs for $_kv and no update-initramfs to build one"
		$priv update-initramfs -c -k "$_kv" ||
			die "update-initramfs -c -k $_kv failed - do not reboot yet"
	fi

	echo "=> initramfs: /boot/initrd.img-$_kv"
}

do_install() {
	[ -n "$destdir" ] || die "install stage needs -d DESTDIR"
	kv=$(kernver)
	[ -n "$kv" ] || die "empty release in $objdir/include/config/kernel.release"

	hdrdir=${destdir%/}/usr/src/linux-headers-$kv
	if [ -e "$hdrdir" ]; then
		echo "=> removing stale headers tree $hdrdir"
		$priv rm -rf "$hdrdir" || die "could not remove $hdrdir"
	fi

	echo "=> install $kv -> $destdir${priv:+ (via $priv)}"
	$priv chimera-buildkernel install "$destdir"

	if [ "$destdir" = "/" ] && [ "$runhooks" = 1 ]; then
		refresh_initramfs "$kv"
	fi

	echo ""
	echo "Installed kernel version: $kv"
	_bn=$(objdir_buildno)
	[ -n "$_bn" ] && echo "Build: $_bn"
	if [ "$destdir" = "/" ]; then
		echo "Modules: /usr/lib/modules/$kv"
	fi
}

resolve_priv() {
	[ -n "$destdir" ] || return 0
	if [ "$(id -u)" = 0 ]; then
		priv=
		return 0
	fi
	if [ -n "$priv" ]; then
		command -v "${priv%% *}" >/dev/null 2>&1 ||
			die "escalation command '$priv' not found"
		return 0
	fi
	[ -w "$destdir" ] && return 0
	for _c in doas sudo; do
		if command -v "$_c" >/dev/null 2>&1; then
			priv=$_c
			return 0
		fi
	done
	die "'$destdir' needs root; install neither doas nor sudo, pass -u CMD, or run the install stage as root"
}

case $stage in
install|all) resolve_priv ;;
esac

case $stage in
prepare|config|build|all) setup_ccache ;;
esac

cd "$srcdir"

case $stage in
prepare) do_prepare ;;
config)  do_config ;;
build)   do_build ;;
install) do_install ;;
all)
	do_prepare
	do_build
	[ -n "$destdir" ] && do_install
	;;
esac

exit 0
