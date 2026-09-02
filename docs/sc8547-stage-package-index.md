# SC8547 canonical stage package index

This is the compact answer to: **which commits belong to each hardware-test
stage?**

Use it together with:

```text
docs/sc8547-stage-test-plan.md        # exact operator/test procedure
docs/sc8547-commit-test-matrix.md     # meaning of every development commit
```

Do not construct a hardware-test branch from `git log` order alone.

Legend:

- `F` functional/runtime or build-required source change
- `D` documentation/test contract
- `C` CI only
- `T` temporary engineering machinery; never cherry-pick into a clean test
  branch

## Stage 0 — baseline telemetry

Base:

```text
159d2e5  F  main baseline telemetry
```

Test procedure: Stage 0 in `sc8547-stage-test-plan.md`.

## Stage 1 — variant + register snapshot

Required functional package:

```text
60c230c  F  variant model/runtime ID/register snapshot
```

Recommended process docs:

```text
d425680  D  initial staged roadmap
ab9c7b0  D  branch/test workflow policy
```

Do not add Stage-3/4 write opt-ins while testing Stage 1.

## Stage 2 — protection decode, read-only

Add on top of passed Stage 1:

```text
bc553aa  F  read-only protection decode
f15fb5a  F  bitfield include required for build
25d4a3d  D  raw/formula/comment ambiguity contract
```

No new charge-pump write belongs to Stage 2.

## Stage 3 — controlled init, CP remains off

Add only after Stage 0-2 real-hardware pass:

```text
aa3c510  F  gated reset/init/watchdog
29e6c6c  F  role-safe profile + fail-closed tightening
17d7037  F  Linux-v7.2 compatibility
```

Docs/CI:

```text
2173e10  D  initial Stage-3 design
d6509c6  D  role-safe Stage-3 documentation
4a0df97  C  focused Linux-v7.2 charging CI
```

Never test `aa3c510` alone on the current plan; use the complete three-commit
functional package.

## Stage 4 — manual single-pump control

Add after Stage 3 passes on the physical pump being tested:

```text
8033a03  F  manual work_mode + single-pump cp_enable
```

Docs/status:

```text
ffd115b  D  Stage-4 design/test contract
1f9ab5c  D  Stage-3/4 roadmap checkpoint
4727cb9  D  Stage-4 focused-v7.2 build status
```

Primary and secondary must pass independently before Stage 5B.

## Stage 5A — read-only virtual pair, current shared-API package

Canonical functional package on top of the earlier physical stages:

```text
18581b0  F  add read-only sc8547_dual coordinator
ec05245  F  build sc8547_dual.ko
6986e8c  F  validate exact primary/secondary pairing
60cb97e  F  shared physical API declaration
e854be9  F  opaque shared API contract
9b29a00  F  implement shared Stage-4 safety/state API
7caaee0  F  dual telemetry consumes shared physical state API
```

Docs/CI:

```text
1801dae  D  virtual coordinator DT architecture
764f821  D  Stage-5A build status
3cbdb91  D  shared physical API ownership contract
edf0998  C  historical two-module focused CI
```

Explicitly exclude temporary refactor machinery:

```text
eaa26b8  T  NEVER
63caf142 T  NEVER
7aa2478  T  NEVER
f5bc4a9  T  NEVER
```

Stage 5A must omit `southchip,allow-experimental-dual-cp`.

## Stage 5B — explicit dual-pump laboratory control

Only after both pumps independently pass Stage 4 and Stage-5A/shared-API
regression passes:

```text
25d0e27  F  gated dual work_mode + dual_enable coordinator
```

Required/recommended docs:

```text
0c92c3a  D  implemented dual control/rollback test contract
2fc76f1  D  Stage-5B focused-v7.2 status/roadmap
```

This is the first virtual dual-pump write stage. It still performs no source
PD/PPS negotiation.

## Stage 6A — read-only USB source + CP correlation

Stage 6A may be built on a passed Stage 5A branch **without Stage 5B**.

Add:

```text
e16d7c2  F  declare read-only sc8547_dual_state API
fc32392  F  implement/export sc8547_dual_get_state()
9ec6520  F  add read-only sc8547_policy_diag
756acee  F  build sc8547_policy_diag.ko
```

CI/docs:

```text
97892e6  C  focused v7.2 W=1 builds all three charging modules
a48fa1f  D  Stage-6 architecture, split 6A/6B/6C
f0a0e9d  D  downstream PPS startup/ramp/rollback evidence
93ba0ed  D  complete stage-by-stage hardware test manual
```

Process/history checkpoints around this stage:

```text
0dc0c2d  D  create commit-by-commit merge/test matrix
eb7c82d  D  register Stage-6A commits/test track in matrix
b502b0f  D  Stage-6A roadmap/build status
df488ef  D  register later Stage-6 protocol evidence
```

Stage 6A must keep `southchip,allow-experimental-dual-cp` absent when the goal
is the independent read-only source-observation track.

## Stage 6B — source-contract bridge

Status: **research/documentation only; no functional source-write commit exists
yet.**

Current Stage-6B research/design package:

```text
3e48606  D  UCSI vs Qualcomm/Oplus source-interface evidence
fedc9b6  D  one-owner qcom_battmgr transaction/ACK constraint
57c91c   D  exact PPS-request + fixed-5V fallback candidate contract
cef2b43  D  gated qcom_battmgr kernel-patch design and K0→K3 test ladder
6aa84fd  D  refresh ownership design with SM8650 variant/gate/service details
```

