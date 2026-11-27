#pragma once

#include <cstdint>
#include <functional>
#include <thread>

#include <radray/types.h>

namespace raynium {
class BatchEnvelope;

class RenderChannel final {
public:
    static radray::unique_ptr<RenderChannel> Create();
    ~RenderChannel();
    bool HasCapacity() const;
    void Publish(BatchEnvelope& batch);
    static void PumpSentMessages() noexcept;
    bool Barrier(std::function<bool()> task);
    bool WaitUntil(const std::function<bool()>& ready, int64_t timeoutNs = -1);
    void NotifyProgress();
    void SetIdlePoll(std::function<bool()> poll);

private:
    class State;

    RenderChannel(radray::unique_ptr<State> state, std::thread worker) noexcept;

    radray::unique_ptr<State> _state;
    std::thread _worker;
};
}  // namespace raynium
