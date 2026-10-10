# SPDX-License-Identifier: GPL-2.0-only

obj-m += nt36532e_ts.o
nt36532e_ts-y := touchscreen/nt36532e.o

obj-m += oneplus_pogo.o
oneplus_pogo-y := pogo/oneplus_pogo.o

obj-m += sc8547_cp.o
sc8547_cp-y := charging/sc8547.o

# Loaded on demand by the bounded userspace pen-charge service.  Keeping the
# module in the boot image ties it to the exact kernel while avoiding an I2C
# address reservation during normal operation.
obj-m += caihong_pen_power.o
caihong_pen_power-y := charging/caihong_pen_power.o

# Optional direct-I2C sensor modules.  A test image loads these only after
# the board DTS enables the matching I2C2 child nodes.
obj-m += mmc5633.o
mmc5633-y := sensors/mmc5633.o

obj-m += tcs3701.o
tcs3701-y := sensors/tcs3701.o

obj-m += inv_icm42607.o
inv_icm42607-y := sensors/inv_icm42607/inv_icm42607_core.o \
	sensors/inv_icm42607/inv_icm42607_accel.o \
	sensors/inv_icm42607/inv_icm42607_gyro.o \
	sensors/inv_icm42607/inv_icm42607_temp.o

obj-m += inv_icm42607_spi.o
inv_icm42607_spi-y := sensors/inv_icm42607/inv_icm42607_spi.o

obj-m += inv_icm42607_i2c.o
inv_icm42607_i2c-y := sensors/inv_icm42607/inv_icm42607_i2c.o

# Caihong front camera sensor.
obj-m += sc820cs.o
sc820cs-y := camera/sc820cs.o

obj-m += sc1320cs.o
sc1320cs-y := camera/sc1320cs.o

obj-m += gt9772.o
gt9772-y := camera/gt9772.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
