#include <raynium/solid_pipeline.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::render;
bool SolidPassProcessor::Validate(const radray::MeshBatch& mesh,const radray::MaterialDescriptor& material,std::string& error) const {
    const bool valid=(material.program==RN_PROGRAM_UV && mesh.layout==RN_LAYOUT_PROCEDURAL && mesh.stride==0) ||
        (material.program==RN_PROGRAM_TEXTURED && mesh.layout==RN_LAYOUT_POSITION_COLOR_UV_LIGHT && mesh.stride==28 && mesh.indexBuffer &&
         material.resources.size()==2 && material.resources[0]<textureCount && material.resources[1]<textureCount);
    if(!valid) error="SOLID/UV mesh layout or material texture references are invalid";
    return valid;
}
bool SolidRenderPipeline::Create(RayniumRuntime& device,std::unique_ptr<SolidRenderPipeline>& out,radray::string& error) {
    if(out) { error="Pipeline output occupied"; return false; }
    radray::unique_ptr<MeshPassResources> resources;
    if(!MeshPassResources::Create(device.GetDevice(),resources,error)) return false;
    out=std::unique_ptr<SolidRenderPipeline>(new SolidRenderPipeline(device,std::move(resources))); return true;
}
bool SolidRenderPipeline::BuildGraph(radray::GraphBuilder& graph,radray::FrameContext& frame,radray::PreparedMeshPass& prepared,radray::Nullable<const void*> data,std::string& error) {
    using namespace radray;
    const auto& input=*static_cast<const SolidFrame*>(data.Get()); const auto& f=input.frame;
    auto& d=_device.GetDevice(); auto& target=input.target;
    const auto color=target.Import(graph);
    if(input.textures) input.textures->BuildGraph(graph,f.changed_textures);
    const auto depth=graph.CreateTexture("Reversed depth",{.Dim=TextureDimension::Dim2D,.Width=target.Width(),.Height=target.Height(),
        .DepthOrArraySize=1,.MipLevels=1,.SampleCount=1,.Format=TextureFormat::D32_FLOAT,.Memory=MemoryType::Device,.Usage=TextureUse::DepthStencilWrite});
    struct Constants { float projection[16],view[16]; int32_t block[4]; float fraction[4],fog[4],distances[4],target[4]; } constants{};
    std::copy_n(f.projection,16,constants.projection); std::copy_n(f.view,16,constants.view); std::copy_n(f.camera_block,3,constants.block);
    constants.block[3]=(f.flags&RN_FRAME_RGSS)!=0; std::copy_n(f.camera_fraction,3,constants.fraction);
    constants.fraction[3]=d.GetBackend()==RenderBackend::Vulkan ? 1.f : -1.f;
    std::copy_n(f.fog_color,4,constants.fog); std::copy_n(f.fog_distances,4,constants.distances);
    constants.target[0]=float(target.Width()); constants.target[1]=float(target.Height());
    std::array<std::byte,256> bytes{}; std::memcpy(bytes.data(),&constants,sizeof(constants));
    auto uniform=frame.Upload("SOLID constants",bytes,BufferUse::CBuffer,error); if(!uniform) return false;
    auto materials=std::make_shared<std::map<uint64_t,MeshMaterialBinding>>();
    const auto uniformState=ResourceState::Buffer(BufferState::CBuffer,ShaderStage::Vertex|ShaderStage::Pixel);
    const auto textureState=ResourceState::Texture(TextureState::ShaderRead,ShaderStage::Vertex|ShaderStage::Pixel);
    for(const auto& visible:prepared.visible) {
        const auto& command=visible.Command(); if(prepared.bindings.contains(command.material)) continue;
        radray::string failure;
        if(!_processor.Resources().PrepareBinding(d,*uniform,*materials,command,input.textures,failure)) { error=failure.c_str(); return false; }
        const auto& program=_processor.Resources().Program(command.program);
        MeshBinding binding{program.pipeline,materials->at(command.material).parameters.get()};
        if(program.vertexInput) { binding.vertexSlot=0; binding.instanceSlot=1; }
        binding.resources.push_back({uniform,nullptr,Access::Constant,ShaderStage::Vertex|ShaderStage::Pixel,{uniformState,uniformState}});
        if(program.textures) for(auto index:command.bindings.resources)
            binding.resources.push_back({nullptr,input.textures->SampledTexture(index),Access::Sample,ShaderStage::Vertex|ShaderStage::Pixel,{textureState,textureState}});
        prepared.bindings.emplace(command.material,std::move(binding));
    }
    frame.KeepAlive(std::move(materials));
    ColorAttachment attachment{color}; std::copy_n(f.fog_color,4,attachment.clear.Value.begin());
    graph.AddMeshPass("SolidRaster",prepared,{{attachment},DepthAttachment{depth},target.Width(),target.Height()});
    return true;
}
}
