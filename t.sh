# CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make menuconfig
# return

CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make -C ./tools/objtool/ all
# CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make -j10

# CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make -C ./tools/objtool/
# gdb --args ./tools/objtool/objtool --orc --link ./vmlinux.unstripped
# return
# cp ./vmlinux ./vmlinux.debug
# gcc tmp-link.c && ./a.out vmlinux.unstripped vmlinux-shifted
powerpc64le-linux-gnu-strip ./vmlinux
# rm vmlinux.strip.*
return

# export INSTALL_MOD_PATH=/home/pasta/Desktop/randomProjects/buildroot-pnv/output/target
# CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make -j100
# powerpc64le-linux-gnu-strip ./vmlinux
# CROSS_COMPILE=powerpc64-linux-gnu- ARCH=powerpc make modules_install
