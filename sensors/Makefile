# SPDX-License-Identifier: GPL-2.0-only

obj-m += bu52053nvx.o
obj-m += mmc5633.o
obj-m += tcs3701.o

# Upstream ICM42607 family support is split into a shared core and the
# transport-specific SPI/I2C front ends.  Caihong's registry places the IMU on
# SSC SPI instance 3; keep both front ends available until the AP/SSC handoff
# is confirmed in the board DTS.
obj-m += inv_icm42607.o
inv_icm42607-y := inv_icm42607/inv_icm42607_core.o \
	inv_icm42607/inv_icm42607_accel.o \
	inv_icm42607/inv_icm42607_gyro.o \
	inv_icm42607/inv_icm42607_temp.o

obj-m += inv_icm42607_spi.o
inv_icm42607_spi-y := inv_icm42607/inv_icm42607_spi.o

obj-m += inv_icm42607_i2c.o
inv_icm42607_i2c-y := inv_icm42607/inv_icm42607_i2c.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
