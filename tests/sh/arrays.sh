# arrays: multi-line, comments, append, element 0, expansions of whole arrays
a=(one two 'three four' "five $HOME")
b=(
	first # a comment
	second
	# a comment line
	"third"
)
c=()
c+=(x)
c+=(y z)
d=(p q r)
d=s
e=(${a[@]})
f=("${a[@]}")
g="${a[*]}"
h=${a[1]}
i=${#a[@]}
j=${#c[*]}
k=("${c[@]}" "${empty_arr[@]}" end)
empty_arr=()
l=("${empty_arr[@]}")
m=(x "${empty_arr[@]}" y)
n=("${a[@]:1:2}")
o=("${d[@]: -2}")
p=("${a[@]%r*}")
q=("${c[@]/#/pre-}")
r=("${c[@]/%/-post}")
s=("${c[@]^^}")
t=(${b[0]} ${b[2]})
u=${a}
v=("${a[2]}"x)
w=(a"${c[@]}"b)
x=x
x+=(y)
z=("${a[@]//o/0}")
