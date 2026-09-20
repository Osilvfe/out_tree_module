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

The current Caihong board integration uses QUPv3 wrapper 1 serial engine 7 at
`0x00a9c000`, 921600 baud, GPIO62/63 for UART TX/RX, GPIO100 for active-high
accessory power, GPIO137 for active-low wake and GPIO14 for active-high TX
enable. The touchpad reports a 2764x1630 range with 23x23 resolution. These
values remain board data and are not hard-coded by the protocol driver.

The complete board description is kept directly in the companion kernel's
`sm8650-oneplus-caihong.dts`. Its serdev child has this shape:

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

## Next bring-up checkpoint

For touchscreen, the next useful hardware log is the probe/firmware-download
path and one raw IRQ packet after the module binds. For pogo, rerun a complete
keyboard/media/touchpad/TX-control regression from the migrated external
module. The GENI FIFO selector remains a minimal kernel-side prerequisite; see
`patches/linux/README.md` and `docs/current-status.md`.
