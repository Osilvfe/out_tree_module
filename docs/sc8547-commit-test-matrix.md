# SC8547 commit-by-commit merge and test matrix

This file is the authoritative history/merge guide for the charging work on
`sc8547-next`.

The development branch is intentionally linear and contains implementation,
documentation, CI, operator tooling and a few one-shot engineering commits.
**Do not equate `git log` order with the order that should be cherry-picked into
`main`.**

For every future charging commit, update this matrix in the same development
cycle. Each entry must answer five questions:

1. What capability or contract does this commit add/change?
2. Does it introduce a hardware write or only observation/documentation/tooling?
3. Which earlier functional commits does it require?
4. What is the first hardware test that becomes meaningful after it?
5. Is it part of the eventual `main` cherry-pick chain, optional, superseded,
   or explicitly forbidden to cherry-pick?

Matrix-only commits whose only purpose is updating this file are self-describing
checkpoints and are not recursively added as another row; otherwise documenting
the matrix update would require an infinite sequence of matrix-update commits.
All functional, CI, other documentation, operator tooling and temporary
engineering commits are listed explicitly.

## Classification

- **F** — functional kernel/module source change; may affect runtime behavior.
- **D** — documentation/test-contract change only.
- **C** — CI/build infrastructure.
- **O** — operator/evidence tooling; no kernel electrical control semantics.
- **T** — temporary engineering machinery; never part of a release/test merge.

Promotion labels:

- **YES, after gate** — intended for `main` only after the stated hardware gate.
- **YES, read-only** — safe candidate to merge earlier once its prerequisite
  telemetry is trusted because it adds no new charge-pump/source control write.
- **OPTIONAL** — useful documentation/CI/tooling but not required by runtime code.
- **SUPERSEDED** — historical step replaced by a later commit.
- **NEVER** — branch-construction artifact; do not cherry-pick.

## Base

`main` base for this development series:

```text
159d2e5 Update SC8547 telemetry test guide
```

The baseline physical driver is telemetry-first. Apart from ADC enable, it does
not intentionally start the charge pump or rewrite protection thresholds.

## Complete history through Stage 5

