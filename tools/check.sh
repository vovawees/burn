#!/usr/bin/env bash
# Local checks of burn: run them before every push (there is no hosted CI).
#   tools/check.sh [--recipes DIR] [--quick] [--btrfs]
# build with warnings as errors and `make check` (every recipe against bash too) with gcc and clang,
# the tests again under ASan + UBSan, every ported command against the bash prototype (when
# ../CleanLinux-prototype exists: git clone --branch burn-prototype https://github.com/vovawees/CleanLinux
# ../CleanLinux-prototype), and with --btrfs snapshots on a real btrfs (root, btrfs-progs).
# --quick: gcc only, no sanitizers and no prototype (tools/hooks/pre-push).
# The recipes default to ../CleanLinux-Recipes/packages.
set -euo pipefail
cd "$(dirname "$0")/.."
RECIPES=../CleanLinux-Recipes/packages QUICK=0 BTRFS=0
while (($#)); do
	case $1 in
		--recipes) RECIPES=$2 && shift 2 ;;
		--quick) QUICK=1 && shift ;;
		--btrfs) BTRFS=1 && shift ;;
		-h | --help) sed -n '2,9p' "$0" | cut -c3- && exit 0 ;;
		*) echo "tools/check.sh: unknown argument: $1" >&2 && exit 2 ;;
	esac
done
step() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }
if [[ ! -d $RECIPES ]]; then
	echo "no recipes in $RECIPES: git clone https://github.com/vovawees/CleanLinux-Recipes ../CleanLinux-Recipes (or --recipes DIR)" >&2
	exit 1
fi
trap 'make -s clean >/dev/null 2>&1; make -s >/dev/null 2>&1 || true' EXIT

ccs=(gcc)
if ((!QUICK)) && command -v clang >/dev/null; then ccs+=(clang); fi
for cc in "${ccs[@]}"; do
	step "build with $cc, warnings are errors"
	make -s clean && make -s CC="$cc" WERROR=1
	step "make check ($cc)"
	make -s CC="$cc" WERROR=1 check RECIPES="$RECIPES"
done
((QUICK)) && exit 0

step "tests under ASan + UBSan"
make -s clean
make -s CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	LDFLAGS="-fsanitize=address,undefined" check RECIPES="$RECIPES"
if [[ -d ../CleanLinux-prototype ]]; then
	step "every ported command against the bash prototype"
	make -s clean && make -s WERROR=1 proto RECIPES="$RECIPES"
else
	echo "(no ../CleanLinux-prototype: the comparison with the bash prototype is skipped)"
fi
if ((BTRFS)); then
	step "snapshots on a loop-mounted btrfs"
	make -s clean && make -s WERROR=1 check-btrfs
fi
