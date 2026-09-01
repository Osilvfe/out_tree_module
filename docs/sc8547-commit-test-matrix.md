# SC8547 commit-by-commit merge and test matrix

This file is the authoritative history/merge guide for the charging work on
`sc8547-next`.

The development branch is intentionally linear and contains implementation,
documentation, CI and a few one-shot engineering commits. **Do not equate
`git log` order with the order that should be cherry-picked into `main`.**

For every future charging commit, update this matrix in the same development
cycle. Each entry must answer five questions:

1. What capability or contract does this commit add/change?
2. Does it introduce a hardware write or only observation/documentation?
3. Which earlier functional commits does it require?
4. What is the first hardware test that becomes meaningful after it?
5. Is it part of the eventual `main` cherry-pick chain, optional, superseded,
   or explicitly forbidden to cherry-pick?

## Classification

- **F** — functional source change; may affect runtime behavior.
- **D** — documentation/test-contract change only.
- **C** — CI/build infrastructure.
- **T** — temporary engineering machinery; never part of a release/test merge.

Promotion labels:

- **YES, after gate** — intended for `main` only after the stated hardware gate.
- **YES, read-only** — safe candidate to merge earlier once basic telemetry is
  trusted because it adds no new charge-pump control write.
- **OPTIONAL** — useful documentation/CI but not required by runtime code.
- **SUPERSEDED** — historical step replaced by a later commit; do not cherry-pick
  it when constructing a clean test branch unless reproducing history.
- **NEVER** — branch-construction artifact; do not cherry-pick.

## Base

`main` base for this development series:

```text
159d2e5 Update SC8547 telemetry test guide
```

The baseline driver on `main` is telemetry-first. Apart from ADC enable, it does
not intentionally start the charge pump or rewrite protection thresholds.

## Complete `sc8547-next` history before Stage 6

