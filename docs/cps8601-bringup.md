# Caihong CPS8601 pen charger

The OPN2402 pen has not yet been confirmed powered, discovered over Bluetooth
or reporting input. The user has confirmed stage4 touchscreen suspend/resume.
Keep that touch module and the v9 Wi-Fi payload unchanged while investigating.

## Stage6 withdrawn; Stage6a boot recovery

The user reported a black screen from power-on after flashing Stage6, with
no visible kernel log at all. This does not locate the failure at module
insertion; the DT change or an earlier boot/display failure remain possible.
Wi-Fi uses a random MAC and the current IP was unknown, so SSH availability and Wi-Fi failure
have **not** been established. The old image synchronously inserted
`caihong_pen_power.ko` before `switch_root`. Even without triggering
`probe_once`, module registration creates devices/links and changes GPIO
configuration. The host ACK/sequence tests did not exercise actual device
registration or the tablet's boot path. No log currently proves a specific
lockup or display failure.

Stage6a removes that init hook, the module payload, `/pmic-glink/pen-power`
and the added hub-3 phandle. Its init and DTB are byte-identical to Stage5;
all archive records except the corrected pogo module and passive helper are
also identical. `--pen-power-module` now fails before reading/building an
image. Confirm boot recovery first. The source below remains experimental;
do not run the historical power-test commands until integration is diagnosed.

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

Stage5 lacked this sequencing. The user's GPIO snapshot shows all five pins
as input/low/pulldown, including GPIO10/15/111. This establishes the missing
GPIO control; it does not measure HBOOST voltage. The withdrawn Stage6 attempted a manual
bounded identification experiment; it has no successful hardware result.

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

## Withdrawn Stage6 design (historical; do not run)

The module creates the dedicated `/pmic-glink/pen-power` DT child under the
actual, bound PMIC-Glink platform device. A managed supplier link orders probe,
suspend and removal against that provider. It also links to the GENI I2C
controller and reserves hub-3 address 0x41 using a dummy client. The packaging
script adds only this child and a unique hub-3 phandle, in addition to the
existing touch properties. Kernel code and SC8547 are unchanged.

At registration it requests GPIO111 high first, GPIO10/15/85 low, GPIO12 input
with pull-up. It sends **no HBOOST request at boot**. The historical test command was:

```sh
sudo caihong-pen-status --probe-power
```

The command sets 5800 mV through owner 32785 and waits for a validated ACK,
raises GPIO10, waits 10 ms, raises GPIO15, waits 2500 ms, then reads chip ID.
Only ID 0x8601 allows reads of firmware, mode, IRQ flags, VIN/IIN/temperature
and EPT. Register selectors are big-endian and values are little-endian,
matching stock's per-byte access. There are no CPS configuration, TX, IRQ-clear
or firmware writes. GPIO111 stays high for the entire test.

On completion or failure it lowers wake, scan and supply and requests the
minimum 2000 mV setting if the transport is still unambiguous. That setting is
not a regulator-off claim. The physical supply switch is disabled first.
A timeout, send failure or transport loss during the test latches `poisoned`:
no further request can consume a delayed response as its own ACK. Callbacks
never sleep; a transport-down event aborts waits. A mutex, wakeup source and
PM callbacks prevent a test from overlapping suspend/removal. One attempt is
accepted per module load, retained across driver unbind/rebind. Reboot before
another hardware experiment, especially after `poisoned=1`; module reload is
not a recovery procedure for an ambiguous firmware response.

The helper normally reads cached results without powering the chip or doing
raw I2C while the provider owns it. To read the result again:

```sh
sudo caihong-pen-status
cat /sys/bus/platform/devices/caihong-pen-power/status
```

`attempted=0 phase=idle result=-61` means no test has run. After a successful
identification expect `phase=done result=0 cleanup=0 valid=0xff chip_id=0x8601`,
with `charge_disable=1 supply=0 wake=0 scan=0`. `valid` bits 0 through 7
correspond to chip ID, firmware, mode, IRQ, VIN, IIN, temperature and EPT;
only set bits have meaningful latched values. `phase` identifies the failing
step. A read-phase result of `-6` still means NACK, now after acknowledged
HBOOST and GPIO wake sequencing. A `-110` with `phase=hboost` means ACK timeout,
so CPS power/wake was not enabled. Neither output pin readback nor an HBOOST
ACK proves physical rail voltage. Identification alone does not prove pen
charging, Bluetooth discovery or touch pen reports.

Software verification includes W=1 module builds, checkpatch and host tests
of the production ACK/power/I2C code with fake hardware: success, rejected
ACKs, malformed/unaligned replies, timeout and late reply, transport loss,
short transfer/NACK, mismatched ID, cleanup timeout and the invariant that
charging is always inhibited. These tests do not validate PMIC firmware or
physical GPIO behavior. See [image build instructions](pogo-keymap.md).
