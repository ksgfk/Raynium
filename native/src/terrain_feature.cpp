#include <raynium/terrain_feature.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include <radray/inline_vector.h>
#include <radray/profiler.h>
#include <raynium/raster_records.h>

namespace raynium {
bool SubmitPreparedTerrainDirectDraws(
    radray::gpu::GraphicsCommandEncoder& encoder,
    std::span<const radray::VisibleMeshDrawCommand> reps,
    const radray::PreparedMeshPassIndirectDraws& draws,
    const radray::MeshPassSubmitArgs& bindings) noexcept {
    const radray::gpu::GpuDeviceCapabilities direct{};
    return radray::SubmitMeshPassIndirectDraws(encoder, reps, draws, {}, {}, direct, bindings);
}

TerrainView::TerrainView(std::pmr::memory_resource& memory)
    : Uploads(&memory) {}

radray::RDGBufferHandle TerrainView::Constants(radray::RDGBuilder& graph, uint32_t width, uint32_t height) {
    if (width == AtlasWidth && height == AtlasHeight) return ConstantsBuffer;
    AtlasWidth = width;
    AtlasHeight = height;
    struct Source {
        const rn_terrain_view_desc* Parameters;
        radray::gpu::GpuBackend Backend;
        uint32_t Width;
        uint32_t Height;
    };
    ConstantsBuffer = graph.CreateDynamicBuffer(
        {.MaxSize = 256, .Usage = radray::gpu::BufferUse::CBuffer},
        radray::RDGUploadSource::Generated(
            256,
            Source{&Frame.Parameters, Backend, width, height},
            +[](const Source& source, radray::RDGUploadWriteContext& ctx) noexcept {
                struct ConstantsData {
                    float Projection[16];
                    float View[16];
                    int32_t Block[4];
                    float Fraction[4];
                    float Fog[4];
                    float Distances[4];
                    float Atlas[4];
                    uint32_t Fade[4];
                } data{};
                static_assert(sizeof(ConstantsData) == 224 && offsetof(ConstantsData, Fade) == 208);
                const auto& p = *source.Parameters;
                std::copy_n(p.projection, 16, data.Projection);
                std::copy_n(p.view, 16, data.View);
                std::copy_n(p.camera_block, 3, data.Block);
                data.Block[3] = (p.flags & RN_FRAME_RGSS) != 0;
                std::copy_n(p.camera_fraction, 3, data.Fraction);
                data.Fraction[3] = source.Backend == radray::gpu::GpuBackend::Vulkan ? 1.f : -1.f;
                std::copy_n(p.fog_color, 4, data.Fog);
                std::copy_n(p.fog_distances, 4, data.Distances);
                data.Atlas[0] = float(source.Width);
                data.Atlas[1] = float(source.Height);
                data.Atlas[2] = 1.f / data.Atlas[0];
                data.Atlas[3] = 1.f / data.Atlas[1];
                data.Fade[0] = p.fade_now_ms;
                data.Fade[1] = p.fade_duration_ms;
                data.Fade[2] = std::bit_cast<uint32_t>(p.fade_duration_ms ? 1.f / float(p.fade_duration_ms) : 0.f);
                std::memset(ctx.Bytes.data(), 0, ctx.Bytes.size());
                std::memcpy(ctx.Bytes.data(), &data, sizeof(data));
                return true;
            }));
    return ConstantsBuffer;
}

void TerrainView::Import(radray::RDGBuilder& graph, radray::gpu::GpuBackend gpuBackend) {
    Backend = gpuBackend;
    for (auto& upload : Uploads) ImportGeometryUpload(upload, graph);
    Uploads.clear();
    if (!Draws || !Draws->HasAnyDraws) return;
    const auto import = [&](const radray::IntrusivePtr<radray::GpuBuffer>& buffer, bool upload,
                            std::span<const radray::byte> bytes, radray::gpu::BufferUses usage) {
        if (!buffer) {
            return graph.CreateDynamicBuffer(
                {.MaxSize = bytes.size(), .Usage = usage, .Residency = StreamResidency(bytes.size())},
                radray::RDGUploadSource::BorrowedBytes(bytes));
        }
        auto handle = graph.ImportPersistentOwned(buffer);
        if (upload) {
            graph.UploadBuffer(handle, 0, radray::RDGUploadSource::BorrowedBytes(bytes));
            graph.MarkOutput(handle);
        }
        return handle;
    };
    Instances = import(Frame.PersistentInstances, Frame.UploadInstances, std::as_bytes(Frame.Instances), radray::gpu::BufferUse::Vertex);
    if (UseIndirect) {
        for (uint32_t layer = 0; layer < 3; ++layer) {
            const auto& args = Draws->Layers[layer].Prepared.IndexedArguments;
            if (!args.empty()) {
                IndirectByLayer[layer] = import(
                    Frame.PersistentIndirect[layer],
                    Frame.UploadIndirect[layer],
                    std::as_bytes(std::span(args)),
                    radray::gpu::BufferUse::Indirect);
            }
        }
    }
}

bool TerrainFeature::Decode(std::span<const radray::byte> bytes, std::span<TerrainView> views, rn_renderer* owner, const radray::Rect& area) {
    wire::Terrain w{};
    if (bytes.size() != sizeof(w)) return false;
    std::memcpy(&w, bytes.data(), sizeof(w));
    if (w.View >= views.size() || w.Stage > OitAccumulate || w.Scissor.X < area.X || w.Scissor.Y < area.Y ||
        uint64_t(w.Scissor.X) + w.Scissor.Width > uint64_t(area.X) + area.Width ||
        uint64_t(w.Scissor.Y) + w.Scissor.Height > uint64_t(area.Y) + area.Height) {
        return false;
    }
    View = &views[w.View];
    Stage = w.Stage;
    Scissor = w.Scissor;
    for (uint32_t i = 0; i < 2; ++i) {
        const auto& m = w.Materials[i];
        const auto* image = ResolveHandle(owner, m.Id);
        if (!image || image->Desc.array_layers != 1 || radray::gpu::IsDepthStencilFormat(image->Texture->GetDesc().Format) ||
            !m.Sampling.mip_count || uint64_t(m.Sampling.base_mip) + m.Sampling.mip_count > image->Desc.mip_levels) {
            return false;
        }
        Materials[i] = {image, m.Sampling, m.Flags, {}};
    }
    const uint32_t required = Stage == OitTransmittance ? 1 : Stage == OitAccumulate ? 3
                                                                                     : 0;
    for (uint32_t i = 0; i < required; ++i) {
        auto* image = ResolveHandle(owner, w.Ids[i]);
        if (!image || w.Mips[i] >= image->Desc.mip_levels || image->Desc.array_layers != 1) return false;
        Sources[i] = image;
        Mips[i] = w.Mips[i];
    }
    return true;
}

void TerrainFeature::Import(radray::RDGBuilder& graph, FrameTextureImports& imports) {
    if (View->Draws->Layers[std::min(Stage, uint32_t(Translucent))].RecordCount) {
        const auto& atlas = Materials[0];
        FrameConstants = View->Constants(
            graph,
            std::max(1u, atlas.Image->Desc.width >> atlas.Sampling.base_mip),
            std::max(1u, atlas.Image->Desc.height >> atlas.Sampling.base_mip));
    }
    for (auto& m : Materials) m.Handle = imports.Import(m.Image);
    for (uint32_t i = 0; i < 3; ++i) {
        if (Sources[i]) Reads[i] = imports.Import(Sources[i]);
    }
}

void TerrainFeature::Declare(radray::RDGRasterPassBuilder& pass) const {
    const uint32_t layer = std::min(Stage, uint32_t(Translucent));
    if (!View->Draws->Layers[layer].RecordCount) return;
    pass.ReadBuffer(FrameConstants, radray::gpu::BufferState::CBuffer).ReadBuffer(View->Instances, radray::gpu::BufferState::Vertex);
    if (View->UseIndirect) pass.ReadBuffer(View->IndirectByLayer[layer], radray::gpu::BufferState::Indirect);
    for (const auto& m : Materials) pass.ReadTexture(m.Handle);
    for (auto h : Reads) {
        if (h.IsValid()) pass.ReadTexture(h);
    }
}

bool TerrainFeature::Prepare(radray::RDGPrepareContext& ctx) {
    const uint32_t layer = std::min(Stage, uint32_t(Translucent));
    const auto& layerData = View->Draws->Layers[layer];
    if (!layerData.RecordCount) return true;
    const bool isOitStage = Stage >= OitBounds;
    if (layer == Translucent && isOitStage != View->Draws->Oit) return false;
    auto& slot = ctx.GetSlot();
    InstanceSlice = ctx.ResolveBuffer(View->Instances);
    InstanceSlice.Size = View->Frame.Instances.size_bytes();
    if (View->UseIndirect) {
        const auto base = ctx.ResolveBuffer(View->IndirectByLayer[layer]);
        IndirectSlice = {base.Buffer, base.Offset, layerData.IndirectSize};
    }
    auto binding = FrameConstantBinding(0, ctx.ResolveBuffer(FrameConstants));
    Constants = slot.AcquireParameterSet(View->Mesh->PassGroupLayout(), {&binding, 1}).Get();
    if (!Constants) return false;
    radray::array<radray::TransientParameterBinding, 4> materialBindings;
    for (uint32_t i = 0; i < 2; ++i) {
        const auto& m = Materials[i];
        auto* srv = GetSamplingView(slot.GetDevice(), *m.Image->Texture, m.Sampling);
        auto* sampler = GetTextureSampler(slot.GetDevice(), m.Flags, m.Sampling);
        if (!srv || !sampler) return false;
        materialBindings[i] = {1 + i, 0, srv};
        materialBindings[i + 2] = {3 + i, 0, sampler};
    }
    Material = slot.AcquireParameterSet(View->Mesh->MaterialGroupLayout(), materialBindings).Get();
    if (!Material) return false;
    if (Sources[0]) {
        radray::array<radray::TransientParameterBinding, 4> bindings;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t j = i < 3 && Sources[i] ? i : 0;
            auto* srv = GetSamplingView(slot.GetDevice(), *Sources[j]->Texture, {Mips[j], 1, 0, 1});
            if (!srv) return false;
            bindings[i] = {i, 0, srv};
        }
        Oit = slot.AcquireParameterSet(View->Mesh->OitGroupLayout(), bindings).Get();
        if (!Oit) return false;
    }
    Active = true;
    return true;
}

bool TerrainFeature::Record(radray::gpu::GraphicsCommandEncoder& enc) const {
    if (!Active) return true;
    RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Record");
    enc.SetScissor(Scissor);
    radray::InlineVector<radray::MeshShaderParameterBinding, 3> passSets;
    passSets.push_back({0, Constants});
    passSets.push_back({1, Material});
    if (Oit) passSets.push_back({2, Oit});
    radray::MeshPassSubmitArgs submitArgs{};
    submitArgs.ParameterSets = {passSets.data(), passSets.size()};
    submitArgs.InstanceStream = radray::gpu::VertexBufferBinding{1, FrameVertexView(InstanceSlice)};
    const uint32_t layer = std::min(Stage, uint32_t(Translucent));
    const auto& layerData = View->Draws->Layers[layer];
    const auto& reps = Stage >= OitBounds ? View->Draws->OitIndirectReps[Stage - OitBounds] : layerData.IndirectReps;
    if (!View->UseIndirect) return SubmitPreparedTerrainDirectDraws(enc, reps, layerData.Prepared, submitArgs);
    return radray::SubmitMeshPassIndirectDraws(enc, reps, layerData.Prepared, IndirectSlice, {}, *View->Capabilities, submitArgs);
}
}  // namespace raynium