| # | Commit | Class | Meaning / new boundary | Future merge and first meaningful test |
|---:|---|:---:|---|---|
| 1 | `60c230c` | F | Models SC8547/SC8547A/SC8547D variants, ID handling, masked shared controls and read-only register snapshot. No new CP/protection write. | **YES, read-only.** First test: both chips' `variant` + `register_dump` unplugged and at normal 5 V. |
| 2 | `d425680` | D | Creates staged development roadmap so later write-capable work has explicit gates. | **OPTIONAL but recommended before testing.** No new hardware test. |
| 3 | `ab9c7b0` | D | Records branch policy and validation workflow (`main` conservative, `sc8547-next` development). | **OPTIONAL.** Administrative/test-process contract only. |
| 4 | `bc553aa` | F | Adds read-only protection register decode alongside raw values. Does not program protection. | **YES, read-only**, requires #1. Compare decode vs raw register dump and downstream evidence. |
| 5 | `25d4a3d` | D | Documents Stage-2 protection decode and the ambiguity in vendor threshold comments/formulas. | **OPTIONAL but recommended with #4.** Prevents later testers from treating decoded values as validated electrical limits. |
| 6 | `2173e10` | D | Documents the proposed gated Stage-3 init before its source implementation. | **OPTIONAL.** Defines the Stage-3 lab contract. |
| 7 | `f15fb5a` | F | Explicitly includes bitfield helpers needed by Stage-2 decode. Runtime semantics unchanged. | **YES with #4.** Compile/test prerequisite, no new hardware action by itself. |
| 8 | `aa3c510` | F | Adds the first write-capable Stage-3 path: explicit opt-in, raw protection profile, controlled init/reset/watchdog handling, CP left disabled. | **YES, after Stage 0-2 gate.** First write test is `apply_init`; verify exact readback, CP remains off and basic PMIC charging still works. |
| 9 | `29e6c6c` | F | Tightens Stage-3 profile handling for primary/secondary role differences and fail-closed behavior. | **YES with #8 before any Stage-3 hardware test.** Do not test #8 alone on current hardware plan. |
| 10 | `3ec6a02` | C | Adds an early SC8547 module CI path. | **SUPERSEDED** by #13/#21. Do not use as the current compile gate. |
| 11 | `d6509c6` | D | Aligns Stage-3 docs with the role-safe source behavior from #9. | **OPTIONAL but recommended.** No new hardware behavior. |
| 12 | `17d7037` | F | Fixes APIs/formatting for torvalds Linux v7.2. | **YES with Stage 3.** Required before treating v7.2 compile results as meaningful. |
| 13 | `4a0df97` | C | Establishes focused Linux-v7.2 `W=1` SC8547 CI, isolating charging from unrelated pogo/touchscreen failures. | **OPTIONAL for local runtime; recommended in repo.** This becomes the charging compile gate. |
| 14 | `ffd115b` | D | Defines Stage-4 single-pump manual-control design, second opt-in, explicit VBUS/VBAT windows and rollback rules. | **OPTIONAL but strongly recommended before #16 testing.** |
| 15 | `1f9ab5c` | D | Records Stage-3 → Stage-4 hardware gates in the roadmap. | **OPTIONAL.** Clarifies that compile success never unlocks the next electrical stage. |
| 16 | `8033a03` | F | Implements Stage-4 manual `work_mode` and `cp_enable`. Enable repeats preflight, sets only shared enable bit, waits 500 ms and validates switching/fault/window state; failure disables CP. | **YES, after Stage-3 hardware pass.** Test primary and secondary **separately**, repeatedly; no dual test yet. |
| 17 | `4727cb9` | D | Marks #16 focused-v7.2 build-verified and splits Stage 5 into read-only 5A and write-capable 5B. | **OPTIONAL.** Important process checkpoint, no runtime change. |
| 18 | `d020fd1` | D | First Stage-5 dual-pump architecture/test document. Chooses an explicit virtual coordinator above two physical CPs. | **OPTIONAL.** Superseded in details by later Stage-5 doc updates, but architectural intent remains. |
| 19 | `18581b0` | F | Adds `sc8547_dual.ko` as a **read-only** virtual coordinator with phandle pairing and aggregate telemetry. No dual CP writes. | **YES, read-only** after basic physical telemetry is sound. First test: peer identity, two ADC snapshots and aggregate IBUS with both pumps off. |
| 20 | `ec05245` | F/build | Adds `sc8547_dual.ko` to the OOT Makefile. | **YES with #19** if building the coordinator. No independent hardware behavior. |
| 21 | `edf0998` | C | Updates focused CI to build both `sc8547_cp.ko` and `sc8547_dual.ko`. | **OPTIONAL for runtime; current repo CI prerequisite.** Supersedes #10 for dual-module builds. |
| 22 | `1801dae` | D | Aligns Stage-5 documentation to the separate virtual coordinator DT node. | **OPTIONAL but recommended with #19.** |
| 23 | `6986e8c` | F | Requires resolved clients to be distinct, bound to the SC8547 physical driver and labelled exact `primary` / `secondary`. | **YES with Stage 5A.** Test deliberate wrong-role DT once; coordinator must refuse the pair without hardware writes. |
| 24 | `764f821` | D | Marks Stage 5A focused-v7.2 build-verified and records its hardware gate. | **OPTIONAL.** Status/documentation only. |
| 25 | `60cb97e` | F/API | Introduces the shared physical-driver API header needed so virtual/policy layers do not duplicate Stage-4 safety logic. Header alone adds no new runtime path. | **YES only as part of the current shared-API chain**, not useful alone. |
| 26 | `e854be9` | F/API | Makes the shared API opaque to physical private structures; exposes only state and controlled operations. | **YES with #25.** Architectural prerequisite for clean layering. |
| 27 | `eaa26b8` | T | Adds a one-shot exact-refactor script used to transform `sc8547.c`. | **NEVER.** Historical engineering tool only. |
| 28 | `63caf142` | T | Adds the one-shot workflow that runs the refactor script and commits the result. | **NEVER.** Historical engineering tool only. |
| 29 | `9b29a00` | F | Actual shared-safety refactor: exports physical state/mode/preflight/enable/disable helpers and makes Stage-4 sysfs use the same implementation. Intended behavior is unchanged; safety logic becomes single-source. | **YES with #25/#26 before current Stage 5B.** Regression-test Stage 4 exactly as before before testing dual control. |
| 30 | `7aa2478` | T | Removes the one-shot refactor script. | **NEVER** in a clean cherry-pick series; #27/#30 cancel branch-only tooling. |
| 31 | `f5bc4a9` | T | Removes the one-shot refactor workflow. | **NEVER** in a clean cherry-pick series; #28/#31 cancel branch-only tooling. |
| 32 | `3cbdb91` | D | Documents the shared API contract: physical driver owns locking, authorization, windows and fail-closed post-checks. | **OPTIONAL but strongly recommended** with #29. |
| 33 | `7caaee0` | F | Moves Stage-5A virtual telemetry onto `sc8547_get_state()`, removing duplicate raw SMBus interpretation from the coordinator. | **YES for the current Stage-5 line**, requires #25/#26/#29. Re-run Stage-5A read-only tests before enabling Stage 5B. |
| 34 | `25d0e27` | F | Implements Stage-5B gated `work_mode` + `dual_enable`: primary→secondary start, reverse stop, two preflights before first enable, final pair check and fail-closed rollback. Third virtual-node opt-in required. | **YES, only after both pumps independently pass Stage 4.** First dual test uses an already validated ratio/source/window and immediately stops after snapshot capture. |
| 35 | `0c92c3a` | D | Documents the implemented Stage-5B controls, rollback, concurrency model and safe failure-path tests. | **OPTIONAL but should accompany #34 for any hardware test branch.** |
| 36 | `2fc76f1` | D | Marks Stage 5B focused Linux-v7.2 `W=1` build-verified and records the current clean cherry-pick chain. | **OPTIONAL.** Status checkpoint only; hardware gate remains unmet. |

