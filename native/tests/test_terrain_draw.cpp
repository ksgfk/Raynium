#include <raynium/terrain_draw.h>
#include <raynium/terrain_feature.h>
#include "test_fakes.h"
#include <raynium/terrain_residency.h>
#include <raynium/world_features.h>
#include <raynium/frame_upload.h>
#include <radray/types.h>
#include <radray/profiler.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
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
    void* Map(uint64_t, uint64_t) noexcept override { ++maps; return nullptr; }
    void Unmap() noexcept override { ++unmaps; }
    void FlushMappedRange(BufferRange) noexcept override { ++flushes; }
    void InvalidateMappedRange(BufferRange) noexcept override { ++invalidates; }
    BufferDescriptor GetDesc() const noexcept override { return _desc; }
    Device* GetDevice() const noexcept override { ++deviceQueries; return nullptr; }
    uint32_t maps{}, unmaps{}, flushes{}, invalidates{};
    mutable uint32_t deviceQueries{};
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
    radray::vector<DrawIndexedIndirectArguments> args;
    InstancedGeometryFeature feature;
    auto decode = [&](size_t runCount, uint32_t stride, uint64_t offset, bool mdi) {
        return feature.DecodeRuns(std::as_bytes(std::span(runs).first(runCount)), 3, stride, offset, 168,
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

bool AnalyzeAndWrite(std::span<const radray::byte> source, rn_material_mode layer, TerrainMeshLayout& layout, radray::vector<radray::byte>& bytes) {
    std::pmr::monotonic_buffer_resource memory;
    TerrainMeshUpload upload{memory};
    if (!AnalyzeTerrainMesh(source, layer, layout, upload)) return false;
    bytes.resize(source.size()); return upload.Write(bytes);
}

radray::vector<radray::byte> Quad(uint32_t group, float plane = 0) {
    float points[4][3]{};
    if (group < 6) {
        const uint32_t a = group / 2, b = (a + 1) % 3, c = (a + 2) % 3;
        for (auto& point : points) point[a] = plane;
        points[1][b] = 1; points[2][b] = 1; points[2][c] = 1; points[3][c] = 1;
        if (group & 1) for (uint32_t i = 0; i < 3; i++) std::swap(points[1][i], points[3][i]);
    }
    radray::vector<radray::byte> bytes(112);
    for (uint32_t v = 0; v < 4; ++v) {
        std::memcpy(bytes.data() + v * 28, points[v], 12);
        const uint32_t color = 0x12340000 + group; std::memcpy(bytes.data() + v * 28 + 12, &color, 4);
    }
    return bytes;
}

TEST(TerrainMeshAnalysis, ReordersSevenGroupsWithoutChangingVertexBytesOrWithinGroupOrder) {
    radray::vector<radray::byte> source(13); for (int g = 6; g >= 0; --g) { auto q = Quad(g); source.insert(source.end(), q.begin(), q.end()); }
    auto q = Quad(0, 2); source.insert(source.end(), q.begin(), q.end());
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(std::span{source}.subspan(13), RN_MATERIAL_OPAQUE, layout, bytes));
    ASSERT_EQ(layout.ranges.size(), 7); EXPECT_EQ(layout.ranges[0].count, 12);
    EXPECT_EQ(layout.groupBits, (radray::array<uint32_t, 7>{1, 2, 4, 8, 16, 32, 64}));
    radray::vector<radray::byte> expected;
    for (uint32_t g = 0; g < 7; g++) { auto first = Quad(g); expected.insert(expected.end(), first.begin(), first.end()); if (g == 0) expected.insert(expected.end(), q.begin(), q.end()); }
    EXPECT_EQ(bytes, expected);
    rn_terrain_view_desc frame{}; rn_instance instance{};
    frame.camera_fraction[0] = frame.camera_fraction[1] = frame.camera_fraction[2] = -2;
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&instance, 1}), 0b1101010u);
    frame.camera_fraction[0] = 3; frame.camera_fraction[1] = frame.camera_fraction[2] = 2;
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&instance, 1}), 0b1010101u);
}

