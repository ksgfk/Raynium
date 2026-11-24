#include <raynium/terrain_residency.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <Eigen/Core>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace raynium {
using namespace radray::gpu;
namespace {
template<class T> bool Array(const T* p,uint32_t n,uint32_t limit=1048576) { return n<=limit && (!n || p); }

radray::MeshDrawCommand BuildFastTerrainCommand(
    radray::MeshDrawCommandBindingStorage& bindingsStorage,
    GraphicsPipelineState* pipeline,
    const TerrainMesh& repMesh,
    const radray::IndexBufferSubAllocation& indexAlloc) {
    const auto indexView=indexAlloc.GetIndexBufferView();
    const auto& range0=repMesh.layout.ranges[0];
    radray::MeshDrawCommand cmd{};
    cmd.Pipeline=pipeline;
    cmd.IndexBuffer=indexView;
    cmd.IndexCount=range0.count;
    cmd.FirstIndex=indexAlloc.GetFirstIndex()+range0.first;
    cmd.VertexOffset=int32_t(repMesh.geometry.GetFirstVertex());
    cmd.ValidElementMask=1u;
    cmd.StencilReference=0u;
    const auto& vi=repMesh.geometry.GetVertexInput();
    radray::MeshDrawCommandBindings bindings{};
    for(const auto& stream:vi.Streams) {
        bindings.VertexBuffers.push_back({stream.Binding,{stream.Buffer.Get(),stream.Offset,stream.Size}});
    }
    for(const auto& layout:vi.Layouts) {
        bindings.VertexStreamLayouts.push_back({layout.Binding,layout.ArrayStride,layout.StepMode});
        if(layout.StepMode==VertexStepMode::Instance) {
            bindings.InstanceStreamBinding=layout.Binding;
        }
    }
    bindingsStorage.Store(cmd,bindings);
    return cmd;
}
const TerrainWireframe* WireIndices(TerrainMesh& mesh,uint32_t mask,radray::IndexBufferPool& pool,
    radray::vector<GeometryUpload>& uploads,uint64_t& uploadBytes) {
    for(auto& wire:mesh.wireframes) if(wire.mask==mask) return &wire;
    uint32_t count=0;
    for(size_t i=0;i<mesh.layout.ranges.size();++i) if(mask&(1u<<i)) count+=mesh.layout.ranges[i].count;
    auto alloc=pool.Allocate(count);
    if(!alloc) { RADRAY_ERR_LOG("Cannot allocate terrain wireframe indices"); return nullptr; }
    radray::vector<radray::byte> bytes(alloc->GetByteSize());
    if(!WriteWireframeIndices(bytes,mesh.layout,mask)) return nullptr;
    uploadBytes+=bytes.size();
    uploads.push_back({alloc->GetGpuBuffer(),alloc->GetByteOffset(),radray::RDGUploadSource::OwnedBytes(std::move(bytes))});
    return &mesh.wireframes.emplace_back(TerrainWireframe{mask,std::move(*alloc)});
}
}  // namespace

void TerrainEyeCrossingCache::IncludePlane(uint32_t axis,double delta) noexcept {
    // Keep a conservative margin when camera and mesh origins have very different magnitudes.
    constexpr double margin=1e-5;
    if(delta<=margin) lower[axis]=std::max(lower[axis],std::min(0.0,delta+margin));
    if(delta>=-margin) upper[axis]=std::min(upper[axis],std::max(0.0,delta-margin));
}

namespace {
struct FaceMask { uint32_t bits; bool mayCross; };

template<bool TrackPlanes>
FaceMask EvaluateFaces(const TerrainHotSlot& hot,const radray::array<int32_t,3>& block,
    const radray::array<double,3>& eye,bool valid,std::span<const rn_instance> instances,
    uint32_t requested,TerrainEyeCrossingCache* cache) noexcept {
    if(hot.layer==RN_MATERIAL_TRANSLUCENT || !(hot.flags&kTerrainHotFlagFiniteBounds) || !valid)
        return {requested,false};
    FaceMask result{hot.groupBits[6]&requested,false};
    if(result.bits==requested) return result;
    for(const auto& instance:instances) {
        for(uint32_t a=0;a<3;++a) {
            const uint32_t positive=hot.groupBits[2*a]&requested, negative=hot.groupBits[2*a+1]&requested;
            if(!(positive|negative)) continue;
            const double position=double(int64_t(block[a])-instance.origin[a])+eye[a];
            if(!std::isfinite(position)) return {requested,false};
            // For bounds inside a section, neither plane can cross the one-block eye box at this distance.
            if(hot.sectionBoundsAxes&(1u<<a)) {
                if(position < -2) { result.bits|=negative; continue; }
                if(position > 18) { result.bits|=positive; continue; }
            }
            if(positive) {
                const double plane=(double(hot.minBound[a])-0.001)-position;
                if(plane<=0) result.bits|=positive;
                if constexpr(TrackPlanes) {
                    if(std::abs(plane)<=1.00001) { cache->IncludePlane(a,plane); result.mayCross=true; }
                }
            }
            if(negative) {
                const double plane=(double(hot.maxBound[a])+0.001)-position;
                if(plane>=0) result.bits|=negative;
                if constexpr(TrackPlanes) {
                    if(std::abs(plane)<=1.00001) { cache->IncludePlane(a,plane); result.mayCross=true; }
                }
            }
        }
        if(result.bits==requested) break;
    }
    return result;
}
}

uint32_t ComputeHotSlotVisibleMask(const TerrainHotSlot& hot,const radray::array<int32_t,3>& block,
    const radray::array<double,3>& eye,bool valid,std::span<const rn_instance> instances,uint32_t requested) noexcept {
    return EvaluateFaces<false>(hot,block,eye,valid,instances,hot.fullMask&requested,nullptr).bits;
}

uint32_t TerrainEyeCrossingCache::Capture(const rn_visible_item& item,const TerrainHotSlot& hot,std::span<const rn_instance> instances) {
    const auto requested=uint8_t(item.element_mask & hot.validMask);
    const auto result=EvaluateFaces<true>(hot,baseBlock,center,eyeValid,instances,requested,this);
    if(result.mayCross) crossingItems.push_back({uint32_t(item.mesh),item.first_instance,item.instance_count,requested,uint8_t(result.bits)});
    return result.bits;
}

bool ProjectedTerrainBounds::Disjoint(const ProjectedTerrainBounds& other) const noexcept {
    return valid && other.valid && (right<other.left || other.right<left || top<other.bottom || other.top<bottom);
}

ProjectedTerrainBounds ProjectTerrainBounds(const TerrainHotSlot& hot,const rn_instance& instance,const rn_terrain_view_desc& frame) noexcept {
    if(!(hot.flags&kTerrainHotFlagFiniteBounds)) return {};
    const auto view=Eigen::Map<const Eigen::Matrix4f>(frame.view);
    const auto projection=Eigen::Map<const Eigen::Matrix4f>(frame.projection);
    const float inf=std::numeric_limits<float>::infinity();
    ProjectedTerrainBounds bounds{inf,-inf,inf,-inf,true};
    for(unsigned corner=0;corner<8;++corner) {
        Eigen::Vector4f position;
        for(unsigned axis=0;axis<3;++axis) {
            const int64_t delta=int64_t(instance.origin[axis])-frame.camera_block[axis];
            if(delta<INT32_MIN || delta>INT32_MAX) return {};
            position[axis]=((corner&(1u<<axis)?hot.maxBound[axis]:hot.minBound[axis])+float(delta))-frame.camera_fraction[axis];
        }
        position.w()=1;
        const Eigen::Vector4f clip=projection*(view*position);
        if(!clip.allFinite() || clip.w()<=0.001f) return {};
        const float x=clip.x()/clip.w(),y=clip.y()/clip.w();
        if(!std::isfinite(x) || !std::isfinite(y)) return {};
        constexpr float margin=0.001f;
        bounds.left=std::min(bounds.left,x-margin); bounds.right=std::max(bounds.right,x+margin);
        bounds.bottom=std::min(bounds.bottom,y-margin); bounds.top=std::max(bounds.top,y+margin);
    }
    return bounds;
}

