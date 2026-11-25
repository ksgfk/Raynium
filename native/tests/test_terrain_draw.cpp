#include <raynium/terrain_draw.h>
#include <raynium/buffer_pool.h>
#include <raynium/terrain_feature.h>
#include "test_fakes.h"
#include <raynium/terrain_residency.h>
#include <raynium/world_features.h>
#include <raynium/frame_plan.h>
#include <raynium/frame_upload.h>
#include <radray/types.h>
#include <radray/profiler.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory_resource>
#include <span>
#include <utility>

namespace raynium {
namespace {
using namespace radray::gpu;

class BufferProbe final : public Buffer {
public:
    explicit BufferProbe(BufferUses usage = BufferUse::Vertex | BufferUse::Index | BufferUse::Indirect, uint64_t size = 65536) noexcept
        : _desc{.Size = size, .Memory = MemoryType::Device, .Usage = usage} {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    radray::Nullable<void*> GetMappedData() noexcept override { return nullptr; }
    bool FlushMappedRange(BufferRange) noexcept override { return true; }
    bool InvalidateMappedRange(BufferRange) noexcept override { return true; }
    BufferDescriptor GetDesc() const noexcept override { return _desc; }
private:
    BufferDescriptor _desc{};
};

class PipelineLayoutProbe final : public PipelineLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    PipelineLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

class PipelineProbe final : public GraphicsPipelineState {
public:
    explicit PipelineProbe(PipelineLayout* layout) noexcept : _layout(layout) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    radray::Nullable<PipelineLayout*> GetLayout() const noexcept override { return _layout; }
private:
    PipelineLayout* _layout{};
};

class ParametersProbe final : public ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t, uint32_t, ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override {}
};

class EncoderProbe final : public GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void SetViewport(radray::Viewport) noexcept override {}
    void SetScissor(radray::Rect) noexcept override {}
    void SetStencilReference(uint8_t) noexcept override {}
    void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept override { pipelines.push_back(pso); }
    void BindVertexBuffers(std::span<const VertexBufferBinding> values) noexcept override {
        for (auto value : values) vertices.push_back(value);
    }
    void BindIndexBuffer(IndexBufferView value) noexcept override { indices.push_back(value); }
    void BindShaderParameterSet(uint32_t group, ShaderParameterSet* set, std::span<const ShaderParameterDynamicOffset>) noexcept override {
        parameterBinds.emplace_back(group, set);
    }
    bool SetPushConstants(uint32_t, uint32_t, std::span<const radray::byte>) noexcept override { return true; }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t vertex, uint32_t instance) noexcept override {
        direct.push_back({count, instances, first, vertex, instance});
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override {}
    void DrawIndirect(Buffer*, uint64_t, uint32_t) noexcept override {}
    void DrawIndexedIndirect(Buffer* buffer, uint64_t offset, uint32_t count) noexcept override {
        indirect.push_back({buffer, offset, count});
    }
    struct Indirect { Buffer* buffer; uint64_t offset; uint32_t count; };
    radray::vector<GraphicsPipelineState*> pipelines;
    radray::vector<VertexBufferBinding> vertices;
    radray::vector<IndexBufferView> indices;
    radray::vector<std::pair<uint32_t, ShaderParameterSet*>> parameterBinds;
    radray::vector<DrawIndexedIndirectArguments> direct;
    radray::vector<Indirect> indirect;
};

TEST(BufferPool, ReuseRequiresCompletedSubmissionAndLastSlotReference) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers=true;
    BufferPool registry;
    const uint32_t usage=RN_BUFFER_MAP_WRITE|RN_BUFFER_COPY_SRC;
    auto allocation=BufferPool::Allocate(device,usage,256);
    ASSERT_TRUE(allocation);
    auto* original=allocation.get();
    auto retained=allocation->device;
    registry.Retire(std::move(allocation),5);
    EXPECT_FALSE(registry.Take(usage,256,3));
    EXPECT_FALSE(registry.Take(usage,256,5));
    retained=nullptr;
    EXPECT_FALSE(registry.Take(usage,256,3));
    auto reused=registry.Take(usage,256,5);
    ASSERT_EQ(reused.get(),original);
    registry.Retire(std::move(reused),5);
    registry.Advance(125);
    EXPECT_FALSE(registry.Take(usage,256,125));
}

TEST(BufferPool, IdleBudgetCapsExactSizeBuckets) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers=true;
    BufferPool registry;
    constexpr uint32_t usage=RN_BUFFER_MAP_WRITE|RN_BUFFER_COPY_SRC;
    constexpr uint64_t size=16u<<20;
    for(uint32_t id=1;id<=5;++id) {
        auto allocation=BufferPool::Allocate(device,usage,size);
        ASSERT_TRUE(allocation);
        registry.Retire(std::move(allocation),0);
    }
    for(unsigned i=0;i<4;++i) EXPECT_TRUE(registry.Take(usage,size,0));
    EXPECT_FALSE(registry.Take(usage,size,0));
}

TEST(BufferPool, FlushesInitialAndDirtyRangesOnPooledMappedStorage) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers=true;
    radray::vector<BufferRange> flushes;
    device.OnFlush=[&](BufferRange range) { flushes.push_back(range); };
    BufferPool registry;
    constexpr uint32_t usage=RN_BUFFER_MAP_WRITE|RN_BUFFER_COPY_SRC|RN_BUFFER_FENCE_ROTATED;
    constexpr uint64_t size=512u<<10;
    ASSERT_TRUE(registry.Prewarm(device));
    ASSERT_TRUE(flushes.empty());
    auto pooled=registry.Take(usage,size,0);
    ASSERT_TRUE(pooled);
    ASSERT_NE(pooled->mapped,nullptr);
    ASSERT_TRUE(pooled->upload->FlushMappedRange({0,size}));
    ASSERT_EQ(flushes.size(),1u);
    EXPECT_EQ(flushes[0].Offset,0u);
    EXPECT_EQ(flushes[0].Size,size);
    const radray::array dirty{rn_mapped_range{pooled.get(),37,19},rn_mapped_range{pooled.get(),201,54}};
    for(const auto& range:dirty) ASSERT_TRUE(range.buffer->upload->FlushMappedRange({range.offset,range.length}));
    ASSERT_EQ(flushes.size(),3u);
    for(size_t i=0;i<dirty.size();++i) {
        EXPECT_EQ(flushes[i+1].Offset,dirty[i].offset);
        EXPECT_EQ(flushes[i+1].Size,dirty[i].length);
    }
}

