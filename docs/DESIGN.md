# Porting react-native-worklets to react-native-windows — design

Research target: **react-native-worklets 0.7.4** against **react-native-windows
0.83.2** (New Architecture, Hermes, NuGet-prebuilt `Microsoft.ReactNative`,
C++/WinRT `/std:c++20`). All package-structure claims below were made by
reading the worklets 0.7.4 sources in `node_modules/react-native-worklets/`
(July 2026).

## 1. How react-native-worklets is built

The package splits cleanly into **portable core C++** and **thin per-platform
glue**.

### 1a. Portable core — `Common/cpp/worklets/` (~40 files)

| Area | Files | Role |
| --- | --- | --- |
| Module proxy | `NativeModules/WorkletsModuleProxy.{h,cpp}`, `NativeModules/JSIWorkletsModuleProxy.{h,cpp}` | The JSI HostObject installed as `global.__workletsModuleProxy`; owns the JS scheduler, UI scheduler, runtime manager, memory manager. |
| Runtime | `WorkletRuntime/WorkletRuntime.{h,cpp}`, `RuntimeManager.{h,cpp}`, decorators | Creates/decorates worklet runtimes; installs globals (`_WORKLET`, `_scheduleOnUI`, …). |
| **Hermes binding** | `WorkletRuntime/WorkletHermesRuntime.{h,cpp}` | **Hermes-bound.** `#include <hermes/hermes.h>`; wraps `std::unique_ptr<facebook::hermes::HermesRuntime>`. Guarded by `#if JS_RUNTIME_HERMES`. |
| Serialization | `SharedItems/Serializable.*`, `Synchronizable.*` | Marshals values/functions between runtimes; relies on Hermes bytecode sharing for worklet functions. |
| Scheduling | `Tools/JSScheduler.{h,cpp}`, `Tools/UIScheduler.{h,cpp}`, `RunLoop/*`, `AnimationFrameQueue/*` | `UIScheduler` is a **base class with a queue**; the actual "wake the UI thread" call is provided per platform. |

Key portability facts:

- **`UIScheduler` (Common) is portable and abstract.** `scheduleOnUI(job)`
  pushes to a queue; `triggerUI()` drains it. Platforms subclass it and
  override `scheduleOnUI` to enqueue **and** post a drain onto their UI loop.
  **This is the single seam a Windows port fills** — and the seam this repo's
  scaffold proves works over RNW's `IReactDispatcher`/`DispatcherQueue`.
- **`JSScheduler` is portable** but needs a `facebook::react::CallInvoker` —
  RNW provides one (`ReactContext.CallInvoker()` / `MakeAbiCallInvoker` in
  `Microsoft.ReactNative.Cxx/ReactContext.h`).
- **`WorkletHermesRuntime` is the hard dependency.** The UI worklet runtime is
  a **second Hermes VM**, constructed from a concrete
  `facebook::hermes::HermesRuntime` (`hermes/hermes.h`). Worklet functions
  ship to it as Hermes bytecode; source and target runtimes must both be
  Hermes. There is no non-Hermes path on 0.7.x.

### 1b. Per-platform glue — what each OS provides

- **iOS**: `WorkletsModule.mm` (grabs `self.bridge.runtime`, builds a
  `MessageQueueThread` over the RN JS run loop, `IOSUIScheduler` over
  `dispatch_get_main_queue()`, a `CADisplayLink` RAF source, then constructs
  `WorkletsModuleProxy` and decorates the RN runtime).
- **Android**: same shape via JNI/fbjni (`AndroidUIScheduler` posts to a Java
  `MessageQueue`).

The glue per OS is small: (1) a TurboModule that can reach the real
`jsi::Runtime` + `CallInvoker` + `MessageQueueThread`; (2) a `UIScheduler`
subclass over the OS UI loop; (3) a vsync `requestAnimationFrame` source. A
**Windows** port needs the same three, **plus a Hermes VM factory for the UI
runtime** — and that last item is the blocker.

### 1c. JS entry contract — you cannot partially install

- `src/specs/NativeWorkletsModule.ts`:
  `TurboModuleRegistry.get<Spec>('WorkletsModule')` with one method,
  `installTurboModule(): boolean`.
- `NativeWorklets` then **requires** `global.__workletsModuleProxy` to be a
  fully populated JSI HostObject (~30 methods: `createSerializable*`,
  `scheduleOnUI`, `executeOnUIRuntimeSync`, `createWorkletRuntime`,
  `scheduleOnRuntime`, `synchronizable*`, feature flags, …). If any of it is
  missing it throws at import.

**Consequence:** a native module named `WorkletsModule` that half-installs
makes the app throw at boot. Any Windows install must populate the **entire**
proxy or not answer to the name at all. This repo's scaffold uses a distinct
name (`RNWWorkletsRuntime`) for exactly this reason.

## 2. Why a real runtime cannot be built app-side today