## Clean functional cherry-pick tracks

### Track A — safest first device session: read-only physical telemetry

Starting from `main` (`159d2e5`):

```text
60c230c
bc553aa
f15fb5a
```

Recommended docs alongside it:

```text
25d4a3d
```

Test in this state before any later functional commit:

1. boot with both physical `0x6f` nodes;
2. verify ID/variant/role;
3. capture `register_dump`, `protection_state`, ADC/status/faults unplugged;
4. attach a normal 5 V source and capture the same data;
5. confirm CP does not unexpectedly switch;
6. confirm existing pmic-glink/basic charging remains functional.

### Track B — Stage 3 controlled init

Only after Track A passes:

```text
aa3c510
29e6c6c
17d7037
```

Recommended docs:

```text
2173e10
d6509c6
```

Test **one physical pump at a time**. `apply_init` must leave the pump off,
protection bytes must read back exactly, ADC/status must remain plausible, and
normal charging must survive. Do not proceed on any unexplained fault.

### Track C — Stage 4 single-pump switching

Only after Stage 3 passes:

```text
8033a03
```

Recommended docs:

```text
ffd115b
1f9ab5c
```

Test primary first, stop it, then test secondary separately. Repeat each several
times with the same controlled source and explicit voltage windows. The first
success criterion is not charging power; it is deterministic enable → switching
validation → disable with no new faults/regressions.

### Track D — Stage 5A read-only virtual pair

This can be tested earlier than dual switching, but for a clean current stack
prefer the shared-API version:

```text
18581b0
ec05245
6986e8c
60cb97e
e854be9
9b29a00
7caaee0
```

`edf0998` is CI-only. Do not cherry-pick #27/#28/#30/#31.

Test with both pumps off. Verify correct pairing, both snapshots and aggregate
IBUS. Re-run Stage-4 single-pump smoke tests after `9b29a00` because it refactors
the Stage-4 code path even though behavior is intended to be unchanged.

### Track E — Stage 5B dual switching

Only after **both** physical pumps have passed Stage 4 on real hardware:

```text
25d0e27
```

Required test document:

```text
0c92c3a
```

Use the Stage-5B test sequence in `sc8547-stage5-dual-coordinator.md`. Start at
an already individually validated mode/source/window. Verify primary validated
switching occurs before secondary; immediately snapshot; then stop. Failure of
secondary or final validation must return both pumps to off.

## Why the temporary API-refactor commits must not be replayed

The four temporary history entries form branch-construction scaffolding:

```text
eaa26b8  add script
63caf142 add workflow
9b29a00  generated functional result
7aa2478  remove script
f5bc4a9  remove workflow
```

A clean test branch needs the **result** (`9b29a00`) plus its API prerequisites,
not the machinery that produced it. Replaying the temporary workflow can create
an unwanted bot commit or attempt to transform code that is already transformed.

## Future Stage-6 commit discipline

Stage 6 and later will follow a stricter pattern:

```text
D: document protocol evidence + test boundary
F: add read-only/refactor capability
C: extend focused CI
D: record exact commit meaning + hardware test step
```

A write-capable source commit will always have its design/test document **before
or in the immediately adjacent commit**, and this matrix will name its exact
predecessors and first safe test. No automatic PD/PPS/CP policy commit may be
marked `YES, after gate` until the source-contract ownership and rollback path
are explicitly documented.