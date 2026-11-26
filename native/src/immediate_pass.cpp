#include <raynium/immediate_pass.h>
#include <raynium/raster_records.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
RasterPassPlan* RasterPassPlan::Decode(std::pmr::memory_resource& memory,std::span<const radray::byte> record,rn_renderer* owner,uint32_t maxColors) {
    struct Wire { rn_texture* id; uint32_t mip,layer,clear,reserved; ColorClearValue value; };
    static_assert(sizeof(Wire)==40);
    wire::Pass header{};
    if(!wire::ReadRasterHeader(record,header) || header.colors>maxColors) return nullptr;
    auto result=FrameArray<RasterPassPlan>(memory,1).data();
    result->_area=header.area;
    result->_colors=FrameArray<Attachment>(memory,header.colors);

    for(uint32_t i=0;i<header.colors+header.depth;++i) {
        Wire wire{};
        std::memcpy(&wire,record.data()+sizeof(header)+i*sizeof(Wire),sizeof(wire));
        bool depth=i==header.colors;
        if(!wire.id && !depth) continue;
        auto* texture=ResolveHandle(owner,wire.id);
        if(!texture || !(texture->desc.flags&RN_TEXTURE_ATTACHMENT) ||
            wire.clear>1 || wire.mip>=texture->desc.mip_levels || wire.layer>=texture->desc.array_layers ||
            depth!=IsDepthStencilFormat(texture->texture->GetDesc().Format) || (depth && wire.clear && !(wire.value.Value[0]>=0 && wire.value.Value[0]<=1)))
            return nullptr;
        for(const auto& previous:result->_colors) if(previous.texture==texture) return nullptr;
        uint32_t w=std::max(1u,texture->desc.width>>wire.mip),h=std::max(1u,texture->desc.height>>wire.mip);
        if(result->_width && (result->_width!=w || result->_height!=h)) return nullptr;
        result->_width=w; result->_height=h;
        (depth?result->_depth:result->_colors[i])={texture,{},wire.mip,wire.layer,wire.clear!=0,wire.value};
    }
    if(uint64_t(header.area.X)+header.area.Width>result->_width || uint64_t(header.area.Y)+header.area.Height>result->_height) return nullptr;
    result->_colorFormats=FrameArray<TextureFormat>(memory,header.colors);
    for(size_t i=0;i<header.colors;++i) {
        auto* texture=result->_colors[i].texture;
        result->_colorFormats[i]=texture?texture->texture->GetDesc().Format:TextureFormat::UNKNOWN;
    }
    return result;
}
bool RasterPassPlan::TryFoldClear(const rn_texture* texture,uint32_t mip,uint32_t layer,const ColorClearValue& value) noexcept {
    if(_area.X || _area.Y || _area.Width!=_width || _area.Height!=_height) return false;
    const auto fold=[&](Attachment& a) {
        if(a.texture!=texture || a.mip!=mip || a.layer!=layer || a.clear) return false;
        a.clear=true;
        a.value=value;
        return true;
    };
    if(IsDepthStencilFormat(texture->texture->GetDesc().Format))
        return !(GetTextureFormatAspects(texture->texture->GetDesc().Format)&TextureAspect::Stencil) && fold(_depth);
    for(auto& color:_colors) if(fold(color)) return true;
    return false;
}
bool RasterPassPlan::AddTerrain(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte> bytes,std::span<TerrainView> views,rn_renderer* owner) {
    auto* terrain=FrameArray<TerrainFeature>(memory,1).data();
    if(!terrain->Decode(bytes,views,owner,_area)) return false;
    const uint32_t layer=std::min(terrain->stage,uint32_t(Translucent));
    if(terrain->view->draws->layers[layer].recordCount) {
        if(layer==Translucent) translucentVertices=terrain->view->translucentVertexPool;
        else terrainVertices=terrain->view->vertexPool;
        if(layer!=Translucent) quadIndices=terrain->view->quadIndexPool;
        if(layer==Translucent || terrain->view->draws->wireframe) translucentIndices=terrain->view->translucentIndexPool;
    }
    ops.emplace_back(terrain);
    return true;
}
bool RasterPassPlan::AddDraw(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte> bytes,rn_renderer* owner) {
    auto* draw=FrameArray<ImmediateDraw>(memory,1).data();
    if(!draw->Decode(memory,bytes,owner,{_colorFormats.data(),_colorFormats.size()},_area)) return false;
    ops.emplace_back(draw);
    return true;
}
bool RasterPassPlan::AddInstancedGeometry(std::pmr::memory_resource& memory,std::pmr::vector<RasterOp>& ops,std::span<const radray::byte> bytes,std::span<const radray::byte> instanceUploads,
    std::pmr::vector<DrawIndexedIndirectArguments>& indirectArgs,bool canIndirect,uint32_t indirectLimit,
    rn_renderer* owner,
    const StaticGeometryRegistry& staticGeometry,const TerrainResidency& terrain) {
    auto* feature=FrameArray<InstancedGeometryFeature>(memory,1).data();
    if(!feature->Decode(memory,bytes,instanceUploads,indirectArgs,canIndirect,indirectLimit,owner,staticGeometry,terrain,
        {_colorFormats.data(),_colorFormats.size()},_area)) return false;
    staticVertices=feature->StaticVertexPool();
    quadIndices=feature->IndexPool();
    needInstanceBuffer=true;
    needGeometryIndirect|=feature->UsesIndirect();
    ops.emplace_back(feature);
    return true;
}
void RasterPassPlan::WriteTimestamp(std::pmr::vector<RasterOp>& ops,uint32_t physicalIndex) {
    ops.emplace_back(physicalIndex);
    _containsTimestamp=true;
}
void RasterPassPlan::Measure(rn_frame_metrics& metrics) const {
    for(const auto& command:_commands) {
        if(auto* draw=command.Get<ImmediateDraw>()) {
            ++metrics.scene_calls;
            metrics.bind_calls+=draw->BindingCount();
        } else if(auto* staticGeometry=command.Get<InstancedGeometryFeature>()) {
            metrics.scene_calls+=staticGeometry->DrawCount();
            metrics.bind_calls+=staticGeometry->BindingCount();
        } else if(auto* feature=command.Get<TerrainFeature>()) {
            const auto& view=*feature->view;
            const auto& layer=view.draws->layers[std::min(feature->stage,uint32_t(Translucent))];
            metrics.terrain_records+=layer.recordCount;
            if(view.useIndirect) {
                for(const auto& b:layer.prepared.Batches)
                    metrics.terrain_calls+=(uint64_t(b.ArgumentCount)+view.indirectLimit-1)/view.indirectLimit;
            } else {
                metrics.terrain_calls+=layer.recordCount;
            }
        }
    }
}
void RasterPassPlan::Replay(radray::RDGBuilder& graph,FrameTextureImports& imports,const FrameTimestamps& timestamps,
    radray::RDGBufferHandle instanceBuffer,radray::RDGBufferHandle geometryIndirectBuffer) {
    const bool full=_area.Width==_width && _area.Height==_height;
    // Initialization passes have to precede the raster node in the graph.
    for(size_t i=0;i<_colors.size();++i) if(_colors[i].texture) _colors[i].handle=imports.Import(_colors[i].texture,_colors[i].clear && full);
    radray::RDGTextureHandle depth;
    if(_depth.texture) depth=imports.Import(_depth.texture,_depth.clear && full &&
        !(GetTextureFormatAspects(_depth.texture->texture->GetDesc().Format)&TextureAspect::Stencil));
    for(auto& command:_commands) {
        if(auto* draw=command.Get<ImmediateDraw>()) draw->Import(graph,imports);
        else if(auto* terrain=command.Get<TerrainFeature>()) terrain->Import(graph,imports);
        else if(auto* staticGeometry=command.Get<InstancedGeometryFeature>()) staticGeometry->Import(graph,imports,instanceBuffer,geometryIndirectBuffer);
    }
    struct Data { RasterPassPlan* pass; const FrameTimestamps* queries; };
    auto pass=graph.AddRasterPass("RenderPearl raster pass",Data{this,&timestamps},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
            encoder.SetScissor(data.pass->_area);
            ImmediatePassStateCache stateCache{};
            stateCache.scissor=data.pass->_area;
            for(const auto& command:data.pass->_commands) {
                if(auto idx=command.Get<uint32_t>()) data.queries->WriteInline(*encoder.GetCommandBuffer(),*idx);
                else if(auto* draw=command.Get<ImmediateDraw>()) { if(!draw->Record(encoder,stateCache)) return false; }
                else if(auto* staticGeometry=command.Get<InstancedGeometryFeature>()) { if(!staticGeometry->Record(encoder,stateCache)) return false; }
                else {
                    if(!command.Get<TerrainFeature>()->Record(encoder)) return false;
                    encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
                    const auto scissor=command.Get<TerrainFeature>()->scissor;
                    encoder.SetScissor(scissor);
                    stateCache.Invalidate();
                    stateCache.scissor=scissor;
                }
            }
            return encoder.IsValid();
        },+[](Data& data,radray::RDGPrepareContext& ctx) noexcept {
            const auto& depth=data.pass->_depth.texture;
            auto format=depth?depth->texture->GetDesc().Format:TextureFormat::UNKNOWN;
            for(auto& cmd:data.pass->_commands) {
                if(auto* draw=cmd.Get<ImmediateDraw>();draw && !draw->Prepare(ctx,format)) return false;
                if(auto* terrain=cmd.Get<TerrainFeature>();terrain && !terrain->Prepare(ctx)) return false;
                if(auto* staticGeometry=cmd.Get<InstancedGeometryFeature>();staticGeometry && !staticGeometry->Prepare(ctx,format)) return false;
            }
            return true;
        });
    for(auto& command:_commands) {
        if(auto* draw=command.Get<ImmediateDraw>()) draw->Declare(pass);
        else if(auto* terrain=command.Get<TerrainFeature>()) terrain->Declare(pass); else if(auto* staticGeometry=command.Get<InstancedGeometryFeature>()) staticGeometry->Declare(pass);
    }
    if(terrainVertices) terrainVertices->AddPassReadAccess(graph,pass);
    if(translucentVertices) translucentVertices->AddPassReadAccess(graph,pass);
    if(staticVertices) staticVertices->AddPassReadAccess(graph,pass);
    if(quadIndices) quadIndices->AddPassReadAccess(graph,pass);
    if(translucentIndices) translucentIndices->AddPassReadAccess(graph,pass);
    if(needInstanceBuffer && instanceBuffer.IsValid()) pass.ReadBuffer(instanceBuffer,BufferState::Vertex);
    if(needGeometryIndirect && geometryIndirectBuffer.IsValid()) pass.ReadBuffer(geometryIndirectBuffer,BufferState::Indirect);
    pass.SetRenderArea(_area);
    for(uint32_t i=0;i<_colors.size();++i) if(_colors[i].handle.IsValid()) {
        const auto& a=_colors[i];
        pass.WriteColor(i,_colors[i].handle,a.clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,a.value,radray::RDGAttachmentSlice{a.mip,a.layer,1});
        graph.MarkOutput(_colors[i].handle);
    }
    if(depth.IsValid()) {
        const auto& a=_depth;
        auto aspects=GetTextureFormatAspects(a.texture->texture->GetDesc().Format);
        pass.SetDepthStencil(depth,TextureState::DepthWrite,a.clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
            aspects&TextureAspect::Stencil?LoadAction::Load:LoadAction::DontCare,aspects&TextureAspect::Stencil?StoreAction::Store:StoreAction::Discard,
            {a.value.Value[0],0},radray::RDGAttachmentSlice{a.mip,a.layer,1});
        graph.MarkOutput(depth);
    }
    if(_containsTimestamp) { pass.NeverCull(); graph.AddOrderBarrier(pass.GetHandle()); }
}
}
