# parameter expansion operators
v=hello.world.tar.gz
empty=
a1=${unset_var:-default} a2=${empty:-default} a3=${v:-default}
b1=${unset_var-default} b2=${empty-default} b3=${v-default}
c1=${unset_var:+alt} c2=${empty:+alt} c3=${v:+alt}
d1=${unset_var+alt} d2=${empty+alt} d3=${v+alt}
e1=${v#*.} e2=${v##*.} e3=${v%.*} e4=${v%%.*}
e5=${v#hello} e6=${v%.tar.gz} e7=${v#"*"} e8=${v%@(.gz|.xz)}
f1=${v/./_} f2=${v//./_} f3=${v/#hello/bye} f4=${v/%gz/xz} f5=${v//[.]/} f6=${v/world}
f7=${v//l/"L"} f8=${v/"hello"/$v}
g1=${v^} g2=${v^^} g3=${v^^}${v,,} G=HeLLo g4=${G,} g5=${G,,}
h1=${v:6} h2=${v:6:5} h3=${v: -2} h4=${v:0:-3} h5=${v:100} h6=${v: -100}
i1=${#v} i2=${#empty}
j1=${unset_var:-"quoted default"} j2="${unset_var:-$v}" j3=${unset_var:-${v%%.*}}
k=${v%.*.*}
l=${v#*[.]}
m=${TESTENV:-x} n=${TESTENV/from/to}
o="${v}" p="pre${v}post"
q=${v//?/x}
r=${v/+(l)/L}
s=${v%%+([a-z.])}
t=${v##*@(.tar)}