TEST(BufferPool, LostDeviceRejectsMappedRanges) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers=true;
    const auto usage=RN_BUFFER_MAP_WRITE|RN_BUFFER_COPY_SRC|RN_BUFFER_FENCE_ROTATED;
    auto entry=BufferPool::Allocate(device,usage,256);
    ASSERT_TRUE(entry);
    device.Lost=true;
    EXPECT_FALSE(entry->upload->FlushMappedRange({0,256}));
    EXPECT_FALSE(entry->upload->FlushMappedRange({0,16}));
}

TEST(BufferPool, GraphRecognizesPersistentUploadAfterHandleRelease) {
    radray::test::FakeGpu gpu;
    gpu.Device->EnableHostBuffers=true;
    auto scheduler=radray::FrameScheduler::Create(gpu.Context,radray::make_unique<radray::test::CallbackPipeline>(),1).Unwrap();
    radray::RDGBuilder graph{scheduler->BeginFrame()};
    auto entry=BufferPool::Allocate(*gpu.Device,RN_BUFFER_FENCE_ROTATED|RN_BUFFER_MAP_WRITE|RN_BUFFER_VERTEX,256);
    ASSERT_TRUE(entry);
    ASSERT_EQ(entry->upload->UseCount(),1u);
    auto handle=entry->Import(graph);
    ASSERT_GT(entry->upload->UseCount(),1u);
    entry.reset();
    const radray::array<radray::byte,4> bytes{};
    graph.UploadBuffer(handle,0,radray::RDGUploadSource::SnapshotBytes(bytes));
    graph.MarkOutput(handle);
    EXPECT_FALSE(graph.Compile());
}

TEST(TerrainViewCache, VersionsRequireMatchingEpochAndExtent) {
    TerrainViewCacheSlot cache{};
    cache.epoch=17;
    cache.visibleVersion=5;
    cache.originsVersion=8;
    cache.instancesVersion=11;
    cache.visible.resize(2);
    cache.instances.resize(1);
    rn_terrain_view_desc view{};
    view.epoch=17;
    view.visible_version=5;
    view.origins_version=8;
    view.instances_version=11;
    view.visible_count=2;
    view.instance_count=1;
    EXPECT_TRUE(cache.MatchesVisible(view));
    EXPECT_TRUE(cache.MatchesOrigins(view));
    EXPECT_TRUE(cache.MatchesInstances(view));
    ++view.instance_count;
    EXPECT_FALSE(cache.MatchesOrigins(view));
    EXPECT_FALSE(cache.MatchesInstances(view));
    --view.instance_count;
    ++view.instances_version;
    EXPECT_TRUE(cache.MatchesOrigins(view));
    EXPECT_FALSE(cache.MatchesInstances(view));
    view.instances_version=11;
    ++view.visible_version;
    EXPECT_FALSE(cache.MatchesVisible(view));
    EXPECT_TRUE(cache.MatchesOrigins(view));
    EXPECT_TRUE(cache.MatchesInstances(view));
    view.visible_version=5;
    ++view.epoch;
    EXPECT_FALSE(cache.MatchesVisible(view));
    EXPECT_FALSE(cache.MatchesOrigins(view));
    EXPECT_FALSE(cache.MatchesInstances(view));
    --view.epoch;
    cache.visibleVersion=view.visible_version=0;
    cache.originsVersion=view.origins_version=0;
    cache.instancesVersion=view.instances_version=0;
    EXPECT_FALSE(cache.MatchesVisible(view));
    EXPECT_FALSE(cache.MatchesOrigins(view));
    EXPECT_FALSE(cache.MatchesInstances(view));
}

class ShaderProbe final : public Shader {
public:
    explicit ShaderProbe(ShaderStages stages) noexcept : _stages(stages) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    ShaderStages GetStages() const noexcept override { return _stages; }
private:
    ShaderStages _stages{};
};

class BindingGroupLayoutProbe final : public BindingGroupLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

class TerrainFakeDevice final : public radray::test::FakeDevice {
public:
    TerrainFakeDevice() {
        EnableHostBuffers=true;
        EnableTextures=true;
        Capabilities.Limits.MaxColorAttachments=8;
        Capabilities.Limits.MaxBufferSize=512ull<<20;
    }
    TextureSupport QueryTextureSupport(const TextureSupportQuery& query) const noexcept override {
        auto support=radray::test::FakeDevice::QueryTextureSupport(query);
        support.Blending=true;
        return support;
    }
    radray::Nullable<radray::unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override {
        return radray::make_unique<ShaderProbe>(desc.Stages);
    }
    radray::Nullable<radray::unique_ptr<BindingGroupLayout>> CreateBindingGroupLayout(const BindingGroupLayoutDescriptor&) noexcept override {
        return radray::make_unique<BindingGroupLayoutProbe>();
    }
    radray::Nullable<radray::unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor&) noexcept override {
        return radray::make_unique<radray::test::FakePipelineLayout>();
    }
    radray::Nullable<radray::unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override {
        return radray::make_unique<radray::test::FakePipeline>(desc.PipelineLayout);
    }
};

