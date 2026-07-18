> 🚨 **READ THIS FIRST: nothing in this repository runs yet.** This is a
> design + scaffold repo — a flag-plant and a collaboration invitation, not a
> shipping library. There is no worklet runtime here, no npm package, no
> animation. If you install this expecting Reanimated on Windows, you will be
> disappointed. If you want to help BUILD Reanimated on Windows, keep reading.

# react-native-windows-worklets

Toward a real **react-native-worklets** (and therefore
**react-native-reanimated 4.x**) runtime on **react-native-windows**.

## The gap

Reanimated-class animation **does not exist on react-native-windows. For
anyone.** react-native-worklets ships iOS and Android glue only; its UI-thread
runtime is a second Hermes VM that no one has ever stood up on Windows. Every
RNW app today either ships without modern animations or shims reanimated to
static no-ops (which is exactly what our own production Windows app does).

This repo is where we do the work in the open instead of in a private tree.

## What is actually in this repo today

| Item | Status |
| --- | --- |
| `docs/DESIGN.md` — a source-verified teardown of worklets 0.7.4 (portable core vs platform glue vs the Hermes-bound parts), why the Hermes binding cannot compile on Windows **even in an RNW source build**, the viable routes, and a phased roadmap | ✅ Done (research is real; §2 corrected 2026-07-18) |
| `docs/SPIKE.md` — scope + pass/fail criteria for the ~1-week experiment that would prove or kill the second-runtime route | ✅ Scoped, ❌ **not executed** |
| `windows/scaffold/WorkletsRuntimeModule.{h,cpp}` — a capability-probe TurboModule + a null-dispatcher-guarded `WindowsUIScheduler` sketch (the `ScheduleOnUI` pattern over RNW's `IReactDispatcher`, the exact analog of `IOSUIScheduler`/`AndroidUIScheduler`) | 🚧 Scaffold; compiling-quality, **excluded from any build**, never shipped in a binary |
| A worklet runtime | ❌ **Does not exist** |
| A JS package | ❌ Does not exist (deliberately no `package.json` — there is nothing to install) |
| Animations of any kind | ❌ No |

The scaffold is an extraction from a production monorepo's staging tree
(Facilitron FIT for Windows, RNW 0.83.2 new-arch); its sibling modules ship in
production, this one does not. Standalone build validation pending.

## Why this is genuinely hard (the one-paragraph version)

The worklets UI runtime is a **second Hermes VM**. Worklets constructs it from
a concrete `facebook::hermes::HermesRuntime` (`#include <hermes/hermes.h>`) and
ships worklet functions to it as Hermes bytecode. **On Windows that C++ class
does not exist.** Hermes reaches RNW as `Microsoft.JavaScript.Hermes`, a
**C-ABI / Node-API DLL**: its headers are `hermes/hermes_api.h`,
`hermes/js_runtime_api.h` and friends — no `facebook::hermes` namespace
anywhere in the package. `Microsoft.ReactNative` itself merely `LoadLibrary`s
`hermes.dll`, resolves `jsr_*` entry points via `GetProcAddress`, and wraps the
resulting `napi_env` in `NodeApiJsiRuntime` to obtain a `jsi::Runtime`. No
component in the process holds a `HermesRuntime` for a native module to borrow.

**Correction (2026-07-18):** an earlier version of this README blamed the
*prebuilt NuGet*, and implied an RNW **source build** would unlock the concrete
runtime. It does not. Our production app already builds RNW from source
(`UseExperimentalNuget=false`, framework `.vcxproj`s in the solution, a built
`Microsoft.ReactNative.dll` on disk) and the C++ Hermes class is still nowhere
in the tree — because the limitation is in how Hermes is **packaged for
Windows**, not in how RNW is consumed. We were wrong about the reason; the
conclusion (worklets' Hermes binding cannot be compiled as written) stands.

The interesting consequence: the Hermes C ABI *does* expose
`jsr_create_runtime` + `jsr_config_set_task_runner`, which plausibly yields a
second runtime on its own thread — reachable as a `jsi::Runtime`, just never as
a `HermesRuntime`. Whether that is enough to carry worklets is **unknown, and
we have not tried it.** [`docs/SPIKE.md`](docs/SPIKE.md) scopes the cheapest
experiment that would settle it. Full receipts, with file paths, in
[`docs/DESIGN.md`](docs/DESIGN.md) §2.

## The roadmap (honest sizes)

1. **Phase 0 — scaffold + probe** (this repo): prove the UI-dispatch seam
   (`ReactContext.UIDispatcher()` → `DispatcherQueue`) a `WindowsUIScheduler`
   is built on. Done as code; unbuilt standalone.
2. **Phase 1 — run the spike** ([`docs/SPIKE.md`](docs/SPIKE.md), ~1 week):
   can a native module in a source-built RNW stand up a *second* runtime via
   the Hermes C ABI and evaluate a function on it off the JS thread? This gates
   everything below it. **Not yet attempted.**
3. **Phase 2 (route B)** — full `__workletsModuleProxy` HostObject over the ABI
   `jsi::Runtime`; reanimated babel plugin re-enabled on windows. Animations
   run, but on the JS thread. Viable regardless of the spike outcome.
4. **Phase 3 (route A′, only if the spike passes)** — the real thing in an RNW
   source build: `WorkletsModule` + `WindowsUIScheduler` +
   `WindowsMessageQueueThread` + vsync RAF queue + a NAPI-backed UI-runtime
   factory, **plus a reimplementation of `WorkletHermesRuntime`** (it cannot be
   compiled as shipped — see `docs/DESIGN.md` §2b).
5. **Phase 4 — reanimated bring-up**, then **offer the Windows platform port
   upstream to Software Mansion**. That is the end state we want: this repo
   exists to make their `windows/` directory possible, not to compete with it.

## Warnings for the eager

- Do **not** name any Windows module `WorkletsModule` until it installs the
  *entire* `__workletsModuleProxy` HostObject. The worklets JS throws at import
  on a half-install (`docs/DESIGN.md` §1c). The scaffold uses a distinct name
  (`RNWWorkletsRuntime`) on purpose.
- Do not enable `react-native-reanimated/plugin` for windows while worklets
  resolves to a no-op shim: transformed `'worklet'` functions that reach a
  missing runtime crash Hermes.

## Contributing / collaborating

If you are Software Mansion, Microsoft's RNW team, or anyone with appetite for
Phase 2 or 3 — open an issue. We have a production RNW 0.83 new-arch app
(4 platforms, one Metro tree) to test against, an ARM64+x64 build lab, and a
proven track record of landing RNW native components (see
[react-native-webview-windows](https://github.com/FacilitronWorks/react-native-webview-windows),
[rnw-native-core](https://github.com/FacilitronWorks/rnw-native-core),
[react-native-windows-camera](https://github.com/FacilitronWorks/react-native-windows-camera)).
What we don't have is infinite hands.

## License

MIT.
