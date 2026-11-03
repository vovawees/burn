# function definitions are recorded, their bodies are not run
one() { echo one; }
two() {
	local x=1
	if true; then echo "$x"; fi
	for i in 1 2; do :; done
	echo "$(date)" `pwd`
}
three()
{
	case $1 in
		a) { echo nested; } ;;
	esac
}
after=1
four () { :; }
unset -f four
two() { :; }
do_build() {
	make -j"$JOBS" \
		V=1
}
x=2