RNW 0.83's prebuilt `Microsoft.ReactNative` NuGet exposes the JS runtime to
native code only as an **ABI/NAPI `jsi::Runtime` wrapper** (`JsiAbiApi.h` /
NodeApiJsi) — **not** as a linkable `facebook::hermes::HermesRuntime` — and
ships neither `hermes/hermes.h` nor a `MessageQueueThread` header on the app
include path. The UI worklet runtime (a second Hermes VM sharing bytecode with
the RN runtime) therefore cannot be constructed from an app project.

| Option | Feasible app-side (NuGet)? | Notes |
| --- | --- | --- |
| **A. Full worklets (separate UI Hermes runtime)** | **No.** | Needs concrete HermesRuntime + ReactCommon internals → requires an RNW **source build** (or an upstream RNW worklets port compiled inside `Microsoft.ReactNative`). |
| **B. JS-thread-only worklets (no UI runtime)** | **Partially.** | Install `__workletsModuleProxy` over the ABI `jsi::Runtime`; `scheduleOnUI`/`runOnUI`/`runOnRuntime` execute the worklet **on the RN JS runtime**. Requires re-enabling the reanimated babel plugin on Windows and authoring the full proxy HostObject in C++/JSI. Animations run, but on the JS thread (no true off-thread UI animation). |
| **C. Status quo — pure-JS shims** | Shipping in our production app today. | reanimated static shim + worklets no-op shim. No animation; never crashes. |

## 3. Phased roadmap

### Phase 0 — Scaffold + capability probe (THIS REPO — done, unbuilt standalone)

`windows/scaffold/WorkletsRuntimeModule.{h,cpp}`:

- `getCapabilities()` (sync) reports `nativeUiRuntime:false`,
  `jsThreadWorkletsFeasible:true`, `hermesConcreteRuntime:false`, dispatcher
  availability, `recommendedInstallMode:"js-thread-shim"`.
- `verifyUiDispatch()` hops onto `UIDispatcher()` and returns UI vs. caller
  thread ids — proving the `DispatcherQueue` seam a `WindowsUIScheduler`
  builds on.
- Contains `WindowsUIScheduler` as a live, null-dispatcher-guarded sketch of
  the `worklets::UIScheduler` subclass, written out in the exact form it takes
  once worklets headers are linkable (see the block comment in the `.cpp`).

### Phase 1 — Decide the route

**A** (RNW source build, real worklets) vs **B** (JS-thread executor).
Recommendation: A if reanimated-heavy UI needs real 60fps off-thread
animation; B is a cheaper stepping stone that already makes `'worklet'`
functions execute for real.

### Phase 2 — Option B: JS-thread worklet executor (medium)

1. Re-enable `react-native-reanimated/plugin` for windows in babel config.
2. Author a C++/JSI installer (attributed module `REACT_INIT` →
   `ExecuteJsi(context, …)`) that builds a HostObject implementing the full
   `WorkletsModuleProxy` surface where `createSerializable*` wrap values in a
   trivial holder (one runtime — no cross-VM marshaling), `scheduleOnUI` &
   friends invoke the held worklet on the JS runtime, `createWorkletRuntime`
   returns a token aliasing the JS runtime, and `synchronizable*` back onto a
   mutex + value cell.
3. Set `global.__RUNTIME_KIND` and `global.__workletsModuleProxy` before
   reanimated imports; swap the reanimated shim for the real JS.

Real but bounded: a large HostObject, not a VM.

### Phase 3 — Option A: real worklets in an RNW source build (multi-week)

Author the Windows platform port mirroring `apple/`:

- `WorkletsModule.{h,cpp}` — TurboModule `WorkletsModule` +
  `installTurboModule`, reaching the concrete `jsi::Runtime`, `CallInvoker`,
  and a `MessageQueueThread` from inside the RNW instance (source access).
- `WindowsUIScheduler.{h,cpp}` — `: public worklets::UIScheduler` over
  `IReactDispatcher` (body already sketched in this repo's scaffold).
- `WindowsMessageQueueThread.{h,cpp}` — `MessageQueueThread` over the RNW JS
  `DispatcherQueue`.
- `WorkletsAnimationFrameQueue.{h,cpp}` — RAF via `DispatcherQueueTimer` /
  composition vsync.
- `HermesUIRuntimeFactory.cpp` — the UI `facebook::hermes::HermesRuntime`,
  linking the same Hermes the RNW instance uses (hence the source build).
- Compile `Common/cpp/worklets/**` into that project; define
  `JS_RUNTIME_HERMES`.

### Phase 4 — Reanimated bring-up + upstream

Un-shim reanimated incrementally behind a feature flag; verify gesture
interplay and layout animations. **When (and only when) this is real, offer
the Windows platform port upstream to Software Mansion** — the portable core
already anticipates platforms; the glue is designed to slot into their layout.
