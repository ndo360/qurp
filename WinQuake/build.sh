#!/bin/bash
# this script builds q1 for raspberry pi
# invoke with ./build.sh
# or ./build.sh clean to clean before build

# directory containing the ARM shared libraries (rootfs, lib/ of SD card)
# specifically libEGL.so and libGLESv2.so
ARM_LIBS=/opt/vc/lib

# directory to find khronos linux make files (with include/ containing
# headers! Make needs them.)
INCLUDES="-I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux "

# prefix of arm cross compiler installed
#CROSS_COMPILE=bcm2708-

# clean
if [ $# -ge 1 ] && [ $1 = clean ]; then
   echo "clean build"
   rm -rf build/*
fi

echo "- - - - - - - - - - - - - - - "
echo "Start Make"
echo "- - - - - - - - - - - - - - - "

make -j4 -f make.rpi $1 ARCH=arm \
	CC=""$CROSS_COMPILE"gcc" USE_SVN=0 USE_CURL=0 USE_OPENAL=0 \
	CFLAGS="-march=armv6 -mfpu=vfp -mfloat-abi=hard $INCLUDES" \
	LDFLAGS="-pthread -lm -L"$ARM_LIBS" -lvchostif -lvcfiled_check -lbcm_host -lkhrn_static -lvchiq_arm -lopenmaxil -L/opt/vc/lib/ -lEGL -lGLESv2 -lvcos -lrt -lSDL -ludev"
	
exit 0

