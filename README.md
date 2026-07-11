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
| `docs/DESIGN.md` — a source-verified teardown of worklets 0.7.4 (portable core vs platform glue vs the Hermes-bound parts), why an app-side port is impossible against the prebuilt RNW NuGet, the two viable routes, and a 4-phase roadmap | ✅ Done (research is real; read it before arguing with us) |
| `windows/scaffold/WorkletsRuntimeModule.{h,cpp}` — a capability-probe TurboModule + a null-dispatcher-guarded `WindowsUIScheduler` sketch (the `ScheduleOnUI` pattern over RNW's `IReactDispatcher`, the exact analog of `IOSUIScheduler`/`AndroidUIScheduler`) | 🚧 Scaffold; compiling-quality, **excluded from any build**, never shipped in a binary |
| A worklet runtime | ❌ **Does not exist** |
| A JS package | ❌ Does not exist (deliberately no `package.json` — there is nothing to install) |
| Animations of any kind | ❌ No |

The scaffold is an extraction from a production monorepo's staging tree
(Facilitron FIT for Windows, RNW 0.83.2 new-arch); its sibling modules ship in
production, this one does not. Standalone build validation pending.

## Why this is genuinely hard (the one-paragraph version)

The worklets UI runtime is a **second Hermes VM** constructed from a concrete
`facebook::hermes::HermesRuntime`, sharing worklet functions with the RN
runtime as Hermes bytecode. RNW's prebuilt `Microsoft.ReactNative` NuGet
exposes the JS runtime to native code only as an ABI/NAPI `jsi::Runtime`
wrapper — no `hermes/hermes.h`, no `MessageQueueThread`, no linkable Hermes.
So the real port lives **inside an RNW source build** (or upstream in RNW
itself), not in an app or an ordinary community module. The full analysis with
file-level receipts is in [`docs/DESIGN.md`](docs/DESIGN.md).

## The roadmap (honest sizes)

1. **Phase 0 — scaffold + probe** (this repo): prove the UI-dispatch seam
   (`ReactContext.UIDispatcher()` → `DispatcherQueue`) a `WindowsUIScheduler`
   is built on. Done as code; unbuilt standalone.
2. **Phase 1 — pick the route:** RNW source build (real, off-thread, multi-week)
   vs JS-thread-only executor (medium; animations run but on the JS thread).
3. **Phase 2 (route B)** — full `__workletsModuleProxy` HostObject over the ABI
   `jsi::Runtime`; reanimated babel plugin re-enabled on windows.
4. **Phase 3 (route A)** — the real thing in an RNW source build:
   `WorkletsModule` + `WindowsUIScheduler` + `WindowsMessageQueueThread` +
   vsync RAF queue + a Hermes UI-runtime factory; compile
   `Common/cpp/worklets/**` with `JS_RUNTIME_HERMES`.
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