`fedc9b6` is the actual ownership-document commit. An earlier version of this
index incorrectly listed non-existent short SHA `538dad3`; do not use that
value when constructing a test/history branch.

Important conclusions represented by the Stage-6B package:

- UCSI `GET_PDOS` may provide standard APDO capability observation;
- UCSI `SET_PDOS` is not an exact PPS sink `(mV,mA)` request;
- UCSI `SET_POWER_LEVEL` is a power ceiling/renegotiation primitive, not the
  exact programmable PPS tuple needed by the downstream control loop;
- an independent second BATTMGR-owner PMIC-Glink client is unsafe;
- if Oplus private PPS properties are required, the existing qcom_battmgr
  transaction owner must be extended/refactored;
- SM8650 PMIC-Glink falls through the `sm8650`→`sm8550` compatible chain and
  uses SM8550-family battmgr handling, but **SoC variant alone is not sufficient
  authorization for the Oplus property namespace**;
- the PMIC-Glink `power-supply` auxiliary device inherits the parent's OF node,
  so a future explicit firmware-ABI gate can live on the PMIC-Glink node and be
  read by the existing qcom_battmgr instance;
- non-SoCCP PPS request candidate is USB SET `0x33`, properties 34/35;
- downstream normal-return candidate explicitly requests fixed PDO 5 V;
- non-SoCCP fixed-5V fallback is BAT SET `0x31`, Oplus `BATT_SET_PDO` property
  25, value 5000 mV;
- Oplus property 25 collides numerically with a different upstream Qualcomm
  extension meaning, so it must never be treated as generic qcom_battmgr ABI;
- Linux v7.2's current SM8350/SM8550 callback does not yet provide the explicit
  validated SET-response handling required for these private operations;
- PMIC-Glink service-down does not make a pending property request successful;
  a timeout/service reset leaves source-contract state unknown and requires
  later re-observation;
- the future kernel-tree bridge is split deliberately into K0 platform/ABI
  gate, K1 validated SET-response parsing, K2 fixed-5V fallback, then K3 exact
  PPS request. Fallback is hardware-tested before PPS elevation.

There is intentionally **nothing to cherry-pick as a Stage-6B functional
hardware write yet**.

Before the first future Stage-6B functional commit exists, the gates in these
files must be satisfied:

```text
docs/sc8547-stage-test-plan.md
docs/sc8547-stage6b-source-interface-research.md
docs/sc8547-stage6b-qcom-battmgr-ownership.md
docs/sc8547-stage6b-request-fallback.md
docs/sc8547-stage6b-qcom-battmgr-patch-design.md
```

Future Stage-6B functional commits are expected to span two repositories and
must be recorded separately:

```text
kernel tree:
  6B-K0  explicit Oplus firmware-ABI gate/plumbing, no callable SET
  6B-K1  validated BAT/USB SET response parser, still no electrical caller
  6B-K2  fixed-5V fallback helper only
  6B-K3  bounded exact PPS request helper

out_tree_module:
  6B-U0  source-only bounded test consumer, both SC8547 pumps required off
```

Do not create or test K2/K3/U0 until real Stage-6A captures and runtime DT have
unblocked the implementation gate.

## Stage 6C — automatic source + CP policy

Status: not implemented.

No Stage-6C functional commits exist.

Entry gate:

```text
Stage 5B hardware PASS
AND
Stage 6B source-only hardware PASS
```

## Cross-stage read-only evidence tooling

The following commits are **optional operator/test tooling** and are not a
charging-stage electrical capability increment:

```text
8044c92  F/tool  read-only scripts/sc8547-stage-capture.sh
273900a  C       validate capture script syntax in focused CI
390b2b3  D       document capture-tool usage/output and stage labels
```

The collector performs no sysfs/device-tree/kernel control write. It may be
cherry-picked into any Stage 0-6A test branch to standardize evidence capture.
For write-capable stages it records before/after snapshots only; the actual
write commands remain manual and are specified by `sc8547-stage-test-plan.md`.

Use environment metadata when collecting evidence:

```text
SC8547_KERNEL_COMMITS
SC8547_DT_COMMITS
SC8547_ADAPTER
SC8547_CABLE
SC8547_NOTES
```

This keeps every saved capture tied to the exact Stage package under test.

## Current clean hardware-test progression

Full write-capable path:

```text
Stage 0
→ Stage 1
→ Stage 2
→ Stage 3 primary + secondary
→ Stage 4 primary + secondary
→ Stage 5A + shared-API regression
→ Stage 5B
→ Stage 6B [future]
→ Stage 6C [future]
```

Independent read-only source path:

```text
Stage 5A PASS
→ Stage 6A
```

## Update rule

Every new charging-related commit must cause one of these outcomes in the same
development cycle:

1. add it to the appropriate Stage package above; or
2. classify it as documentation/CI/temporary/tooling and record why it is not
   part of the clean electrical-capability cherry-pick set.

For a write-capable Stage, the index must also state the exact previous hardware
PASS required before the new functional commit may be tested.

If a future commit cannot be assigned to one Stage, it is too broad and should
be split before hardware testing.
