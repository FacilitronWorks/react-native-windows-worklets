#pragma once
#include "NativeModules.h"

using namespace winrt::Microsoft::ReactNative;

// ============================================================================
// WorkletsRuntimeModule — a SCAFFOLD + capability probe for bringing
// react-native-worklets 0.7.x (and therefore react-native-reanimated 4.x
// animations) to react-native-windows (New Architecture, Hermes,
// NuGet-prebuilt Microsoft.ReactNative).
//
// ⚠️ THIS IS NOT A WORKING WORKLET RUNTIME. It is the foundational scaffold:
// it (a) proves the one RNW primitive a real port hooks — the UI-thread
// dispatcher (DispatcherQueue) reached via ReactContext.UIDispatcher(), the
// exact analog of iOS's dispatch_get_main_queue()-based IOSUIScheduler and
// Android's AndroidUIScheduler — and (b) exposes a diagnostic surface
// describing why a full runtime cannot be assembled app-side against the
// prebuilt NuGet. The complete rationale + phased plan lives in
// docs/DESIGN.md.
//
// WHY A REAL RUNTIME IS HARD HERE (short version; full version in the doc):
//   react-native-worklets' Common/cpp is written against the CONCRETE
//   facebook::hermes::HermesRuntime (see WorkletHermesRuntime.h:
//   `#include <hermes/hermes.h>`, `std::unique_ptr<HermesRuntime>`) plus
//   ReactCommon internals (MessageQueueThread, CallInvoker, JSBigStringBuffer).
//   RNW 0.83's prebuilt Microsoft.ReactNative surfaces the JS runtime to
//   native code only as an ABI/NAPI jsi::Runtime wrapper (JsiAbiApi.h /
//   NodeApiJsi), NOT as a linkable facebook::hermes::HermesRuntime, and it
//   does not ship hermes.h / a MessageQueueThread header on the app include
//   path. So the UI worklet runtime (a SECOND Hermes VM on the UI thread)
//   cannot be constructed from the app. The two viable routes (RNW source
//   build vs a JS-thread-only worklet executor installed over the ABI
//   jsi::Runtime) are laid out in docs/DESIGN.md.
//
// This module is auto-registered by AddAttributedModules(builder, true) in a
// consuming app's package provider. It is exposed to JS as
// NativeModules.RNWWorkletsRuntime.
//
// IMPORTANT: it is deliberately NOT named "WorkletsModule". The real
// react-native-worklets JS package looks up
// TurboModuleRegistry.get('WorkletsModule') and then REQUIRES
// global.__workletsModuleProxy to be a fully populated JSI HostObject; a
// native module that answered to that name but only half-installed would make
// NativeWorklets throw at construction. You cannot "partially" install
// worklets — either the entire proxy HostObject exists, or nothing may answer
// to the name. Keeping a distinct name lets this scaffold coexist with a
// pure-JS worklets shim, which is what a consuming app should resolve today.
//
// PROVENANCE: extracted from a production monorepo's staging tree (Facilitron
// FIT for Windows, RNW 0.83.2 new-arch). This scaffold has NOT been part of a
// shipped binary; it is authored to compiling quality and its ScheduleOnUI
// null-dispatcher guard reflects a real crash class we hit in sibling modules
// (posting to a null dispatcher during instance teardown). Standalone build
// validation pending.
// ============================================================================

namespace winrt::ReactNativeWindowsWorklets::implementation
{
    REACT_MODULE(WorkletsRuntimeModule, L"RNWWorkletsRuntime");
    struct WorkletsRuntimeModule
    {
        REACT_INIT(Initialize);
        void Initialize(ReactContext const &reactContext) noexcept
        {
            m_context = reactContext;
        }

        // Synchronous capability probe. Returns a plain object describing what
        // the current RNW host can and cannot provide for a worklets port, so
        // bring-up code / the design doc can assert against reality:
        //   {
        //     scaffold: true,
        //     nativeUiRuntime: false,       // no 2nd Hermes VM available here
        //     jsThreadWorkletsFeasible: true,
        //     hermesConcreteRuntime: false, // only ABI/NAPI jsi::Runtime exposed
        //     uiDispatcherAvailable: <bool>,
        //     jsDispatcherAvailable: <bool>,
        //     recommendedInstallMode: "js-thread-shim",
        //     workletsVersion: "0.7.4",
        //     notes: "<one-line pointer to the design doc>"
        //   }
        // Kept SYNC so bring-up probes read it without an async hop.
        REACT_SYNC_METHOD(getCapabilities);
        JSValue getCapabilities() noexcept;

        // Proof-of-primitive: hop a job onto the RNW UI-thread dispatcher
        // (DispatcherQueue) and confirm we ran there. This is the single native
        // capability a real WindowsUIScheduler (see .cpp) would be built on —
        // the analog of IOSUIScheduler::scheduleOnUI doing
        // dispatch_async(dispatch_get_main_queue(), ...). Resolves:
        //   { hopped: true, hadThreadAccessBefore: <bool>,
        //     uiThreadId: <number>, callerThreadId: <number> }
        // Rejects only if no UI dispatcher exists on the context.
        REACT_METHOD(verifyUiDispatch);
        void verifyUiDispatch(ReactPromise<JSValue> promise) noexcept;

    private:
        ReactContext m_context{nullptr};
    };
}
