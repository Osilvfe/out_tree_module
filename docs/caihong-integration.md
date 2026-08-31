# OnePlus Pad Pro (caihong) bring-up notes

## NT36532E

The standalone SPI driver implements the released Novatek no-flash path:

- SPI read/write command bit and dummy-byte handling.
- trim-ID detection (`32 65 03`) and single/cascade detection.
- firmware `NVT`/`MOD` marker and version-complement checks.
- ILM/DLM/info/overlay partitions, including the cascade second-header format.
- SRAM download, 3-byte HW-CRC bank setup, cascade auto-copy, boot-ready and reset-state polling.
- ten-slot touch input; Caihong's 21200x30000 range selects the high-resolution packet layout.
- pen X/Y, pressure, tilt, distance and two side buttons; pen format byte is 66.
- no-flash firmware is downloaded again on resume.

The current Caihong DTS can be used without inventing a reset GPIO or regulator. Enable `novatek,pen-support;` for stylus input.

## Pogo keyboard/touchpad

The released OnePlus protocol is UART based. Receive frames are:

`55 x8 | F1/F2 | A1 | A2 | cmd | len | payload | CRC16_BE | FE | AA x4`

CRC is MSB-first polynomial `0x8005`, default init `0xc596`, over `F1/F2` through the payload (`len + 5` bytes). Commands `0x01`, `0x02` and `0x03` are keyboard, media keys and touchpad respectively. Touchpad contacts use five bytes each and support five slots.

Still unresolved for **caihong hardware**:

- whether `uart7` is physically the pogo UART (currently only the strongest candidate),
- UART baud rate,
- plug/wake GPIO,
- TX/TX-enable GPIO/pinctrl,
- accessory power path,
- whether a host sync packet must be transmitted before the keyboard begins periodic reporting.

The driver therefore does not hard-code these values. The first version is receive-capable; TX handshake/LED/suspend control will be added after Caihong pin/baud identification.

Suggested DTS shape once pins are confirmed:

```dts
&uart7 {
    status = "okay";
    pogo {
        compatible = "oneplus,caihong-pogo";
        current-speed = <BAUD>;
        wake-gpios = <&tlmm WAKE GPIO_ACTIVE_LOW>;
        tx-enable-gpios = <&tlmm TX_EN GPIO_ACTIVE_HIGH>;
        touchpad-size-x = <4096>;
        touchpad-size-y = <4096>;
    };
};
```
