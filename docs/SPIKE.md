# Spike: can a second JS runtime be stood up off-thread inside RNW?

**Status: scoped, NOT executed.** Nothing below has been attempted. This
document exists so that the next person spends a week answering the question
instead of a month assuming an answer.

## Why this spike exists

[`DESIGN.md`](DESIGN.md) §2 establishes what is *not* possible: worklets'
`WorkletHermesRuntime` needs `facebook::hermes::HermesRuntime`, and that C++
class does not exist on Windows — Hermes ships as a C-ABI/Node-API DLL, and
`Microsoft.ReactNative` itself only ever holds a NAPI-backed `jsi::Runtime`. An
RNW source build does **not** change this.

What §2e leaves open is the interesting half: the Hermes C ABI exposes
`jsr_create_runtime` and `jsr_config_set_task_runner`, which *look like* they
provide the two things worklets fundamentally needs — a second runtime, and a
thread of its own. Nobody has checked whether they actually deliver that inside
an RNW process.

**That is a one-week question gating a multi-month project.** Answer it first.

## The question, stated precisely

> Can a native module inside a source-built `Microsoft.ReactNative` create a
> **second, independent JS runtime**, obtain it as a `jsi::Runtime&`, and
> evaluate a trivial function on it **on a non-JS thread**, without
> destabilising the main RN runtime?

Explicitly **not** asked here: worklet serialization, shared values, reanimated
compatibility, animation, or performance. Those are the *next* questions, and
several of them are harder than this one. Scope discipline is the whole point.

## What to build

A throwaway probe module — extend the existing capability-probe scaffold
(`windows/scaffold/WorkletsRuntimeModule.cpp`) rather than starting fresh. Five
steps, each independently informative:

| # | Step | Proves |
| --- | --- | --- |
| 1 | Link/resolve the Hermes C ABI from a module inside the source build: `LoadLibraryAsPeerFirst(L"hermes.dll")` + `GetProcAddress` for `jsr_*`, mirroring `Shared/HermesRuntimeHolder.cpp`. | The ABI is reachable from module code at all, and we get the *same* `hermes.dll` the instance uses. |
| 2 | `jsr_create_config` → `jsr_create_runtime` → `jsr_runtime_get_node_api_env`. | A second runtime can coexist with RN's in one process. **Most likely failure point** — Hermes may or may not tolerate a second runtime under RNW's loader/singleton assumptions. |
| 3 | Wrap the `napi_env` with `NodeApiJsiRuntime` (`node_modules/.node-api-jsi/node-api-jsi-*/src/NodeApiJsiRuntime.{h,cpp}`) to get a `jsi::Runtime&`. | The runtime is usable through the same abstraction the rest of worklets' portable core speaks. |
| 4 | `jsr_config_set_task_runner` with a task runner backed by a dedicated thread (or a `DispatcherQueueController`); evaluate `function(a,b){return a+b}` and call it. Return the result **and** the executing thread id to JS. | Off-JS-thread execution is real, not the JS thread wearing a hat. |
| 5 | Drive it from the scaffold's existing `verifyUiDispatch()` seam: schedule work from the JS thread onto runtime #2 and marshal a result back. | The `UIScheduler` seam composes with the second runtime — the actual worklets shape in miniature. |

Deliberately out of scope: any `WorkletsModule` naming (see the README
warning), any `__workletsModuleProxy` install, any JS-facing API beyond a debug
probe method, any bytecode transfer.

## Pass / fail

**PASS** — all of:
- Runtime #2 constructs and survives alongside the RN runtime; app remains
  stable across a reload and a suspend/resume cycle.
- The trivial function evaluates on runtime #2 and returns the correct value.
- The reported executing thread id is **≠** the JS thread id and ≠ the UI
  thread id.
- No crash, no `VerifyElseCrash`, no Hermes assert, over a few minutes of
  repeated scheduling; clean `jsr_delete_runtime` teardown.

A pass means route A′ in `DESIGN.md` §2f is worth planning — **not** that
worklets works. The serialization problem (§2e) is still unsolved and is the
next gate.

**FAIL** — any of:
- `jsr_create_runtime` fails or destabilises the primary runtime (global state,
  loader, or ICU singletons in `hermes.dll`).
- The second runtime can only be driven from the JS thread.
- `NodeApiJsiRuntime` cannot be instantiated outside `Microsoft.ReactNative`'s
  internals (link visibility / it is not exported).

A fail closes route A′ *on the current Hermes packaging* and makes the
conclusion actionable upstream: the ask to microsoft/hermes-windows becomes
concrete and evidence-backed ("ship the C++ VM headers/libs" or "support N
runtimes per process"), and route B (`DESIGN.md` §2f) becomes the realistic
ceiling for animation on RNW.

**Either outcome is a win.** A cheap fail is the second-best result and far
better than discovering it in week six of a port.

## Estimate

**~1 week, one engineer** with existing RNW source-build fluency (the build lab
and source-build config already exist; a cold start adds days, not hours).

| Step | Rough |
| --- | --- |
| 1–2 (ABI + second runtime) | 1–2 days — where the real risk lives |
| 3 (NodeApiJsiRuntime wrap) | 0.5–1 day, mostly build/link plumbing |
| 4 (task runner + off-thread eval) | 1–2 days |
| 5 (scheduler seam) | 1 day |
| Write-up | 0.5 day |

If step 2 fails, the spike ends on **day 2** with a real answer. That
asymmetry — cheap failure, bounded success — is the justification for doing it
before anything else.

## A note on scale

Even a clean pass leaves a **large** project: `react-native-worklets` is
maintained by a team at Software Mansion, and the Windows port would still need
a reimplemented Hermes binding, worklet-function transfer without bytecode
APIs, a vsync RAF source, the full `__workletsModuleProxy` surface, and
reanimated bring-up on top. This spike does not shorten that work. It only
tells us, cheaply, whether the work has a floor to stand on.
