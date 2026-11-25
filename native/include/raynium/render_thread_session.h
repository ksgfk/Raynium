#pragma once

#include <radray/types.h>
#include <functional>
#include <thread>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace raynium {

class MoveOnlyTask final {
public:
    template<class F> requires (!std::is_same_v<std::decay_t<F>,MoveOnlyTask>)
    MoveOnlyTask(F&& task) {
        using Fn=std::decay_t<F>;
        if constexpr(sizeof(Fn)<=sizeof(_storage) && alignof(Fn)<=alignof(std::max_align_t) &&
                     std::is_nothrow_move_constructible_v<Fn>) {
            Store<Fn>(std::forward<F>(task));
        } else {
            auto indirect=[owner=radray::make_unique<Fn>(std::forward<F>(task))]() mutable { (*owner)(); };
            Store<decltype(indirect)>(std::move(indirect));
        }
    }
    MoveOnlyTask(MoveOnlyTask&& other) noexcept : _ops(std::exchange(other._ops,nullptr)) {
        if(_ops) _ops->move(other._storage,_storage);
    }
    ~MoveOnlyTask() { if(_ops) _ops->destroy(_storage); }
    void operator()() { _ops->invoke(_storage); }

private:
    struct Ops {
        void (*invoke)(void*);
        void (*move)(void*,void*) noexcept;
        void (*destroy)(void*) noexcept;
    };
    template<class Fn,class F> void Store(F&& fn) {
        static const Ops ops{
            [](void* p) { (*static_cast<Fn*>(p))(); },
            [](void* from,void* to) noexcept {
                auto* source=static_cast<Fn*>(from);
                ::new(to) Fn(std::move(*source));
                std::destroy_at(source);
            },
            [](void* p) noexcept { std::destroy_at(static_cast<Fn*>(p)); }
        };
        ::new(static_cast<void*>(_storage)) Fn(std::forward<F>(fn));
        _ops=&ops;
    }
    alignas(std::max_align_t) std::byte _storage[64];
    const Ops* _ops;
};

class RenderThreadSession final {
public:
    static radray::unique_ptr<RenderThreadSession> Create();
    ~RenderThreadSession();

    void Post(MoveOnlyTask task);
    static void PumpSentMessages() noexcept;
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
