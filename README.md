# out_tree_module

Linux drivers not yet merged upstream, being brought up for the OnePlus Pad
Pro (SM8650, `oneplus,caihong`). “Out-of-tree” describes upstream status; it
does not mean Caihong integration is removed from the companion kernel tree.

## Modules

- `nt36532e_ts.ko`: Novatek NT36532E no-flash SPI touchscreen + pen.
- `oneplus_pogo.ko`: OnePlus/Oplus pogo keyboard/touchpad protocol over UART
  using `serdev`.
- `sc8547_cp.ko`: Southchip SC8547/SC8547A dual charge-pump bring-up driver.

The touchscreen driver is written against the DTS currently used by Caihong:
`spi4`, GPIO162 falling-edge interrupt, `firmware-name`, standard touchscreen
coordinate transform properties, and optional `novatek,pen-support`.

The latest project checkpoint and hardware caveats are summarized in
[`docs/current-status.md`](docs/current-status.md). The companion kernel tree
is the single source of truth for board data and keeps the required
integration in `sm8650-oneplus-caihong.dts`, with device policy in
Caihong-specific files and only small hooks in generic Qualcomm drivers.
[`patches/linux/`](patches/linux/) contains reproducibility snapshots rather
than an alternative integration location.

Caihong has two SC8547-family charge pumps at I2C address `0x6f` on separate
I2C hubs: the primary SC8547A is on hub 2 and the secondary SC8547-family IC is
on hub 0. Probe remains passive, but the driver now also contains explicitly
gated experimental control and bounded pulse interfaces used through the
Stage 7D13 hardware checkpoint. Automatic charging is paused after an
unresolved one-sided primary-IBUS excursion; none of these controls should be
enabled as a production policy. See [`docs/current-status.md`](docs/current-status.md)
and [`docs/caihong-charging.md`](docs/caihong-charging.md).

## Build

```sh
make KDIR=/path/to/kernel/build
```

Expected modules include:

```text
nt36532e_ts.ko
oneplus_pogo.ko
sc8547_cp.ko
```

## Touchscreen DTS

The existing node is sufficient. Uncomment the pen flag when testing stylus:

```dts
&spi4 {
    status = "okay";

    touchscreen@0 {
        compatible = "novatek,nt36532e";
        reg = <0>;
        interrupts-extended = <&tlmm 162 IRQ_TYPE_EDGE_FALLING>;
        spi-max-frequency = <12000000>;
        panel = <&panel>;
        novatek,pen-support;
        firmware-name = "novatek/DT-novatek-nt36532.bin";
        touchscreen-size-x = <21200>;
        touchscreen-size-y = <30000>;
        touchscreen-max-pressure = <1000>;
        touchscreen-swapped-x-y;
        touchscreen-inverted-x;
        pinctrl-0 = <&ts_default>;
        pinctrl-names = "default";
    };
};
```

## SC8547 DTS

The frozen experimental board data is kept directly in the companion kernel's
`sm8650-oneplus-caihong.dts`. For telemetry-only use, omit all
`southchip,allow-experimental-*` properties and their experimental limits. A
minimal passive shape is:

```dts
&i2c_hub_0 {
    clock-frequency = <400000>;
    status = "okay";

    charger@6f {
        compatible = "southchip,sc8547";
        reg = <0x6f>;
        southchip,role = "secondary";
    };
};

&i2c_hub_2 {
    clock-frequency = <400000>;
    status = "okay";

    charger@6f {
        compatible = "southchip,sc8547a";
        reg = <0x6f>;
        southchip,role = "primary";
    };
};
```

The `southchip,role` property is currently a local bring-up aid and is not an
upstream binding. The driver also accepts the downstream-compatible strings
`oplus,sc8547a` and `slave_vphy_sc8547` for comparison/testing.

## Pogo DTS

The current Caihong UART/pin assignment is preserved directly in the companion
kernel's `sm8650-oneplus-caihong.dts`. Its serdev child is equivalent to:

```dts
&uart7 {
    status = "okay";

    pogo {
        compatible = "oneplus,caihong-pogo";
        current-speed = <921600>;
        power-gpios = <&tlmm 100 GPIO_ACTIVE_HIGH>;
        wake-gpios = <&tlmm 137 GPIO_ACTIVE_LOW>;
        tx-enable-gpios = <&tlmm 14 GPIO_ACTIVE_HIGH>;
        touchpad-size-x = <2764>;
        touchpad-size-y = <1630>;
        touchpad-resolution-x = <23>;
        touchpad-resolution-y = <23>;
        oneplus,crc-ibm-init = <0xc596>;
    };
};
```

The board node selects GENI FIFO mode. The companion Caihong kernel tree
contains the required generic, DT-selected serial-core hook; the archived
patch in `patches/linux/` records the original change. The serdev module cannot
change the parent UART transfer mode after probe.
