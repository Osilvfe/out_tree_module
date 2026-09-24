# SPDX-License-Identifier: GPL-2.0-only

obj-m += nt36532e_ts.o
nt36532e_ts-y := touchscreen/nt36532e.o

obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o

obj-m += sc8547_cp.o
sc8547_cp-y := charging/sc8547.o

# Direct-I2C sensor bring-up modules.  The test image loads these only after
# the board DTS enables the matching I2C2 child nodes.
obj-m += mmc5633.o
mmc5633-y := sensors/mmc5633.o

obj-m += tcs3701.o
tcs3701-y := sensors/tcs3701.o

# Caihong front camera: safe probe/V4L2 graph bring-up only for now.
obj-m += sc820cs.o
sc820cs-y := camera/sc820cs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
