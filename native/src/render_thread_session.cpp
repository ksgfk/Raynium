#include <raynium/render_thread_session.h>
#include <raynium/exception.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace raynium {

struct RenderThreadSession::State {
    explicit State(HANDLE signal) noexcept : progress(signal) {}
    ~State() { CloseHandle(progress); }

    void Run() noexcept try {
        SetThreadDescription(GetCurrentThread(), L"Raynium Render");
        for (;;) {
            const bool pending = poll && poll();
            std::optional<MoveOnlyTask> task;
            {
                std::unique_lock lock(mutex);
                const auto ready = [&] { return stopping || !queue.empty(); };
                if (pending) {
                    if (!changed.wait_for(lock, std::chrono::milliseconds(1), ready)) continue;
                } else changed.wait(lock, ready);
                if (queue.empty()) return;
                task.emplace(std::move(queue.front()));
                queue.pop_front();
            }
            (*task)();
        }
    } catch (...) {
        AbortUnexpectedException("Raynium Render");
    }

    bool Wait(const std::function<bool()>& ready, int64_t timeoutNs) {
        const auto start = std::chrono::steady_clock::now();
        while (!ready()) {
            DWORD timeout = INFINITE;
            if (timeoutNs >= 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeoutNs) return false;
                timeout = static_cast<DWORD>(std::min<int64_t>((timeoutNs - elapsed - 1) / 1000000 + 1, INFINITE - 1));
            }
            const auto wake = MsgWaitForMultipleObjectsEx(1, &progress, timeout, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (wake == WAIT_OBJECT_0 + 1) {
                // Dispatch synchronous HWND messages while leaving posted game events to SDL.
                RenderThreadSession::PumpSentMessages();
            } else if (wake != WAIT_OBJECT_0 && wake != WAIT_TIMEOUT) {
                RADRAY_ABORT("Render thread wait failed: {}", GetLastError());
            }
        }
        return true;
    }

    HANDLE progress;
    std::mutex mutex;
    std::condition_variable changed;
    radray::deque<MoveOnlyTask> queue;
    std::function<bool()> poll;
    bool stopping{};
};

radray::unique_ptr<RenderThreadSession> RenderThreadSession::Create() {
    auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        RADRAY_ERR_LOG("Cannot create render progress event: {}", GetLastError());
        return {};
    }
    auto state = radray::make_unique<State>(event);
    try {
        std::thread worker(&State::Run, state.get());
        return radray::unique_ptr<RenderThreadSession>(new RenderThreadSession(std::move(state), std::move(worker)));
    } catch (const std::system_error& failure) {
        RADRAY_ERR_LOG("Cannot start render thread: {}", failure.what());
        return {};
    }
}

RenderThreadSession::RenderThreadSession(radray::unique_ptr<State> state, std::thread worker) noexcept
    : _state(std::move(state)), _worker(std::move(worker)) {}

RenderThreadSession::~RenderThreadSession() {
    {
        std::lock_guard lock(_state->mutex);
        _state->stopping = true;
    }
    _state->changed.notify_one();
    _worker.join();
}

void RenderThreadSession::PumpSentMessages() noexcept {
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

void RenderThreadSession::Post(MoveOnlyTask task) {
    {
        std::lock_guard lock(_state->mutex);
        _state->queue.push_back(std::move(task));
    }
    _state->changed.notify_one();
}

bool RenderThreadSession::Barrier(std::function<bool()> task) {
    std::atomic<bool> done{};
    bool result{};
    Post([&, state = _state.get()] {
        result = task();
        done.store(true, std::memory_order_release);
        SetEvent(state->progress);
    });
    WaitUntil([&] { return done.load(std::memory_order_acquire); });
    return result;
}

bool RenderThreadSession::WaitUntil(const std::function<bool()>& ready, int64_t timeoutNs) {
    return _state->Wait(ready, timeoutNs);
}

void RenderThreadSession::NotifyProgress() { SetEvent(_state->progress); }

void RenderThreadSession::SetIdlePoll(std::function<bool()> poll) { _state->poll = std::move(poll); }

}
