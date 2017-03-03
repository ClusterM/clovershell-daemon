### 
### clovershell daemon (c) Cluster, 2017
### http://clusterrr.com
### clusterrr@clusterrr.com
###

CFLAGS-NES += -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3,-rpath,-nostartfiles
CC-NES = arm-linux-gnueabihf-gcc
TARGET=mod/bin/clovershell
HMOD=clovershell.hmod

all: $(HMOD)

$(HMOD): $(TARGET)
	cd mod && tar -czvf ../$(HMOD) *

$(TARGET): clovershell.c
	$(CC-NES) -g -Wall $(CFLAGS-NES) $(LDFLAGS-NES) $< -o $(TARGET)

clean:
	rm -f $(TARGET) $(HMOD) *~ \#*\#
