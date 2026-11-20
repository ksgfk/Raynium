#include <raynium/frame_record.h>
#include <raynium/mesh_pass.h>
#include <gtest/gtest.h>
#include <cstring>

namespace raynium {
namespace {
using namespace radray::gpu;
class BufferProbe final : public Buffer {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    void* Map(uint64_t,uint64_t) noexcept override { ++maps; return nullptr; }
    void Unmap() noexcept override { ++unmaps; }
    void FlushMappedRange(BufferRange) noexcept override { ++flushes; }
    void InvalidateMappedRange(BufferRange) noexcept override { ++invalidates; }
    BufferDescriptor GetDesc() const noexcept override { return {.Size=16384}; }
    Device* GetDevice() const noexcept override { ++deviceQueries; return nullptr; }
    uint32_t maps{},unmaps{},flushes{},invalidates{};
    mutable uint32_t deviceQueries{};
};
class ParametersProbe final : public ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t,uint32_t,ShaderParameterValue value) noexcept override { ++writes; last=std::get<ShaderBufferBinding>(value); return true; }
    bool FlushWrites() noexcept override { ++flushes; return true; }
    void ResetBindingsForReuse() noexcept override { ++resets; }
    uint32_t writes{},flushes{},resets{};
    ShaderBufferBinding last{};
};
class EncoderProbe final : public GraphicsCommandEncoder {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    CommandBuffer* GetCommandBuffer() const noexcept override { return nullptr; }
    void SetViewport(radray::Viewport) noexcept override {}
    void SetScissor(radray::Rect) noexcept override {}
    void SetStencilReference(uint8_t) noexcept override {}
    void BindGraphicsPipelineState(GraphicsPipelineState*) noexcept override {}
    void BindVertexBuffers(std::span<const VertexBufferBinding> values) noexcept override { for(auto value:values) vertices.push_back(value); }
    void BindIndexBuffer(IndexBufferView value) noexcept override { indices.push_back(value); }
    void BindShaderParameterSet(uint32_t,ShaderParameterSet*,std::span<const ShaderParameterDynamicOffset>) noexcept override { ++parameterBinds; }
    bool SetPushConstants(uint32_t,uint32_t,std::span<const radray::byte>) noexcept override { return true; }
    void DrawIndexed(uint32_t count,uint32_t instances,uint32_t first,int32_t vertex,uint32_t instance) noexcept override { direct.push_back({count,instances,first,vertex,instance}); }
    void Draw(uint32_t,uint32_t,uint32_t,uint32_t) noexcept override {}
    void DrawIndirect(Buffer*,uint64_t,uint32_t) noexcept override {}
    void DrawIndexedIndirect(Buffer* buffer,uint64_t offset,uint32_t count) noexcept override { indirect.push_back({buffer,offset,count}); }
    struct Indirect { Buffer* buffer; uint64_t offset; uint32_t count; };
    radray::vector<VertexBufferBinding> vertices;
    radray::vector<IndexBufferView> indices;
    radray::vector<DrawIndexedIndirectArguments> direct;
    radray::vector<Indirect> indirect;
    uint32_t parameterBinds{};
};
void ExpectReadOnly(const BufferProbe& buffer,const ParametersProbe& parameters) {
    EXPECT_EQ(buffer.maps+buffer.unmaps+buffer.flushes+buffer.invalidates+buffer.deviceQueries,0u);
    EXPECT_EQ(parameters.writes+parameters.flushes+parameters.resets,0u);
}
TEST(TerrainFrameRecord, FrameConstantCarriesBindingAndPhysicalSlice) {
    BufferProbe buffer;
    const auto entry=FrameConstantBinding(31,{&buffer,512,256});
    EXPECT_EQ(entry.BindingId,31u); EXPECT_EQ(entry.ArrayElement,0u);
    const auto value=std::get<ShaderBufferBinding>(entry.Value);
    EXPECT_EQ(value.Target,&buffer); EXPECT_EQ(value.Range.Offset,512u); EXPECT_EQ(value.Range.Size,256u);
}
TEST(TerrainFrameUpload, QuadIndicesWriteOnlyTheFinalUnalignedRange) {
    radray::vector<radray::byte> storage(50,radray::byte{0x7f});
    ASSERT_TRUE(WriteQuadIndices(std::span{storage}.subspan(1,48),8));
    const radray::array<uint32_t,12> expected{0,1,2,2,3,0,4,5,6,6,7,4};
    EXPECT_EQ(std::memcmp(storage.data()+1,expected.data(),48),0);
    EXPECT_EQ(storage.front(),radray::byte{0x7f}); EXPECT_EQ(storage.back(),radray::byte{0x7f});
    EXPECT_FALSE(WriteQuadIndices(std::span{storage}.subspan(1,47),8));
    EXPECT_FALSE(WriteQuadIndices({},1)); EXPECT_TRUE(WriteQuadIndices({},0));
}
TEST(SceneFrameUpload, ExpandedLinesPreserveBothEndpointSides) {
    radray::array<radray::byte,50> storage{};
    storage.fill(radray::byte{0x7f});
    ASSERT_TRUE(WriteLineIndices(std::span{storage}.subspan(1,48),8));
    const radray::array<uint32_t,12> expected{0,1,2,3,2,1,4,5,6,7,6,5};
    EXPECT_EQ(std::memcmp(storage.data()+1,expected.data(),48),0);
    EXPECT_EQ(storage.front(),radray::byte{0x7f}); EXPECT_EQ(storage.back(),radray::byte{0x7f});
    EXPECT_FALSE(WriteLineIndices({},2)); EXPECT_TRUE(WriteLineIndices({},0));
}
TEST(TerrainFrameRecord, DirectAndIndirectConsumePreparedListsWithoutResourceMutation) {
    BufferProbe buffer; MeshMaterialBinding material;
    auto parameters=radray::make_unique<ParametersProbe>(); auto* probe=parameters.get(); material.parameters=std::move(parameters);
    TerrainFrameLists lists;
    for(uint32_t i=0;i<6;++i) lists.arguments.push_back({6,1,3+i*6,17,2});
    lists.batches.push_back({{&buffer,64,512},{&buffer,128,4},&material,1,5});
    lists.layers[0]={0,1,1,5,5,30};
    TerrainRecord data{&lists,0,2,false,{&buffer,4096,32},{&buffer,8192,120}};
    EncoderProbe direct; ASSERT_TRUE(RecordTerrainDraws(data,direct));
    ASSERT_EQ(direct.direct.size(),5u); EXPECT_EQ(direct.direct.front().FirstIndex,9u); EXPECT_EQ(direct.direct.back().FirstIndex,33u);
    ASSERT_EQ(direct.vertices.size(),2u); EXPECT_EQ(direct.vertices[0].View.Offset,4096u);
    EXPECT_EQ(direct.indices[0].Offset,128u);
    data.useIndirect=true; EncoderProbe indirect; ASSERT_TRUE(RecordTerrainDraws(data,indirect));
    ASSERT_EQ(indirect.indirect.size(),3u);
    EXPECT_EQ(indirect.indirect[0].offset,8212u); EXPECT_EQ(indirect.indirect[1].offset,8252u); EXPECT_EQ(indirect.indirect[2].offset,8292u);
    EXPECT_EQ(indirect.indirect[0].count,2u); EXPECT_EQ(indirect.indirect[2].count,1u);
    EXPECT_EQ(lists.arguments.size(),6u); EXPECT_EQ(lists.batches[0].first,1u);
    ExpectReadOnly(buffer,*probe);
}
}
}
