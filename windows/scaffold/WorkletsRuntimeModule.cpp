// ⚠️ SCAFFOLD — see WorkletsRuntimeModule.h. Not a worklet runtime.
#include "pch.h"
#include "WorkletsRuntimeModule.h"

#include <coroutine>
#include <cstdint>
#include <functional>

// This translation unit intentionally does NOT include any react-native-worklets
// header. The worklets Common/cpp is not on an app's include path and, more
// fundamentally, is bound to the concrete facebook::hermes::HermesRuntime which
// the prebuilt Microsoft.ReactNative does not expose (see the header and
// docs/DESIGN.md). Everything here is self-contained C++/WinRT against
// Microsoft.ReactNative's public ABI.

using namespace winrt;
using namespace winrt::Microsoft::ReactNative;

namespace
{
    // ------------------------------------------------------------------------
    // Awaiter that resumes the coroutine on an IReactDispatcher's thread. This
    // is exactly the mechanism a real Windows UI worklet scheduler would use to
    // marshal jobs onto the UI thread.
    //
    // NOTE the null-dispatcher guard: during instance teardown the dispatcher
    // can be null; Post() on a null handle is a guaranteed null-vtable call.
    // Resuming inline on the current thread is the safe degenerate behavior.
    // (This guard exists because the unguarded copy-paste of this awaiter was a
    // confirmed latent crash class across a production app's module set.)
    // ------------------------------------------------------------------------
    struct ResumeOnDispatcher
    {
        IReactDispatcher dispatcher;

        bool await_ready() const noexcept
        {
            return dispatcher && dispatcher.HasThreadAccess();
        }
        void await_suspend(std::coroutine_handle<> resume) const
        {
            if (!dispatcher)
            {
                // Instance tearing down / dispatcher not attached: resuming inline on
                // the current thread beats a guaranteed null-vtable call.
                resume();
                return;
            }
            dispatcher.Post([resume]() noexcept { resume(); });
        }
        void await_resume() const noexcept {}
    };

    // ------------------------------------------------------------------------
    // WindowsUIScheduler — the DESIGN SKETCH of the class a real worklets port
    // would author as windows/worklets/WindowsUIScheduler.{h,cpp}, implementing
    // worklets::UIScheduler (Common/cpp/worklets/Tools/UIScheduler.h). It is
    // the 1:1 analog of apple/worklets/apple/IOSUIScheduler.mm and
    // android/.../AndroidUIScheduler.cpp.
    //
    // It is presented STANDALONE (it does not derive from worklets::UIScheduler
    // and takes std::function jobs directly) precisely because the worklets
    // base-class header is not available against the prebuilt NuGet. When
    // worklets is compiled into a react-native-windows SOURCE build (Phase 3 in
    // docs/DESIGN.md), this becomes:
    //
    //     class WindowsUIScheduler final
    //         : public worklets::UIScheduler,
    //           public std::enable_shared_from_this<WindowsUIScheduler> {
    //      public:
    //       explicit WindowsUIScheduler(IReactDispatcher uiDispatcher)
    //           : uiDispatcher_(std::move(uiDispatcher)) {}
    //       void scheduleOnUI(std::function<void()> job) override {
    //         if (uiDispatcher_ && uiDispatcher_.HasThreadAccess()) {
    //           job();                       // already on UI thread -> run inline
    //           return;
    //         }
    //         UIScheduler::scheduleOnUI(std::move(job));  // enqueue in base queue
    //         if (!scheduledOnUI_.exchange(true)) {
    //           uiDispatcher_.Post([weak = weak_from_this()]() noexcept {
    //             if (auto self = weak.lock()) self->triggerUI();  // drains queue
    //           });
    //         }
    //       }
    //      private:
    //       IReactDispatcher uiDispatcher_;
    //     };
    //
    // The body below is the drainable-queue behaviour, kept live so the
    // scaffold actually exercises the DispatcherQueue path at runtime.
    // ------------------------------------------------------------------------
    class WindowsUIScheduler
    {
    public:
        explicit WindowsUIScheduler(IReactDispatcher uiDispatcher) noexcept
            : m_uiDispatcher(std::move(uiDispatcher))
        {
        }