bool TerrainResidency::GroupDisjointClassicItems(std::span<const rn_instance> instances,const rn_terrain_view_desc& frame) {
    auto& items=_fastClassicTranslucent;
    if(items.size()<3 || (_translucentVertices->GetPageCount()<2 && _indices->GetPageCount()<2)) return false;
    const auto samePage=[](const auto& a,const auto& b) { return a.vertexPageId==b.vertexPageId && a.indexPageId==b.indexPageId; };
    _classicBounds.clear();
    for(const auto& item:items) {
        _classicBounds.push_back(item.arg.InstanceCount==1?
            ProjectTerrainBounds(_hotSlots[item.slotId],instances[item.arg.FirstInstance],frame):ProjectedTerrainBounds{});
    }
    bool reordered=false;
    for(size_t i=1;i+1<items.size();++i) {
        if(samePage(items[i-1],items[i])) continue;
        for(size_t j=i+1;j<std::min(items.size(),i+8);++j) {
            if(!samePage(items[i-1],items[j])) continue;
            if(!std::all_of(_classicBounds.begin()+i,_classicBounds.begin()+j,
                [&](const auto& crossed) { return _classicBounds[j].Disjoint(crossed); })) break;
            std::rotate(items.begin()+i,items.begin()+j,items.begin()+j+1);
            std::rotate(_classicBounds.begin()+i,_classicBounds.begin()+j,_classicBounds.begin()+j+1);
            reordered=true;
            break;
        }
    }
    return reordered;
}

void TerrainEyeCrossingCache::Reset(const radray::array<int32_t,3>& block,const radray::array<double,3>& eye,bool valid) noexcept {
    baseBlock=block;
    center=eye;
    eyeValid=valid;
    crossingItems.clear();
    lower.fill(-1);
    upper.fill(1);
    lastDelta={};
}

bool TerrainEyeCrossingCache::Matches(const radray::array<int32_t,3>& block,const radray::array<double,3>& eye,
    bool valid,std::span<const TerrainHotSlot> slots,std::span<const rn_instance> instances) noexcept {
    if(valid!=eyeValid) return false;
    if(!valid || (block==baseBlock && eye==center)) return true;
    radray::array<double,3> delta;
    bool insidePlanes=true;
    for(size_t a=0;a<3;++a) {
        delta[a]=double(int64_t(block[a])-baseBlock[a])+eye[a]-center[a];
        if(!(std::abs(delta[a])<1.0)) return false;
        if(delta[a]!=0 && !(delta[a]>lower[a] && delta[a]<upper[a])) insidePlanes=false;
    }
    if(insidePlanes || lastDelta==delta) return true;
    for(const auto& item:crossingItems) {
        const auto mask=ComputeHotSlotVisibleMask(slots[item.mesh],block,eye,true,
            instances.subspan(item.firstInstance,item.instanceCount),item.requestedMask);
        if(mask!=item.effectiveMask) return false;
    }
    lastDelta=delta;
    return true;
}

void EmitHotSlotSolidIndirectArgs(const TerrainHotSlot& hot,const TerrainMeshLayout* wideFallbackLayout,
    uint32_t quadBaseFirstIndex,uint32_t mask,uint32_t firstInstance,uint32_t instanceCount,
    radray::vector<DrawIndexedIndirectArguments>& outArgs) {
    mask&=hot.validMask;
    if(!mask || !instanceCount) return;
    if(instanceCount==1 && mask==hot.validMask) {
        outArgs.push_back({hot.indexCount,1,quadBaseFirstIndex,int32_t(hot.firstVertex),firstInstance});
        return;
    }
    const bool wide=(hot.flags&kTerrainHotFlagWideCounts)!=0;
    if(wide && !wideFallbackLayout) return;
    const auto endIndex=[&](uint32_t range) {
        if(wide) {
            const auto& r=wideFallbackLayout->ranges[range];
            return r.first+r.count;
        }
        return range==6 ? hot.indexCount : uint32_t(hot.rangeEndQuads[range])*6;
    };
    while(mask) {
        const uint32_t first=std::countr_zero(mask);
        const uint32_t count=instanceCount==1 ? std::countr_one(mask>>first) : 1;
        const uint32_t begin=first ? endIndex(first-1) : 0;
        outArgs.push_back({endIndex(first+count-1)-begin,instanceCount,quadBaseFirstIndex+begin,int32_t(hot.firstVertex),firstInstance});
        mask&=~(((1u<<count)-1)<<first);
    }
}

void TerrainLayerDrawData::Clear() noexcept {
    prepared.Clear();
    indirectReps.clear();
    fastCommands.clear();
    fastBindings.Clear();

    indirectSize=0;
    recordCount=0;
    selectedGroups=0;
    indicesInstances=0;
}

void TerrainViewDrawData::Clear() noexcept {
    for(auto& layer:layers) layer.Clear();
    for(auto& reps:oitIndirectReps) reps.clear();
    oit=false;
    wireframe=false;
    hasAnyDraws=false;
    totalIndirectArguments=0;
}

TerrainResidency::TerrainResidency(radray::DeviceContext& device,radray::unique_ptr<radray::VertexBufferPool> vertices,
    radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
    radray::unique_ptr<radray::IndexBufferPool> quadPool,radray::unique_ptr<radray::IndexBufferPool> indices)
    :_device(device),_vertices(std::move(vertices)),_translucentVertices(std::move(translucentVertices)),
     _quadPool(std::move(quadPool)),_indices(std::move(indices)) {
    _state.meshesBySlot.reserve(4096);
    _hotSlots.reserve(4096);
    _fastClassicTranslucent.reserve(1024);
}

void TerrainResidency::BeginSlotViews(std::span<const rn_terrain_view_desc> views,radray::FrameSlot& slot) {
    if(!views.empty()) {
        const bool wireframe=std::any_of(views.begin(),views.end(),[](const auto& view){ return (view.flags&RN_FRAME_WIREFRAME)!=0; });
        if(_state.wireframeActive && !wireframe) {
            ++_layerGeometryVersion[0];
            ++_layerGeometryVersion[1];
            for(auto& mesh:_state.meshesBySlot) {
                if(!mesh.active || mesh.wireframes.empty()) continue;
                for(auto& wire:mesh.wireframes) {
                    slot.Emplace<TerrainWireframe>(std::move(wire));
                }
                mesh.wireframes.clear();
            }
        }
        _state.wireframeActive=wireframe;
        _views.resize(views.size());
        for(auto& view:_views) if(!view) view=radray::make_unique<TerrainViewCacheSlot>();
    }
    _quadPool->TrimEmptyPages();
    // Retired allocations from two frames ago were released during FrameScheduler::BeginFrame.
    if(_vertices && (_vertices->GetPageCount()>1 || (_state.residentMeshCount==0 && _vertices->GetPageCount()>0))) {
        _vertices->TrimEmptyPages();
    }
    if(_translucentVertices && (_translucentVertices->GetPageCount()>1 || (_state.residentMeshCount==0 && _translucentVertices->GetPageCount()>0))) {
        _translucentVertices->TrimEmptyPages();
    }
    _indices->TrimEmptyPages();
}

