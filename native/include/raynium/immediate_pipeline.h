#pragma once
#include <raynium/render_program.h>
#include <raynium/resource_handles.h>

namespace raynium {
struct ImmediatePipeline {
    static radray::unique_ptr<ImmediatePipeline> Create(radray::gpu::Device&,RenderProgramLibrary&,const rn_pipeline_desc&,
        radray::Nullable<radray::gpu::PipelineCache*> cache);
    RasterPipelineState state;
    RasterProgram* program{};
    radray::Nullable<radray::gpu::PipelineCache*> cache{};
    radray::unordered_map<radray::gpu::TextureFormat,radray::unique_ptr<radray::gpu::GraphicsPipelineState>> variants;
    radray::gpu::GraphicsPipelineState* Resolve(radray::gpu::Device&,radray::gpu::TextureFormat depth);
};
}
struct rn_pipeline : raynium::ResourceHandle<rn_pipeline> {
    radray::unique_ptr<raynium::ImmediatePipeline> impl;
};
