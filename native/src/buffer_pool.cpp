#include <raynium/buffer_pool.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace raynium {
bool BufferPool::Valid(const rn_buffer_desc& desc, uint64_t maxSize) {
    const uint32_t uses = desc.usage & ~RN_BUFFER_CLIENT_STORAGE;
    return desc.struct_size == sizeof(desc) && uses && !(uses & ~2047u) && desc.size &&
           (!(uses & RN_BUFFER_FENCE_ROTATED) || ((uses & RN_BUFFER_MAP_WRITE) && !(uses & RN_BUFFER_COPY_DST))) &&
           desc.size <= maxSize && desc.size <= PTRDIFF_MAX &&
           (!(uses & RN_BUFFER_MAP_READ) || !(uses & ~(RN_BUFFER_MAP_READ | RN_BUFFER_COPY_DST)));
}

radray::unique_ptr<rn_buffer> BufferPool::Allocate(radray::gpu::Device& gpu, uint32_t uses, uint64_t size) {
    auto entry = radray::make_unique<rn_buffer>();
    entry->Size = size;
    entry->Usage = uses;
    radray::gpu::BufferUses gpuUses{};
    if (uses & RN_BUFFER_COPY_SRC) gpuUses |= radray::gpu::BufferUse::CopySource;
    if (uses & RN_BUFFER_COPY_DST) gpuUses |= radray::gpu::BufferUse::CopyDestination;
    if (uses & RN_BUFFER_VERTEX) gpuUses |= radray::gpu::BufferUse::Vertex;
    if (uses & RN_BUFFER_INDEX) gpuUses |= radray::gpu::BufferUse::Index;
    if (uses & RN_BUFFER_UNIFORM) gpuUses |= radray::gpu::BufferUse::CBuffer;
    if (uses & RN_BUFFER_TEXEL) gpuUses |= radray::gpu::BufferUse::Resource;
    if (uses & RN_BUFFER_INDIRECT) gpuUses |= radray::gpu::BufferUse::Indirect | radray::gpu::BufferUse::CopySource;
    if (uses & RN_BUFFER_MAP_READ) return entry;
    if (uses & RN_BUFFER_FENCE_ROTATED) {
        // A map-only allocation still needs a valid upload-buffer binding.
        if (!gpuUses) gpuUses = radray::gpu::BufferUse::CopySource;
        const auto memory = gpuUses == radray::gpu::BufferUse::CopySource
                                ? radray::GpuBufferMemory::HostUpload
                                : radray::GpuBufferMemory::DeviceUploadWithFallback;
        entry->Gpu = radray::GpuBuffer::Create(gpu, {size, gpuUses, memory}).Release();
        if (!entry->Gpu) return {};
        entry->Mapped = entry->Gpu->GetMappedData().Get();
        if (!entry->Mapped) return {};
    } else {
        entry->Gpu = radray::GpuBuffer::Create(gpu, {size, gpuUses | radray::gpu::BufferUse::CopyDestination}).Release();
        if (!entry->Gpu) return {};
    }
    return entry;
}

namespace {
constexpr uint64_t kMaxPooledBuffer = 16u << 20;
constexpr uint64_t kPoolBudget = 64u << 20;
constexpr uint64_t kIdleSubmissions = 120;

uint64_t PoolKey(uint32_t usage, uint64_t size) {
    return (uint64_t(usage & ~(RN_BUFFER_COPY_DST | RN_BUFFER_MAP_WRITE)) << 32) | size;
}
}  // namespace

bool BufferPool::Prewarm(radray::gpu::Device& gpu) {
    constexpr uint64_t kPrewarmSize = 512u << 10;
    for (uint32_t binding : {RN_BUFFER_UNIFORM, RN_BUFFER_COPY_SRC}) {
        const uint32_t usage = RN_BUFFER_FENCE_ROTATED | RN_BUFFER_MAP_WRITE | binding;
        auto entry = Allocate(gpu, usage, kPrewarmSize);
        if (!entry) return false;
        _pool[PoolKey(usage, kPrewarmSize)].push_back({std::move(entry), 0});
        _poolBytes += kPrewarmSize;
    }
    _nextExpiry = kIdleSubmissions;
    return true;
}

radray::unique_ptr<rn_buffer> BufferPool::Take(uint32_t uses, uint64_t size, uint64_t completed) {
    if (size > kMaxPooledBuffer || (uses & RN_BUFFER_MAP_READ)) return {};
    auto found = _pool.find(PoolKey(uses, size));
    if (found == _pool.end()) return {};
    auto& bucket = found->second;
    for (size_t i = bucket.size(); i > 0; --i) {
        auto& retired = bucket[i - 1];
        if (retired.Sequence > completed || retired.Entry->Gpu->UseCount() != 1) continue;
        auto entry = std::move(retired.Entry);
        if (i != bucket.size()) retired = std::move(bucket.back());
        bucket.pop_back();
        if (bucket.empty()) _pool.erase(found);
        entry->Usage = uses;
        _poolBytes -= size;
        return entry;
    }
    return {};
}

void BufferPool::Retire(radray::unique_ptr<rn_buffer> entry, uint64_t sequence) {
    if (IsReadbackBuffer(*entry) || entry->Size > kMaxPooledBuffer || entry->Size > kPoolBudget - _poolBytes) return;
    radray::vector<radray::byte>().swap(entry->ShadowHost);
    entry->Mapped = entry->Gpu->GetMappedData().Get();
    const auto key = PoolKey(entry->Usage, entry->Size);
    _poolBytes += entry->Size;
    _pool[key].push_back({std::move(entry), sequence});
    _nextExpiry = std::min(_nextExpiry, sequence + kIdleSubmissions);
}

void BufferPool::Advance(uint64_t submitted) {
    if (submitted < _nextExpiry) return;
    _nextExpiry = UINT64_MAX;
    for (auto it = _pool.begin(); it != _pool.end();) {
        std::erase_if(it->second, [&](Retired& item) {
            const auto expiry = item.Sequence + kIdleSubmissions;
            if (expiry <= submitted) {
                _poolBytes -= item.Entry->Size;
                return true;
            }
            _nextExpiry = std::min(_nextExpiry, expiry);
            return false;
        });
        if (it->second.empty())
            it = _pool.erase(it);
        else
            ++it;
    }
}
}  // namespace raynium
