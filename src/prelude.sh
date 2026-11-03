# shellcheck shell=bash disable=SC2034,SC2154,SC2329
# SPDX-License-Identifier: MIT
#
# The helpers recipe functions (do_*, *_extra, post_install) get, as the bash prototype gave them.
# burn writes WORK/env.sh for every build: the configuration, these helpers, the recipe (sourced
# again: bash runs its functions), the build type's bt_* and the build's variables. Each phase is
# one bash process that sources env.sh and calls the function.

msg() { printf '==> %s\n' "$*" >&2; }
msg2() { printf '  -> %s\n' "$*" >&2; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }
in_list() {
	local n=$1 x
	shift
	for x; do [[ $x == "$n" ]] && return 0; done
	return 1
}
run_fn() {
	if declare -F "$1" >/dev/null; then "$1"; fi
	return 0
}
is_elf() { [[ $(LC_ALL=C head -c 4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n') == 7f454c46 ]]; }
vercmp() { "$BURN_SELF" vercmp "$1" "$2"; }

# features
use() { [[ ${FEAT[$1]:-0} == 1 ]]; }
use_enable() { if use "$1"; then echo "--enable-${2:-$1}"; else echo "--disable-${2:-$1}"; fi; }
use_with() { if use "$1"; then echo "--with-${2:-$1}"; else echo "--without-${2:-$1}"; fi; }
meson_feature() { if use "$1"; then echo "-D${2:-$1}=enabled"; else echo "-D${2:-$1}=disabled"; fi; }
meson_use() { if use "$1"; then echo "-D${2:-$1}=true"; else echo "-D${2:-$1}=false"; fi; }
cmake_use() { if use "$1"; then echo "-D$2=ON"; else echo "-D$2=OFF"; fi; }
has_option() { in_list "$1" "${options[@]}"; }

# bt_configure, bt_build, ... in a recipe function: the step of the build type, done by burn itself
# (`burn __bt`) with the values the function has now
__burn_bt() {
	local -a __a=("$__burn_type" "$1") __e=()
	local __v
	for __v in name SRC BUILD DESTDIR WORK JOBS out_of_tree CBUILD CHOST CARCH CROSS SYSROOT R \
		LOCALES MESON_AUTO_FEATURES CPU_CFLAGS CACHE_DIR LTO_ON CC CXX AR NM RANLIB STRIP OBJCOPY \
		PKG_CONFIG PKG_CONFIG_LIBDIR CFLAGS CXXFLAGS LDFLAGS RUSTFLAGS meson_subdir cmake_subdir; do
		if [[ -v $__v ]]; then __a+=("$__v=${!__v}"); fi
	done
	for __v in configure_args meson_args cmake_args make_args make_install_args cargo_args; do
		eval "__a+=(\"@$__v\" \"\${#${__v}[@]}\" \"\${${__v}[@]}\")"
	done
	if [[ $1 == configure ]]; then
		case $__burn_type in
			autotools) mapfile -t __e < <(cd "$(bt_dir)" && run_fn configure_extra) ;;
			cmake) mapfile -t __e < <(run_fn cmake_extra) ;;
			meson) mapfile -t __e < <(run_fn meson_extra) ;;
		esac
		__a+=(@extra "${#__e[@]}" "${__e[@]}")
	fi
	"$BURN_SELF" __bt "${__a[@]}"
}

# __burn_step PHASE [DIR]: the step, then DIR as the working directory, where the prototype's
# bt_* functions left the shell
__burn_step() {
	local __rc=0
	__burn_bt "$1" || __rc=$?
	if [[ -n ${2-} ]]; then cd "$2" 2>/dev/null || :; fi
	return "$__rc"
}

# cargo's variables (CARGO_HOME, RUSTFLAGS, ...) stay set after a cargo step, as they did
__burn_cargo_env() {
	local __x
	__x=$(__burn_bt env) && eval "$__x"
}

__burn_bt_define() {
	__burn_type=$1
	case $1 in
		autotools)
			bt_dir() { if ((out_of_tree)); then printf '%s\n' "$BUILD"; else printf '%s\n' "$SRC"; fi; }
			bt_configure() { __burn_step configure "$(bt_dir)"; }
			bt_build() { __burn_step build "$(bt_dir)"; }
			bt_check() { __burn_step check "$(bt_dir)"; }
			bt_install() { __burn_step install "$(bt_dir)"; }
			;;
		cargo)
			bt_prepare() { __burn_step prepare "$SRC" && __burn_cargo_env; }
			bt_build() { __burn_step build "$SRC" && __burn_cargo_env; }
			bt_check() { __burn_step check "$SRC" && __burn_cargo_env; }
			bt_install() { __burn_step install "$SRC" && __burn_cargo_env; }
			;;
		cmake | meson)
			bt_configure() { __burn_step configure; }
			bt_build() { __burn_step build; }
			bt_check() { __burn_step check; }
			bt_install() { __burn_step install; }
			;;
		make)
			bt_build() { __burn_step build "$SRC"; }
			bt_check() { __burn_step check "$SRC"; }
			bt_install() { __burn_step install "$SRC"; }
			;;
		python)
			bt_build() { __burn_step build "$SRC"; }
			bt_check() { __burn_step check "$SRC"; }
			bt_install() { __burn_step install; }
			;;
		custom | meta) bt_install() { __burn_step install; } ;;
	esac
}