TEST(TerrainMeshAnalysis, OutlyingBoundsSparseGroupsAndEveryInstanceRemainConservative) {
    auto source = Quad(0, 32); TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_CUTOUT, layout, bytes));
    EXPECT_EQ(layout.minimum[0], 32); EXPECT_EQ(layout.maximum[0], 32);
    rn_terrain_view_desc frame{}; const rn_instance instances[]{{{-31, 0, 0}, 1}, {{-33, 0, 0}, 1}};
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {instances, 1}), 0u);
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, instances), 1u);
    frame.camera_block[0] = INT32_MAX; rn_instance far{{INT32_MAX - 32, 0, 0}, 1};
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&far, 1}), 1u);
    frame.camera_fraction[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&far, 1}), 1u);
    source = Quad(5); auto second = Quad(2); source.insert(source.end(), second.begin(), second.end());
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, bytes));
    EXPECT_EQ(layout.groupBits[2], 1); EXPECT_EQ(layout.groupBits[5], 2);
    frame = {}; frame.camera_fraction[1] = -2; frame.camera_fraction[2] = 2; rn_instance origin{};
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&origin, 1}), 0u);
}

TEST(TerrainMeshAnalysis, NonplanarInconsistentDegenerateAndNonfiniteQuadsStayGeneral) {
    auto source = Quad(0); float value = .1f; std::memcpy(source.data() + 56, &value, 4);
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, bytes)); EXPECT_EQ(layout.groupBits[6], 1);
    source = Quad(0); value = 2; std::memcpy(source.data() + 84 + 4, &value, 4);
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, bytes)); EXPECT_EQ(layout.groupBits[6], 1);
    source = Quad(6); ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, bytes)); EXPECT_EQ(layout.groupBits[6], 1);
    source = Quad(0); value = std::numeric_limits<float>::quiet_NaN(); std::memcpy(source.data(), &value, 4);
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, bytes)); EXPECT_FALSE(layout.finiteBounds);
    rn_terrain_view_desc frame{}; rn_instance instance{};
    EXPECT_EQ(TerrainVisibleMask(layout, {{}, true}, frame, {&instance, 1}), 1u);
    EXPECT_TRUE(AnalyzeAndWrite({}, RN_MATERIAL_OPAQUE, layout, bytes)); EXPECT_TRUE(layout.ranges.empty());
    EXPECT_FALSE(AnalyzeAndWrite(std::span{source}.first(111), RN_MATERIAL_OPAQUE, layout, bytes));
}

TEST(TerrainMeshAnalysis, TranslucencyPreservesCompilerOrder) {
    auto source = Quad(5); auto q = Quad(0); source.insert(source.end(), q.begin(), q.end());
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_TRANSLUCENT, layout, bytes));
    EXPECT_EQ(bytes, source); ASSERT_EQ(layout.ranges.size(), 1); EXPECT_EQ(layout.ranges[0].count, 12);
    EXPECT_TRUE(layout.originalQuadGroups.empty());
}

TEST(TerrainMeshAnalysis, PackedWireframeIndicesRestoreOriginalVerticesForEveryFaceMask) {
    const radray::array<uint32_t, 11> groups{6, 0, 5, 2, 0, 1, 4, 3, 6, 2, 5};
    radray::vector<radray::byte> source;
    for (size_t n = 0; n < groups.size(); ++n) { auto q = Quad(groups[n], float(n)); source.insert(source.end(), q.begin(), q.end()); }
    TerrainMeshLayout layout; radray::vector<radray::byte> grouped;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, grouped));
    ASSERT_EQ(layout.originalQuadGroups.size(), 5u);
    ASSERT_NE(grouped, source);
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
            EXPECT_EQ(std::memcmp(grouped.data() + actual * 28, source.data() + (q * 4 + vertex) * 28, 28), 0);
        }
    }
    EXPECT_FALSE(WriteWireframeIndices({}, layout, 128));
    EXPECT_FALSE(WriteWireframeIndices({}, layout, 127));
}

TEST(TerrainMeshAnalysis, SingleGroupNeedsNoRetainedWireframePermutation) {
    TerrainMeshLayout layout; radray::vector<radray::byte> grouped;
    ASSERT_TRUE(AnalyzeAndWrite(Quad(4), RN_MATERIAL_CUTOUT, layout, grouped));
    EXPECT_TRUE(layout.originalQuadGroups.empty());
    radray::array<radray::byte, 24> indices{}, expected{};
    ASSERT_TRUE(WriteWireframeIndices(indices, layout, 1)); ASSERT_TRUE(WriteQuadIndices(expected, 4));
    EXPECT_EQ(indices, expected); EXPECT_TRUE(WriteWireframeIndices({}, layout, 0));
    ASSERT_TRUE(AnalyzeAndWrite({}, RN_MATERIAL_OPAQUE, layout, grouped));
    EXPECT_TRUE(WriteWireframeIndices({}, layout, 0));
}

