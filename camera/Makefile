# SPDX-License-Identifier: GPL-2.0-only

obj-m += sc820cs.o
obj-m += gt9772.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
