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

Current Stage-6B research package:

```text
3e48606  D  UCSI vs Qualcomm/Oplus source-interface evidence
538dad3  D  one-owner qcom_battmgr transaction/ACK constraint
57c91c   D  exact PPS-request + fixed-5V fallback candidate contract
```

Important conclusions represented by these commits:

- UCSI `GET_PDOS` may provide standard APDO capability observation;
- UCSI `SET_PDOS` is not an exact PPS sink `(mV,mA)` request;
- an independent second BATTMGR-owner PMIC-Glink client is unsafe;
- if Oplus private PPS properties are required, the existing qcom_battmgr
  transaction owner must be extended/refactored;
- non-SoCCP PPS request candidate is USB SET `0x33`, properties 34/35;
- downstream normal-return candidate explicitly requests fixed PDO 5 V;
- non-SoCCP fixed-5V fallback is BAT SET `0x31`, Oplus `BATT_SET_PDO` property
  25, value 5000 mV;
- Oplus property 25 collides numerically with a different upstream Qualcomm
  extension meaning, so it must never be treated as generic qcom_battmgr ABI.

There is intentionally **nothing to cherry-pick as a Stage-6B functional
hardware write yet**.

Before the first future Stage-6B functional commit exists, the gate in
`sc8547-stage-test-plan.md` and `sc8547-stage6b-request-fallback.md` must be
satisfied.

## Stage 6C — automatic source + CP policy

Status: not implemented.

No Stage-6C functional commits exist.

Entry gate:

```text
Stage 5B hardware PASS
AND
Stage 6B source-only hardware PASS
```

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
2. classify it as documentation/CI/temporary and record why it is not part of
   the clean functional cherry-pick set.

If a future commit cannot be assigned to one Stage, it is too broad and should
be split before hardware testing.