bool SlotPromotedBuffer::Prepare(Device& device,bool unchanged,uint64_t bytes,BufferUses usage,
    radray::IntrusivePtr<radray::GpuBuffer>& output,bool& upload,bool hasDraws) {
    constexpr uint64_t minCapacity=4096;
    uint32_t nextStable=0;
    if(bytes && unchanged && bytes==lastBytes) {
        nextStable=stableFrames;
        if(hasDraws && nextStable<2) ++nextStable;
    }
    const bool persistent=hasDraws && nextStable==2;
    const uint64_t capacity=buffer ? buffer->GetDesc().Size : 0;
    const bool grow=persistent && bytes>capacity;
    const bool needUpload=persistent && (stableFrames<2 || grow);
    if(grow) {
        const uint64_t deviceLimit=device.GetCapabilities().Limits.MaxBufferSize;
        const uint64_t limit=deviceLimit ? deviceLimit : std::numeric_limits<uint64_t>::max();
        if(bytes>limit) {
            RADRAY_ERR_LOG("Terrain buffer of {} bytes exceeds device limit {}",bytes,limit);
            return false;
        }
        const uint64_t desired=std::max(bytes,minCapacity);
        const uint64_t newCapacity=desired>std::bit_floor(limit) ? limit : std::bit_ceil(desired);
        auto created=radray::GpuBuffer::Create(device,{.Size=newCapacity,.Usage=usage|BufferUse::CopyDestination});
        if(!created) { RADRAY_ERR_LOG("Cannot promote terrain buffer to {} bytes",newCapacity); return false; }
        buffer=created.Release();
    } else if(!bytes || (capacity>minCapacity && bytes<capacity/4)) {
        buffer={};
    }
    lastBytes=bytes;
    stableFrames=nextStable;
    output=persistent ? buffer : nullptr;
    upload=needUpload;
    return true;
}

bool TerrainResidency::EnsureQuadIndices(uint32_t vertexCount,radray::vector<GeometryUpload>& uploads,
    radray::FrameSlot* retireSlot,uint64_t* uploadBytesOut,bool* grewOut) {
    if(grewOut) *grewOut=false;
    if(vertexCount<=_quadVertexCapacity) return true;
    uint32_t capacity=65536;
    while(capacity<vertexCount) {
        if(capacity>UINT32_MAX/2) { RADRAY_ERR_LOG("Shared quad index capacity overflow"); return false; }
        capacity*=2;
    }
    auto allocation=_quadPool->Allocate(capacity/4*6);
    if(!allocation) { RADRAY_ERR_LOG("Cannot allocate shared quad indices"); return false; }
    auto& owner=*allocation;
    if(uploadBytesOut) *uploadBytesOut+=owner.GetByteSize();
    uploads.push_back({owner.GetGpuBuffer(),owner.GetByteOffset(),radray::RDGUploadSource::Generated(owner.GetByteSize(),capacity,
        +[](const uint32_t& vertices,radray::RDGUploadWriteContext& context) noexcept { return WriteQuadIndices(context.Bytes,vertices); })});
    if(_quadIndices && retireSlot) retireSlot->Emplace<radray::IndexBufferSubAllocation>(std::move(_quadIndices));
    _quadIndices=std::move(owner);
    _quadVertexCapacity=capacity;
    ++_layerGeometryVersion[0];
    ++_layerGeometryVersion[1];
    if(grewOut) *grewOut=true;
    return true;
}

bool TerrainResidency::ResyncQuadMeshes() {
    ++_layerGeometryVersion[0];
    ++_layerGeometryVersion[1];
    for(uint32_t slotId=1;slotId<_state.meshesBySlot.size();++slotId) {
        auto& mesh=_state.meshesBySlot[slotId];
        if(!mesh.active || mesh.layout.layer==RN_MATERIAL_TRANSLUCENT) continue;
        if(!ValidateAndSyncHotSlot(slotId)) return false;
    }
    return true;
}

bool TerrainResidency::ValidateAndSyncHotSlot(uint32_t slotId) {
    auto& mesh=_state.meshesBySlot[slotId];
    const auto& index=mesh.layout.layer==RN_MATERIAL_TRANSLUCENT?mesh.indices:_quadIndices;
    if(!index) { RADRAY_ERR_LOG("Terrain mesh references missing indices"); return false; }
    if(mesh.layout.ranges.empty() || mesh.layout.ranges.size()>=32) {
        RADRAY_ERR_LOG("Invalid terrain range layout"); return false;
    }
    const uint64_t vertex=mesh.geometry.GetFirstVertex();
    if(vertex>INT32_MAX) { RADRAY_ERR_LOG("Invalid terrain vertex offset"); return false; }
    if(mesh.vertexPageId>UINT16_MAX || index.GetPageIndex()>UINT16_MAX) {
        RADRAY_ERR_LOG("Terrain geometry pool page index overflow"); return false;
    }
    mesh.fullMask=(1u<<mesh.layout.ranges.size())-1;
    mesh.validMask=0;
    for(uint32_t n=0;n<mesh.layout.ranges.size();++n) {
        const auto& range=mesh.layout.ranges[n];
        const uint64_t first=uint64_t(index.GetFirstIndex())+range.first;
        if(uint64_t(range.first)+range.count>index.GetIndexCount() || first>UINT32_MAX) {
            RADRAY_ERR_LOG("Invalid terrain index range"); return false;
        }
        if(range.count) mesh.validMask|=1u<<n;
    }
    if(slotId>=_hotSlots.size()) _hotSlots.resize(size_t(slotId)+1);
    TerrainHotSlot hot{};
    for(uint32_t a=0;a<3;++a) {
        hot.minBound[a]=float(mesh.layout.minimum[a]);
        hot.maxBound[a]=float(mesh.layout.maximum[a]);
        if(hot.minBound[a]>=0 && hot.maxBound[a]<=16) hot.sectionBoundsAxes|=uint8_t(1u<<a);
    }
    hot.firstVertex=uint32_t(vertex);
    hot.vertexPageId=uint16_t(mesh.vertexPageId);
    hot.indexPageId=uint16_t(index.GetPageIndex());
    hot.layer=uint8_t(mesh.layout.layer);
    hot.fullMask=uint8_t(mesh.fullMask);
    hot.validMask=uint8_t(mesh.validMask);
    hot.flags=kTerrainHotFlagActive|kTerrainHotFlagHasIndices|(mesh.layout.finiteBounds?kTerrainHotFlagFiniteBounds:0u);
    hot.indexCount=mesh.layout.vertexCount/4*6;
    if(mesh.layout.layer==RN_MATERIAL_TRANSLUCENT) {
        hot.firstIndex=index.GetFirstIndex()+mesh.layout.ranges[0].first;
        hot.groupBits[6]=uint8_t(mesh.layout.groupBits[6]);
    } else {
        for(uint32_t g=0;g<7;++g) hot.groupBits[g]=uint8_t(mesh.layout.groupBits[g]);
        for(size_t r=0;r<std::min<size_t>(6,mesh.layout.ranges.size());++r) {
            const auto& range=mesh.layout.ranges[r];
            const uint32_t endQuads=(range.first+range.count)/6;
            if(endQuads>UINT16_MAX) hot.flags|=kTerrainHotFlagWideCounts;
            else hot.rangeEndQuads[r]=uint16_t(endQuads);
        }
    }
    _hotSlots[slotId]=hot;
    return true;
}