TEST(TerrainResidencyCache, NonReuseRescanWithMatchingVersionsHitsCacheAndPromotesBuffers) {
    std::ifstream file(RAYNIUM_SHADER_PACK_PATH,std::ios::binary|std::ios::ate);
    ASSERT_TRUE(file.is_open());
    const auto packSize=size_t(file.tellg());
    file.seekg(0);
    radray::vector<radray::byte> packBytes(packSize);
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(packBytes.data()),std::streamsize(packSize)));

    auto fakeDevice=radray::make_shared<TerrainFakeDevice>();
    radray::DeviceContext context{fakeDevice,&fakeDevice->Queue};
    radray::unique_ptr<MeshPassResources> resources;
    ASSERT_TRUE(MeshPassResources::Create(context.GetDevice(),packBytes,resources));

    const radray::array layouts{VertexBufferLayout{0,28,VertexStepMode::Vertex},VertexBufferLayout{1,16,VertexStepMode::Instance}};
    const radray::array attributes{radray::VertexInputAttribute{0,0,"POSITION",0,VertexFormat::FLOAT32X3}};
    auto vertices=radray::VertexBufferPool::Create(context.GetDevice(),{.VertexStride=28,.PageVertexCapacity=4096,.AdditionalUsage=BufferUse::Resource,.Layouts=layouts,.Attributes=attributes}).Unwrap();
    auto translucentVertices=radray::VertexBufferPool::Create(context.GetDevice(),{.VertexStride=28,.PageVertexCapacity=4096,.AdditionalUsage=BufferUse::Resource,.Layouts=layouts,.Attributes=attributes}).Unwrap();
    auto quadPool=radray::IndexBufferPool::Create(context.GetDevice(),{.PageIndexCapacity=65536*6/4,.AdditionalUsage=BufferUse::Resource}).Unwrap();
    auto indices=radray::IndexBufferPool::Create(context.GetDevice(),{.PageIndexCapacity=4096,.AdditionalUsage=BufferUse::Resource}).Unwrap();
    TerrainResidency residency(context,std::move(vertices),std::move(translucentVertices),std::move(quadPool),std::move(indices));
    auto scheduler=radray::FrameScheduler::Create(context,radray::make_unique<radray::test::CallbackPipeline>(),2).Unwrap();

    auto* owner=reinterpret_cast<rn_renderer*>(uintptr_t{1});
    rn_texture albedo{},light{};
    albedo.owner=owner; albedo.live=true;
    light.owner=owner; light.live=true;

    radray::array<radray::byte,112> quadVertices{};
    rn_terrain_mesh_layout meshLayout{};
    meshLayout.group_quad_counts[6]=1;
    meshLayout.flags=RN_TERRAIN_LAYOUT_FINITE_BOUNDS;
    meshLayout.maximum[0]=meshLayout.maximum[1]=meshLayout.maximum[2]=1.0f;
    rn_mesh_update meshUpdate{1,quadVertices.data(),4,RN_MATERIAL_OPAQUE,&meshLayout};
    rn_visible_item visibleItem{1,UINT32_MAX,0,0,1};
    rn_instance initialInstance{{0,0,0},1.0f};

    auto makeView=[&](uint64_t seq,uint64_t visVer,uint64_t origVer,uint64_t instVer,const rn_instance* instPtr,bool withMesh) {
        rn_terrain_view_desc view{};
        view.struct_size=sizeof(view);
        view.epoch=1;
        view.output_sequence=seq;
        view.meshes=withMesh?&meshUpdate:nullptr;
        view.mesh_count=withMesh?1u:0u;
        view.visible=&visibleItem;
        view.visible_count=1;
        view.instances=instPtr;
        view.instance_count=1;
        view.terrain_anisotropy=1;
        view.terrain_albedo_texture=&albedo;
        view.terrain_light_texture=&light;
        for(int i=0;i<16;i+=5) view.projection[i]=view.view[i]=1.0f;
        view.visible_version=visVer;
        view.origins_version=origVer;
        view.instances_version=instVer;
        return view;
    };

    auto runFrame=[&](const rn_terrain_view_desc& view,TerrainFrame& outFrame) {
        auto& slot=scheduler->BeginFrame();
        residency.BeginSlotViews({&view,1},slot);
        std::pmr::vector<GeometryUpload> geometry(&slot.GetMemoryResource());
        const bool ok=residency.PrepareFrame(owner,view,*resources,{slot,geometry,residency.View(0),true},outFrame);
        EXPECT_TRUE(scheduler->RunFrame());
        return ok;
    };

    TerrainFrame frame{};
    ASSERT_TRUE(runFrame(makeView(1,10,10,10,&initialInstance,true),frame));
    EXPECT_FALSE(frame.persistentInstances);
    EXPECT_FALSE(frame.persistentIndirect[0]);

    // Frame 2: non-reuse rescan with identical versions hits MatchesVisible/MatchesOrigins/MatchesInstances and skips copying instances.
    rn_instance sentinelInstance{{999,999,999},0.25f};
    ASSERT_TRUE(runFrame(makeView(2,10,10,10,&sentinelInstance,false),frame));
    ASSERT_EQ(residency.View(0).instances.size(),1u);
    EXPECT_EQ(residency.View(0).instances[0].origin[0],0);
    EXPECT_FLOAT_EQ(residency.View(0).instances[0].visibility,1.0f);
    EXPECT_EQ(residency.View(0).instanceBuffer.stableFrames,1u);
    EXPECT_EQ(residency.View(0).argumentBuffers[0].stableFrames,1u);

    // Frame 3: second consecutive hit promotes both instance and indirect argument buffers.
    ASSERT_TRUE(runFrame(makeView(3,10,10,10,&sentinelInstance,false),frame));
    EXPECT_TRUE(frame.persistentInstances);
    EXPECT_TRUE(frame.uploadInstances);
    EXPECT_TRUE(frame.persistentIndirect[0]);
    EXPECT_TRUE(frame.uploadIndirect[0]);

    // Frame 4: steady reuse avoids uploading both buffers.
    ASSERT_TRUE(runFrame(makeView(4,10,10,10,&sentinelInstance,false),frame));
    EXPECT_TRUE(frame.persistentInstances);
    EXPECT_FALSE(frame.uploadInstances);
    EXPECT_TRUE(frame.persistentIndirect[0]);
    EXPECT_FALSE(frame.uploadIndirect[0]);

    // Frame 5: visibility-only change bumps instances_version -> updates instances while keeping indirect arguments promoted.
    rn_instance fadedInstance{{0,0,0},0.5f};
    ASSERT_TRUE(runFrame(makeView(5,10,10,20,&fadedInstance,false),frame));
    EXPECT_FLOAT_EQ(residency.View(0).instances[0].visibility,0.5f);
    EXPECT_FALSE(frame.persistentInstances);
    EXPECT_TRUE(frame.persistentIndirect[0]);
    EXPECT_FALSE(frame.uploadIndirect[0]);
}

TEST(FrameOpInline, StoresSmallTrivialPayloadsInlineAndLargeByPointer) {
    struct Small { uint32_t value; };
    struct Large { uint64_t a,b; };
    using Op=FrameOp<uint32_t,Small,Large>;
    static_assert(Op::kInline<uint32_t>);
    static_assert(Op::kInline<Small>);
    static_assert(!Op::kInline<Large>);
    static_assert(std::is_trivially_destructible_v<Op>);

    Op empty{};
    EXPECT_FALSE(empty.Get<uint32_t>().has_value());
    EXPECT_FALSE(empty.Get<Small>().has_value());
    EXPECT_EQ(empty.Get<Large>(),nullptr);

    Op inlineU32(42u);
    ASSERT_TRUE(inlineU32.Get<uint32_t>().has_value());
    EXPECT_EQ(*inlineU32.Get<uint32_t>(),42u);
    EXPECT_FALSE(inlineU32.Get<Small>().has_value());
    EXPECT_EQ(inlineU32.Get<Large>(),nullptr);

    Op inlineSmall(Small{99u});
    ASSERT_TRUE(inlineSmall.Get<Small>().has_value());
    EXPECT_EQ(inlineSmall.Get<Small>()->value,99u);
    EXPECT_FALSE(inlineSmall.Get<uint32_t>().has_value());

    Large payload{123,456};
    Op external(&payload);
    EXPECT_EQ(external.Get<Large>(),&payload);
    EXPECT_FALSE(external.Get<uint32_t>().has_value());
}