        // Post a job to run on the UI thread. If we are already on the UI
        // thread, run it inline (matches IOSUIScheduler's [NSThread isMainThread]
        // fast path). Otherwise marshal via the DispatcherQueue. Null-guarded:
        // scheduling against a torn-down instance is a silent no-op, never a
        // null-vtable call.
        void ScheduleOnUI(std::function<void()> job) const
        {
            if (!m_uiDispatcher)
            {
                return;
            }
            if (m_uiDispatcher.HasThreadAccess())
            {
                job();
                return;
            }
            m_uiDispatcher.Post([job = std::move(job)]() noexcept {
                job();
            });
        }

        bool HasUiThreadAccess() const noexcept
        {
            return m_uiDispatcher && m_uiDispatcher.HasThreadAccess();
        }

    private:
        IReactDispatcher m_uiDispatcher{nullptr};
    };

    uint64_t CurrentThreadId() noexcept
    {
        return static_cast<uint64_t>(::GetCurrentThreadId());
    }

    // The verify coroutine. Free fire_and_forget so it owns everything by value
    // across the co_await suspension point (the context + promise are cheap,
    // copyable ABI handles).
    fire_and_forget VerifyUiDispatchAsync(
        ReactContext context,
        ReactPromise<JSValue> promise) noexcept
    {
        try
        {
            auto uiDispatcher = context.Handle().UIDispatcher();
            if (!uiDispatcher)
            {
                promise.Reject("RNWWorkletsRuntime: no UI dispatcher on context");
                co_return;
            }

            const uint64_t callerThreadId = CurrentThreadId();
            const bool hadThreadAccessBefore = uiDispatcher.HasThreadAccess();

            WindowsUIScheduler scheduler{uiDispatcher};

            // Hop onto the UI thread — the exact marshal a real UI worklet
            // scheduler performs before touching the UI worklet runtime.
            co_await ResumeOnDispatcher{uiDispatcher};

            const uint64_t uiThreadId = CurrentThreadId();

            JSValueObject result;
            result["hopped"] = true;
            result["hadThreadAccessBefore"] = hadThreadAccessBefore;
            result["hasUiThreadAccessNow"] = scheduler.HasUiThreadAccess();
            result["callerThreadId"] = static_cast<double>(callerThreadId);
            result["uiThreadId"] = static_cast<double>(uiThreadId);
            promise.Resolve(JSValue{std::move(result)});
        }
        catch (hresult_error const &e)
        {
            promise.Reject(to_string(e.message()).c_str());
        }
        catch (...)
        {
            promise.Reject("RNWWorkletsRuntime: unexpected error during UI dispatch verify");
        }
    }
}

namespace winrt::ReactNativeWindowsWorklets::implementation
{
    JSValue WorkletsRuntimeModule::getCapabilities() noexcept
    {
        bool uiDispatcherAvailable = false;
        bool jsDispatcherAvailable = false;
        try
        {
            if (m_context)
            {
                uiDispatcherAvailable = static_cast<bool>(m_context.Handle().UIDispatcher());
                jsDispatcherAvailable = static_cast<bool>(m_context.Handle().JSDispatcher());
            }
        }
        catch (...)
        {
            // leave as false; getCapabilities must never throw into JS
        }

        JSValueObject caps;
        caps["scaffold"] = true;
        // A second Hermes VM on the UI thread (the real worklets UI runtime) is
        // not constructible from an app against the prebuilt NuGet.
        caps["nativeUiRuntime"] = false;
        // Worklets executed on the RN JS runtime ARE feasible over the ABI
        // jsi::Runtime (see docs/DESIGN.md, Phase 2).
        caps["jsThreadWorkletsFeasible"] = true;
        // Only an ABI/NAPI jsi::Runtime wrapper is exposed, not a concrete
        // facebook::hermes::HermesRuntime.
        caps["hermesConcreteRuntime"] = false;
        caps["uiDispatcherAvailable"] = uiDispatcherAvailable;
        caps["jsDispatcherAvailable"] = jsDispatcherAvailable;
        caps["recommendedInstallMode"] = "js-thread-shim";
        caps["workletsVersion"] = "0.7.4";
        caps["notes"] = "scaffold only; see docs/DESIGN.md in this repo";
        return JSValue{std::move(caps)};
    }

    void WorkletsRuntimeModule::verifyUiDispatch(ReactPromise<JSValue> promise) noexcept
    {
        VerifyUiDispatchAsync(m_context, promise);
    }
}