TEST(TerrainMeshAnalysis, InverseProjectionEyeIncludesTranslationAndRejectsInvalidMatrices) {
    rn_terrain_view_desc frame{};
    for (uint32_t i = 0; i < 4; i++) frame.projection[i * 5] = frame.view[i * 5] = 1;
    EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame.projection[10] = frame.projection[15] = 0; frame.projection[11] = -1; frame.projection[14] = 1; frame.projection[12] = -2;
    auto eye = ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0], 2, 1e-6);
    frame.view[12] = -3; eye = ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0], 5, 1e-6);
    frame.projection[0] = std::numeric_limits<float>::infinity(); EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame = {}; EXPECT_FALSE(ComputeTerrainEye(frame).valid);
}

TEST(TerrainPreanalyzedLayout, MatchesAnalyzeTerrainMeshAndRejectsMalformedLayouts) {
    const radray::array<uint32_t, 11> groups{6, 0, 5, 2, 0, 1, 4, 3, 6, 2, 5};
    radray::vector<radray::byte> source;
    for (size_t n = 0; n < groups.size(); ++n) {
        auto q = Quad(groups[n], float(n));
        source.insert(source.end(), q.begin(), q.end());
    }
    TerrainMeshLayout expected;
    radray::vector<radray::byte> grouped;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, expected, grouped));

    rn_terrain_mesh_layout wire{};
    for (uint32_t g : groups) ++wire.group_quad_counts[g];
    wire.flags = RN_TERRAIN_LAYOUT_FINITE_BOUNDS | RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS;
    for (uint32_t a = 0; a < 3; ++a) {
        wire.minimum[a] = float(expected.minimum[a]);
        wire.maximum[a] = float(expected.maximum[a]);
    }
    wire.original_quad_groups = expected.originalQuadGroups.data();
    wire.original_quad_groups_bytes = uint32_t(expected.originalQuadGroups.size());

    TerrainMeshLayout actual;
    ASSERT_TRUE(BuildPreanalyzedTerrainMeshLayout(wire, expected.vertexCount, RN_MATERIAL_OPAQUE, actual));
    EXPECT_EQ(actual.vertexCount, expected.vertexCount);
    EXPECT_EQ(actual.layer, expected.layer);
    EXPECT_EQ(actual.finiteBounds, expected.finiteBounds);
    EXPECT_EQ(actual.groupBits, expected.groupBits);
    EXPECT_EQ(actual.originalQuadGroups, expected.originalQuadGroups);
    ASSERT_EQ(actual.ranges.size(), expected.ranges.size());
    for (size_t i = 0; i < expected.ranges.size(); ++i) {
        EXPECT_EQ(actual.ranges[i].first, expected.ranges[i].first);
        EXPECT_EQ(actual.ranges[i].count, expected.ranges[i].count);
    }
    for (uint32_t a = 0; a < 3; ++a) {
        EXPECT_DOUBLE_EQ(actual.minimum[a], expected.minimum[a]);
        EXPECT_DOUBLE_EQ(actual.maximum[a], expected.maximum[a]);
    }

    // Rejection paths.
    auto badCount = wire; badCount.group_quad_counts[0] += 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badCount, expected.vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badBounds = wire; badBounds.minimum[1] = 100.0f; badBounds.maximum[1] = 1.0f;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badBounds, expected.vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badWf = wire; badWf.original_quad_groups_bytes -= 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badWf, expected.vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badReserved = wire; badReserved.reserved = 1;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badReserved, expected.vertexCount, RN_MATERIAL_OPAQUE, actual));
    auto badTranslucent = wire;
    EXPECT_FALSE(BuildPreanalyzedTerrainMeshLayout(badTranslucent, expected.vertexCount, RN_MATERIAL_TRANSLUCENT, actual));

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
    radray::vector<radray::byte> source;
    // Sparse groups: 0 (+X, 2 quads), 2 (+Y, 1 quad), 3 (-Y, 1 quad), 6 (general, 1 quad).
    for (auto q : {Quad(0, 0), Quad(0, 1), Quad(2, 0.5f), Quad(3, 1.5f), Quad(6, 0)}) {
        source.insert(source.end(), q.begin(), q.end());
    }
    TerrainMeshLayout layout;
    radray::vector<radray::byte> grouped;
    ASSERT_TRUE(AnalyzeAndWrite(source, RN_MATERIAL_OPAQUE, layout, grouped));

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
