#pragma once

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <raynium/render_program.h>
#include <raynium/resource_handles.h>

namespace raynium {
class ImmediatePipeline final {
public:
    static radray::unique_ptr<ImmediatePipeline> Create(
        radray::gpu::Device& device,
        RenderProgramLibrary& resources,
        const rn_pipeline_desc& desc,
        radray::Nullable<radray::gpu::PipelineCache*> cache);

    radray::gpu::GraphicsPipelineState* Resolve(radray::gpu::Device& device, radray::gpu::TextureFormat depthFormat);

    RasterPipelineState State;
    RasterProgram* Program{};

private:
    radray::Nullable<radray::gpu::PipelineCache*> _cache{};
    radray::unordered_map<radray::gpu::TextureFormat, radray::unique_ptr<radray::gpu::GraphicsPipelineState>> _variants;
};
}  // namespace raynium

struct rn_pipeline : raynium::ResourceHandle<rn_pipeline> {
    radray::unique_ptr<raynium::ImmediatePipeline> Impl;
};
