#pragma once
#include <raynium/render_program.h>

namespace raynium {
struct ImmediatePipeline {
    static radray::shared_ptr<ImmediatePipeline> Create(radray::gpu::Device&,RenderProgramLibrary&,const rn_pipeline_desc&,
        radray::Nullable<radray::gpu::PipelineCache*> cache);
    RasterPipelineState state;
    RasterProgram* program{};
    radray::Nullable<radray::gpu::PipelineCache*> cache{};
    radray::unordered_map<radray::gpu::TextureFormat,radray::unique_ptr<radray::gpu::GraphicsPipelineState>> variants;
    radray::gpu::GraphicsPipelineState* Resolve(radray::gpu::Device&,radray::gpu::TextureFormat depth);
};
class ImmediatePipelineRegistry final {
public:
    void Insert(uint32_t id,radray::shared_ptr<ImmediatePipeline> item) { _items.emplace(id,std::move(item)); }
    bool Destroy(uint32_t id) { return _items.erase(id)!=0; }
    radray::shared_ptr<ImmediatePipeline> Find(uint32_t id) const;
private:
    radray::unordered_map<uint32_t,radray::shared_ptr<ImmediatePipeline>> _items;
};
}
