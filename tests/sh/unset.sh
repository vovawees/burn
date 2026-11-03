# unset of variables, of environment variables and of functions
a=1 b=2 c=(x y)
unset a
unset -v c
unset TESTENV2
TESTENV2=back
unset nothing_here
f() { :; }
g() { :; }
unset -f f
b+=3
