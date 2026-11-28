~/Desktop/randomProjects/qemu/build/qemu-system-ppc64  \
        -M pseries,x-vof=on -serial mon:stdio \
        -cpu POWER9 -m 1G -kernel vmlinux \
        -append "console=hvc0 rootwait root=/dev/sda crashkernel=2G smt=off fadump=on" \
        -drive file=/home/pasta/Desktop/randomProjects/buildroot/./output/images/rootfs.ext2,if=scsi,index=0,format=raw \
        -display curses \
        -qmp unix:/tmp/qmp-sock,nowait,server=on  -smp 1,maxcpus=100 \
        -nographic -s -S \
        # -netdev user,id=net0,hostfwd=tcp::2222-:22 -device e1000e,netdev=net0 \
