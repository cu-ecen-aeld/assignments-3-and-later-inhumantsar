#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$(realpath $1)
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

# to avoid retyping this
MAKE_CMD="make ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # clean source tree
    echo -e "\n#\n# Cleaning the source tree\n#"
    $MAKE_CMD mrproper || (echo "Failed!" && exit 1)

    # set up the config
    echo -e "\n#\n# Setting up the kernel config\n#"
    $MAKE_CMD defconfig || (echo "Failed!" && exit 1)

    # build the base kernel. -jN tells the compiler to use N cores
    # we'll use all of the cores available to the system here.
    cores=$(nproc --all)
    echo -e "#\n# Building the kernel, using $cores CPU cores\n#"
    ($MAKE_CMD -j$cores all && echo "Kernel built") || (echo "Failed!" && exit 1)

    # build kernel modules
    # NOTE: not used in this assignment
    # echo -e "\n#\n# Building kernel modules, using $cores CPU cores\n#"
    # ($MAKE_CMD -j$cores modules && echo "Modules built") || (echo "Failed!" && exit 1)

    # build devicetree
    echo -e "\n#\n# Building the devicetree, using $cores CPU cores\n#"
    ($MAKE_CMD -j$cores dtbs && echo "Device tree built") || (echo "Failed!" && exit 1)
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# Create necessary base directories
ROOTFS_DIR="${OUTDIR}/rootfs"
mkdir -p \
    "${ROOTFS_DIR}/bin" \
    "${ROOTFS_DIR}/dev" \
    "${ROOTFS_DIR}/etc" \
    "${ROOTFS_DIR}/home" \
    "${ROOTFS_DIR}/lib" \
    "${ROOTFS_DIR}/lib64" \
    "${ROOTFS_DIR}/proc" \
    "${ROOTFS_DIR}/sbin" \
    "${ROOTFS_DIR}/sys" \
    "${ROOTFS_DIR}/usr/bin" \
    "${ROOTFS_DIR}/usr/lib" \
    "${ROOTFS_DIR}/usr/sbin" \
    "${ROOTFS_DIR}/var/log"


cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
else
    cd busybox
fi

# Make and install busybox
$MAKE_CMD -j$(nproc --all)
$MAKE_CMD CONFIG_PREFIX=${ROOTFS_DIR} install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${ROOTFS_DIR}/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${ROOTFS_DIR}/bin/busybox | grep "Shared library"

# Add library dependencies to rootfs
# TODO: interpreter?
(cp ${SYSROOT}/lib/ld-linux-aarch64.so.1 \
    ${ROOTFS_DIR}/lib/ \
 && echo "Copied ld-linux-aarch64.so.1 to ${ROOTFS_DIR}/lib/") \
|| (echo "Unable to copy ld-linux-aarch64.so.1" && exit 1)

for l in "libm.so.6" "libresolv.so.2" "libc.so.6"; do
    (cp ${SYSROOT}/lib64/${l} \
        ${ROOTFS_DIR}/lib64/ \
     && echo "Copied ${l} to ${ROOTFS_DIR}/lib64/") \
    || (echo "Unable to copy ${l}" && exit 1)
done

# Make device nodes
echo "Creating device node for /dev/null"
sudo mknod -m 666 ${ROOTFS_DIR}/dev/null c 1 3
echo "Creating device node for /dev/console"
sudo mknod -m 666 ${ROOTFS_DIR}/dev/console c 1 5

# Clean and build the writer utility
echo "Building writer"
cd $FINDER_APP_DIR
$MAKE_CMD clean
$MAKE_CMD

${CROSS_COMPILE}readelf -a writer | grep "program interpreter"
${CROSS_COMPILE}readelf -a writer | grep "Shared library"

# Copy the finder related scripts and executables to the /home directory
# on the target rootfs
echo "Copying writer binary and helper scripts to ${ROOTFS_DIR}/home/"
mkdir ${ROOTFS_DIR}/home/conf
cp -r conf/* "${ROOTFS_DIR}/home/conf/"
cp finder.sh "${ROOTFS_DIR}/home/"
cp finder-test.sh "${ROOTFS_DIR}/home/"
cp autorun-qemu.sh "${ROOTFS_DIR}/home/"
cp writer "${ROOTFS_DIR}/home/"

# Chown the root directory
sudo chown -R root: ${ROOTFS_DIR}

# Create initramfs.cpio.gz
cd $ROOTFS_DIR
echo "Creating initramfs.cpio.gz"
INITRAMFS_PATH=${OUTDIR}/initramfs.cpio
find . | cpio -H newc -ov --owner root:root > $INITRAMFS_PATH
gzip -f $INITRAMFS_PATH
ls -alh ${INITRAMFS_PATH}.gz