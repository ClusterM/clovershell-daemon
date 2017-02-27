### 
### I think it's not worth to make such a small project
### modular. So this is a simple gnu Makefile...
###

.DELETE_ON_ERROR:
.PHONY: install clean all

CFLAGS-NES += -L../nesmini/lib -L../nesmini/usr/lib -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3,-sysroot=/usr/home/cluster/nesmini,-rpath,-nostartfiles
CC-NES = arm-linux-gnueabihf-gcc

all: clovershell

clovershell: clovershell.c
	$(CC-NES) -g -Wall $(CFLAGS-NES) $(LDFLAGS-NES) $< -o clovershell

clean:
	-$(RM) clovershell *~ \#*\#
