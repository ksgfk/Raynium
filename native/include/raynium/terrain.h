#pragma once
#include <raynium/runtime.h>
#include <raynium/bridge.h>
#include <map>
#include <vector>
#include <unordered_map>

namespace raynium {
class Terrain final {
public:
    static bool Create(RayniumRuntime&, const rn_terrain_desc&, radray::unique_ptr<Terrain>&,
                       rn_preview_info*, radray::string&) noexcept;
    bool Validate(const rn_terrain_frame&, radray::string&) const noexcept;
    bool Prepare(radray::render::CommandBuffer&, uint32_t slot, const rn_terrain_frame&, radray::string&) noexcept;
    void Render(radray::render::GraphicsCommandEncoder&, uint32_t slot, bool direct) noexcept;
    void Release(radray::render::CommandBuffer&, uint32_t slot) noexcept;
    void Submitted(uint64_t input) noexcept { _submitted = input + 1; }
    bool Finish(radray::string&) noexcept;
    radray::render::Fence* Ready() const noexcept { return _r.ready.get(); }
    radray::render::Fence* Available() const noexcept { return _r.available.get(); }
    ~Terrain() noexcept = default;
private:
    struct SharedImage {
        radray::unique_ptr<radray::render::Texture> texture;
        radray::render::ExternalTextureExport exported;
    };
    struct Frame {
        radray::unique_ptr<radray::render::Buffer> staging, constants, instances, indirect;
        radray::unique_ptr<radray::render::ShaderParameterSet> bindings;
        radray::unique_ptr<radray::render::QueryPool> timestamps;
        radray::unique_ptr<radray::render::Buffer> timestampReadback;
        uint64_t stagingSize{}, drawSize{};
        uint32_t anisotropy{1};
        bool measured{}, measuring{}, timingReadbackInitialized{};
        std::vector<radray::render::DrawIndexedIndirectArguments> args;
        std::vector<uint32_t> pages;
    };
    struct Resources {
        std::vector<SharedImage> images;
        radray::unique_ptr<radray::render::Texture> privateAtlas;
        radray::unique_ptr<radray::render::TextureView> atlasView, lightView;
        radray::unique_ptr<radray::render::Sampler> atlasSampler, lightSampler;
        std::map<uint32_t,radray::unique_ptr<radray::render::Sampler>> anisotropicSamplers;
        radray::unique_ptr<radray::render::BindingGroupLayout> group;
        radray::unique_ptr<radray::render::PipelineLayout> layout;
        radray::unique_ptr<radray::render::Shader> vs, ps;
        radray::unique_ptr<radray::render::GraphicsPipelineState> pipeline;
        radray::unique_ptr<radray::render::Buffer> indices, initialUpload;
        radray::unique_ptr<radray::render::Fence> ready, available;
        radray::render::ExternalFenceExport readyExport, availableExport;
        radray::unique_ptr<radray::render::CommandAllocator> bootstrapAllocator;
        radray::unique_ptr<radray::render::CommandBuffer> bootstrap;
        Frame frames[2];
    };
    struct Page {
        radray::unique_ptr<radray::render::Buffer> buffer;
        std::map<uint32_t, uint32_t> free;
        bool initialized{};
    };
    struct Mesh { uint32_t page, offset, count; uint32_t groups[7]{}; };
    struct Retired { Mesh mesh; uint64_t fence; };
    Terrain(RayniumRuntime& runtime, Resources&& resources, rn_terrain_desc desc) noexcept
        : _runtime(runtime), _r(std::move(resources)), _desc(desc) {}
    Mesh Allocate(uint32_t count);
    void Reclaim();
    RayniumRuntime& _runtime;
    Resources _r;
    rn_terrain_desc _desc;
    std::vector<Page> _pages;
    std::unordered_map<uint64_t, Mesh> _meshes;
    std::vector<Retired> _retired;
    uint64_t _submitted{1}, _epoch{};
    bool _privateInitialized{};
    std::vector<double> _gpuUploadMs, _gpuDrawMs;
};
}
