<$PLAN9/src/mkhdr
<|osxvers

TARG=sixeldraw

WSYSOFILES=\
	devdraw.$O\
	latin1.$O\
	mouseswap.$O\
	winsize.$O\

OFILES=$WSYSOFILES sixel.$O

HFILES=\
	devdraw.h\

<$PLAN9/src/mkone

$O.drawclient: drawclient.$O drawfcall.$O
	$LD -o $target $prereq

$O.mklatinkbd: mklatinkbd.$O
	$LD -o $target $prereq

latin1.$O: latin1.h

latin1.h: $PLAN9/lib/keyboard $O.mklatinkbd
	./$O.mklatinkbd -r $PLAN9/lib/keyboard | sed 's/, }/ }/' >$target

CLEANFILES=$O.macargv $O.mklatinkbd latin1.h

install: mklatinkbd.install
install:Q: 
	if [ $MACARGV ]; then
		mk $MKFLAGS macargv.install
	fi
