# Porting react-native-worklets to react-native-windows — design

Research target: **react-native-worklets 0.7.4** against **react-native-windows
0.83.2** (New Architecture, Hermes, C++/WinRT `/std:c++20`). All
package-structure claims below were made by reading the worklets 0.7.4 sources
in `node_modules/react-native-worklets/` (July 2026).

> **Revision, 2026-07-18.** §2 previously claimed the Hermes blocker was a
> consequence of consuming `Microsoft.ReactNative` as a **prebuilt NuGet**, and
> that an RNW **source build** would therefore unlock a concrete
> `facebook::hermes::HermesRuntime`. **That was wrong**, and §2 is rewritten
> below. The blocker is not prebuilt-vs-source: Hermes on Windows ships as a
> **C-ABI / Node-API DLL with no C++ `HermesRuntime` class at all**, so RNW's
> own source cannot reach one either. The conclusion ("worklets'
> `WorkletHermesRuntime` cannot be compiled as written") survives; the reason
> changed, and the shape of the workaround changed with it. Receipts in §2.

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

## 2. Why `WorkletHermesRuntime` cannot be compiled on Windows — the real reason

This section was rewritten after actually checking a source build. The earlier
version blamed NuGet consumption; the truth is more fundamental and, oddly,
more tractable.

### 2a. The source build is real, and it does not help

Our production app (Facilitron FIT for Windows) builds `Microsoft.ReactNative`
**from source**, not from the NuGet:

- `fit-mobile/windows/ExperimentalFeatures.props` sets
  `<UseExperimentalNuget>false</UseExperimentalNuget>`.
- `fit-mobile/windows/FitMobile.sln` carries the five RNW framework projects
  (`Folly`, `fmt`, `ReactCommon`, `Common`, `Microsoft.ReactNative`) as
  `node_modules/react-native-windows/**/*.vcxproj` references.
- The artifact exists:
  `node_modules/react-native-windows/target/ARM64/Debug/Microsoft.ReactNative/Microsoft.ReactNative.dll`.

So "just build from source" is not a hypothetical for us — it is the shipping
configuration. **It still does not yield a `facebook::hermes::HermesRuntime`.**

### 2b. There is no C++ `HermesRuntime` on Windows, for anyone

`hermes/hermes.h` — the header `WorkletHermesRuntime.h` includes — **does not
exist anywhere in the dependency tree.** Searching `node_modules` and the
restored NuGet cache for a path matching `hermes/hermes.h` returns nothing.

What the pinned Hermes package
(`~/.nuget/packages/microsoft.javascript.hermes/0.0.0-2607.16001-3c6569ec`,
selected by `HermesVersion` in `ExperimentalFeatures.props`) actually ships
under `build/native/include/` is:

| Header | What it is |
| --- | --- |
| `hermes/hermes_api.h`, `hermes/hermes_icu.h`, `hermes/js_runtime_api.h` | **C ABI.** `jsr_*` / `hermes_*` free functions returning `napi_status`. |
| `jsi/jsi.h`, `jsi/decorator.h`, `jsi/threadsafe.h`, … | Plain JSI. |
| `node-api/js_native_api.h`, `node-api/node_api.h` | Node-API. |

There is **no C++ class header** — no `facebook::hermes` namespace, no
`makeHermesRuntime()`. Binaries under `build/native/win32/<arch>/` are
`hermes.dll` + `hermes.lib`, exporting that C ABI.

The one header in the tree named `hermes.h` is
`node_modules/react-native/ReactCommon/jsi/jsi/hermes.h` (also vendored at
`node_modules/.node-api-jsi/node-api-jsi-*/jsi/jsi/hermes.h`). That is
`class IHermes : public jsi::ICast` — RN's **abstract** Hermes-capability
interface (`getSHRuntime()`, `getVMRuntimeUnsafe()`), reached via
`jsi::castInterface`. It is `<jsi/hermes.h>`, not `<hermes/hermes.h>`, and it
is an interface, not the VM class worklets constructs.

### 2c. RNW itself only ever holds a NAPI-backed `jsi::Runtime`

The decisive file is
`node_modules/react-native-windows/Shared/HermesRuntimeHolder.cpp` (compiled
into the framework via `node_modules/react-native-windows/Shared/Shared.vcxitems`).
It does not link Hermes as C++ at all — it resolves it **dynamically, by name**:

```cpp
class HermesFuncResolver : public IFuncResolver {
 public:
  HermesFuncResolver() : libHandle_(LoadLibraryAsPeerFirst(L"hermes.dll")) {}
  FuncPtr getFuncPtr(const char *funcName) override {
    return reinterpret_cast<FuncPtr>(GetProcAddress(libHandle_, funcName));
  }
  ...
```

…then wraps the resulting `napi_env` in `NodeApiJsiRuntime`
(`node_modules/.node-api-jsi/node-api-jsi-*/src/NodeApiJsiRuntime.{h,cpp}`) to
produce a `jsi::Runtime`. `JSEngine.props:15` even defaults `HermesNoLink=true`
for Release, i.e. not linking Hermes statically is the *intended* posture.

**Therefore:** the ABI/NAPI `jsi::Runtime` is not a NuGet-consumption artifact
that a source build peels away. It is what `Microsoft.ReactNative` holds
internally, in its own source, on every build configuration. A native module
in the same process cannot obtain a concrete `HermesRuntime&` because **no
component in the process has one.**

### 2d. `JS_RUNTIME_HERMES` is not an RNW define

Worklets guards its Hermes binding with `#if JS_RUNTIME_HERMES`
(`WorkletHermesRuntime.h`). That symbol appears **nowhere** in
`node_modules/react-native-windows/`. RNW's own Hermes switch is a different
one: `React.Cpp.props:64` defines `USE_HERMES` when `$(UseHermes)=='true'`.
Defining `JS_RUNTIME_HERMES` for a Windows worklets build is therefore an
opt-in a porter would add — but doing so only activates a translation unit
whose `#include <hermes/hermes.h>` cannot resolve (§2b).

### 2e. What this *does* unlock — a second runtime, just not a `HermesRuntime`

The refutation is not entirely bad news. Worklets needs a **second JS runtime
it can drive from a non-JS thread**, and the C ABI exposes exactly that
(`build/native/include/hermes/js_runtime_api.h`):

- `jsr_create_config` / `jsr_create_runtime` / `jsr_delete_runtime` — create an
  independent Hermes runtime instance in-process.
- `jsr_config_set_task_runner` — give that runtime its own task runner, i.e.
  its own thread. This is the seam a `WindowsUIScheduler` would drive.
- `jsr_runtime_get_node_api_env` → a `napi_env`, which
  `NodeApiJsiRuntime` turns into a `jsi::Runtime&` — the same construction RNW
  uses for the main runtime (§2c).
- `jsr_create_prepared_script` / `jsr_run_script_buffer` — the bytecode-ish
  primitives worklets' function shipping would have to be rebuilt on.

So the honest statement is **not** "a second Hermes VM is impossible on
Windows." It is: *a second Hermes VM is reachable through the C ABI, but
worklets' `WorkletHermesRuntime` is written against a C++ class that does not
exist on this platform, so that file must be reimplemented rather than
compiled.* Whether the NAPI path is sufficient — particularly for cross-runtime
worklet serialization, which leans on Hermes bytecode sharing — is
**unproven**, and is precisely what [`SPIKE.md`](SPIKE.md) is scoped to answer.

### 2f. Options, restated

| Option | Feasible? | Notes |
| --- | --- | --- |
| **A. Full worklets, concrete `HermesRuntime`** | **No — and a source build does not change this.** | `hermes/hermes.h` does not exist in the tree; Hermes-windows ships a C ABI only (§2b/§2c). Would require microsoft/hermes-windows to also ship the C++ VM headers/libs, or an upstream change. |
| **A′. Second runtime via the Hermes C ABI** | **Unknown — worth a spike.** | `jsr_create_runtime` + `jsr_config_set_task_runner` + `NodeApiJsiRuntime` plausibly yields an off-thread `jsi::Runtime`. Requires reimplementing `WorkletHermesRuntime` on NAPI and re-solving worklet serialization. See `SPIKE.md`. |
| **B. JS-thread-only worklets (no UI runtime)** | **Partially.** | Install `__workletsModuleProxy` over the existing `jsi::Runtime`; `scheduleOnUI`/`runOnUI`/`runOnRuntime` execute on the RN JS runtime. Needs the reanimated babel plugin re-enabled on Windows and the full proxy HostObject authored in C++/JSI. Animations run, but on the JS thread. |
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

**A′** (second runtime over the Hermes C ABI) vs **B** (JS-thread executor).
Route A as originally written — a concrete `facebook::hermes::HermesRuntime` —
is off the table on this platform (§2b/§2c), so Phase 1 now *starts with*
[`SPIKE.md`](SPIKE.md): a few days of work that answers whether A′ is real
before anyone commits multi-week effort to it. If the spike passes, A′ is the
route for true off-thread animation; if it fails, B is the ceiling and is still
a genuine improvement over the status quo.

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

### Phase 3 — Option A′: real worklets in an RNW source build (multi-week, gated on the spike)

> Revised: this phase is contingent on [`SPIKE.md`](SPIKE.md) passing. The
> source build is necessary but **not** sufficient — see §2a. The UI runtime
> below must be built on the Hermes **C ABI** (`jsr_create_runtime` +
> `NodeApiJsiRuntime`), not on `facebook::hermes::HermesRuntime`, which does
> not exist on Windows.

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
- `HermesUIRuntimeFactory.cpp` — the UI runtime built via `jsr_create_config` /
  `jsr_create_runtime` / `jsr_config_set_task_runner` /
  `jsr_runtime_get_node_api_env`, wrapped with `NodeApiJsiRuntime` to yield a
  `jsi::Runtime&` (mirroring `Shared/HermesRuntimeHolder.cpp`).
- A **replacement for `WorkletHermesRuntime.{h,cpp}`** — it cannot be compiled
  as shipped (§2b). Reimplement its surface on the NAPI runtime, and re-solve
  worklet-function transfer without `facebook::hermes` bytecode APIs
  (`jsr_create_prepared_script` is the likely substitute; unproven).
- Compile the rest of `Common/cpp/worklets/**` into that project. Note that
  defining `JS_RUNTIME_HERMES` alone does **not** work (§2d).

### Phase 4 — Reanimated bring-up + upstream

Un-shim reanimated incrementally behind a feature flag; verify gesture
interplay and layout animations. **When (and only when) this is real, offer
the Windows platform port upstream to Software Mansion** — the portable core
already anticipates platforms; the glue is designed to slot into their layout.