TEST(TerrainBounds, OnlyStrictlySeparatedScreenBoundsCanCross) {
    TerrainHotSlot hot{};
    hot.flags=kTerrainHotFlagFiniteBounds;
    hot.maxBound[0]=hot.maxBound[1]=hot.maxBound[2]=1;
    rn_terrain_view_desc frame{};
    for(int i=0;i<16;i+=5) frame.projection[i]=frame.view[i]=1;
    rn_instance first{},other{};
    auto a=ProjectTerrainBounds(hot,first,frame);
    other.origin[2]=3;
    EXPECT_FALSE(a.Disjoint(ProjectTerrainBounds(hot,other,frame)));
    other.origin[0]=1;
    EXPECT_FALSE(a.Disjoint(ProjectTerrainBounds(hot,other,frame)));
    other.origin[0]=2;
    EXPECT_TRUE(a.Disjoint(ProjectTerrainBounds(hot,other,frame)));
    frame.projection[15]=0;
    EXPECT_FALSE(a.Disjoint(ProjectTerrainBounds(hot,other,frame)));
}

TEST(StaticGeometry, BuiltinsStayResidentAndUserIdsCanBeReused) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers=true;
    radray::unique_ptr<StaticGeometryRegistry> registry;
    ASSERT_TRUE(StaticGeometryRegistry::Create(device,registry));
    const auto* particle=registry->Find(StaticGeometryRegistry::kParticleUnitQuadId);
    const auto* shadow=registry->Find(StaticGeometryRegistry::kShadowUnitQuadId);
    ASSERT_NE(particle,nullptr);
    ASSERT_NE(shadow,nullptr);
    EXPECT_EQ(particle->indexCount,6u);
    EXPECT_EQ(shadow->geometry.GetFirstVertex(),4u);
    EXPECT_FALSE(registry->Remove(2));
    radray::array<radray::byte,96> vertices{};
    uint32_t page=UINT32_MAX;
    EXPECT_FALSE(registry->Upsert(15,vertices,4,6,page));
    ASSERT_TRUE(registry->Upsert(16,vertices,4,6,page));
    EXPECT_EQ(page,0u);
    ASSERT_TRUE(registry->Remove(16));
    EXPECT_EQ(registry->Find(16),nullptr);
    ASSERT_TRUE(registry->Upsert(16,vertices,4,6,page));
    registry->BeginSlot();
    EXPECT_NE(registry->Find(1)->geometry.GetBuffer(),nullptr);
}

TEST(InstancedGeometry, ValidatesStrideAndCombinesAdjacentRunsOnTheSamePage) {
    radray::test::FakeDevice device;
    device.EnableHostBuffers = true;
    radray::unique_ptr<StaticGeometryRegistry> geometry;
    ASSERT_TRUE(StaticGeometryRegistry::Create(device, geometry));
    auto indices = radray::IndexBufferPool::Create(device, {.PageIndexCapacity = 64}).Release();
    ASSERT_TRUE(indices);
    auto quad = indices->Allocate(6);
    ASSERT_TRUE(quad);
    const radray::array runs{InstancedGeometryFeature::WireRun{1, 0, 1, 0}, InstancedGeometryFeature::WireRun{2, 1, 2, 0}};
    const radray::array layouts{VertexBufferLayout{1, 56, VertexStepMode::Instance}};
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::vector<DrawIndexedIndirectArguments> args(&memory);
    InstancedGeometryFeature feature{};
    auto decode = [&](size_t runCount, uint32_t stride, uint64_t offset, bool mdi) {
        return feature.DecodeRuns(memory,std::as_bytes(std::span(runs).first(runCount)), 3, stride, offset, 168,
            layouts, *geometry, *quad, args, mdi, 2);
    };
    EXPECT_FALSE(decode(2, 64, 0, true));
    EXPECT_FALSE(decode(2, 56, UINT64_MAX, true));
    EXPECT_FALSE(decode(2, 56, 1, true));
    ASSERT_TRUE(decode(1, 56, 0, true));
    EXPECT_FALSE(feature.UsesIndirect());
    EXPECT_TRUE(args.empty());
    ASSERT_TRUE(decode(2, 56, 0, false));
    EXPECT_EQ(feature.DrawCount(), 2u);
    EXPECT_TRUE(args.empty());
    ASSERT_TRUE(decode(2, 56, 0, true));
    EXPECT_TRUE(feature.UsesIndirect());
    EXPECT_EQ(feature.DrawCount(), 1u);
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].FirstInstance, 0u);
    EXPECT_EQ(args[1].FirstInstance, 1u);
    EXPECT_EQ(args[1].InstanceCount, 2u);
}

class TerrainPromotion : public ::testing::Test {
protected:
    enum Result { Transient, Upload, Reuse };
    radray::test::FakeDevice device;
    SlotPromotedBuffer cache;
    radray::IntrusivePtr<radray::GpuBuffer> output;
    bool upload{};

    void SetUp() override { device.EnableHostBuffers=true; }

    void ExpectFrame(uint64_t bytes,bool unchanged,Result expected,bool hasDraws=true) {
        SCOPED_TRACE(::testing::Message()<<"bytes="<<bytes<<" unchanged="<<unchanged<<" hasDraws="<<hasDraws);
        ASSERT_TRUE(cache.Prepare(device,unchanged,bytes,BufferUse::Vertex,output,upload,hasDraws));
        EXPECT_EQ(bool(output),expected!=Transient);
        EXPECT_EQ(upload,expected==Upload);
    }
};

