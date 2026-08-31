# out_tree_module

Out-of-tree Linux drivers being brought up for the OnePlus Pad Pro (SM8650,
`oneplus,caihong`).

## Modules

- `nt36532e_ts.ko`: Novatek NT36532E no-flash SPI touchscreen + pen.
- `oneplus_pogo.ko`: OnePlus/Oplus pogo keyboard/touchpad protocol over UART
  using `serdev`.

The touchscreen driver is written against the DTS currently used by Caihong:
`spi4`, GPIO162 falling-edge interrupt, `firmware-name`, standard touchscreen
coordinate transform properties, and optional `novatek,pen-support`.

Pogo transport and report formats are implemented, but the Caihong-specific
pogo UART baud and sideband GPIO pin numbers still need to be confirmed from
hardware/downstream DT before enabling the node. They are intentionally not
hard-coded.

## Build

```sh
make KDIR=/path/to/kernel/build
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
