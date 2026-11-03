# scalars, quoting, escapes, several assignments per line
a=plain
b='single $quoted'
c="double $a ${a}"
d=con"cat"'en'ated$a
e=  # empty
f=""
g=$'ansi\ttab\nnewline \x41\101 \' \\ \cA\e'
h="esc \$a \"q\" \\ \` x\
y"
i=back\ slash\ space
j=a k=b ; l=c
m=1; m+=2; m+="3 4"
n+=new
o=a=b=c
p=x#notacomment q=#hash
r='multi
line'
s="it's" t='say "hi"'
u=$a/$b
v=${a}suffix
w=a\
b
x=$'\''
y=semi\;colon z="tab	in"
