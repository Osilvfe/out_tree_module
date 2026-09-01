# SPDX-License-Identifier: GPL-2.0-only

obj-m += nt36532e_ts.o
nt36532e_ts-y := touchscreen/nt36532e.o

obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o

obj-m += sc8547_cp.o
sc8547_cp-y := charging/sc8547.o

obj-m += sc8547_dual.o
sc8547_dual-y := charging/sc8547_dual.o

obj-m += sc8547_policy_diag.o
sc8547_policy_diag-y := charging/sc8547_policy_diag.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