TEST_F(TerrainPromotion, ChangesUseTransientStorageAndSecondStableFramePromotes) {
    ExpectFrame(32,false,Transient);
    ExpectFrame(32,true,Transient);
    ExpectFrame(32,true,Upload);
    const auto first=output;
    ExpectFrame(32,true,Reuse);
    ExpectFrame(32,false,Transient);
    ExpectFrame(32,true,Transient);
    ExpectFrame(32,true,Upload);
    EXPECT_EQ(output,first);
}

TEST_F(TerrainPromotion, AllocatesOnlyAtCapacityBoundaries) {
    device.Capabilities.Limits.MaxBufferSize=12000;
    radray::vector<uint64_t> allocations;
    device.OnCreateBuffer=[&](const BufferDescriptor& desc) { allocations.push_back(desc.Size); };
    for(auto [bytes,capacity]:{std::pair{20u,4096u}, {64u,4096u}, {1024u,4096u}, {4096u,4096u},
            {4097u,8192u}, {8192u,8192u}, {8193u,12000u}, {12000u,12000u}, {3000u,12000u}, {2999u,4096u}}) {
        SCOPED_TRACE(bytes);
        ExpectFrame(bytes,false,Transient);
        ExpectFrame(bytes,true,Transient);
        ExpectFrame(bytes,true,Upload);
        ASSERT_TRUE(output);
        EXPECT_EQ(output->GetDesc().Size,capacity);
    }
    EXPECT_EQ(allocations,(radray::vector<uint64_t>{4096,8192,12000,4096}));
    ExpectFrame(0,false,Transient);
    EXPECT_FALSE(cache.buffer);
}

TEST_F(TerrainPromotion, FailedGrowthPreservesCacheAndOutputsForRetry) {
    device.Capabilities.Limits.MaxBufferSize=200;
    ExpectFrame(65,false,Transient);
    ExpectFrame(65,true,Transient);
    ExpectFrame(65,true,Upload);
    const auto original=output;
    ASSERT_TRUE(original);
    EXPECT_EQ(original->GetDesc().Size,200u);
    ExpectFrame(201,false,Transient);
    ExpectFrame(201,true,Transient);
    output=original;
    const auto expectFailure=[&] {
        EXPECT_FALSE(cache.Prepare(device,true,201,BufferUse::Vertex,output,upload));
        EXPECT_EQ(output,original);
        EXPECT_FALSE(upload);
        EXPECT_EQ(cache.buffer,original);
        EXPECT_EQ(cache.stableFrames,1u);
        EXPECT_EQ(cache.lastBytes,201u);
    };
    expectFailure();
    device.Capabilities.Limits.MaxBufferSize=4096;
    device.BuffersUntilFailure=0;
    expectFailure();
    device.BuffersUntilFailure=1;
    ExpectFrame(201,true,Upload);
    EXPECT_NE(output,original);
    ExpectFrame(201,true,Reuse);
}

TEST_F(TerrainPromotion, SizeChangesRestartObservationEvenOnReportedHits) {
    ExpectFrame(4100,true,Transient);
    ExpectFrame(4100,true,Transient);
    ExpectFrame(4100,true,Upload);
    const auto first=output;
    ExpectFrame(4200,true,Transient);
    ExpectFrame(4200,true,Transient);
    ExpectFrame(4200,true,Upload);
    EXPECT_EQ(output,first);
    ExpectFrame(20,true,Transient);
    EXPECT_FALSE(cache.buffer);
    ExpectFrame(20,true,Transient);
    ExpectFrame(20,true,Upload);
    EXPECT_NE(output,first);
}

TEST_F(TerrainPromotion, NoDrawFramesPreserveReadyInstancesAndDeferUploads) {
    ExpectFrame(65,false,Transient);
    ExpectFrame(65,true,Transient);
    ExpectFrame(65,true,Transient,false);
    EXPECT_FALSE(cache.buffer);
    ExpectFrame(65,true,Upload);
    const auto first=output;
    ExpectFrame(65,true,Transient,false);
    ExpectFrame(65,true,Reuse);
    EXPECT_EQ(output,first);

    ExpectFrame(65,false,Transient,false);
    ExpectFrame(65,true,Transient,false);
    ExpectFrame(65,true,Transient);
    ExpectFrame(65,true,Upload);
    EXPECT_EQ(output,first);
    ExpectFrame(0,true,Transient,false);
    EXPECT_FALSE(cache.buffer);
}

TEST(TerrainSubmission, DirectAndIndirectConsumeTheSameIndexedBatch) {
    PipelineLayoutProbe layout;
    PipelineProbe pso(&layout);
    BufferProbe indices,arguments;
    radray::MeshDrawCommand command{};
    command.Pipeline=&pso;
    command.IndexBuffer={&indices,0,4};
    command.IndexCount=6;
    command.ValidElementMask=1;
    radray::MeshDrawCommandBindingStorage storage;
    storage.Store(command,{});
    radray::array reps{radray::VisibleMeshDrawCommand{.CachedCommand=&command,.InstanceCount=1,.ElementMask=1}};
    radray::PreparedMeshPassIndirectDraws draws;
    draws.IndexedArguments={{6,1,0,4,0},{12,2,6,8,3}};
    draws.Batches.push_back({.Indexed=true,.IndexBuffer=command.IndexBuffer,.ArgumentCount=2,.HasNonZeroFirstInstance=true});
    EncoderProbe encoder;
    ASSERT_TRUE(SubmitPreparedTerrainDirectDraws(encoder,reps,draws,{}));
    ASSERT_EQ(encoder.direct.size(),2u);
    EXPECT_EQ(std::memcmp(encoder.direct.data(),draws.IndexedArguments.data(),2*sizeof(DrawIndexedIndirectArguments)),0);
    GpuDeviceCapabilities caps{};
    caps.Features.IndirectDraw=true;
    caps.Features.IndirectFirstInstance=true;
    caps.Limits.MaxDrawIndirectCount=4;
    ASSERT_TRUE(radray::SubmitMeshPassIndirectDraws(encoder,reps,draws,{&arguments,0,40},{},caps,{}));
    ASSERT_EQ(encoder.indirect.size(),1u);
    EXPECT_EQ(encoder.indirect[0].count,2u);
}