bool TerrainResidency::PrepareFrame(const TextureRegistry& uploaded,const rn_terrain_view_desc& f,
    const MeshPassResources& resources,SlotUploads slot,TerrainFrame& frame) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Prepare");

    constexpr uint32_t kAllowedFlags=RN_FRAME_RGSS|RN_FRAME_IMPROVED_TRANSPARENCY|RN_FRAME_WIREFRAME|
        RN_FRAME_REUSE_VISIBLE|RN_FRAME_REUSE_INSTANCES;
    if(f.struct_size!=sizeof(f) || (f.flags&~kAllowedFlags) || !f.epoch || f.epoch<_state.epoch ||
       !Array(f.indices,f.index_count,65536) || !Array(f.meshes,f.mesh_count,65536) ||
       !Array(f.removed_meshes,f.removed_mesh_count) || !Array(f.visible,f.visible_count) || !Array(f.instances,f.instance_count))
        { RADRAY_ERR_LOG("Invalid frame layout, epoch or array: sequence={} epoch={} resident_epoch={}",f.output_sequence,f.epoch,_state.epoch); return false; }
    const bool wireframe=(f.flags&RN_FRAME_WIREFRAME)!=0;
    const bool oit=(f.flags&RN_FRAME_IMPROVED_TRANSPARENCY)!=0;
    const bool reuseVisible=(f.flags&RN_FRAME_REUSE_VISIBLE)!=0;
    const bool reuseInstances=(f.flags&RN_FRAME_REUSE_INSTANCES)!=0;
    if(wireframe && !resources.SupportsWireframe()) { RADRAY_ERR_LOG("Terrain wireframe unsupported on this device"); return false; }
    if(reuseInstances && !reuseVisible) {
        RADRAY_ERR_LOG("RN_FRAME_REUSE_INSTANCES requires RN_FRAME_REUSE_VISIBLE"); return false;
    }
    auto& cache=slot.cache;
    auto& draws=cache.draws;
    const bool warm=cache.epoch==f.epoch;
    if(reuseVisible) {
        if(!warm || f.epoch!=_state.epoch ||
           f.visible_count || f.mesh_count || f.removed_mesh_count) {
            RADRAY_ERR_LOG("Invalid RN_FRAME_REUSE_VISIBLE usage: cached={} epoch={} vis={} meshes={} removed={}",
                warm,f.epoch,f.visible_count,f.mesh_count,f.removed_mesh_count);
            return false;
        }
        if(reuseInstances && (!warm || f.instance_count)) {
            RADRAY_ERR_LOG("Invalid RN_FRAME_REUSE_INSTANCES usage: cached={} instances={}",warm,f.instance_count);
            return false;
        }
    }
    const bool hasVisibleItems=reuseVisible?!cache.visible.empty():(f.visible_count!=0);
    if(hasVisibleItems) {
        if(f.terrain_anisotropy>16 || f.terrain_albedo_texture<16 || f.terrain_light_texture<16 ||
           !uploaded.Find(f.terrain_albedo_texture) || !uploaded.Find(f.terrain_light_texture))
            { RADRAY_ERR_LOG("Invalid terrain texture binding: albedo={} light={} anisotropy={}",f.terrain_albedo_texture,f.terrain_light_texture,f.terrain_anisotropy); return false; }
    }

    auto& state=_state;
    frame={};
    frame.parameters=f; frame.draws=&draws; frame.geometry=&slot.geometry;
    auto& scalar=frame.parameters;
    scalar.indices=nullptr; scalar.meshes=nullptr; scalar.removed_meshes=nullptr; scalar.visible=nullptr; scalar.instances=nullptr;

    if(f.epoch!=state.epoch) {
        for(auto& owner:state.meshesBySlot) {
            if(owner.active) {
                owner.active=false;
                slot.frame.Emplace<TerrainMesh>(std::move(owner));
            }
        }
        state.meshesBySlot.clear();
        _hotSlots.clear();
        state.residentMeshCount=0;
        for(auto& v:_layerGeometryVersion) ++v;
    }
    state.epoch=f.epoch;


    uint64_t uploadBytes=0;
    if(f.removed_mesh_count) {
        for(uint32_t i=0;i<f.removed_mesh_count;++i) {
            const uint64_t rawId=f.removed_meshes[i];
            if(!rawId || rawId>=state.meshesBySlot.size()) continue;
            auto& owner=state.meshesBySlot[size_t(rawId)];
            if(!owner.active) continue;
            ++_layerGeometryVersion[owner.layout.layer];
            ++_partitionVersion;
            if(rawId<_hotSlots.size()) _hotSlots[size_t(rawId)]={};
            owner.active=false;
            --state.residentMeshCount;
            slot.frame.Emplace<TerrainMesh>(std::move(owner));
            owner={};
        }
    }

    uint32_t needed=_quadVertexCapacity;
    uint32_t maxMeshSlot=0;
    for(const auto& x:std::span{f.meshes,f.mesh_count}) {
        if(!x.id || x.id>kMaxMeshSlot || !x.vertices || !x.vertex_count || x.vertex_count%4 || x.vertex_count>1048576 || x.layer>RN_MATERIAL_TRANSLUCENT)
            { RADRAY_ERR_LOG("Invalid terrain mesh: id={} vertices={} layer={}",x.id,x.vertex_count,uint32_t(x.layer)); return false; }
        maxMeshSlot=std::max(maxMeshSlot,uint32_t(x.id));
        if(x.layer!=RN_MATERIAL_TRANSLUCENT) needed=std::max(needed,x.vertex_count);
    }
    if(maxMeshSlot>=state.meshesBySlot.size()) {
        state.meshesBySlot.resize(size_t(maxMeshSlot)+1);
    }
    if(maxMeshSlot>=_hotSlots.size()) {
        _hotSlots.resize(size_t(maxMeshSlot)+1);
    }
    bool quadGrew=false;
    if(!EnsureQuadIndices(needed,slot.geometry,&slot.frame,&uploadBytes,&quadGrew)) return false;

    if(f.mesh_count || f.index_count || quadGrew) {
        if(f.mesh_count) ++_partitionVersion;
        const uint64_t syncStamp=++_syncStamp;
        _changedSlotsScratch.clear();
        const auto markChanged=[&](uint32_t slotId) {
            auto& m=state.meshesBySlot[slotId];
            if(m.dirtyStamp!=syncStamp) {
                m.dirtyStamp=syncStamp;
                _changedSlotsScratch.push_back(slotId);
            }
        };
        if(quadGrew) {
            for(uint32_t slotId=1;slotId<state.meshesBySlot.size();++slotId) {
                const auto& mesh=state.meshesBySlot[slotId];
                if(mesh.active && mesh.layout.layer!=RN_MATERIAL_TRANSLUCENT) markChanged(slotId);
            }
        }

        for(const auto& x:std::span{f.meshes,f.mesh_count}) {
            TerrainMesh mesh;
            mesh.active=true;
            const std::span<const radray::byte> vertexBytes{static_cast<const radray::byte*>(x.vertices),size_t(x.vertex_count)*28};
            auto& pool=x.layer==RN_MATERIAL_TRANSLUCENT?*_translucentVertices:*_vertices;
            if(x.layout) {
                if(!BuildPreanalyzedTerrainMeshLayout(*x.layout,x.vertex_count,x.layer,mesh.layout)) return false;
                auto allocation=pool.Allocate(x.vertex_count);
                if(!allocation) { RADRAY_ERR_LOG("Cannot allocate terrain vertices"); return false; }
                mesh.geometry=std::move(*allocation);
                mesh.vertexPageId=mesh.geometry.GetPageIndex();
                uploadBytes+=vertexBytes.size();
                slot.geometry.push_back({mesh.geometry.GetGpuBuffer(),mesh.geometry.GetByteOffset(),
                    radray::RDGUploadSource::BorrowedBytes(vertexBytes)});
            } else {
                auto* upload=slot.frame.Emplace<TerrainMeshUpload>(slot.frame.GetMemoryResource());
                if(!AnalyzeTerrainMesh(vertexBytes,x.layer,mesh.layout,*upload)) return false;
                auto allocation=pool.Allocate(x.vertex_count);
                if(!allocation) { RADRAY_ERR_LOG("Cannot allocate terrain vertices"); return false; }
                mesh.geometry=std::move(*allocation);
                mesh.vertexPageId=mesh.geometry.GetPageIndex();
                uploadBytes+=upload->source.size();
                slot.geometry.push_back({mesh.geometry.GetGpuBuffer(),mesh.geometry.GetByteOffset(),radray::RDGUploadSource::Generated(upload->source.size(),upload,
                    +[](TerrainMeshUpload* const& source,radray::RDGUploadWriteContext& context) noexcept { return source->Write(context.Bytes); })});
            }
            const uint32_t slotId=uint32_t(x.id);
            auto& previous=state.meshesBySlot[slotId];
            if(previous.active) {
                ++_layerGeometryVersion[previous.layout.layer];
                for(auto& wire:previous.wireframes) {
                    slot.frame.Emplace<TerrainWireframe>(std::move(wire));
                }
                previous.wireframes.clear();
                if(x.layer==RN_MATERIAL_TRANSLUCENT && previous.layout.layer==x.layer) mesh.indices=std::move(previous.indices);
                previous.active=false;
                slot.frame.Emplace<TerrainMesh>(std::move(previous));
            } else {
                ++state.residentMeshCount;
            }
            ++_layerGeometryVersion[x.layer];
            previous=std::move(mesh);
            markChanged(slotId);
        }

        for(const auto& x:std::span{f.indices,f.index_count}) {
            if(!x.mesh_id || x.mesh_id>=state.meshesBySlot.size()) {
                RADRAY_ERR_LOG("Invalid transparent index update: mesh={} count={}",x.mesh_id,x.count); return false;
            }
            auto& mesh=state.meshesBySlot[size_t(x.mesh_id)];
            if(!mesh.active || mesh.layout.layer!=RN_MATERIAL_TRANSLUCENT || x.reserved ||
               !Array(x.indices,x.count,16u<<20) || x.count!=mesh.layout.vertexCount/4*6)
                { RADRAY_ERR_LOG("Invalid transparent index update: mesh={} count={}",x.mesh_id,x.count); return false; }
            auto allocation=_indices->Allocate(x.count);
            if(!allocation) { RADRAY_ERR_LOG("Cannot allocate terrain indices"); return false; }
            auto& owner=*allocation;
            uploadBytes+=owner.GetByteSize();
            slot.geometry.push_back({owner.GetGpuBuffer(),owner.GetByteOffset(),
                radray::RDGUploadSource::BorrowedBytes(std::as_bytes(std::span{x.indices,x.count}))});
            if(mesh.indices) slot.frame.Emplace<radray::IndexBufferSubAllocation>(std::move(mesh.indices));
            mesh.indices=std::move(owner);
            ++_layerGeometryVersion[RN_MATERIAL_TRANSLUCENT];
            markChanged(uint32_t(x.mesh_id));
        }

        for(uint32_t slotId:_changedSlotsScratch) {
            if(!ValidateAndSyncHotSlot(slotId)) return false;
        }
    }

    std::span<const rn_visible_item> activeVisible;
    std::span<const rn_instance> activeInstances;
    bool visibleIdentical=false;
    bool instancesIdentical=false;
    bool originsIdentical=false;
    {
        if(reuseVisible) {
            visibleIdentical=true;
        } else {
            visibleIdentical=warm && f.visible_count==cache.visible.size() &&
                (f.visible_count==0 || std::memcmp(f.visible,cache.visible.data(),size_t(f.visible_count)*sizeof(rn_visible_item))==0);
            if(!visibleIdentical) {
                cache.visible.assign(f.visible,f.visible+f.visible_count);
                cache.partitionVersion=0;
            }
        }
        activeVisible=cache.visible;

        if(reuseInstances) {
            instancesIdentical=true;
            originsIdentical=true;
        } else {
            instancesIdentical=warm && f.instance_count==cache.instances.size() &&
                (f.instance_count==0 || std::memcmp(f.instances,cache.instances.data(),size_t(f.instance_count)*sizeof(rn_instance))==0);
            if(instancesIdentical) {
                originsIdentical=true;
            } else {
                if(visibleIdentical && warm && f.instance_count==cache.instances.size()) {
                    originsIdentical=true;
                    for(uint32_t i=0;i<f.instance_count;++i) {
                        const auto& a=f.instances[i].origin;
                        const auto& b=cache.instances[i].origin;
                        if(a[0]!=b[0] || a[1]!=b[1] || a[2]!=b[2]) { originsIdentical=false; break; }
                    }
                }
                cache.instances.assign(f.instances,f.instances+f.instance_count);
            }
        }
        activeInstances=cache.instances;
    }

    frame.instances=activeInstances;
    radray::array<size_t,3> visibleCounts{};
    uint64_t totalBuiltBatches=0;
    uint64_t totalBuiltRecords=0;
    uint64_t uploadedIndirectBytes=0;

    {
        const auto eye=ComputeTerrainEye(f);
        const radray::array<int32_t,3> camBlock{f.camera_block[0],f.camera_block[1],f.camera_block[2]};
        const radray::array<double,3> fracEye{
            double(f.camera_fraction[0])+eye.offset[0],
            double(f.camera_fraction[1])+eye.offset[1],
            double(f.camera_fraction[2])+eye.offset[2]
        };
        const bool fracEyeFinite=eye.valid && std::isfinite(fracEye[0]) && std::isfinite(fracEye[1]) && std::isfinite(fracEye[2]);

        bool classicReordered=false;
        const bool canReuseCache=warm && draws.oit==oit && draws.wireframe==wireframe && visibleIdentical && cache.partitionVersion==_partitionVersion;
        if(visibleIdentical && cache.partitionVersion==_partitionVersion && cache.maxInstanceEnd>activeInstances.size()) {
            RADRAY_ERR_LOG("Cached visible instance range exceeds current instance count");
            return false;
        }
        radray::array<bool,3> layerHit{false,false,false};
        if(canReuseCache) {
            for(uint32_t layer=0;layer<2;++layer) {
                layerHit[layer]=originsIdentical && cache.layers[layer].valid &&
                    cache.layers[layer].geometryVersion==_layerGeometryVersion[layer] &&
                    cache.layers[layer].crossing.Matches(camBlock,fracEye,fracEyeFinite,_hotSlots,activeInstances);
            }
            layerHit[2]=cache.layers[2].valid && cache.layers[2].geometryVersion==_layerGeometryVersion[2];
        }

        if(!warm || draws.oit!=oit || draws.wireframe!=wireframe) {
            draws.Clear();
            layerHit={false,false,false};
        } else {
            for(uint32_t layer=0;layer<3;++layer) {
                if(layerHit[layer]) continue;
                draws.layers[layer].Clear();
                if(layer==RN_MATERIAL_TRANSLUCENT) {
                    for(auto& reps:draws.oitIndirectReps) reps.clear();
                }
            }
        }
        draws.oit=oit;
        draws.wireframe=wireframe;

        for(uint32_t layer=0;layer<3;++layer) {
            if(layerHit[layer]) visibleCounts[layer]=cache.layers[layer].visibleCount;
        }

        const bool anyDirty=!layerHit[0] || !layerHit[1] || !layerHit[2];
        if(anyDirty) {
            for(uint32_t layer=0;layer<2;++layer) {
                if(layerHit[layer]) continue;
                auto& crossing=cache.layers[layer].crossing;
                crossing.Reset(camBlock,fracEye,fracEyeFinite);
                if(wireframe) _wirePages[layer].clear();
                for(auto& bucket:_fastPageBuckets[layer]) {
                    bucket.args.clear();
                    bucket.repSlotId=0;
                    bucket.hasNonZeroFirstInstance=false;
                }
            }
            if(!layerHit[2]) {
                if(oit) {
                    for(auto& bucket:_fastOitBuckets) bucket.args.clear();
                    _fastOitBuckets.clear();
                    _oitPageLookup.clear();
                } else {
                    _fastClassicTranslucent.clear();
                }
            }

            const uint32_t quadBaseFirstIndex=_quadIndices?_quadIndices.GetFirstIndex():0u;
            const auto* hotData=_hotSlots.data();
            const size_t hotCount=_hotSlots.size();
            const auto* meshesData=state.meshesBySlot.data();
            const auto* instancesData=activeInstances.data();
            const size_t instanceCountTotal=activeInstances.size();
            size_t lastOitBucket=0;

            bool preparedItems=true;
            const auto processItem=[&](const rn_visible_item& item,const TerrainHotSlot& hot,uint32_t layer) {
                const uint32_t requested=item.element_mask==UINT32_MAX?uint32_t(hot.fullMask):item.element_mask;
                if(layer<2u) {
                    const uint32_t mask=cache.layers[layer].crossing.Capture(item,hot,{instancesData+item.first_instance,item.instance_count});
                    if(!mask) return;
                    draws.layers[layer].selectedGroups+=std::popcount(mask);
                    ++visibleCounts[layer];
                    uint32_t page=hot.vertexPageId;
                    auto& mesh=state.meshesBySlot[item.mesh];
                    const TerrainWireframe* wire=nullptr;
                    if(wireframe) {
                        if(!mesh.layout.originalQuadGroups.empty()) {
                            wire=WireIndices(mesh,requested,*_indices,slot.geometry,uploadBytes);
                            if(!wire) { preparedItems=false; return; }
                        }
                        const uint64_t indexPage=wire?uint64_t(wire->indices.GetPageIndex())+1:0;
                        auto& pages=_wirePages[layer];
                        page=pages.try_emplace((indexPage<<16)|hot.vertexPageId,uint32_t(pages.size())).first->second;
                    }
                    auto& buckets=_fastPageBuckets[layer];
                    if(page>=buckets.size()) buckets.resize(size_t(page)+1);
                    auto& bucket=buckets[page];
                    if(bucket.args.empty()) {
                        bucket.repSlotId=uint32_t(item.mesh);
                        bucket.wireMask=wire?requested:0;
                    }
                    bucket.hasNonZeroFirstInstance|=(item.first_instance!=0u);
                    if(wire) {
                        bucket.args.push_back({wire->indices.GetIndexCount(),item.instance_count,wire->indices.GetFirstIndex(),
                            int32_t(hot.firstVertex),item.first_instance});
                        return;
                    }
                    const TerrainMeshLayout* wideLayout=(hot.flags&kTerrainHotFlagWideCounts)?&meshesData[size_t(item.mesh)].layout:nullptr;
                    EmitHotSlotSolidIndirectArgs(hot,wideLayout,quadBaseFirstIndex,mask,item.first_instance,item.instance_count,bucket.args);
                } else {
                    const uint32_t mask=requested&uint32_t(hot.validMask);
                    if(!mask) return;
                    draws.layers[2].selectedGroups+=std::popcount(mask);
                    ++visibleCounts[2];
                    const DrawIndexedIndirectArguments arg{
                        hot.indexCount,item.instance_count,hot.firstIndex,
                        int32_t(hot.firstVertex),item.first_instance};
                    if(oit) {
                        FastOitBucket* target=nullptr;
                        if(lastOitBucket<_fastOitBuckets.size() &&
                           _fastOitBuckets[lastOitBucket].vertexPageId==hot.vertexPageId &&
                           _fastOitBuckets[lastOitBucket].indexPageId==hot.indexPageId) {
                            target=&_fastOitBuckets[lastOitBucket];
                        } else {
                            const uint32_t key=(uint32_t(hot.vertexPageId)<<16)|hot.indexPageId;
                            const auto [it,inserted]=_oitPageLookup.try_emplace(key,_fastOitBuckets.size());
                            lastOitBucket=it->second;
                            if(inserted) {
                                _fastOitBuckets.push_back({hot.vertexPageId,hot.indexPageId,uint32_t(item.mesh),false,{}});
                                _fastOitBuckets.back().args.reserve(256);
                            }
                            target=&_fastOitBuckets[lastOitBucket];
                        }
                        target->hasNonZeroFirstInstance|=(item.first_instance!=0u);
                        target->args.push_back(arg);
                    } else {
                        const uint64_t sortKey=(uint64_t(item.sort_key)<<32)|uint64_t(hot.vertexPageId);
                        _fastClassicTranslucent.push_back({sortKey,item.sort_key,uint32_t(item.mesh),hot.vertexPageId,hot.indexPageId,arg});
                    }
                }
            };

            if(cache.partitionVersion!=_partitionVersion) {
                {
                    for(auto& list:cache.layerItems) list.clear();
                    cache.maxInstanceEnd=0;
                }
                for(size_t i=0;i<activeVisible.size();++i) {
                    const auto& item=activeVisible[i];
                    if(!item.mesh || item.mesh>=hotCount || !item.instance_count ||
                       uint64_t(item.first_instance)+item.instance_count>instanceCountTotal) {
                        RADRAY_ERR_LOG("Invalid visible mesh or instance range: mesh={} first={} count={}",item.mesh,item.first_instance,item.instance_count);
                        return false;
                    }
                    const auto& hot=hotData[size_t(item.mesh)];
                    if(!(hot.flags&kTerrainHotFlagActive) || !(hot.flags&kTerrainHotFlagHasIndices) ||
                       !item.element_mask || (item.element_mask!=UINT32_MAX && (item.element_mask&~uint32_t(hot.fullMask)))) {
                        RADRAY_ERR_LOG("Invalid visible terrain submesh: mesh={} mask={}",item.mesh,item.element_mask);
                        return false;
                    }
                    {
                        cache.maxInstanceEnd=std::max(cache.maxInstanceEnd,uint64_t(item.first_instance)+item.instance_count);
                        cache.layerItems[hot.layer].push_back(uint32_t(i));
                    }
                    if(layerHit[hot.layer]) continue;
                    processItem(item,hot,hot.layer);
                }
                cache.partitionVersion=_partitionVersion;
            } else {
                for(uint32_t layer=0;layer<3;++layer) {
                    if(layerHit[layer]) continue;
                    for(uint32_t idx:cache.layerItems[layer]) {
                        const auto& item=activeVisible[idx];
                        processItem(item,hotData[size_t(item.mesh)],layer);
                    }
                }
            }

            if(!preparedItems) return false;
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.BuildArguments");
                for(uint32_t layer=0;layer<2;++layer) {
                    if(layerHit[layer]) continue;
                    auto& layerData=draws.layers[layer];
                    auto& prepared=layerData.prepared;
                    size_t totalArgs=0;
                    size_t activeBuckets=0;
                    for(const auto& bucket:_fastPageBuckets[layer]) {
                        if(!bucket.args.empty()) {
                            totalArgs+=bucket.args.size();
                            ++activeBuckets;
                        }
                    }
                    if(totalArgs>UINT32_MAX) { RADRAY_ERR_LOG("Terrain indirect argument count overflow"); return false; }
                    if(totalArgs) {
                        prepared.IndexedArguments.reserve(totalArgs);
                        prepared.Batches.reserve(activeBuckets);
                        layerData.fastCommands.reserve(activeBuckets);
                        for(const auto& bucket:_fastPageBuckets[layer]) {
                            if(bucket.args.empty()) continue;
                            const auto& repMesh=meshesData[bucket.repSlotId];
                            const auto* indices=&_quadIndices;
                            if(bucket.wireMask) {
                                for(const auto& wire:repMesh.wireframes) if(wire.mask==bucket.wireMask) { indices=&wire.indices; break; }
                            }
                            const uint32_t batchIdx=uint32_t(prepared.Batches.size());
                            const uint32_t firstArg=uint32_t(prepared.IndexedArguments.size());
                            prepared.IndexedArguments.insert(prepared.IndexedArguments.end(),bucket.args.begin(),bucket.args.end());
                            prepared.Batches.push_back({
                                .CommandIndex=batchIdx,
                                .Indexed=true,
                                .IndexBuffer=indices->GetIndexBufferView(),
                                .FirstArgument=firstArg,
                                .ArgumentCount=uint32_t(bucket.args.size()),
                                .HasNonZeroFirstInstance=bucket.hasNonZeroFirstInstance});
                            layerData.fastCommands.push_back(BuildFastTerrainCommand(
                                layerData.fastBindings,wireframe?resources.WireframePipeline():resources.Pipeline(layer),repMesh,*indices));
                        }
                        layerData.indirectReps.resize(activeBuckets);
                        for(size_t b=0;b<activeBuckets;++b) {
                            layerData.indirectReps[b]={
                                .SortKey=0u,
                                .CachedCommand=&layerData.fastCommands[b],
                                .FirstInstance=0u,
                                .InstanceCount=1u,
                                .ElementMask=1u};
                        }
                    }
                    layerData.recordCount=uint32_t(prepared.IndexedArguments.size());
                    {
                        auto& cached=cache.layers[layer];
                        cached.geometryVersion=_layerGeometryVersion[layer];
                        cached.visibleCount=visibleCounts[layer];
                        cached.valid=true;
                    }
                }

                if(!layerHit[2]) {
                    auto& layerData=draws.layers[2];
                    auto& prepared=layerData.prepared;
                    if(oit) {
                        size_t totalArgs=0;
                        for(const auto& bucket:_fastOitBuckets) totalArgs+=bucket.args.size();
                        if(totalArgs>UINT32_MAX) { RADRAY_ERR_LOG("Terrain indirect argument count overflow"); return false; }
                        if(totalArgs) {
                            const size_t batchCount=_fastOitBuckets.size();
                            prepared.IndexedArguments.reserve(totalArgs);
                            prepared.Batches.reserve(batchCount);
                            layerData.fastCommands.reserve(batchCount*3);
                            for(size_t b=0;b<batchCount;++b) {
                                const auto& bucket=_fastOitBuckets[b];
                                const auto& repMesh=meshesData[bucket.repSlotId];
                                const uint32_t firstArg=uint32_t(prepared.IndexedArguments.size());
                                prepared.IndexedArguments.insert(prepared.IndexedArguments.end(),bucket.args.begin(),bucket.args.end());
                                prepared.Batches.push_back({
                                    .CommandIndex=uint32_t(b),
                                    .Indexed=true,
                                    .IndexBuffer=repMesh.indices.GetIndexBufferView(),
                                    .FirstArgument=firstArg,
                                    .ArgumentCount=uint32_t(bucket.args.size()),
                                    .HasNonZeroFirstInstance=bucket.hasNonZeroFirstInstance});
                                layerData.fastCommands.push_back(BuildFastTerrainCommand(layerData.fastBindings,resources.Pipeline(OitBounds),repMesh,repMesh.indices));
                                layerData.fastCommands.push_back(BuildFastTerrainCommand(layerData.fastBindings,resources.Pipeline(OitTransmittance),repMesh,repMesh.indices));
                                layerData.fastCommands.push_back(BuildFastTerrainCommand(layerData.fastBindings,resources.Pipeline(OitAccumulate),repMesh,repMesh.indices));
                            }
                            for(uint32_t s=0;s<3;++s) {
                                draws.oitIndirectReps[s].resize(batchCount);
                                for(size_t b=0;b<batchCount;++b) {
                                    draws.oitIndirectReps[s][b]={
                                        .SortKey=0u,
                                        .CachedCommand=&layerData.fastCommands[b*3+s],
                                        .FirstInstance=0u,
                                        .InstanceCount=1u,
                                        .ElementMask=1u};
                                }
                            }
                        }
                    } else if(!_fastClassicTranslucent.empty()) {
                        if(_fastClassicTranslucent.size()>=2) {
                            bool strictlyDescending=true;
                            for(size_t i=1;i<_fastClassicTranslucent.size();++i) {
                                if(_fastClassicTranslucent[i-1].rawSortKey<=_fastClassicTranslucent[i].rawSortKey) { strictlyDescending=false; break; }
                            }
                            if(strictlyDescending) std::reverse(_fastClassicTranslucent.begin(),_fastClassicTranslucent.end());
                            bool sorted=true;
                            for(size_t i=1;i<_fastClassicTranslucent.size();++i) {
                                if(_fastClassicTranslucent[i-1].sortKey>_fastClassicTranslucent[i].sortKey) { sorted=false; break; }
                            }
                            if(!sorted) {
                                RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Sort");
                                std::stable_sort(_fastClassicTranslucent.begin(),_fastClassicTranslucent.end(),
                                    [](const FastClassicTranslucentItem& a,const FastClassicTranslucentItem& b) noexcept {
                                        return a.sortKey<b.sortKey;
                                    });
                            }
                        }
                        classicReordered=GroupDisjointClassicItems(activeInstances,f);
                        if(_fastClassicTranslucent.size()>UINT32_MAX) { RADRAY_ERR_LOG("Terrain indirect argument count overflow"); return false; }
                        prepared.IndexedArguments.reserve(_fastClassicTranslucent.size());
                        _fastBatchRepSlots.clear();
                        uint16_t prevVPage=UINT16_MAX,prevIPage=UINT16_MAX;
                        for(const auto& item:_fastClassicTranslucent) {
                            const uint32_t argIdx=uint32_t(prepared.IndexedArguments.size());
                            prepared.IndexedArguments.push_back(item.arg);
                            if(!prepared.Batches.empty() && item.vertexPageId==prevVPage && item.indexPageId==prevIPage) {
                                auto& batch=prepared.Batches.back();
                                ++batch.ArgumentCount;
                                batch.HasNonZeroFirstInstance|=(item.arg.FirstInstance!=0u);
                            } else {
                                const uint32_t batchIdx=uint32_t(prepared.Batches.size());
                                const auto& repMesh=meshesData[item.slotId];
                                prepared.Batches.push_back({
                                    .CommandIndex=batchIdx,
                                    .Indexed=true,
                                    .IndexBuffer=repMesh.indices.GetIndexBufferView(),
                                    .FirstArgument=argIdx,
                                    .ArgumentCount=1u,
                                    .HasNonZeroFirstInstance=(item.arg.FirstInstance!=0u)});
                                _fastBatchRepSlots.push_back(item.slotId);
                                prevVPage=item.vertexPageId;
                                prevIPage=item.indexPageId;
                            }
                        }
                        const size_t batchCount=prepared.Batches.size();
                        layerData.fastCommands.reserve(batchCount);
                        for(size_t b=0;b<batchCount;++b) {
                            const auto& repMesh=meshesData[_fastBatchRepSlots[b]];
                            layerData.fastCommands.push_back(BuildFastTerrainCommand(
                                layerData.fastBindings,wireframe?resources.WireframePipeline():resources.Pipeline(Translucent),repMesh,repMesh.indices));
                        }
                        layerData.indirectReps.resize(batchCount);
                        for(size_t b=0;b<batchCount;++b) {
                            layerData.indirectReps[b]={
                                .SortKey=0u,
                                .CachedCommand=&layerData.fastCommands[b],
                                .FirstInstance=0u,
                                .InstanceCount=1u,
                                .ElementMask=1u};
                        }
                    }
                    layerData.recordCount=uint32_t(prepared.IndexedArguments.size());
                    {
                        cache.layers[2]={_layerGeometryVersion[2],{},visibleCounts[2],!classicReordered};
                    }
                }
            }
        }

        draws.totalIndirectArguments=0;
        draws.hasAnyDraws=false;
        for(uint32_t layer=0;layer<3;++layer) {
            auto& layerData=draws.layers[layer];
            if(layerData.recordCount) {
                if(layerData.recordCount>UINT32_MAX-draws.totalIndirectArguments) {
                    RADRAY_ERR_LOG("Terrain indirect argument count overflow"); return false;
                }
                layerData.indirectSize=uint64_t(layerData.recordCount)*sizeof(DrawIndexedIndirectArguments);
                draws.totalIndirectArguments+=layerData.recordCount;
                draws.hasAnyDraws=true;
            }
            totalBuiltRecords+=layerData.recordCount;
            totalBuiltBatches+=layerData.prepared.Batches.size();
        }

        cache.epoch=f.epoch;
        if(!cache.instanceBuffer.Prepare(_device.GetDevice(),instancesIdentical,frame.instances.size_bytes(),
            BufferUse::Vertex,frame.persistentInstances,frame.uploadInstances,draws.hasAnyDraws)) return false;
        if(slot.useIndirect) {
            for(uint32_t layer=0;layer<3;++layer) {
                auto bytes=draws.layers[layer].indirectSize;
                if(!cache.argumentBuffers[layer].Prepare(_device.GetDevice(),layerHit[layer],bytes,BufferUse::Indirect,
                    frame.persistentIndirect[layer],frame.uploadIndirect[layer])) return false;
                if(!frame.persistentIndirect[layer] || frame.uploadIndirect[layer]) uploadedIndirectBytes+=bytes;
            }
        }
    }

    frame.hasTranslucent=draws.layers[RN_MATERIAL_TRANSLUCENT].recordCount!=0;
    RADRAY_PROFILE_PLOT("Raynium.Visible.Solid",double(visibleCounts[0]));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Cutout",double(visibleCounts[1]));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Translucent",double(visibleCounts[2]));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.ArgumentBytes",double(uint64_t(draws.totalIndirectArguments)*sizeof(DrawIndexedIndirectArguments)));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.UploadedArgumentBytes",double(uploadedIndirectBytes));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltRecords",double(totalBuiltRecords));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltBatches",double(totalBuiltBatches));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.OitParameterBuilds",double(frame.hasTranslucent && oit?1:0));
