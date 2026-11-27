#include <raynium/render_channel.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <raynium/exception.h>
#include <raynium/renderer_frontend.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace raynium {
class RenderChannel::State final {
public:
    struct BarrierSlot {
        std::function<bool()>* Task{};
        bool Result{};
    };

    State(HANDLE progress, HANDLE wake) noexcept : Progress(progress), Wake(wake) {}
    ~State() {
        CloseHandle(Progress);
        CloseHandle(Wake);
    }

    void SignalProgress() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (ProgressWaiters.load(std::memory_order_relaxed) != 0) {
            SetEvent(Progress);
        }
    }

    void Run() noexcept try {
        SetThreadDescription(GetCurrentThread(), L"Raynium Render");
        for (;;) {
            // A barrier may arrive just after a batch publication. Observe the barrier first,
            // then drain the mailbox before servicing that barrier.
            auto* barrier = BarrierSlotPtr.load(std::memory_order_acquire);
            if (auto* batch = QueuedBatch.exchange(nullptr, std::memory_order_acq_rel)) {
                SignalProgress();
                batch->Frontend->Consume(*batch);
                continue;
            }
            if (barrier) {
                barrier->Result = (*barrier->Task)();
                BarrierSlotPtr.store(nullptr, std::memory_order_release);
                SignalProgress();
                continue;
            }
            if (Stopping.load(std::memory_order_acquire)) return;
            const bool pending = Poll && Poll();
            WaitForSingleObject(Wake, pending ? 2 : INFINITE);
        }
    } catch (...) {
        AbortUnexpectedException("Raynium Render");
    }

    bool Wait(const std::function<bool()>& ready, int64_t timeoutNs) {
        if (ready()) return true;
        const auto start = std::chrono::steady_clock::now();
        ProgressWaiters.fetch_add(1, std::memory_order_seq_cst);
        auto releaseWaiter = radray::MakeScopeGuard([&]() noexcept {
            ProgressWaiters.fetch_sub(1, std::memory_order_release);
        });
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (!ready()) {
            DWORD timeout = INFINITE;
            if (timeoutNs >= 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeoutNs) return false;
                timeout = static_cast<DWORD>(std::min<int64_t>((timeoutNs - elapsed - 1) / 1000000 + 1, INFINITE - 1));
            }
            const auto result = MsgWaitForMultipleObjectsEx(1, &Progress, timeout, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0 + 1) {
                // Dispatch synchronous HWND messages while leaving posted game events to SDL.
                RenderChannel::PumpSentMessages();
            } else if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
                RADRAY_ABORT("Render thread wait failed: {}", GetLastError());
            }
        }
        return true;
    }

    HANDLE Progress{};
    HANDLE Wake{};
    alignas(64) std::atomic<BatchEnvelope*> QueuedBatch{};
    std::atomic<BarrierSlot*> BarrierSlotPtr{};
    std::function<bool()> Poll;
    std::atomic<bool> Stopping{};
    alignas(64) std::atomic<uint32_t> ProgressWaiters{};
};

radray::unique_ptr<RenderChannel> RenderChannel::Create() {
    auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        RADRAY_ERR_LOG("Cannot create render progress event: {}", GetLastError());
        return {};
    }
    auto wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) {
        CloseHandle(event);
        return {};
    }
    auto state = radray::make_unique<State>(event, wake);
    try {
        std::thread worker(&State::Run, state.get());
        return radray::unique_ptr<RenderChannel>(new RenderChannel(std::move(state), std::move(worker)));
    } catch (const std::system_error& failure) {
        RADRAY_ERR_LOG("Cannot start render thread: {}", failure.what());
        return {};
    }
}

RenderChannel::RenderChannel(radray::unique_ptr<State> state, std::thread worker) noexcept
    : _state(std::move(state)), _worker(std::move(worker)) {}

RenderChannel::~RenderChannel() {
    _state->Stopping.store(true, std::memory_order_release);
    SetEvent(_state->Wake);
    _worker.join();
}

void RenderChannel::PumpSentMessages() noexcept {
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

bool RenderChannel::HasCapacity() const {
    return !_state->QueuedBatch.load(std::memory_order_acquire);
}

void RenderChannel::Publish(BatchEnvelope& batch) {
    if (_state->QueuedBatch.exchange(&batch, std::memory_order_release)) RADRAY_ABORT("Render mailbox occupied");
    SetEvent(_state->Wake);
}

bool RenderChannel::Barrier(std::function<bool()> task) {
    State::BarrierSlot slot{&task, false};
    _state->BarrierSlotPtr.store(&slot, std::memory_order_release);
    SetEvent(_state->Wake);
    WaitUntil([&] { return !_state->BarrierSlotPtr.load(std::memory_order_acquire); });
    return slot.Result;
}

bool RenderChannel::WaitUntil(const std::function<bool()>& ready, int64_t timeoutNs) {
    return _state->Wait(ready, timeoutNs);
}

void RenderChannel::NotifyProgress() {
    _state->SignalProgress();
}

void RenderChannel::SetIdlePoll(std::function<bool()> poll) {
    _state->Poll = std::move(poll);
}
}  // namespace raynium
