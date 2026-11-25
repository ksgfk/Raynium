#pragma once
#include <radray/types.h>
#include <functional>
#include <thread>

namespace raynium {
class BatchEnvelope;
class RenderChannel final {
public:
    static radray::unique_ptr<RenderChannel> Create();
    ~RenderChannel();
    bool HasCapacity() const;
    void Publish(BatchEnvelope&);
    static void PumpSentMessages() noexcept;
    bool Barrier(std::function<bool()> task);
    bool WaitUntil(const std::function<bool()>& ready,int64_t timeoutNs=-1);
    void NotifyProgress();
    void SetIdlePoll(std::function<bool()> poll);
private:
    struct State;
    RenderChannel(radray::unique_ptr<State>,std::thread) noexcept;
    radray::unique_ptr<State> _state;
    std::thread _worker;
};
}
