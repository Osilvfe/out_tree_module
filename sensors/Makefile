# SPDX-License-Identifier: GPL-2.0-only

obj-m += bu52053nvx.o
obj-m += mmc5633.o
obj-m += tcs3701.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