#ifdef RADRAY_ENABLE_PROFILER
    const radray::array<const char*,3> groups{"Raynium.Terrain.Solid.SelectedGroups","Raynium.Terrain.Cutout.SelectedGroups","Raynium.Terrain.Translucent.SelectedGroups"};
    const radray::array<const char*,3> records{"Raynium.Terrain.Solid.Records","Raynium.Terrain.Cutout.Records","Raynium.Terrain.Translucent.Records"};
    const radray::array<const char*,3> batches{"Raynium.Terrain.Solid.Batches","Raynium.Terrain.Cutout.Batches","Raynium.Terrain.Translucent.Batches"};
    for(size_t layer=0;layer<3;++layer) {
        RADRAY_PROFILE_PLOT(groups[layer],double(draws.layers[layer].selectedGroups));
        RADRAY_PROFILE_PLOT(records[layer],double(draws.layers[layer].recordCount));
        RADRAY_PROFILE_PLOT(batches[layer],double(draws.layers[layer].prepared.Batches.size()));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes",double(state.residentMeshCount));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexPages",double(_vertices->GetPageCount()+_translucentVertices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexPages",double(_indices->GetPageCount()+_quadPool->GetPageCount()));
#ifdef RADRAY_ENABLE_PROFILER
    if(f.output_sequence%120==0) {
        const uint64_t vertexReserved=_vertices->GetCapacityBytes()+_translucentVertices->GetCapacityBytes();
        const uint64_t indexReserved=(_indices->GetCapacityBytes()+_quadPool->GetCapacityBytes());
        const auto vertexFree=_vertices->GetFreeBytes()+_translucentVertices->GetFreeBytes(),indexFree=(_indices->GetFreeBytes()+_quadPool->GetFreeBytes());
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexReservedBytes",double(vertexReserved));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexReservedBytes",double(indexReserved));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexFreeBytes",double(vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexFreeBytes",double(indexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexAllocatedBytes",double(vertexReserved-vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexAllocatedBytes",double(indexReserved-indexFree));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes",double(uploadBytes));
    return true;
}

void TerrainResidency::Reset() noexcept try {
    _state.meshesBySlot.clear();
    _hotSlots.clear();
    _state.residentMeshCount=0;
    _state.epoch=0;
    _state.wireframeActive=false;
    _views.clear();
    _changedSlotsScratch.clear();
    for(auto& layerBuckets:_fastPageBuckets) layerBuckets.clear();
    _fastOitBuckets.clear();
    _fastClassicTranslucent.clear();
    _fastBatchRepSlots.clear();
    _quadIndices={};
    _quadVertexCapacity=0;
} catch(...) { AbortUnexpectedException("TerrainResidency::Reset"); }

uint64_t TerrainResidency::ResidentBytes() const noexcept {
    uint64_t bytes=_quadIndices?_quadIndices.GetByteSize():0;
    for(const auto& mesh:_state.meshesBySlot) {
        if(!mesh.active) continue;
        bytes+=mesh.geometry.GetByteSize();
        if(mesh.indices) bytes+=mesh.indices.GetByteSize();
        for(const auto& wire:mesh.wireframes) bytes+=wire.indices.GetByteSize();
    }
    return bytes;
}

void TerrainResidency::MeasureGeometry(rn_frame_metrics& metrics) const noexcept {
    metrics.vertex_capacity=_vertices->GetCapacityBytes()+_translucentVertices->GetCapacityBytes();
    metrics.index_capacity=(_indices->GetCapacityBytes()+_quadPool->GetCapacityBytes());
    metrics.geometry_allocated=_vertices->GetAllocatedBytes()+_translucentVertices->GetAllocatedBytes()+(_indices->GetAllocatedBytes()+_quadPool->GetAllocatedBytes());
}
}
