# OnePlus Pad Pro (caihong) bring-up notes

## NT36532E

The standalone SPI driver implements the released Novatek no-flash path:

- SPI read/write command bit and dummy-byte handling.
- trim-ID detection (`32 65 03`) and single/cascade detection.
- firmware `NVT`/`MOD` marker and version-complement checks.
- ILM/DLM/info/overlay partitions, including the cascade second-header format.
- SRAM download, 3-byte HW-CRC bank setup, cascade auto-copy, boot-ready and reset-state polling.
- ten-slot touch input; Caihong's 21200x30000 range selects the high-resolution packet layout.
- point checksum and the separate 14-byte pen checksum follow the released vendor implementation.
- pen X/Y, pressure, tilt, distance and two side buttons; pen format byte is 66.
- no-flash firmware is downloaded again on resume.

The exact `b_16.0.0_pad_pro` vendor source confirms that IRQ handling reads the point packet first, validates the normal point checksum, then validates the pen block independently when pen support is enabled. The current Caihong DTS can therefore be used without inventing a reset GPIO or regulator. Enable `novatek,pen-support;` for stylus input.

## Pogo keyboard/touchpad

The released OnePlus protocol is UART based.

Keyboard -> PAD frames:

`55 x8 | F1/F2 | A1 | A2 | cmd | len | payload | CRC16_BE | FE | AA x4`

PAD -> keyboard frames use exactly the same framing with the source/destination addresses reversed:

`55 x8 | F1 | A2 | A1 | cmd | len | payload | CRC16_BE | FE | AA x4`

CRC is MSB-first polynomial `0x8005`, default init `0xc596`, over `F1/F2` through the payload (`len + 5` bytes). Commands `0x01`, `0x02` and `0x03` are keyboard, media keys and touchpad respectively. `0x2f` is the keyboard sync/heartbeat upload command. Touchpad contacts use five bytes each and support five slots.

The vendor implementation also establishes an important startup property: the keyboard sends a power-up sync itself roughly 400 ms after power-on and then sends heartbeat traffic roughly every 100 ms. A mainline driver therefore does **not** need to transmit an unverified startup packet during probe. TX support can be added safely for commands whose payloads are known (LED/LCD/touchpad controls) without making probe depend on a speculative handshake.

Still unresolved for **caihong hardware**:

- whether `uart7` is physically the pogo UART (currently only the strongest candidate),
- UART baud rate,
- plug/wake GPIO,
- TX/TX-enable GPIO/pinctrl,
- accessory power path and active polarities.

The driver intentionally does not hard-code those board-specific values.

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

## Next bring-up checkpoint

For touchscreen, the next useful hardware log is the probe/firmware-download path and one raw IRQ packet after the module binds. For pogo, the highest-value evidence is the UART controller and pin state while the physical keyboard is attached; once baud/pins are known the already-decoded receive protocol can be exercised without first implementing host TX.
