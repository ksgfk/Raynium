#include <raynium/renderer.h>
#include <cstring>

namespace raynium {
namespace {
template<class T> bool Array(const T* p,uint32_t count,uint32_t limit=1048576) { return count<=limit && (!count || p); }
struct ConvertedFrame {
    radray::SceneUpdate scene;
    std::vector<radray::IndexUpdate> indices;
    std::vector<radray::MeshUpdate> meshes;
    std::vector<std::vector<radray::DrawRange>> ranges;
    std::vector<radray::MaterialDescriptor> materials;
    std::vector<radray::PrimitiveUpdate> primitives;
    std::vector<radray::VisibleItem> visible;
    bool Convert(const rn_render_frame_desc& f,radray::string& error) {
        const auto fail=[&](const char* e) { error=e; return false; };
        if(f.struct_size!=sizeof(f) || f.reserved || (f.flags&~13u) ||
           !Array(f.indices,f.index_count,65536) || !Array(f.meshes,f.mesh_count,65536) || !Array(f.materials,f.material_count,65536) ||
           !Array(f.primitives,f.primitive_count,65536) || !Array(f.removed_indices,f.removed_index_count) || !Array(f.removed_meshes,f.removed_mesh_count) ||
           !Array(f.removed_materials,f.removed_material_count) || !Array(f.removed_primitives,f.removed_primitive_count) ||
           !Array(f.visible,f.visible_count) || !Array(f.instances,f.instance_count)) return fail("Invalid frame layout or array");
        for(uint32_t i=0;i<f.index_count;++i) { const auto& x=f.indices[i]; indices.push_back({x.id,x.indices,x.count,x.reserved}); }
        ranges.resize(f.mesh_count);
        for(uint32_t i=0;i<f.mesh_count;++i) {
            const auto& x=f.meshes[i];
            if(!Array(x.submeshes,x.submesh_count,65536) || x.layout>RN_LAYOUT_POSITION_COLOR_UV_LIGHT ||
               (x.layout==RN_LAYOUT_PROCEDURAL ? (x.vertex_stride || x.vertices || x.index_buffer) : (!x.vertices || x.vertex_stride!=28 || !x.index_buffer))) return fail("Invalid MC vertex layout");
            for(uint32_t j=0;j<x.submesh_count;++j) {
                const auto& range=x.submeshes[j]; if(range.count%3) return fail("MC submesh must contain triangle indices");
                ranges[i].push_back({range.first,range.count,range.base_vertex,range.reserved});
            }
            meshes.push_back({x.id,x.vertices,x.index_buffer,ranges[i].data(),x.vertex_count,x.vertex_stride,x.layout,x.submesh_count});
        }
        for(uint32_t i=0;i<f.material_count;++i) {
            const auto& x=f.materials[i];
            if(x.program<RN_PROGRAM_UV || x.program>RN_PROGRAM_TEXTURED || x.anisotropy>16) return fail("Invalid MC material");
            radray::MaterialDescriptor material{x.id,x.program};
            if(x.program==RN_PROGRAM_TEXTURED) material.resources={x.albedo_texture,x.light_texture};
            material.parameters.resize(sizeof(x.anisotropy)); std::memcpy(material.parameters.data(),&x.anisotropy,sizeof(x.anisotropy));
            materials.push_back(std::move(material));
        }
        for(uint32_t i=0;i<f.primitive_count;++i) { const auto& p=f.primitives[i]; primitives.push_back({p.id,p.mesh,p.material}); }
        for(uint32_t i=0;i<f.visible_count;++i) { const auto& v=f.visible[i]; visible.push_back({v.primitive,v.submesh,v.depth_key,v.first_instance,v.instance_count}); }
        scene={f.epoch,indices.data(),meshes.data(),materials.data(),primitives.data(),f.removed_indices,f.removed_meshes,f.removed_materials,f.removed_primitives,
            f.index_count,f.mesh_count,f.material_count,f.primitive_count,f.removed_index_count,f.removed_mesh_count,f.removed_material_count,f.removed_primitive_count};
        return true;
    }
};
}
bool Renderer::Create(RayniumRuntime& device,radray::unique_ptr<Renderer>& out,radray::string& error) {
    if(out) { error="Renderer output occupied"; return false; }
    std::unique_ptr<SolidRenderPipeline> pipeline;
    if(!SolidRenderPipeline::Create(device,pipeline,error)) return false;
    auto* borrowed=pipeline.get(); std::unique_ptr<radray::Renderer> renderer; std::string failure;
    renderer=radray::Renderer::Create(device,std::move(pipeline),failure).Release();
    if(!renderer) { error=failure.c_str(); return false; }
    out=radray::unique_ptr<Renderer>(new Renderer(device,std::move(renderer),borrowed)); return true;
}
bool Renderer::Finish(radray::string& error) { std::string failure; const bool result=_renderer->Finish(failure); if(!result) error=failure.c_str(); return result; }
void Renderer::AttachTextures(SharedTextures* textures) noexcept {
    _textures=textures; ++_textureVersion; _pipeline->TextureCount(textures ? textures->Count() : 0);
}
bool Renderer::Render(RenderTarget& target,const rn_render_frame_desc& f,radray::string& error) {
    if(!f.output_sequence || f.output_sequence==UINT64_MAX || f.output_sequence!=target.Submitted()+1) { error="Output sequence must start at 1 and increase by one"; return false; }
    if(_textures ? !_textures->Validate(f.input_sequence,f.changed_textures,error) : (f.input_sequence || f.changed_textures)) {
        if(error.empty()) error="Frame has inputs without a shared texture set"; return false;
    }
    ConvertedFrame converted; if(!converted.Convert(f,error)) return false;
    const radray::ViewSubmission view{converted.visible,{reinterpret_cast<const std::byte*>(f.instances),size_t(f.instance_count)*sizeof(rn_instance)},sizeof(rn_instance)};
    radray::Submission submission; submission.forceDirect=(f.flags&RN_FRAME_DIRECT)!=0;
    submission.profile=(f.flags&RN_FRAME_PROFILE)!=0;
    if(target.Submitted()) submission.waits.push_back({target.Consumed(),target.Submitted()});
    submission.signals.push_back({target.Ready(),f.output_sequence});
    if(_textures) { submission.waits.push_back({_textures->Available(),f.input_sequence}); submission.signals.push_back({_textures->Released(),f.input_sequence+1}); }
    const SolidFrame data{target,_textures,f}; std::string failure;
    if(!_renderer->Render(_scene,converted.scene,view,_textureVersion,&data,submission,failure)) { error=failure.c_str(); return false; }
    target.Commit(f.output_sequence); if(_textures) _textures->Commit(f.changed_textures); return true;
}
}