TEST(TerrainSubmission, AutoRequiresEveryCapabilityAndForcedModesRespectFailures) {
    GpuDeviceCapabilities caps;
    bool indirect = true;
    EXPECT_TRUE(SelectTerrainSubmission(0, caps, indirect)); EXPECT_FALSE(indirect);
    caps.Features.IndirectDraw = true; caps.Features.IndirectFirstInstance = true;
    for (auto limit : {0u, 1u}) {
        caps.Limits.MaxDrawIndirectCount = limit;
        EXPECT_TRUE(SelectTerrainSubmission(0, caps, indirect)); EXPECT_FALSE(indirect);
        EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT, caps, indirect));
    }
    caps.Limits.MaxDrawIndirectCount = 4;
    EXPECT_TRUE(SelectTerrainSubmission(0, caps, indirect)); EXPECT_TRUE(indirect);
    EXPECT_TRUE(SelectTerrainSubmission(RN_RENDERER_DIRECT, caps, indirect)); EXPECT_FALSE(indirect);
    caps.Features.IndirectFirstInstance = false;
    EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT, caps, indirect));
    EXPECT_FALSE(SelectTerrainSubmission(3, caps, indirect)); EXPECT_FALSE(SelectTerrainSubmission(4, caps, indirect));
}

TEST(TerrainFrameUpload, FrameConstantCarriesBindingAndPhysicalSlice) {
    BufferProbe buffer;
    const auto entry = FrameConstantBinding(31, {&buffer, 512, 256});
    EXPECT_EQ(entry.BindingId, 31u); EXPECT_EQ(entry.ArrayElement, 0u);
    const auto value = std::get<ShaderBufferBinding>(entry.Value);
    EXPECT_EQ(value.Target, &buffer); EXPECT_EQ(value.Range.Offset, 512u); EXPECT_EQ(value.Range.Size, 256u);
}

TEST(TerrainFrameUpload, QuadIndicesWriteOnlyTheFinalUnalignedRange) {
    radray::vector<radray::byte> storage(50, radray::byte{0x7f});
    ASSERT_TRUE(WriteQuadIndices(std::span{storage}.subspan(1, 48), 8));
    const radray::array<uint32_t, 12> expected{0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4};
    EXPECT_EQ(std::memcmp(storage.data() + 1, expected.data(), 48), 0);
    EXPECT_EQ(storage.front(), radray::byte{0x7f}); EXPECT_EQ(storage.back(), radray::byte{0x7f});
    EXPECT_FALSE(WriteQuadIndices(std::span{storage}.subspan(1, 47), 8));
    EXPECT_FALSE(WriteQuadIndices({}, 1)); EXPECT_TRUE(WriteQuadIndices({}, 0));
}

TEST(SceneFrameUpload, ExpandedLinesPreserveBothEndpointSides) {
    radray::array<radray::byte, 50> storage{};
    storage.fill(radray::byte{0x7f});
    ASSERT_TRUE(WriteLineIndices(std::span{storage}.subspan(1, 48), 8));
    const radray::array<uint32_t, 12> expected{0, 1, 2, 3, 2, 1, 4, 5, 6, 7, 6, 5};
    EXPECT_EQ(std::memcmp(storage.data() + 1, expected.data(), 48), 0);
    EXPECT_EQ(storage.front(), radray::byte{0x7f}); EXPECT_EQ(storage.back(), radray::byte{0x7f});
    EXPECT_FALSE(WriteLineIndices({}, 2)); EXPECT_TRUE(WriteLineIndices({}, 0));
}

rn_terrain_mesh_layout GroupLayout(std::span<const uint32_t> groups,radray::vector<uint8_t>& packed) {
    rn_terrain_mesh_layout wire{};
    for(auto group:groups) ++wire.group_quad_counts[group];
    if(!groups.empty()) {
        wire.flags=RN_TERRAIN_LAYOUT_FINITE_BOUNDS;
        std::fill_n(wire.maximum,3,1.5f);
    }
    if(std::count_if(std::begin(wire.group_quad_counts),std::end(wire.group_quad_counts),[](auto n){return n>0;})>1) {
        packed.assign((groups.size()*3+7)/8,0);
        for(size_t i=0;i<groups.size();++i) {
            auto bit=i*3;
            packed[bit/8]|=uint8_t(groups[i]<<(bit%8));
            if(bit%8>5) packed[bit/8+1]|=uint8_t(groups[i]>>(8-bit%8));
        }
        wire.flags|=RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS;
        wire.original_quad_groups=packed.data();
        wire.original_quad_groups_bytes=uint32_t(packed.size());
    }
    return wire;
}

TEST(TerrainPreanalyzedLayout, PackedWireframeIndicesRestoreOriginalVerticesForEveryFaceMask) {
    const radray::array<uint32_t, 11> groups{6, 0, 5, 2, 0, 1, 4, 3, 6, 2, 5};
    radray::vector<uint8_t> packed;
    auto wire=GroupLayout(groups,packed);
    TerrainMeshLayout layout;
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(wire,uint32_t(4*groups.size()),RN_MATERIAL_OPAQUE,layout));
    const radray::array<uint32_t, 6> vertices{0, 1, 2, 2, 3, 0};
    for (uint32_t mask = 0; mask < 128; ++mask) {
        size_t selected = 0; for (auto g : groups) if (mask & layout.groupBits[g]) ++selected;
        radray::vector<radray::byte> storage(selected * 24 + 2, radray::byte{0x7f});
        ASSERT_TRUE(WriteWireframeIndices(std::span{storage}.subspan(1, selected * 24), layout, mask));
        EXPECT_EQ(storage.front(), radray::byte{0x7f}); EXPECT_EQ(storage.back(), radray::byte{0x7f});
        size_t index = 0;
        for (size_t q = 0; q < groups.size(); ++q) if (mask & layout.groupBits[groups[q]]) for (auto vertex : vertices) {
            uint32_t actual; std::memcpy(&actual, storage.data() + 1 + index++ * 4, 4);
            ASSERT_LT(actual, layout.vertexCount);
            const auto group=groups[q];
            size_t prior=std::count_if(groups.begin(),groups.end(),[&](auto g){return g<group;});
            prior+=std::count(groups.begin(),groups.begin()+q,group);
            EXPECT_EQ(actual,prior*4+vertex);
        }
    }
    EXPECT_FALSE(WriteWireframeIndices({}, layout, 128));
    EXPECT_FALSE(WriteWireframeIndices({}, layout, 127));
}

