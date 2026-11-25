#include <raynium/render_channel.h>
#include <raynium/renderer_frontend.h>
#include <raynium/exception.h>
#include <radray/scope_guard.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace raynium {

struct RenderChannel::State {
    State(HANDLE progress,HANDLE wake) noexcept : progress(progress),wake(wake) {}
    ~State() { CloseHandle(progress); CloseHandle(wake); }

    void SignalProgress() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (progressWaiters.load(std::memory_order_relaxed) != 0) {
            SetEvent(progress);
        }
    }

    void Run() noexcept try {
        SetThreadDescription(GetCurrentThread(), L"Raynium Render");
        for (;;) {
            // A barrier may arrive just after a batch publication. Observe the barrier first,
            // then drain the mailbox before servicing that barrier.
            auto* barrier=barrierSlot.load(std::memory_order_acquire);
            if(auto* batch=queuedBatch.exchange(nullptr,std::memory_order_acq_rel)) {
                SignalProgress();
                batch->frontend->Consume(*batch);
                continue;
            }
            if(barrier) {
                barrier->result=(*barrier->task)();
                barrierSlot.store(nullptr,std::memory_order_release);
                SignalProgress();
                continue;
            }
            if(stopping.load(std::memory_order_acquire)) return;
            const bool pending=poll && poll();
            WaitForSingleObject(wake,pending?2:INFINITE);
        }
    } catch (...) {
        AbortUnexpectedException("Raynium Render");
    }

    bool Wait(const std::function<bool()>& ready, int64_t timeoutNs) {
        if (ready()) return true;
        const auto start = std::chrono::steady_clock::now();
        progressWaiters.fetch_add(1, std::memory_order_seq_cst);
        auto releaseWaiter = radray::MakeScopeGuard([&]() noexcept {
            progressWaiters.fetch_sub(1, std::memory_order_release);
        });
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (!ready()) {
            DWORD timeout = INFINITE;
            if (timeoutNs >= 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeoutNs) return false;
                timeout = static_cast<DWORD>(std::min<int64_t>((timeoutNs - elapsed - 1) / 1000000 + 1, INFINITE - 1));
            }
            const auto result = MsgWaitForMultipleObjectsEx(1, &progress, timeout, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0 + 1) {
                // Dispatch synchronous HWND messages while leaving posted game events to SDL.
                RenderChannel::PumpSentMessages();
            } else if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
                RADRAY_ABORT("Render thread wait failed: {}", GetLastError());
            }
        }
        return true;
    }

    struct BarrierSlot { std::function<bool()>* task; bool result{}; };
    HANDLE progress,wake;
    alignas(64) std::atomic<BatchEnvelope*> queuedBatch{};
    std::atomic<BarrierSlot*> barrierSlot{};
    std::function<bool()> poll;
    std::atomic<bool> stopping{};
    alignas(64) std::atomic<uint32_t> progressWaiters{};
};

radray::unique_ptr<RenderChannel> RenderChannel::Create() {
    auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        RADRAY_ERR_LOG("Cannot create render progress event: {}", GetLastError());
        return {};
    }
    auto wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!wake) { CloseHandle(event); return {}; }
    auto state = radray::make_unique<State>(event,wake);
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
    _state->stopping.store(true,std::memory_order_release);
    SetEvent(_state->wake);
    _worker.join();
}

void RenderChannel::PumpSentMessages() noexcept {
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

bool RenderChannel::HasCapacity() const { return !_state->queuedBatch.load(std::memory_order_acquire); }
void RenderChannel::Publish(BatchEnvelope& batch) {
    if(_state->queuedBatch.exchange(&batch,std::memory_order_release)) RADRAY_ABORT("Render mailbox occupied");
    SetEvent(_state->wake);
}
bool RenderChannel::Barrier(std::function<bool()> task) {
    State::BarrierSlot slot{&task};
    _state->barrierSlot.store(&slot,std::memory_order_release);
    SetEvent(_state->wake);
    WaitUntil([&] { return !_state->barrierSlot.load(std::memory_order_acquire); });
    return slot.result;
}

bool RenderChannel::WaitUntil(const std::function<bool()>& ready, int64_t timeoutNs) {
    return _state->Wait(ready, timeoutNs);
}

void RenderChannel::NotifyProgress() { _state->SignalProgress(); }

void RenderChannel::SetIdlePoll(std::function<bool()> poll) { _state->poll = std::move(poll); }

}