| # | Commit | Class | Meaning / new boundary | Future merge and first meaningful test |
|---:|---|:---:|---|---|
| 1 | `60c230c` | F | Models SC8547/SC8547A/SC8547D variants, ID handling, masked shared controls and read-only register snapshot. No new CP/protection write. | **YES, read-only.** First test: both chips' `variant` + `register_dump` unplugged and at normal 5 V. |
| 2 | `d425680` | D | Creates staged development roadmap so later write-capable work has explicit gates. | **OPTIONAL but recommended before testing.** |
| 3 | `ab9c7b0` | D | Records branch policy and validation workflow (`main` conservative, `sc8547-next` development). | **OPTIONAL.** Process contract only. |
| 4 | `bc553aa` | F | Adds read-only protection register decode alongside raw values. Does not program protection. | **YES, read-only**, requires #1. Compare decode vs raw register dump/downstream evidence. |
| 5 | `25d4a3d` | D | Documents Stage-2 decode and ambiguity in vendor threshold comments/formulas. | **OPTIONAL but recommended with #4.** Prevents decoded values being mistaken for validated electrical limits. |
| 6 | `2173e10` | D | Documents proposed gated Stage-3 init before source implementation. | **OPTIONAL.** Defines Stage-3 lab contract. |
| 7 | `f15fb5a` | F | Explicitly includes bitfield helpers needed by Stage-2 decode. Runtime semantics unchanged. | **YES with #4.** Compile prerequisite only. |
| 8 | `aa3c510` | F | Adds first write-capable Stage-3 path: explicit opt-in, raw profile, controlled init/reset/watchdog, CP left disabled. | **YES, after Stage 0-2 gate.** First write test: `apply_init`; exact readback, CP off, basic PMIC charging intact. |
| 9 | `29e6c6c` | F | Tightens Stage-3 profile handling for primary/secondary role differences and fail-closed behavior. | **YES with #8 before any Stage-3 hardware test.** Do not test #8 alone on the current plan. |
| 10 | `3ec6a02` | C | Adds early SC8547 module CI. | **SUPERSEDED** by #13/#21. |
| 11 | `d6509c6` | D | Aligns Stage-3 docs with role-safe source behavior. | **OPTIONAL but recommended.** |
| 12 | `17d7037` | F | Fixes APIs/formatting for torvalds Linux v7.2. | **YES with Stage 3.** Required for meaningful v7.2 compile validation. |
| 13 | `4a0df97` | C | Establishes focused Linux-v7.2 `W=1` charging CI, isolating unrelated workstreams. | **OPTIONAL for runtime; recommended in repo.** |
| 14 | `ffd115b` | D | Defines Stage-4 single-pump design, second opt-in, VBUS/VBAT windows and rollback. | **OPTIONAL but strongly recommended before #16 testing.** |
| 15 | `1f9ab5c` | D | Records Stage-3 → Stage-4 hardware gates. | **OPTIONAL.** |
| 16 | `8033a03` | F | Implements Stage-4 manual `work_mode` and `cp_enable`; enable repeats preflight, sets shared enable bit, waits 500 ms, validates switching/fault/window and disables on failure. | **YES, after Stage-3 hardware pass.** Test primary and secondary separately/repeatedly; no dual test yet. |
| 17 | `4727cb9` | D | Marks #16 focused-v7.2 build-verified and splits Stage 5 into 5A/5B. | **OPTIONAL.** |
| 18 | `d020fd1` | D | First Stage-5 virtual coordinator architecture/test document. | **OPTIONAL; later docs refine it.** |
| 19 | `18581b0` | F | Adds `sc8547_dual.ko` read-only virtual coordinator with phandle pairing and aggregate telemetry. | **YES, read-only.** First test: peer identity, both ADC snapshots and aggregate IBUS with pumps off. |
| 20 | `ec05245` | F | Builds `sc8547_dual.ko`. | **YES with #19.** |
| 21 | `edf0998` | C | Focused CI builds `sc8547_cp.ko` + `sc8547_dual.ko`. | **OPTIONAL for runtime; repo CI.** |
| 22 | `1801dae` | D | Aligns Stage-5 docs to separate virtual coordinator DT node. | **OPTIONAL but recommended with #19.** |
| 23 | `6986e8c` | F | Requires distinct clients bound to physical driver with exact primary/secondary roles. | **YES with Stage 5A.** Test one deliberate wrong-role DT; pair must be refused without writes. |
| 24 | `764f821` | D | Marks Stage 5A focused-v7.2 build-verified and records hardware gate. | **OPTIONAL.** |
| 25 | `60cb97e` | F | Introduces shared physical-driver API header so upper layers do not duplicate Stage-4 safety logic. | **YES as part of current shared-API chain.** Header alone adds no runtime path. |
| 26 | `e854be9` | F | Makes physical API opaque to private structures. | **YES with #25.** |
| 27 | `eaa26b8` | T | Adds one-shot exact-refactor script. | **NEVER.** |
| 28 | `63caf142` | T | Adds one-shot refactor workflow. | **NEVER.** |
| 29 | `9b29a00` | F | Exports physical state/mode/preflight/enable/disable helpers and makes Stage-4 sysfs use them. Intended behavior unchanged; safety logic becomes single-source. | **YES with #25/#26.** Re-run Stage-4 regression test before any dual write test. |
| 30 | `7aa2478` | T | Removes one-shot refactor script. | **NEVER** in a clean cherry-pick series. |
| 31 | `f5bc4a9` | T | Removes one-shot refactor workflow. | **NEVER** in a clean cherry-pick series. |
| 32 | `3cbdb91` | D | Documents physical API contract and ownership of locking/windows/fail-closed checks. | **OPTIONAL but strongly recommended with #29.** |
| 33 | `7caaee0` | F | Moves Stage-5A virtual telemetry onto `sc8547_get_state()`, removing duplicate raw-register interpretation. | **YES for current Stage-5 line.** Re-run Stage-5A read-only tests. |
| 34 | `25d0e27` | F | Implements Stage-5B gated mode + dual enable: primary→secondary start, reverse stop, two preflights, final pair check, rollback. Third opt-in required. | **YES only after both pumps pass Stage 4.** First dual test uses an already validated ratio/source/window and stops immediately after snapshots. |
| 35 | `0c92c3a` | D | Documents Stage-5B controls, rollback, concurrency and safe failure-path tests. | **OPTIONAL but should accompany #34 in any hardware-test branch.** |
| 36 | `2fc76f1` | D | Marks Stage 5B focused Linux-v7.2 `W=1` build-verified and records clean chain. | **OPTIONAL.** Hardware gate remains unmet. |

