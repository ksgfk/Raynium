#include <raynium/immediate_draw.h>
#include <raynium/raster_records.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;
ImmediateDraw::Binding ImmediateDraw::ReadBinding(const radray::byte* base,size_t index) noexcept {
    Binding b{};
    std::memcpy(&b,base+index*sizeof(Binding),sizeof(Binding));
    return b;
}
bool ImmediateDraw::Decode(std::pmr::memory_resource& memory,std::span<const radray::byte> bytes,rn_renderer* owner,std::span<const TextureFormat> colors,const radray::Rect& area) {
    static_assert(sizeof(Binding)==48);
    wire::Draw h{};
    if(!wire::ReadRasterHeader(bytes,h) || h.scissor.X<area.X || h.scissor.Y<area.Y ||
       uint64_t(h.scissor.X)+h.scissor.Width>uint64_t(area.X)+area.Width || uint64_t(h.scissor.Y)+h.scissor.Height>uint64_t(area.Y)+area.Height) return false;
    auto* pipeline=ResolveHandle(owner,h.pipeline);
    _pipeline=pipeline?pipeline->impl.get():nullptr;
    if(!_pipeline || colors.size()!=_pipeline->state.colors.size()) return false;
    for(size_t i=0;i<colors.size();++i) if(colors[i]!=_pipeline->state.colors[i].Format) return false;
    _kind=h.kind; _count=h.count; _instances=h.instances; _first=h.first; _firstInstance=h.firstInstance; _base=h.base; _scissor=h.scissor;
    if(_kind==4 && (_count<3 || _count>UINT32_MAX/3)) return false;
    bytes=bytes.subspan(sizeof(h));
    _bindingBytes=bytes.data();
    _resources=FrameArray<Resource>(memory,h.bindings);
    bool hasIndirect=false;
    for(uint32_t i=0;i<h.bindings;++i) {
        auto& r=_resources[i];
        const auto b=ReadBinding(_bindingBytes,i);
        r.kind=b.kind;
        if(b.kind==5) {
            auto* image=ResolveHandle(owner,reinterpret_cast<rn_texture*>(b.resource));
            if(!image || !b.sampling.mip_count || b.sampling.base_mip>=image->desc.mip_levels ||
               b.sampling.mip_count>image->desc.mip_levels-b.sampling.base_mip) return false;
            r.texture=image;
        } else {
            constexpr uint32_t usage[]{RN_BUFFER_VERTEX,RN_BUFFER_INDEX,RN_BUFFER_INDIRECT,RN_BUFFER_UNIFORM,RN_BUFFER_TEXEL};
            r.buffer=ResolveHandle(owner,reinterpret_cast<rn_buffer*>(b.resource));
            if(b.kind>=5 || !r.buffer || !(r.buffer->usage&usage[b.kind]) || !r.buffer->Contains(b.offset,b.size)) return false;
            if(b.kind==1 && ((b.format!=2 && b.format!=4) || b.offset>UINT32_MAX ||
               (_kind==1 && (uint64_t(_first)+_count)>b.size/b.format))) return false;
            if(b.kind==2) {
                if((_kind!=2 && _kind!=3) || _count>b.size/(_kind==2?16:20)) return false;
                hasIndirect=true;
            }
        }
    }
    if((_kind==2 || _kind==3) && !hasIndirect) return false;
    bytes=bytes.subspan(size_t(h.bindings)*sizeof(Binding));
    _push=FramePushConstants(memory,bytes.first(h.push),_pipeline->program->pushBytes);
    return true;
}