TEST(TerrainPreanalyzedLayout, SingleGroupNeedsNoRetainedWireframePermutation) {
    TerrainMeshLayout layout;
    radray::vector<uint8_t> packed;
    const radray::array groups{4u};
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(GroupLayout(groups,packed),4,RN_MATERIAL_CUTOUT,layout));
    EXPECT_TRUE(layout.originalQuadGroups.empty());
    radray::array<radray::byte, 24> indices{}, expected{};
    ASSERT_TRUE(WriteWireframeIndices(indices, layout, 1)); ASSERT_TRUE(WriteQuadIndices(expected, 4));
    EXPECT_EQ(indices, expected); EXPECT_TRUE(WriteWireframeIndices({}, layout, 0));
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout({},0,RN_MATERIAL_OPAQUE,layout));
    EXPECT_TRUE(WriteWireframeIndices({}, layout, 0));
}

TEST(TerrainPreanalyzedLayout, InverseProjectionEyeIncludesTranslationAndRejectsInvalidMatrices) {
    rn_terrain_view_desc frame{};
    for (uint32_t i = 0; i < 4; i++) frame.projection[i * 5] = frame.view[i * 5] = 1;
    EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame.projection[10] = frame.projection[15] = 0; frame.projection[11] = -1; frame.projection[14] = 1; frame.projection[12] = -2;
    auto eye = ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0], 2, 1e-6);
    frame.view[12] = -3; eye = ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0], 5, 1e-6);
    frame.projection[0] = std::numeric_limits<float>::infinity(); EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame = {}; EXPECT_FALSE(ComputeTerrainEye(frame).valid);
}

TEST(TerrainPreanalyzedLayout, DecodesGroupRangesAndRejectsMalformedLayouts) {
    const radray::array<uint32_t, 11> groups{6, 0, 5, 2, 0, 1, 4, 3, 6, 2, 5};
    radray::vector<uint8_t> packed;
    auto wire=GroupLayout(groups,packed);
    constexpr uint32_t vertexCount=uint32_t(groups.size())*4;
    TerrainMeshLayout actual;
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(wire,vertexCount,RN_MATERIAL_OPAQUE,actual));
    EXPECT_EQ(actual.vertexCount,vertexCount);
    EXPECT_EQ(actual.originalQuadGroups,packed);
    ASSERT_EQ(actual.ranges.size(),7u);
    uint32_t first=0;
    for(size_t i=0;i<7;++i) {
        EXPECT_EQ(actual.groupBits[i],1u<<i);
        EXPECT_EQ(actual.ranges[i].first,first);
        EXPECT_EQ(actual.ranges[i].count,wire.group_quad_counts[i]*6);
        first+=actual.ranges[i].count;
    }

    // Rejection paths.
    auto badCount = wire; badCount.group_quad_counts[0] += 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badCount, vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badBounds = wire; badBounds.minimum[1] = 100.0f; badBounds.maximum[1] = 1.0f;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badBounds, vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badWf = wire; badWf.original_quad_groups_bytes -= 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badWf, vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badReserved = wire; badReserved.reserved = 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badReserved, vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badTranslucent = wire;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badTranslucent, vertexCount, RN_MATERIAL_TRANSLUCENT, actual));

    // Valid translucent preanalyzed layout (only group 6, no wireframe groups).
    rn_terrain_mesh_layout transWire{};
    transWire.group_quad_counts[6] = 2;
    transWire.flags = RN_TERRAIN_LAYOUT_FINITE_BOUNDS;
    transWire.maximum[0] = transWire.maximum[1] = transWire.maximum[2] = 1.0f;
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(transWire, 8, RN_MATERIAL_TRANSLUCENT, actual));
    ASSERT_EQ(actual.ranges.size(), 1u);
    EXPECT_EQ(actual.ranges[0].count, 12u);
    EXPECT_TRUE(actual.originalQuadGroups.empty());
}

TEST(TerrainHotSlotFastPath, PackedAndWideRangesMatchEveryFaceMask) {
    for (bool wide : {false,true}) {
        TerrainHotSlot hot{};
        hot.fullMask=hot.validMask=127;
        hot.firstVertex=40;
        if(wide) hot.flags|=kTerrainHotFlagWideCounts;
        TerrainMeshLayout layout{};
        for(uint32_t r=0;r<7;++r) {
            const uint32_t quads=(wide && r==0) ? 70000 : r+1;
            layout.ranges.push_back({hot.indexCount,quads*6});
            hot.indexCount+=quads*6;
            if(r<6 && !wide) hot.rangeEndQuads[r]=uint16_t(hot.indexCount/6);
        }
        for(uint32_t mask=0;mask<=127;++mask) {
            for(uint32_t instances : {1u,3u}) {
                radray::vector<DrawIndexedIndirectArguments> actual,reference;
                EmitHotSlotSolidIndirectArgs(hot,wide ? &layout : nullptr,100,mask,7,instances,actual);
                for(uint32_t r=0;r<7;++r) {
                    if(!(mask&(1u<<r))) continue;
                    const auto& range=layout.ranges[r];
                    if(instances==1 && !reference.empty() && reference.back().FirstIndex+reference.back().IndexCount==100+range.first)
                        reference.back().IndexCount+=range.count;
                    else reference.push_back({range.count,instances,100+range.first,40,7});
                }
                ASSERT_EQ(actual.size(),reference.size());
                if(!actual.empty()) EXPECT_EQ(std::memcmp(actual.data(),reference.data(),actual.size()*sizeof(actual[0])),0);
            }
        }
    }
}

