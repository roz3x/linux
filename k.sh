ARCH=powerpc CROSS_COMPILE=powerpc64le-linux-gnu- make  ./tools/objtool && ./tools/objtool/objtool --link --alt-fixup ./vmlinux.unstripped



ARCH=powerpc CROSS_COMPILE=powerpc64le-linux-gnu- make clean defconfig 
ARCH=powerpc CROSS_COMPILE=powerpc64le-linux-gnu- make 

cp vmlinux.unstripped vmlinux.unstripped.orig

ARCH=powerpc CROSS_COMPILE=powerpc64le-linux-gnu- make ./tools/objtool
cp vmlinux.unstripped.orig vmlinux.unstripped && ./tools/objtool/objtool --link --alt-fixup ./vmlinux.unstripped && cp vmlinux.unstripped vmlinux && powerpc64le-linux-gnu-strip ./vmlinux
qemu-system-ppc64 -kernel ./vmlinux -nographic 