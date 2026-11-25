#pragma once

#include <radray/types.h>
#include <functional>
#include <thread>

namespace raynium {

class RenderThreadSession final {
public:
    static radray::unique_ptr<RenderThreadSession> Create();
    ~RenderThreadSession();

    void Post(std::function<void()> task);
    bool Barrier(std::function<bool()> task);
    bool WaitUntil(const std::function<bool()>& ready, int64_t timeoutNs = -1);
    void NotifyProgress();
    // Install on the worker. Return true while a GPU completion still needs polling.
    void SetIdlePoll(std::function<bool()> poll);

private:
    struct State;
    RenderThreadSession(radray::unique_ptr<State> state, std::thread worker) noexcept;
    radray::unique_ptr<State> _state;
    std::thread _worker;
};

}
