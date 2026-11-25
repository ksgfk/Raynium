#pragma once
#include <raynium/bridge.h>
#include <radray/runtime/rdg.h>
#include <radray/types.h>
#include <raynium/resource_handles.h>

namespace raynium {
class BufferPool final {
public:
    bool Prewarm(radray::gpu::Device&);
    static bool Valid(const rn_buffer_desc&,uint64_t maxSize);
    static radray::unique_ptr<rn_buffer> Allocate(radray::gpu::Device&,uint32_t usage,uint64_t size);
    radray::unique_ptr<rn_buffer> Take(uint32_t usage,uint64_t size,uint64_t completed);
    void Retire(radray::unique_ptr<rn_buffer> buffer,uint64_t sequence);
    void Advance(uint64_t submitted);
private:
    struct Retired { radray::unique_ptr<rn_buffer> entry; uint64_t sequence; };
    radray::unordered_map<uint64_t,radray::vector<Retired>> _pool;
    uint64_t _poolBytes{},_nextExpiry{UINT64_MAX};
};
}
