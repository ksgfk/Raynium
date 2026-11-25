#include <raynium/immediate_pass.h>
#include <raynium/raster_records.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
radray::shared_ptr<ImmediatePass> ImmediatePass::Decode(std::span<const radray::byte> record,const TextureRegistry& registry,uint32_t maxColors) {
    struct Wire { uint32_t id,mip,layer,clear; ColorClearValue value; };
    static_assert(sizeof(Wire)==32);
    wire::Pass header{};
    if(!wire::ReadRasterHeader(record,header) || header.colors>maxColors) return nullptr;
    auto result=radray::make_shared<ImmediatePass>();
    result->_area=header.area;
    result->_colors.resize(header.colors);
    radray::unordered_set<uint32_t> seen;
    for(uint32_t i=0;i<header.colors+header.depth;++i) {
        Wire wire{};
        std::memcpy(&wire,record.data()+sizeof(header)+i*sizeof(Wire),sizeof(wire));
        bool depth=i==header.colors;
        if(!wire.id && !depth) continue;
        auto* texture=registry.Find(wire.id);
        if(!texture || !(texture->desc.flags&RN_TEXTURE_ATTACHMENT) || !seen.insert(wire.id).second ||
            wire.clear>1 || wire.mip>=texture->desc.mip_levels || wire.layer>=texture->desc.array_layers ||
            depth!=IsDepthStencilFormat(texture->texture->GetDesc().Format) || (depth && wire.clear && !(wire.value.Value[0]>=0 && wire.value.Value[0]<=1)))
            return nullptr;
        uint32_t w=std::max(1u,texture->desc.width>>wire.mip),h=std::max(1u,texture->desc.height>>wire.mip);
        if(result->_width && (result->_width!=w || result->_height!=h)) return nullptr;
        result->_width=w; result->_height=h;
        (depth?result->_depth:result->_colors[i])={texture->texture,wire.mip,wire.layer,wire.clear!=0,wire.value};
    }
    if(uint64_t(header.area.X)+header.area.Width>result->_width || uint64_t(header.area.Y)+header.area.Height>result->_height) return nullptr;
    result->_colorFormats.reserve(result->_colors.size());
    for(const auto& a:result->_colors) result->_colorFormats.push_back(a.texture?a.texture->GetDesc().Format:TextureFormat::UNKNOWN);
    return result;
}
bool ImmediatePass::AddTerrain(std::span<const radray::byte> bytes,std::span<TerrainView> views,const TextureRegistry& textures) {
    TerrainFeature terrain;
    if(!terrain.Decode(bytes,views,textures,_area)) return false;
    _commands.emplace_back(std::move(terrain));
    return true;
}
bool ImmediatePass::AddDraw(std::span<const radray::byte> bytes,const ImmediatePipelineRegistry& pipelines,const BufferRegistry& buffers,const TextureRegistry& textures) {
    ImmediateDraw draw;
    if(!draw.Decode(bytes,pipelines,buffers,textures,{_colorFormats.data(),_colorFormats.size()},_area)) return false;
    _commands.emplace_back(std::move(draw));
    return true;
}
bool ImmediatePass::AddInstancedGeometry(std::span<const radray::byte> bytes,std::span<const radray::byte> instanceUploads,
    radray::vector<DrawIndexedIndirectArguments>& indirectArgs,bool canIndirect,uint32_t indirectLimit,
    const ImmediatePipelineRegistry& pipelines,const BufferRegistry& buffers,const TextureRegistry& textures,
    const StaticGeometryRegistry& staticGeometry,const TerrainResidency& terrain) {
    InstancedGeometryFeature feature;
    if(!feature.Decode(bytes,instanceUploads,indirectArgs,canIndirect,indirectLimit,pipelines,buffers,textures,staticGeometry,terrain,
        {_colorFormats.data(),_colorFormats.size()},_area)) return false;
    _commands.emplace_back(std::move(feature));
    return true;
}
void ImmediatePass::WriteTimestamp(uint32_t physicalIndex) { _commands.emplace_back(physicalIndex); _containsTimestamp=true; }
void ImmediatePass::Measure(rn_frame_metrics& metrics) const {
    for(const auto& command:_commands) {
        if(auto* draw=std::get_if<ImmediateDraw>(&command)) {
            ++metrics.scene_calls;
            metrics.bind_calls+=draw->BindingCount();
        } else if(auto* staticGeometry=std::get_if<InstancedGeometryFeature>(&command)) {
            metrics.scene_calls+=staticGeometry->DrawCount();
            metrics.bind_calls+=staticGeometry->BindingCount();
        } else if(auto* feature=std::get_if<TerrainFeature>(&command)) {
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
void ImmediatePass::Replay(radray::RDGBuilder& graph,FrameTextureImports& imports,const FrameTimestamps& timestamps,
    radray::RDGBufferHandle instanceBuffer,radray::RDGBufferHandle geometryIndirectBuffer) {
    const bool full=_area.Width==_width && _area.Height==_height;
    // Initialization passes have to precede the raster node in the graph.
    radray::vector<radray::RDGTextureHandle> colors(_colors.size());
    for(size_t i=0;i<_colors.size();++i) if(_colors[i].texture) colors[i]=imports.Import(_colors[i].texture,_colors[i].clear && full);
    radray::RDGTextureHandle depth;
    if(_depth.texture) depth=imports.Import(_depth.texture,_depth.clear && full &&
        !(GetTextureFormatAspects(_depth.texture->GetDesc().Format)&TextureAspect::Stencil));
    for(auto& command:_commands) {
        if(auto* draw=std::get_if<ImmediateDraw>(&command)) draw->Import(graph,imports);
        else if(auto* terrain=std::get_if<TerrainFeature>(&command)) terrain->Import(graph,imports);
        else if(auto* staticGeometry=std::get_if<InstancedGeometryFeature>(&command)) staticGeometry->Import(graph,imports,instanceBuffer,geometryIndirectBuffer);
    }
    struct Data { ImmediatePass* pass; const FrameTimestamps* queries; };
    auto pass=graph.AddRasterPass("RenderPearl raster pass",Data{this,&timestamps},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
            encoder.SetScissor(data.pass->_area);
            ImmediatePassStateCache stateCache{};
            stateCache.scissor=data.pass->_area;
            for(const auto& command:data.pass->_commands) {
                if(auto* idx=std::get_if<uint32_t>(&command)) data.queries->WriteInline(*encoder.GetCommandBuffer(),*idx);
                else if(auto* draw=std::get_if<ImmediateDraw>(&command)) { if(!draw->Record(encoder,stateCache)) return false; }
                else if(auto* staticGeometry=std::get_if<InstancedGeometryFeature>(&command)) { if(!staticGeometry->Record(encoder,stateCache)) return false; }
                else {
                    if(!std::get<TerrainFeature>(command).Record(encoder)) return false;
                    encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
                    const auto scissor=std::get<TerrainFeature>(command).scissor;
                    encoder.SetScissor(scissor);
                    stateCache.Invalidate();
                    stateCache.scissor=scissor;
                }
            }
            return encoder.IsValid();
        },+[](Data& data,radray::RDGPrepareContext& ctx) noexcept {
            const auto& depth=data.pass->_depth.texture;
            auto format=depth?depth->GetDesc().Format:TextureFormat::UNKNOWN;
            for(auto& cmd:data.pass->_commands) {
                if(auto* draw=std::get_if<ImmediateDraw>(&cmd);draw && !draw->Prepare(ctx,format)) return false;
                if(auto* terrain=std::get_if<TerrainFeature>(&cmd);terrain && !terrain->Prepare(ctx)) return false;
                if(auto* staticGeometry=std::get_if<InstancedGeometryFeature>(&cmd);staticGeometry && !staticGeometry->Prepare(ctx,format)) return false;
            }
            return true;
        });
    const radray::VertexBufferPool* terrainVertices{};
    const radray::VertexBufferPool* translucentVertices{};
    const radray::VertexBufferPool* staticVertices{};
    const radray::IndexBufferPool* quadIndices{},*translucentIndices{};
    bool needInstanceBuffer=false;
    bool needGeometryIndirect=false;
    for(auto& command:_commands) {
        if(auto* draw=std::get_if<ImmediateDraw>(&command)) draw->Declare(pass);
        else if(auto* terrain=std::get_if<TerrainFeature>(&command)) {
            terrain->Declare(pass);
            const uint32_t layer=std::min(terrain->stage,uint32_t(Translucent));
            if(terrain->view->draws->layers[layer].recordCount) {
                if(layer==Translucent) translucentVertices=terrain->view->translucentVertexPool;
                else terrainVertices=terrain->view->vertexPool;
                if(layer!=Translucent) quadIndices=terrain->view->quadIndexPool;
                if(layer==Translucent || terrain->view->draws->wireframe) translucentIndices=terrain->view->translucentIndexPool;
            }
        } else if(auto* staticGeometry=std::get_if<InstancedGeometryFeature>(&command)) {
            staticGeometry->Declare(pass);
            staticVertices=staticGeometry->StaticVertexPool();
            quadIndices=staticGeometry->IndexPool();
            needInstanceBuffer=true;
            needGeometryIndirect|=staticGeometry->UsesIndirect();
        }
    }
    if(terrainVertices) terrainVertices->AddPassReadAccess(graph,pass);
    if(translucentVertices) translucentVertices->AddPassReadAccess(graph,pass);
    if(staticVertices) staticVertices->AddPassReadAccess(graph,pass);
    if(quadIndices) quadIndices->AddPassReadAccess(graph,pass);
    if(translucentIndices) translucentIndices->AddPassReadAccess(graph,pass);
    if(needInstanceBuffer && instanceBuffer.IsValid()) pass.ReadBuffer(instanceBuffer,BufferState::Vertex);
    if(needGeometryIndirect && geometryIndirectBuffer.IsValid()) pass.ReadBuffer(geometryIndirectBuffer,BufferState::Indirect);
    pass.SetRenderArea(_area);
    for(uint32_t i=0;i<_colors.size();++i) if(colors[i].IsValid()) {
        const auto& a=_colors[i];
        pass.WriteColor(i,colors[i],a.clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,a.value,radray::RDGAttachmentSlice{a.mip,a.layer,1});
        graph.MarkOutput(colors[i]);
    }
    if(depth.IsValid()) {
        const auto& a=_depth;
        auto aspects=GetTextureFormatAspects(a.texture->GetDesc().Format);
        pass.SetDepthStencil(depth,TextureState::DepthWrite,a.clear?LoadAction::Clear:LoadAction::Load,StoreAction::Store,
            aspects&TextureAspect::Stencil?LoadAction::Load:LoadAction::DontCare,aspects&TextureAspect::Stencil?StoreAction::Store:StoreAction::Discard,
            {a.value.Value[0],0},radray::RDGAttachmentSlice{a.mip,a.layer,1});
        graph.MarkOutput(depth);
    }
    if(_containsTimestamp) { pass.NeverCull(); graph.AddOrderBarrier(pass.GetHandle()); }
}
}
