#!/bin/bash -e

#
# Tested on ubuntu 20.n with no other cross tools installed other than
# 'gcc-aarch64-linux-gnu g++-aarch64-linux-gnu'. These are used for
# bootstrapping a bit faster
#

TOOLDIR=$BASE_DIR/buildtools

unset CROSS_COMPILE
unset CC
unset CXX
unset LD
unset AR
unset AS
unset OBJCOPY
unset RANLIB
unset CFLAGS
unset LDFLAGS
unset ASFLAGS
unset INCLUDES
unset WARNINGS
unset DEFINES

export PATH=$TOOLDIR/bin:$TOOLDIR/usr/bin:/bin:/usr/bin

KERNEL_PATCHFILE="$BASE_DIR/patches/0001-KVM-external-hypervisor-5.10-kernel-baseport.patch"
TTRIPLET="aarch64-linux-gnu"
HTRIPLET="x86_64-unknown-linux-gnu"
NJOBS=`nproc`

clean()
{
	cd $BASE_DIR/oss/binutils-gdb; git clean -xfd || true
	cd $BASE_DIR/oss/gcc; git clean -xfd || true
	cd $BASE_DIR/oss/glibc; git clean -xfd || true
	cd $BASE_DIR/oss/qemu; git clean -xfd || true
	cd $BASE_DIR/oss/linux; git clean -xfd || true
}

binutils-gdb()
{
	mkdir -p $BASE_DIR/oss/binutils-gdb/build
	cd $BASE_DIR/oss/binutils-gdb/build
	 ../configure --prefix=/usr --target=$TTRIPLET --host=$HTRIPLET --build=$HTRIPLET \
		      --disable-nls --disable-multilib --with-sysroot=$TOOLDIR
	make -j$NJOBS
	make DESTDIR=$TOOLDIR install
}

kernel_headers()
{
	cd $BASE_DIR/oss/linux
	OUT=$(git apply --check $KERNEL_PATCHFILE 2>&1 | wc -l)
	if [ $OUT != "0" ]; then
		echo "Skipping kernel patch, already applied?"
	else
		echo "Patching kernel"
		git am $KERNEL_PATCHFILE
	fi
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 INSTALL_HDR_PATH=$TOOLDIR/usr headers_install
}

kernel()
{
	cd $BASE_DIR/oss/linux
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j$NJOBS defconfig Image modules
}

glibc()
{
	mkdir -p $BASE_DIR/oss/glibc/build
	cd $BASE_DIR/oss/glibc/build
	../configure --prefix=/usr --host=$TTRIPLET --build=$HTRIPLET -without-cvs --disable-nls \
		     --disable-sanity-checks --enable-obsolete-rpc --disable-profile --disable-debug \
		     --without-selinux --without-tls --with-arch=armv8-a --enable-threads=posix \
		     --with-headers=$TOOLDIR/usr/include --disable-werror
	make -j$NJOBS
	make DESTDIR=$TOOLDIR install
	make DESTDIR=$TOOLDIR install-headers
}

gcc()
{
	mkdir -p $BASE_DIR/oss/gcc/build
	cd $BASE_DIR/oss/gcc/build
	../configure --prefix=/usr --target=$TTRIPLET --host=$HTRIPLET --build=$HTRIPLET \
		     --disable-nls --enable-threads --disable-plugins --disable-multilib \
		     --disable-bootstrap --disable-libsanitizer --enable-languages=c,c++ \
		     --with-sysroot=/
	make -j$NJOBS
	make DESTDIR=$TOOLDIR install
}

qemu()
{
	mkdir -p $BASE_DIR/oss/qemu/build
	cd $BASE_DIR/oss/qemu/build
	../configure --prefix=$TOOLDIR/usr --target-list=aarch64-softmmu --enable-modules
	make -j$NJOBS
	make install
}

if [ "x$1" = "xclean" ]; then
	clean
	exit 0
fi

binutils-gdb
kernel_headers
glibc
gcc
qemu
kernel