TEST(TerrainHotSlotFastPath, CrossingMaskAndContiguousArgumentsMatchReference) {
    const radray::array groups{0u,0u,2u,3u,6u};
    radray::vector<uint8_t> packed;
    TerrainMeshLayout layout;
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(GroupLayout(groups,packed),20,RN_MATERIAL_OPAQUE,layout));

    TerrainHotSlot hot{};
    for (uint32_t a = 0; a < 3; ++a) {
        hot.minBound[a] = float(layout.minimum[a]);
        hot.maxBound[a] = float(layout.maximum[a]);
    }
    hot.firstVertex = 40;
    hot.layer = RN_MATERIAL_OPAQUE;
    hot.fullMask = uint8_t((1u << layout.ranges.size()) - 1u);
    hot.validMask = hot.fullMask;
    hot.flags = kTerrainHotFlagActive | kTerrainHotFlagFiniteBounds | kTerrainHotFlagHasIndices;
    hot.indexCount=layout.vertexCount/4*6;
    for (uint32_t g=0;g<7;++g) hot.groupBits[g]=uint8_t(layout.groupBits[g]);
    for (size_t r=0;r<layout.ranges.size() && r<6;++r)
        hot.rangeEndQuads[r]=uint16_t((layout.ranges[r].first+layout.ranges[r].count)/6);

    rn_terrain_view_desc view{};
    view.camera_block[0] = 0; view.camera_block[1] = 64; view.camera_block[2] = 0;
    view.camera_fraction[0] = 0.2f; view.camera_fraction[1] = 0.2f; view.camera_fraction[2] = 0.5f;
    // Distant chunk at X = 160, Y = 64 (same Y slab -> moving Y across 64.5 must change the cached mask).
    const rn_instance inst{{160, 64, 0}, 1.0f};
    const radray::array<int32_t, 3> camBlock{view.camera_block[0], view.camera_block[1], view.camera_block[2]};
    const radray::array<double, 3> fracEye{view.camera_fraction[0], view.camera_fraction[1], view.camera_fraction[2]};
    TerrainEyeCrossingCache crossing{camBlock,fracEye,{},true};
    const rn_visible_item item{0,UINT32_MAX,0,0,1};
    const uint32_t hotMask = crossing.Capture(item,hot,{&inst,1});
    ASSERT_EQ(crossing.crossingItems.size(),1u);
    const uint32_t refMask = TerrainVisibleMask(layout, {{}, true}, view, {&inst, 1});
    EXPECT_EQ(hotMask, refMask);

    const rn_instance distant{{160, 256, -144}, 1.0f};
    TerrainEyeCrossingCache empty{camBlock, fracEye, {}, true};
    empty.Capture(item, hot, {&distant, 1});
    EXPECT_TRUE(empty.crossingItems.empty());
    EXPECT_TRUE(empty.Matches(camBlock, {0.9, 0.8, 0.1}, true, {&hot, 1}, {&distant, 1}));
    auto general=item;
    general.element_mask=hot.groupBits[6];
    empty.Capture(general,hot,{&inst,1});
    EXPECT_TRUE(empty.crossingItems.empty());

    auto bounded=hot;
    for(uint32_t a=0;a<3;++a)
        if(hot.minBound[a]>=0 && hot.maxBound[a]<=16) bounded.sectionBoundsAxes|=uint8_t(1u<<a);
    for(double x : {-200.0,-2.01,-2.0,0.5,16.0,18.0,18.01,200.0}) {
        const radray::array<double,3> eye{x,0.5,0.5};
        TerrainEyeCrossingCache reference{camBlock,eye,{},true},optimized{camBlock,eye,{},true};
        const rn_instance local{{0,64,0},1};
        EXPECT_EQ(reference.Capture(item,hot,{&local,1}),optimized.Capture(item,bounded,{&local,1}));
        EXPECT_EQ(reference.crossingItems.size(),optimized.crossingItems.size());
    }

    // Motion inside the box keeps an unchanged effective mask.
    rn_terrain_view_desc moved = view;
    moved.camera_fraction[1] = 0.3f;
    EXPECT_TRUE(crossing.Matches({moved.camera_block[0],moved.camera_block[1],moved.camera_block[2]}, {moved.camera_fraction[0], moved.camera_fraction[1], moved.camera_fraction[2]}, true,{&hot,1},{&inst,1}));

    // Crossing the distant chunk's Y=64.0 min plane (camera Y drops below 64.0 - 0.001) changes the cached mask.
    moved.camera_block[1] = 63; moved.camera_fraction[1] = 0.9f;
    EXPECT_FALSE(crossing.Matches({moved.camera_block[0],moved.camera_block[1],moved.camera_block[2]}, {moved.camera_fraction[0], moved.camera_fraction[1], moved.camera_fraction[2]}, true,{&hot,1},{&inst,1}));

    for (double dx : {-0.99, -0.4, 0.0, 0.3, 0.99}) {
        for (double dy : {-0.99, -0.20101, -0.201, -0.20099, 0.0, 0.99}) {
            for (double dz : {-0.99, 0.0, 0.99}) {
                const radray::array<double, 3> testEye{fracEye[0]+dx, fracEye[1]+dy, fracEye[2]+dz};
                const auto expected = ComputeHotSlotVisibleMask(hot, camBlock, testEye, true, {&inst, 1});
                EXPECT_EQ(crossing.Matches(camBlock, testEye, true, {&hot, 1}, {&inst, 1}), expected == hotMask);
            }
        }
    }

    // Direct MDI emission: groups 0 (bit 0, 12 indices) + 2 (bit 1, 6 indices) merge to 18; bit 2 skipped; group 6 (bit 3, 6 indices) separate.
    radray::vector<DrawIndexedIndirectArguments> args;
    EmitHotSlotSolidIndirectArgs(hot, nullptr, 100u, (1u << 0) | (1u << 1) | (1u << 3), 7u, 1u, args);
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].IndexCount, 18u);
    EXPECT_EQ(args[0].FirstIndex, 100u);
    EXPECT_EQ(args[0].VertexOffset, 40);
    EXPECT_EQ(args[0].FirstInstance, 7u);
    EXPECT_EQ(args[0].InstanceCount, 1u);
    EXPECT_EQ(args[1].IndexCount, 6u);
    EXPECT_EQ(args[1].FirstIndex, 124u);

    // Multi-instance does not merge contiguous groups.
    args.clear();
    EmitHotSlotSolidIndirectArgs(hot, nullptr, 100u, (1u << 0) | (1u << 1), 3u, 4u, args);
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].IndexCount, 12u);
    EXPECT_EQ(args[0].InstanceCount, 4u);
    EXPECT_EQ(args[1].IndexCount, 6u);
    EXPECT_EQ(args[1].FirstIndex, 112u);
    EXPECT_EQ(args[1].InstanceCount, 4u);

    // Wide-counts fallback matches normal emission.
    hot.flags |= kTerrainHotFlagWideCounts;
    radray::vector<DrawIndexedIndirectArguments> wideArgs;
    EmitHotSlotSolidIndirectArgs(hot, &layout, 100u, (1u << 0) | (1u << 1) | (1u << 3), 7u, 1u, wideArgs);
    ASSERT_EQ(wideArgs.size(), 2u);
    EXPECT_EQ(wideArgs[0].IndexCount, 18u);
    EXPECT_EQ(wideArgs[1].FirstIndex, 124u);
}
}  // namespace
}  // namespace raynium

int main(int argc, char** argv) {
#ifdef RADRAY_ENABLE_PROFILER
    tracy::StartupProfiler();
#endif
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
#ifdef RADRAY_ENABLE_PROFILER
    tracy::ShutdownProfiler();
#endif
    return result;
}
