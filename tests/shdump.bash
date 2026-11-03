# SPDX-License-Identifier: MIT
# bash FILE...: the bash side of tests/shcompat.sh. Sources the files the way the bash prototype
# sourced recipes (nounset, nullglob, extglob) and prints the variables the files set, changed or
# unset and the functions they define, in the format of tests/shdump.c.
set -u
shopt -s nullglob extglob
__skip() {
	case $1 in
		__* | _ | BASH* | FUNCNAME | PIPESTATUS | LINENO | RANDOM | SRANDOM | SECONDS | EPOCH* | \
			HISTCMD | OPTIND | OPTARG | COMP_* | COLUMNS | LINES) return 0 ;;
	esac
	return 1
}
__put() {
	local s=${1//\\/\\\\}
	s=${s//$'\n'/\\n}
	printf '  <%s>\n' "$s"
}
__dump() {
	local __n __d __flags __x __e
	local -a __vals
	for __n in $(compgen -v | LC_ALL=C sort); do
		__skip "$__n" && continue
		__d=$(declare -p "$__n" 2>/dev/null) || continue
		[[ ${__snap[$__n]+set} && ${__snap[$__n]} == "$__d" ]] && continue
		__flags=${__d#declare -}
		__flags=${__flags%% *}
		__x=''
		[[ $__flags == *x* ]] && __x=' export'
		if [[ $__flags == *a* ]]; then
			eval "__vals=(\"\${${__n}[@]}\")"
			printf '%s array%s %d\n' "$__n" "$__x" "${#__vals[@]}"
			for __e in "${__vals[@]}"; do __put "$__e"; done
		elif [[ $__d == *=* ]]; then
			printf '%s scalar%s\n' "$__n" "$__x"
			__put "${!__n}"
		fi
	done
	for __n in $(printf '%s\n' "${!__snap[@]}" | LC_ALL=C sort); do
		declare -p "$__n" >/dev/null 2>&1 || echo "unset $__n"
	done
}
declare -A __snap=()
for __n in $(compgen -v); do
	__skip "$__n" || __snap[$__n]=$(declare -p "$__n" 2>/dev/null)
done
__funcs_before=$(declare -F | LC_ALL=C sort)
for __f; do
	# shellcheck source=/dev/null
	. "$__f" || {
		echo "error: bash failed on $__f"
		exit 1
	}
done
__dump
LC_ALL=C comm -13 <(printf '%s\n' "$__funcs_before") <(declare -F | LC_ALL=C sort) |
	sed 's/^declare -f /function /'