## Stage 6 commits and supporting process commits

| # | Commit | Class | Meaning / new boundary | Future merge and first meaningful test |
|---:|---|:---:|---|---|
| 37 | `0dc0c2d` | D | Creates this authoritative commit-by-commit merge/test matrix. It distinguishes functional, docs, CI and temporary commits and defines clean Stage/Track construction. | **OPTIONAL but strongly recommended for all future testing.** No runtime change. |
| 38 | `a48fa1f` | D | Documents Stage-6 source-contract architecture and explicitly splits 6A read-only diagnostics, 6B future source-request bridge and 6C future automatic policy. Records that downstream Fixed PDO and PPS are distinct operations. | **OPTIONAL but required reading before Stage-6B write work.** No runtime change. |
| 39 | `e16d7c2` | F | Declares a read-only `sc8547_dual_state` API above the virtual coordinator. It exposes two physical snapshots + aggregate IBUS and contains no control operation. | **YES, read-only**, used by Stage 6A. Header alone changes no runtime behavior. |
| 40 | `fc32392` | F | Implements/exports `sc8547_dual_get_state()`. The coordinator serializes the snapshot with its own mutex and obtains physical state only through the physical-driver API. No new CP/source write. | **YES, read-only**, after current shared-API Stage 5A. First test is a Stage-5A snapshot regression with pumps off. |
| 41 | `9ec6520` | F | Adds `sc8547_policy_diag.c`, a separate Stage-6A platform driver. It reads an explicitly named USB power supply plus `sc8547_dual_get_state()` and exposes `usb_supply`, `source_state`, `combined_state`. No `store`, `power_supply_set_property`, Glink write or CP write. | **YES, read-only**, after #39/#40 and Stage-5A telemetry. First test: CPs off, unplugged → 5 V → naturally negotiated fixed-PD → PPS-capable source observations. |
| 42 | `756acee` | F | Adds `sc8547_policy_diag.ko` to OOT build. | **YES with #41** when testing Stage 6A. No independent runtime behavior. |
| 43 | `97892e6` | C | Focused Linux-v7.2 CI now builds all three charging modules: physical, dual coordinator and Stage-6A diagnostic. | **OPTIONAL for runtime; current repo CI.** Focused v7.2 `W=1` run passed for this commit. |
| 44 | `f0a0e9d` | D | Records the recovered downstream PPS session sequence and rollback evidence: non-SoCCP 34/35 mV/mA requests, APDO gating, conservative 5.5-V start, source ramp before CP start and fail-closed teardown. | **OPTIONAL but mandatory reading before Stage 6B.** Does not unlock source writes. |
| 45 | `93ba0ed` | D | Adds the operator-facing Stage 0→6C hardware test manual. Every Stage names functional commits, DT preparation, commands, pass/stop criteria and the next allowed Stage. | **OPTIONAL but strongly recommended on every hardware-test branch.** This is the canonical execution procedure, not runtime code. |
| 46 | `3e48606` | D | Separates mainline UCSI/USB-PD capability observation from exact Oplus/QCOM PPS request semantics. Establishes that `GET_PDOS` may expose APDO capabilities while `SET_PDOS`/`SET_POWER_LEVEL` are not exact `(mV,mA)` sink PPS requests. | **Stage-6B research only.** First meaningful hardware use: add UCSI/USB-PD capability capture to Stage-6A F3; no source write. |
| 47 | `fedc9b6` | D | Establishes BATTMGR transaction ownership: a second PMIC-Glink client with the same owner is unsafe because responses are owner-broadcast while qcom_battmgr has one unkeyed completion/error transaction state. | **Stage-6B architecture only.** Forbids an OOT parallel BATTMGR client; no hardware write. |
| 48 | `57c91c` | D | Documents candidate exact PPS request and deterministic fixed-5V return path: USB SET `0x33` properties 34/35 and non-SoCCP BAT SET `0x31`, Oplus property 25, 5000 mV. Records the property-25 collision with upstream meanings. | **Stage-6B research only.** Does not authorize sending these values; first future use is K2/K3 design after Stage-6A/runtime-DT evidence. |
| 49 | `f814688` | D | Creates the compact canonical Stage package index so test branches are built from per-Stage packages rather than linear git history. | **OPTIONAL but strongly recommended.** Use it before every Stage branch construction. |
| 50 | `cef2b43` | D | Designs the gated kernel-tree `qcom_battmgr` extension and splits future write work into K0 gate/plumbing → K1 validated SET parser → K2 fixed-5V fallback → K3 bounded PPS helper → later U0 source-only consumer. | **Stage-6B design only.** Explicitly blocks K0-K3 implementation until real Stage-6A/runtime-DT evidence exists. |
| 51 | `9f4b908` | D | Registers #50 in the Stage package index and makes fallback-before-PPS ordering part of the canonical Stage-6B package contract. | **OPTIONAL process checkpoint.** No runtime change. |
| 52 | `8044c92` | O | Adds `scripts/sc8547-stage-capture.sh`, a read-only evidence collector for Stage 0/1/2/3/4/5A/5B/6A/6B snapshots, module provenance, sysfs and test context. It intentionally performs no control write. | **OPTIONAL for any Stage.** First meaningful use is Stage-0/1 baseline capture; safe to reuse before/after later manual writes. |
| 53 | `273900a` | C | Extends focused CI to syntax-check the read-only stage capture script. | **OPTIONAL CI.** No device/runtime behavior. |
| 54 | `390b2b3` | D | Documents capture-tool invocation, metadata variables and evidence-set expectations. | **OPTIONAL but recommended** when using #52 so saved results remain tied to exact kernel/DT/source conditions. |
| 55 | `6aa84fd` | D | Refreshes qcom_battmgr ownership design with current evidence: SM8650 matches the SM8550-family battmgr path, PMIC-Glink auxiliary power-supply inherits the parent OF node so an explicit firmware-ABI gate can live there, current SET responses need explicit validation, and service-down can leave a pending request to time out. Also marks fixed-5V fallback as established candidate rather than an unknown. | **Stage-6B design only.** Narrows K0/K1 implementation points; no electrical write. |
| 56 | `f8749ef` | D | Corrects the Stage package index (`fedc9b6`, not nonexistent `538dad3`), registers #55, records K0-K3/U0 cross-repository structure and classifies #52-#54 as cross-Stage read-only tooling. | **OPTIONAL process checkpoint, strongly recommended before future branch construction.** No runtime change. |
| 57 | `8d2d993` | D | Defines the future qcom_battmgr exported API identity/lifetime contract: no raw opcode/property export, qcom_battmgr must validate that a referenced psy is its own USB supply before private-state access, supply name is discovery only, public units are uV/uA, and K2/K3 remain synchronous. | **Stage-6B design only.** First future use is K0 public/private API review; no electrical write and no relaxation of the hardware-data gate. |
| 58 | `f298c88` | D | Registers #57 in the canonical Stage package and adds its API identity/lifetime rules to the Stage-6B prerequisites. | **OPTIONAL process checkpoint, recommended before future K0 branch construction.** No runtime change. |