void ImmediateDraw::Import(radray::RDGBuilder& graph,FrameTextureImports& imports) {
    for(auto& r:_resources) {
        if(r.kind==5) r.imageHandle=imports.Import(r.texture);
        else r.bufferHandle=r.buffer->Import(graph);
    }
    if(_pipeline->program->drawBasesLayout) {
        const bool indirect=_kind==2 || _kind==3;
        if(indirect) {
            _drawBases=graph.CreateBuffer({.Size=uint64_t(_count)*256,
                .Usage=BufferUse::CopyDestination|BufferUse::CBuffer});
            size_t argIndex=0;
            while(argIndex<_resources.size() && _resources[argIndex].kind!=2) ++argIndex;
            const auto& args=_resources[argIndex];
            const auto argBinding=ReadBinding(_bindingBytes,argIndex);
            struct CopyBases { radray::RDGBufferHandle from,to; uint64_t offset; uint32_t count,stride; };
            graph.AddCopyPass("Draw argument bases",CopyBases{args.bufferHandle,_drawBases,argBinding.offset,_count,_kind==2?16u:20u},
                +[](const CopyBases& op,radray::RDGCopyPassContext& ctx) noexcept {
                    const auto src=ctx.ResolveBuffer(op.from),dst=ctx.ResolveBuffer(op.to);
                    for(uint32_t i=0;i<op.count;++i) ctx.GetCommandBuffer().CopyBufferToBuffer(dst.Buffer.Get(),dst.Offset+uint64_t(i)*256,
                        src.Buffer.Get(),src.Offset+op.offset+uint64_t(i+1)*op.stride-8,8);
                    return true;
                }).ReadBuffer(args.bufferHandle).WriteBuffer(_drawBases);
        } else {
            struct DrawBaseBuffers {
                explicit DrawBaseBuffers(std::pmr::memory_resource* memory):buffers(memory) {}
                std::pmr::unordered_map<uint64_t,radray::RDGBufferHandle> buffers;
            };
            auto& blackboard=graph.GetBlackboard();
            auto cache=blackboard.Get<DrawBaseBuffers>();
            if(!cache) cache=blackboard.Emplace<DrawBaseBuffers>(imports.Scratch());
            radray::array<uint32_t,2> bases{_kind==0?_first:uint32_t(_base),_firstInstance};
            auto [it,fresh]=cache.Get()->buffers.try_emplace(uint64_t(bases[1])<<32 | bases[0]);
            if(fresh) it->second=graph.CreateDynamicBuffer({.MaxSize=256,.Usage=BufferUse::CBuffer},radray::RDGUploadSource::Generated(256,bases,
                +[](const radray::array<uint32_t,2>& value,radray::RDGUploadWriteContext& ctx) noexcept {
                    std::memcpy(ctx.Bytes.data(),value.data(),sizeof(value)); return true;
                }));
            _drawBases=it->second;
        }
    }
    if(_kind==4) {
        const uint64_t bytes=uint64_t(_count-2)*3*sizeof(uint32_t);
        _fan=graph.CreateDynamicBuffer({.MaxSize=bytes,.Usage=BufferUse::Index},radray::RDGUploadSource::Generated(bytes,_count,
            +[](const uint32_t& count,radray::RDGUploadWriteContext& ctx) noexcept {
                auto* dest=reinterpret_cast<uint32_t*>(ctx.Bytes.data());
                for(uint32_t i=1;i+1<count;++i) { *dest++=0; *dest++=i; *dest++=i+1; }
                return true;
            }));
    }
}
void ImmediateDraw::Declare(radray::RDGRasterPassBuilder& pass) const {
    constexpr BufferState states[]{BufferState::Vertex,BufferState::Index,BufferState::Indirect,BufferState::CBuffer,BufferState::ShaderRead};
    for(const auto& r:_resources) {
        if(r.kind==5) pass.ReadTexture(r.imageHandle);
        else pass.ReadBuffer(r.bufferHandle,states[r.kind]);
    }
    if(_drawBases.IsValid()) pass.ReadBuffer(_drawBases,BufferState::CBuffer);
    if(_fan.IsValid()) pass.ReadBuffer(_fan,BufferState::Index);
}
bool ImmediateDraw::Prepare(radray::RDGPrepareContext& ctx,TextureFormat depth) {
    auto& slot=ctx.GetSlot();
    auto& device=slot.GetDevice();
    _pso=_pipeline->Resolve(device,depth);
    if(!_pso) return false;
    const auto& program=*_pipeline->program;
    FrameParameterBindings bindings(slot);
    auto& groups=bindings.groups;
    size_t vertexCount=0;
    for(const auto& r:_resources) vertexCount+=r.kind==0;
    _vertices=FrameArray<VertexBufferBinding>(slot.GetMemoryResource(),vertexCount);
    size_t vertex=0;
    for(size_t i=0;i<_resources.size();++i) {
        const auto& r=_resources[i];
        const auto b=ReadBinding(_bindingBytes,i);
        if(r.kind==5) {
            if(program.RequiresBinding(1,b.slot)) {
                auto* view=GetSamplingView(device,*r.texture->texture,b.sampling);
                if(!view) return false;
                groups[1].push_back({b.slot,0,view});
            }
            if(program.HasSampler(b.slot+1u)) {
                auto* sampler=GetTextureSampler(device,b.format,b.sampling);
                if(!sampler) return false;
                groups[1].push_back({b.slot+1u,0,sampler});
            }
            continue;
        }
        if(r.kind==3) {
            if(program.RequiresBinding(0,b.slot)) {
                auto slice=ctx.ResolveBuffer(r.bufferHandle);
                slice.Offset+=b.offset; slice.Size=b.size;
                groups[0].push_back(FrameConstantBinding(b.slot,slice));
            }
            continue;
        }
        if(r.kind==4) {
            if(program.RequiresBinding(1,b.slot)) {
                auto slice=ctx.ResolveBuffer(r.bufferHandle);
                slice.Offset+=b.offset; slice.Size=b.size;
                groups[1].push_back({b.slot,0,ShaderTexelBufferBinding{slice.Buffer.Get(),{slice.Offset,slice.Size},ToTextureFormat(rn_pixel_format(b.format))}});
            }
            continue;
        }
        auto slice=ctx.ResolveBuffer(r.bufferHandle);
        slice.Offset+=b.offset; slice.Size=b.size;
        switch(r.kind) {
        case 0: _vertices[vertex++]={b.slot,FrameVertexView(slice)}; break;
        case 1:
            if(slice.Offset>UINT32_MAX) return false;
            _indices={slice.Buffer.Get(),uint32_t(slice.Offset),b.format}; break;
        case 2: _indirect=slice; break;
        }
    }
    if(_drawBases.IsValid()) {
        auto slice=ctx.ResolveBuffer(_drawBases);
        uint32_t count=(_kind==2 || _kind==3)?_count:1;
        slice.Size=256;
        _drawBaseSets=FrameArray<ShaderParameterSet*>(slot.GetMemoryResource(),count);
        for(uint32_t i=0;i<count;++i,slice.Offset+=256) {
            auto binding=FrameConstantBinding(0,slice);
            auto set=slot.AcquireParameterSet(program.drawBasesLayout,{&binding,1});
            if(!set) return false;
            _drawBaseSets[i]=set.Get();
        }
    }
    if(_fan.IsValid()) _indices=FrameIndexView(ctx.ResolveBuffer(_fan));
    if(((_kind==1 || _kind==3 || _kind==4) && !_indices.Target) || ((_kind==2 || _kind==3) && !_indirect.Buffer)) return false;
    for(uint32_t i=0;i<2;++i) if(program.groups[i]) {
        auto& group=groups[i];
        EnsureOrderedBindings(group);
        _sets[i]=slot.AcquireParameterSet(program.groups[i],{group.data(),group.size()}).Get();
        if(!_sets[i]) return false;
    }
    return true;
}
bool ImmediateDraw::Record(GraphicsCommandEncoder& enc,ImmediatePassStateCache& cache) const noexcept {
    cache.BindPipeline(enc,_pso);
    cache.SetScissor(enc,_scissor);
    cache.SetStencil(enc,0);
    for(uint32_t i=0;i<2;++i) if(_sets[i]) cache.BindParameterSet(enc,i,_sets[i]);
    if(!_push.empty() && !enc.SetPushConstants(0,0,{_push.data(),_push.size()})) return false;
    if(!_vertices.empty()) enc.BindVertexBuffers({_vertices.data(),_vertices.size()});
    if(_indices.Target) cache.BindIndexBuffer(enc,_indices);
    if(!_drawBaseSets.empty()) {
        if(_kind==2 || _kind==3) {
            for(uint32_t i=0;i<_count;++i) {
                enc.BindShaderParameterSet(2,_drawBaseSets[i]);
                if(_kind==2) enc.DrawIndirect(_indirect.Buffer.Get(),_indirect.Offset+uint64_t(i)*16,1);
                else enc.DrawIndexedIndirect(_indirect.Buffer.Get(),_indirect.Offset+uint64_t(i)*20,1);
            }
            return enc.IsValid();
        }
        enc.BindShaderParameterSet(2,_drawBaseSets.front());
    }
    switch(_kind) {
    case 0: enc.Draw(_count,_instances,_first,_firstInstance); break;
    case 1: enc.DrawIndexed(_count,_instances,_first,_base,_firstInstance); break;
    case 2: enc.DrawIndirect(_indirect.Buffer.Get(),_indirect.Offset,_count); break;
    case 3: enc.DrawIndexedIndirect(_indirect.Buffer.Get(),_indirect.Offset,_count); break;
    case 4: enc.DrawIndexed(3*(_count-2),_instances,0,_base,_firstInstance); break;
    }
    return enc.IsValid();
}
}
