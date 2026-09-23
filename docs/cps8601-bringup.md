# Caihong CPS8601 pen charger

The OPN2402 pen has not yet been confirmed powered, discovered over Bluetooth
or reporting input. The user has confirmed stage4 touchscreen suspend/resume.
Keep that touch module and the v9 Wi-Fi payload unchanged while investigating.

## Observed communication

The original helper used the removed `/sys/class/i2c-adapter` class. After
correcting discovery to `/sys/bus/i2c/devices`, the user obtained errno 6
(`ENXIO`) from the targeted read at hub 3/address 0x41. The GENI I2C driver's
error table maps a NACK to `-ENXIO` with the message "slv unresponsive, check
its power/reset-ln". This establishes adapter discovery and a failed transfer,
not a successful CPS chip ID read or proof of a failed/absent chip.

The updated `caihong-pen-status.py` also reads the existing main TLMM debugfs
snapshot for GPIO10/12/15/85/111. It selects the `f100000.pinctrl` bank, since
other GPIO banks can reuse local pin numbers. It does not request GPIO lines,
change direction, toggle outputs or measure HBOOST voltage. If debugfs is not
mounted, the helper reports that the snapshot is unavailable.

## Stock power dependencies

The board's `oplus-chg-23926.dtsi` uses I2C hub 3 (`i2c@98c000`), address
0x41, HBOOST default 5800 mV, and these TLMM pins:

| Pin | Stock role | Relevant behavior |
| --- | --- | --- |
| GPIO10 | Supply switch | High enables the supply path |
| GPIO12 | IRQ | Input, falling edge, pull-up when active |
| GPIO15 | Sleep/wake | High wakes the chip; stock waits up to 2500 ms |
| GPIO85 | Scan | Separate control; do not assume a level enables charging |
| GPIO111 | Off-state | High disallows charging; low allows it |

`oplus_cps8601.c:init_work_func()` first sets HBOOST, then drives the supply
switch high. After 10 ms it runs the stock firmware-update path, then raises
wake, waits, and checks chip ID. It next sets protection thresholds and IRQ
handling before normal charging. A mainline diagnostic should not copy the
automatic firmware-flashing or charging-enablement steps just to read an ID.

The current image lacks this HBOOST/GPIO sequencing. NACK is consistent with
an unpowered or sleeping chip; the actual pin and supply states are still
unconfirmed. The first power experiment should hold charging disallowed,
perform a bounded power/wake/ID/status check, and restore the disabled state.
No such active power experiment is implemented or run by the passive helper.

## HBOOST protocol and integration

`oplus_wireless_pen_glink.c/.h` uses owner 32785, request/response type 1,
opcode 0x10007. The request consists of the 12-byte PMIC-Glink header, one
byte `reg_vout`, and three zero padding bytes. Stock computes the register
value as `(millivolts - 2000) / 50`, so the board's 5800 mV setting is 76.
The response is a 12-byte header and a 32-bit status; zero means success.
Stock waits 2500 ms. Its cleanup setting of 2000 mV is the minimum voltage
request, not evidence of a true regulator-off command.

This owner is independent of BATTMGR owner 32778 and must not modify the
paused SC8547 policy. Mainline `devm_pmic_glink_client_alloc()` takes the
transport from the client's parent device. An I2C client cannot be passed
directly as that device. A supported PMIC-Glink child/provider integration is
needed, including transport-down handling, serialized requests, exact response
validation and cleanup. A lost response must not be assumed successful.

## Source references

Paths relative to the stock tree `external/oneplus-sm8650-pad-pro`:

- `kernel_platform/qcom/proprietary/devicetree/oplus/oplus_chg/oplus-chg-23926.dtsi`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_cps8601.c`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_wireless_pen_glink.c`
- `vendor/oplus/kernel/charger/wireless_pen/oplus_wireless_pen_glink.h`

Mainline references are `drivers/i2c/busses/i2c-qcom-geni.c`,
`drivers/pinctrl/qcom/pinctrl-msm.c` and `drivers/soc/qcom/pmic_glink.c`.
