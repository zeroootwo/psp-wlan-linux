TARGET = wlanlog
OBJS = main.o
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -G0 -Wall -fno-pic
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
