#include <raynium/world_features.h>

#include <algorithm>
#include <cstring>

#include <radray/logger.h>
#include <raynium/frame_upload.h>
#include <raynium/raster_records.h>

namespace raynium {
ShaderResourceBindings::Binding ShaderResourceBindings::ReadBinding(const radray::byte* base, size_t index) noexcept {
    Binding b{};
    std::memcpy(&b, base + index * sizeof(Binding), sizeof(Binding));
    return b;
}

size_t ShaderResourceBindings::BindingCount() const noexcept {
    return Resources.size() + 1;
}

bool ShaderResourceBindings::Decode(
    std::pmr::memory_resource& memory,
    rn_pipeline* pipelineId,
    const radray::Rect& scissorRect,
    uint32_t bindingCount,
    uint32_t pushBytes,
    std::span<const radray::byte> bindingSpan,
    std::span<const radray::byte> pushData,
    rn_renderer* owner,
    std::span<const radray::gpu::TextureFormat> colors,
    const radray::Rect& area) {
    if (bindingCount > 256 || pushBytes % 4 || bindingSpan.size() != uint64_t(bindingCount) * sizeof(Binding) ||
        pushData.size() < pushBytes || scissorRect.X < area.X || scissorRect.Y < area.Y ||
        uint64_t(scissorRect.X) + scissorRect.Width > uint64_t(area.X) + area.Width ||
        uint64_t(scissorRect.Y) + scissorRect.Height > uint64_t(area.Y) + area.Height) {
        return false;
    }
    auto* handle = ResolveHandle(owner, pipelineId);
    Pipeline = handle ? handle->Impl.get() : nullptr;
    if (!Pipeline || Pipeline->Program->DrawBasesLayout || colors.size() != Pipeline->State.Colors.size()) return false;
    for (size_t i = 0; i < colors.size(); ++i) {
        if (colors[i] != Pipeline->State.Colors[i].Format) return false;
    }
    Scissor = scissorRect;
    BindingBytes = bindingSpan.data();
    Resources = FrameArray<Resource>(memory, bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        auto& r = Resources[i];
        const auto b = ReadBinding(BindingBytes, i);
        r.Kind = b.Kind;
        if (b.Kind == 5) {
            auto* image = ResolveHandle(owner, reinterpret_cast<rn_texture*>(b.Resource));
            if (!image || !b.Sampling.mip_count || b.Sampling.base_mip >= image->Desc.mip_levels ||
                b.Sampling.mip_count > image->Desc.mip_levels - b.Sampling.base_mip) {
                return false;
            }
            r.Texture = image;
        } else if (b.Kind == 3 || b.Kind == 4) {
            const uint32_t reqUsage = b.Kind == 3 ? RN_BUFFER_UNIFORM : RN_BUFFER_TEXEL;
            r.Buffer = ResolveHandle(owner, reinterpret_cast<rn_buffer*>(b.Resource));
            if (!r.Buffer || !(r.Buffer->Usage & reqUsage) || !BufferContainsRange(*r.Buffer, b.Offset, b.Size)) return false;
        } else {
            return false;
        }
    }
    Push = FramePushConstants(memory, pushData.first(pushBytes), Pipeline->Program->PushBytes);
    return true;
}

void ShaderResourceBindings::Import(radray::RDGBuilder& graph, FrameTextureImports& imports) {
    for (auto& r : Resources) {
        if (r.Kind == 5)
            r.ImageHandle = imports.Import(r.Texture);
        else
            r.BufferHandle = ImportBuffer(*r.Buffer, graph);
    }
}

void ShaderResourceBindings::Declare(radray::RDGRasterPassBuilder& pass) const {
    for (const auto& r : Resources) {
        if (r.Kind == 5)
            pass.ReadTexture(r.ImageHandle);
        else
            pass.ReadBuffer(r.BufferHandle, r.Kind == 3 ? radray::gpu::BufferState::CBuffer : radray::gpu::BufferState::ShaderRead);
    }
}

bool ShaderResourceBindings::Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth) {
    auto& slot = ctx.GetSlot();
    auto& device = slot.GetDevice();
    Pso = Pipeline->Resolve(device, depth);
    if (!Pso) return false;
    const auto& program = *Pipeline->Program;
    FrameParameterBindings bindings(slot);
    for (size_t i = 0; i < Resources.size(); ++i) {
        const auto& r = Resources[i];
        const auto b = ReadBinding(BindingBytes, i);
        if (r.Kind == 5) {
            if (program.RequiresBinding(1, b.Slot)) {
                auto* view = GetSamplingView(device, *r.Texture->Texture, b.Sampling);
                if (!view) return false;
                bindings.Group(1).push_back({b.Slot, 0, view});
            }
            if (program.HasSampler(b.Slot + 1u)) {
                auto* sampler = GetTextureSampler(device, b.Format, b.Sampling);
                if (!sampler) return false;
                bindings.Group(1).push_back({b.Slot + 1u, 0, sampler});
            }
            continue;
        }
        if (r.Kind == 3) {
            if (program.RequiresBinding(0, b.Slot)) {
                auto slice = ctx.ResolveBuffer(r.BufferHandle);
                slice.Offset += b.Offset;
                slice.Size = b.Size;
                bindings.Group(0).push_back(FrameConstantBinding(b.Slot, slice));
            }
        } else if (program.RequiresBinding(1, b.Slot)) {
            auto slice = ctx.ResolveBuffer(r.BufferHandle);
            slice.Offset += b.Offset;
            slice.Size = b.Size;
            bindings.Group(1).push_back({
                b.Slot,
                0,
                radray::gpu::ShaderTexelBufferBinding{slice.Buffer.Get(), {slice.Offset, slice.Size}, ToTextureFormat(rn_pixel_format(b.Format))},
            });
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (!program.Groups[i]) continue;
        auto& group = bindings.Group(i);
        EnsureOrderedBindings(group);
        Sets[i] = slot.AcquireParameterSet(program.Groups[i], {group.data(), group.size()}).Get();
        if (!Sets[i]) return false;
    }
    return true;
}

bool ShaderResourceBindings::Bind(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept {
    cache.BindPipeline(enc, Pso);
    cache.SetScissor(enc, Scissor);
    cache.SetStencil(enc, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        if (Sets[i]) cache.BindParameterSet(enc, i, Sets[i]);
    }
    if (!Push.empty() && !enc.SetPushConstants(0, 0, {Push.data(), Push.size()})) return false;
    return true;
}

const radray::VertexBufferPool* InstancedGeometryFeature::StaticVertexPool() const noexcept {
    return _staticVertexPool;
}

const radray::IndexBufferPool* InstancedGeometryFeature::IndexPool() const noexcept {
    return _indexPool;
}

bool InstancedGeometryFeature::UsesIndirect() const noexcept {
    return _usesIndirect;
}

size_t InstancedGeometryFeature::BindingCount() const noexcept {
    return _shader.BindingCount();
}

bool InstancedGeometryFeature::Decode(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> bytes,
    std::span<const radray::byte> instanceUploads,
    std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
    bool canIndirect,
    uint32_t indirectLimit,
    rn_renderer* owner,
    const StaticGeometryRegistry& staticGeometry,
    const TerrainResidency& terrain,
    std::span<const radray::gpu::TextureFormat> colors,
    const radray::Rect& area) {
    static_assert(sizeof(WireRun) == 16);
    wire::Geometry h{};
    if (!wire::ReadRasterHeader(bytes, h) || h.UploadOffset > instanceUploads.size() ||
        uint64_t(h.InstanceCount) * h.InstanceStride > instanceUploads.size() - h.UploadOffset) {
        return false;
    }
    const uint64_t bindingBytes = uint64_t(h.Bindings) * sizeof(ShaderResourceBindings::Binding);
    const uint64_t runBytes = uint64_t(h.RunCount) * sizeof(WireRun);
    const uint64_t pushAligned = (uint64_t(h.Push) + 7u) & ~uint64_t(7u);
    const auto bindingSpan = bytes.subspan(sizeof(h), size_t(bindingBytes));
    const auto runSpan = bytes.subspan(sizeof(h) + size_t(bindingBytes), size_t(runBytes));
    const auto pushSpan = bytes.subspan(sizeof(h) + size_t(bindingBytes) + size_t(runBytes), size_t(pushAligned));
    if (!_shader.Decode(memory, h.Pipeline, h.Scissor, h.Bindings, h.Push, bindingSpan, pushSpan, owner, colors, area)) return false;
    _indexPool = &terrain.QuadIndexPool();
    return DecodeRuns(
        memory,
        runSpan,
        h.InstanceCount,
        h.InstanceStride,
        h.UploadOffset,
        instanceUploads.size(),
        _shader.Pipeline->State.Vertices,
        staticGeometry,
        terrain.QuadIndices(),
        indirectArgs,
        canIndirect,
        indirectLimit);
}

bool InstancedGeometryFeature::DecodeRuns(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> runs,
    uint32_t instanceCount,
    uint32_t stride,
    uint64_t offset,
    uint64_t uploadSize,
    std::span<const radray::gpu::VertexBufferLayout> layouts,
    const StaticGeometryRegistry& geometry,
    const radray::IndexBufferSubAllocation& quadIndices,
    std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
    bool canIndirect,
    uint32_t indirectLimit) {
    if (!quadIndices || !instanceCount || !stride || runs.empty() || runs.size() % sizeof(WireRun)) return false;
    if (std::none_of(layouts.begin(), layouts.end(), [&](const radray::gpu::VertexBufferLayout& layout) {
            return layout.Binding == 1 && layout.StepMode == radray::gpu::VertexStepMode::Instance && layout.ArrayStride == stride;
        })) {
        return false;
    }
    _instanceBytes = uint64_t(instanceCount) * stride;
    if (offset > uploadSize || _instanceBytes > uploadSize - offset) return false;
    _uploadOffset = offset;
    _quadFirstIndex = quadIndices.GetFirstIndex();
    _indirectLimit = std::max(1u, indirectLimit);
    _usesIndirect = false;
    auto batches = FrameArray<Batch>(memory, runs.size() / sizeof(WireRun));
    _batches = batches.first(0);
    for (size_t i = 0; i < runs.size(); i += sizeof(WireRun)) {
        WireRun r{};
        std::memcpy(&r, runs.data() + i, sizeof(WireRun));
        if (r.Reserved || !r.InstanceCount || uint64_t(r.FirstInstance) + r.InstanceCount > instanceCount) return false;
        const StaticMesh* mesh = geometry.Find(r.MeshId);
        if (!mesh || !mesh->Geometry || mesh->Geometry.GetFirstVertex() > INT32_MAX ||
            mesh->IndexCount > quadIndices.GetIndexCount()) {
            RADRAY_ERR_LOG("InstancedGeometryFeature invalid static mesh id={}", r.MeshId);
            return false;
        }
        auto* vb = mesh->Geometry.GetBuffer();
        const uint64_t vbSize = vb->GetDesc().Size;
        const uint32_t firstVertex = uint32_t(mesh->Geometry.GetFirstVertex());
        if (canIndirect && !_batches.empty() && _batches.back().VertexBuffer == vb) {
            auto& prev = _batches.back();
            if (!prev.Indirect) {
                if (indirectArgs.size() > UINT32_MAX - 2u) return false;
                const uint32_t argOffset = uint32_t(indirectArgs.size());
                indirectArgs.push_back({prev.IndexCountOrArgCount, prev.InstanceCount, _quadFirstIndex, int32_t(prev.FirstVertexOrArgOffset), prev.FirstInstance});
                indirectArgs.push_back({mesh->IndexCount, r.InstanceCount, _quadFirstIndex, int32_t(firstVertex), r.FirstInstance});
                prev.FirstVertexOrArgOffset = argOffset;
                prev.IndexCountOrArgCount = 2u;
                prev.FirstInstance = 0;
                prev.InstanceCount = 0;
                prev.Indirect = true;
                _usesIndirect = true;
            } else {
                if (indirectArgs.size() >= UINT32_MAX) return false;
                indirectArgs.push_back({mesh->IndexCount, r.InstanceCount, _quadFirstIndex, int32_t(firstVertex), r.FirstInstance});
                ++prev.IndexCountOrArgCount;
            }
        } else {
            batches[_batches.size()] = {vb, vbSize, firstVertex, mesh->IndexCount, r.FirstInstance, r.InstanceCount, false};
            _batches = batches.first(_batches.size() + 1);
        }
    }
    _staticVertexPool = &geometry.VertexPool();
    _indexView = quadIndices.GetIndexBufferView();
    return true;
}

size_t InstancedGeometryFeature::DrawCount() const noexcept {
    size_t total = 0;
    for (const auto& b : _batches) {
        if (b.Indirect) {
            total += (uint64_t(b.IndexCountOrArgCount) + _indirectLimit - 1u) / _indirectLimit;
        } else {
            ++total;
        }
    }
    return total;
}

void InstancedGeometryFeature::Import(
    radray::RDGBuilder& graph,
    FrameTextureImports& imports,
    radray::RDGBufferHandle instanceBuffer,
    radray::RDGBufferHandle indirectBuffer) {
    _shader.Import(graph, imports);
    _instanceBuffer = instanceBuffer;
    _indirectBuffer = indirectBuffer;
}

void InstancedGeometryFeature::Declare(radray::RDGRasterPassBuilder& pass) const {
    _shader.Declare(pass);
}

bool InstancedGeometryFeature::Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth) {
    if (!_shader.Prepare(ctx, depth)) return false;
    auto base = ctx.ResolveBuffer(_instanceBuffer);
    _instanceSlice = {base.Buffer, base.Offset + _uploadOffset, _instanceBytes};
    if (_usesIndirect) {
        _indirectSlice = ctx.ResolveBuffer(_indirectBuffer);
    }
    return true;
}

bool InstancedGeometryFeature::Record(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept {
    if (!_shader.Bind(enc, cache)) return false;
    cache.BindIndexBuffer(enc, _indexView);
    const radray::gpu::VertexBufferBinding instBinding{1, FrameVertexView(_instanceSlice)};
    enc.BindVertexBuffers({&instBinding, 1});
    radray::gpu::Buffer* lastVb = nullptr;
    for (const auto& b : _batches) {
        if (b.VertexBuffer != lastVb) {
            const radray::gpu::VertexBufferBinding localBinding{0, radray::gpu::VertexBufferView{b.VertexBuffer, 0, b.VertexBufferSize}};
            enc.BindVertexBuffers({&localBinding, 1});
            lastVb = b.VertexBuffer;
        }
        if (b.Indirect) {
            uint32_t done = 0;
            while (done < b.IndexCountOrArgCount) {
                const uint32_t chunk = std::min(b.IndexCountOrArgCount - done, _indirectLimit);
                const uint64_t byteOffset = _indirectSlice.Offset + (uint64_t(b.FirstVertexOrArgOffset) + done) * sizeof(radray::gpu::DrawIndexedIndirectArguments);
                enc.DrawIndexedIndirect(_indirectSlice.Buffer.Get(), byteOffset, chunk);
                done += chunk;
            }
        } else {
            enc.DrawIndexed(b.IndexCountOrArgCount, b.InstanceCount, _quadFirstIndex, int32_t(b.FirstVertexOrArgOffset), b.FirstInstance);
        }
    }
    return enc.IsValid();
}
}  // namespace raynium