Matrix-only commits that merely update this table are intentionally not given a
new row. Their effect is the updated matrix itself.

### Meaning of the Stage-6A checkpoint

At #43, source-contract **observation** is build-complete. Nothing in #38-#43
can request another USB voltage/current contract. Nothing automatically starts
a charge pump. This is a deliberate hardware-test checkpoint before Stage 6B.

Later Stage-6B research/documentation (#44 onward) improves the evidence model
but does not change the Stage-6A electrical boundary. There is still no
source-contract SET implementation in `out_tree_module`.

The Stage-6A DT is development-only:

```dts
sc8547_policy_diag: charge-policy-diagnostic {
    compatible = "southchip,sc8547-policy-diagnostic";
    southchip,charge-pump = <&sc8547_dual>;
    southchip,usb-power-supply-name = "qcom-battmgr-usb";
};
```

The `sc8547_dual` node does **not** need
`southchip,allow-experimental-dual-cp` for Stage 6A. Omitting that property keeps
Stage-5B writable virtual controls hidden while Stage 6A reads pair state.

## Clean functional cherry-pick tracks

### Track A — safest first device session: read-only physical telemetry

Starting from `main` (`159d2e5`):

```text
60c230c
bc553aa
f15fb5a
```

Recommended doc: `25d4a3d`.

Test:

1. boot with both physical `0x6f` nodes;
2. verify ID/variant/role;
3. capture `register_dump`, `protection_state`, ADC/status/faults unplugged;
4. attach normal 5 V and repeat;
5. confirm no unexpected CP switching;
6. confirm pmic-glink/basic charging remains functional.

### Track B — Stage 3 controlled init

Only after Track A passes:

```text
aa3c510
29e6c6c
17d7037
```

Recommended docs: `2173e10`, `d6509c6`.

Test one physical pump at a time. `apply_init` must leave the pump off,
protection bytes must read back exactly, ADC/status must remain plausible and
normal charging must survive. Stop on any unexplained fault.

### Track C — Stage 4 single-pump switching

Only after Stage 3 passes:

```text
8033a03
```

Recommended docs: `ffd115b`, `1f9ab5c`.

Test primary, stop it, then secondary separately. Repeat both with the same
controlled source/windows. The first success criterion is deterministic
`enable -> switching validation -> disable`, not maximum charging power.

### Track D — Stage 5A read-only virtual pair

For the current shared-API stack:

```text
18581b0
ec05245
6986e8c
60cb97e
e854be9
9b29a00
7caaee0
```

`edf0998` is CI-only. Never replay #27/#28/#30/#31.

Test with both pumps off. Verify pairing, both snapshots and aggregate IBUS.
Re-run the Stage-4 single-pump smoke test after `9b29a00` because it refactors
that path even though intended behavior is unchanged.

### Track E — Stage 5B dual switching

Only after **both** physical pumps pass Stage 4:

```text
25d0e27
```

Required test document: `0c92c3a`.

Use the Stage-5B sequence in `sc8547-stage5-dual-coordinator.md`. Verify primary
validated switching precedes secondary; snapshot immediately; stop. Secondary
or final-validation failure must return both pumps to off.

### Track F — Stage 6A read-only source/CP correlation

This track does **not** require Stage-5B hardware validation and must be tested
with the dual-write DT opt-in omitted.

Starting from the current shared-API Stage-5A Track D, add:

```text
e16d7c2
fc32392
9ec6520
756acee
```

`97892e6` is CI-only. Read `a48fa1f` for the Stage-6 split and `f0a0e9d` for the
current downstream PPS-session evidence. Neither documentation commit adds a
runtime dependency.

`fc32392` was authored later on the linear development branch, after Stage-5B
source existed, but its patch adds only the read-only exported state function
around Stage-5A code. When constructing a read-only hardware branch, do **not**
cherry-pick `25d0e27` merely to get Stage 6A. If Git reports context conflict,
resolve only the read-only API addition against the Stage-5A coordinator and
review the resulting diff for absence of `dual_enable`/mode-write additions.

Test Track F strictly in this order:

#### F0 — unplugged

1. omit `southchip,allow-experimental-dual-cp`;
2. boot/load physical + dual + policy-diagnostic modules;
3. confirm the virtual coordinator has no writable Stage-5B controls;
4. read `usb_supply`, `source_state`, `combined_state`;
5. capture physical and virtual register/state snapshots before/after reads;
6. verify the diagnostic layer caused no CP/register/source change.

#### F1 — ordinary 5 V

1. attach a known ordinary 5 V source;
2. capture `/sys/class/power_supply/qcom-battmgr-usb/*` relevant properties;
3. capture Stage-6A `source_state` and `combined_state`;
4. compare Qualcomm VBUS with both SC8547 VBUS ADCs;
5. leave both CPs off.

This is the unit/scaling sanity gate. Do not interpret later PD/PPS data until
5 V observation is internally consistent.

#### F2 — fixed PD observation

1. use a known PD adapter/cable;
2. allow the existing Qualcomm firmware/mainline stack to select its normal
   fixed contract by itself;
3. Stage 6A must not request a contract;
4. capture USB type, voltage/current values and both CP VBUS ADCs;
5. verify CPs remain off.

#### F3 — PPS-capable observation

1. use a known PPS-capable adapter/cable;
2. observe whether `USB_TYPE` becomes `PD_PPS` naturally;
3. capture all Stage-6A source fields and pair telemetry with CPs off;
4. record adapter/cable and direct qcom-battmgr sysfs values alongside the
   diagnostic output;
5. additionally capture `/sys/class/usb_power_delivery/` and partner source PDO
   capabilities when UCSI exposes them.

F3 is evidence for Stage 6B design; it is **not** permission to send PPS SET
messages.

## Cross-stage evidence tool

Commits `8044c92`/`273900a`/`390b2b3` form an optional read-only evidence-tool
package. They may be added to any test branch without changing charging control
semantics.

Example:

```sh
SC8547_KERNEL_COMMITS='<exact stage package>' \
SC8547_DT_COMMITS='<exact DT commit>' \
SC8547_ADAPTER='<adapter>' \
SC8547_CABLE='<cable>' \
./scripts/sc8547-stage-capture.sh 1 unplugged
```

For write-capable stages, run the collector before and after the **manual** write
sequence; the script itself must remain read-only.

## Why the temporary API-refactor commits must not be replayed

The branch-construction scaffolding was:

```text
eaa26b8  add script
63caf142 add workflow
9b29a00  generated functional result
7aa2478  remove script
f5bc4a9  remove workflow
```

A clean test branch needs the result (`9b29a00`) plus API prerequisites, not the
machinery that produced it.

## Stage 6B implementation discipline

Stage 6A is a hard read-only checkpoint. Current Stage-6B commits are design and
evidence only.

If real Caihong data confirms the Oplus firmware ABI, the first write-capable
implementation must be split across explicit checkpoints:

```text
kernel tree
  K0  firmware-ABI gate/private plumbing; no callable source SET
  K1  validated BAT/USB SET response parsing; still no electrical caller
  K2  fixed-5V fallback helper only
  K3  bounded exact PPS helper; K2 used on partial failure

out_tree_module
  U0  bounded source-only manual consumer; both CPs required off
```

K2 must be hardware-tested before K3. U0 must not enable a charge pump. Only a
later Stage 6C may combine a validated source bridge with Stage-5B CP control.

Before K0-K3 source code is created, the documentation/hardware evidence must
identify or verify:

- exact Qualcomm/Oplus firmware opcode + property semantics;
- effective Caihong SoCCP/non-SoCCP property namespace in runtime/compiled DT;
- explicit firmware-ABI gate independent of generic SoC variant;
- Fixed-PDO vs PPS separation;
- units and valid ranges;
- APDO capability-discovery path;
- validated SET response/ACK condition;
- service-down/detach behavior;
- CP-off source-only test;
- guaranteed explicit fallback to fixed/basic 5-V charging;
- first conservative voltage/current request bounded by real adapter capability.

Only after the source bridge independently passes those tests may Stage 6C
combine source requests with charge-pump start/ramping.
