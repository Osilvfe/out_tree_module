# out_tree_module

Out-of-tree Linux drivers being brought up for the OnePlus Pad Pro (SM8650,
`oneplus,caihong`).

## Modules

- `nt36532e_ts.ko`: Novatek NT36532E no-flash SPI touchscreen + pen.
- `oneplus_pogo.ko`: OnePlus/Oplus pogo keyboard/touchpad protocol over UART
  using `serdev`.
- `sc8547_cp.ko`: Southchip SC8547/SC8547A dual charge-pump bring-up driver.

The touchscreen driver is written against the DTS currently used by Caihong:
`spi4`, GPIO162 falling-edge interrupt, `firmware-name`, standard touchscreen
coordinate transform properties, and optional `novatek,pen-support`.

The pogo driver remains in the tree, but current work is focused on charging.

Caihong has two SC8547-family charge pumps at I2C address `0x6f` on separate
I2C hubs: the primary SC8547A is on hub 2 and the secondary SC8547-family IC is
on hub 0. The current standalone driver is deliberately telemetry-only: it
enables the ADC, reads status/fault/voltage/current data, and never enables the
charge pump or rewrites protection limits during probe. See
[`docs/caihong-charging.md`](docs/caihong-charging.md).

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

## SC8547 telemetry DTS

Use this only for the current safe bring-up stage:

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

## Pogo DTS skeleton

Do **not** copy GPIO numbers from another OnePlus tablet. Fill in the Caihong
values only after pin identification:

```dts
&uart7 {
    status = "okay";

    pogo {
        compatible = "oneplus,caihong-pogo";
        current-speed = <BAUD_TO_CONFIRM>;
        wake-gpios = <&tlmm WAKE_GPIO GPIO_ACTIVE_LOW>;
        tx-enable-gpios = <&tlmm TX_ENABLE_GPIO GPIO_ACTIVE_HIGH>;
        touchpad-size-x = <TOUCHPAD_X_MAX>;
        touchpad-size-y = <TOUCHPAD_Y_MAX>;
        oneplus,crc-ibm-init = <0xc596>;
    };
};
```
