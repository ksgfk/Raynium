#include <raynium/immediate_pass.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
radray::shared_ptr<ImmediatePass> ImmediatePass::Decode(std::span<const radray::byte> record,const TextureRegistry& registry,uint32_t maxColors) {
    struct Header { uint32_t opcode,size,colors,depth; radray::Rect area; };
    struct Wire { uint32_t id,mip,layer,clear; ColorClearValue value; };
    static_assert(sizeof(Header)==32 && sizeof(Wire)==32);
    Header header{};
    if(record.size()<sizeof(header)) return nullptr;
    std::memcpy(&header,record.data(),sizeof(header));
    if(header.colors>maxColors || header.depth>1 || record.size()!=sizeof(header)+(uint64_t(header.colors)+header.depth)*sizeof(Wire) ||
        header.area.X<0 || header.area.Y<0 || !header.area.Width || !header.area.Height) return nullptr;
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
    return result;
}
bool ImmediatePass::AddTerrain(std::span<const radray::byte> bytes,std::span<TerrainView> views,const TextureRegistry& textures) {
    TerrainFeature terrain;
    if(!terrain.Decode(bytes,views,textures,_area)) return false;
    _commands.emplace_back(std::move(terrain));
    return true;
}
bool ImmediatePass::AddDraw(std::span<const radray::byte> bytes,const ImmediatePipelineRegistry& pipelines,const BufferRegistry& buffers,const TextureRegistry& textures) {
    radray::vector<TextureFormat> formats;
    for(auto& a:_colors) formats.push_back(a.texture?a.texture->GetDesc().Format:TextureFormat::UNKNOWN);
    ImmediateDraw draw;
    if(!draw.Decode(bytes,pipelines,buffers,textures,formats,_area)) return false;
    _commands.emplace_back(std::move(draw));
    return true;
}
void ImmediatePass::WriteTimestamp(uint32_t physicalIndex) { _commands.emplace_back(physicalIndex); _containsTimestamp=true; }
void ImmediatePass::Measure(rn_frame_metrics& metrics) const {
    for(const auto& command:_commands) {
        if(auto* draw=std::get_if<ImmediateDraw>(&command)) {
            ++metrics.scene_calls;
            metrics.bind_calls+=draw->BindingCount();
        } else if(auto* feature=std::get_if<TerrainFeature>(&command)) {
            const auto& view=*feature->view;
            const auto& layer=view.lists.layers[std::min(feature->stage,uint32_t(Translucent))];
            metrics.terrain_records+=layer.recordCount;
            for(const auto& b:std::span{view.lists.batches}.subspan(layer.firstBatch,layer.batchCount))
                metrics.terrain_calls+=view.useIndirect?(uint64_t(b.count)+view.indirectLimit-1)/view.indirectLimit:b.count;
        }
    }
}
void ImmediatePass::Replay(radray::RDGBuilder& graph,FrameTextureImports& imports,const FrameTimestamps& timestamps) {
    const bool full=_area.Width==_width && _area.Height==_height;
    // Initialization passes have to precede the raster node in the graph.
    radray::vector<radray::RDGTextureHandle> colors(_colors.size());
    for(size_t i=0;i<_colors.size();++i) if(_colors[i].texture) colors[i]=imports.Import(_colors[i].texture,_colors[i].clear && full);
    radray::RDGTextureHandle depth;
    if(_depth.texture) depth=imports.Import(_depth.texture,_depth.clear && full &&
        !(GetTextureFormatAspects(_depth.texture->GetDesc().Format)&TextureAspect::Stencil));
    for(auto& command:_commands) if(auto* draw=std::get_if<ImmediateDraw>(&command)) draw->Import(graph,imports);
        else if(auto* terrain=std::get_if<TerrainFeature>(&command)) terrain->Import(graph,imports);
    struct Data { ImmediatePass* pass; const FrameTimestamps* queries; };
    auto pass=graph.AddRasterPass("RenderPearl raster pass",Data{this,&timestamps},
        +[](const Data& data,radray::RDGRasterPassContext& context) noexcept {
            auto& encoder=context.GetEncoder();
            encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
            encoder.SetScissor(data.pass->_area);
            for(const auto& command:data.pass->_commands) {
                if(auto* idx=std::get_if<uint32_t>(&command)) data.queries->WriteInline(*encoder.GetCommandBuffer(),*idx);
                else if(auto* draw=std::get_if<ImmediateDraw>(&command)) { if(!draw->Record(encoder)) return false; }
                else {
                    if(!std::get<TerrainFeature>(command).Record(encoder)) return false;
                    encoder.SetViewport({0,0,float(data.pass->_width),float(data.pass->_height),0,1});
                    encoder.SetScissor(std::get<TerrainFeature>(command).scissor);
                }
            }
            return encoder.IsValid();
        },+[](Data& data,radray::RDGPrepareContext& ctx) noexcept {
            const auto& depth=data.pass->_depth.texture;
            auto format=depth?depth->GetDesc().Format:TextureFormat::UNKNOWN;
            for(auto& cmd:data.pass->_commands) {
                if(auto* draw=std::get_if<ImmediateDraw>(&cmd);draw && !draw->Prepare(ctx,format)) return false;
                if(auto* terrain=std::get_if<TerrainFeature>(&cmd);terrain && !terrain->Prepare(ctx)) return false;
            }
            return true;
        });
    for(auto& command:_commands) if(auto* draw=std::get_if<ImmediateDraw>(&command)) draw->Declare(pass);
        else if(auto* terrain=std::get_if<TerrainFeature>(&command)) terrain->Declare(pass);
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
