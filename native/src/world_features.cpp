#include <raynium/world_features.h>
#include <raynium/raster_records.h>
#include <raynium/frame_upload.h>
#include <radray/logger.h>
#include <algorithm>
#include <cstring>

namespace raynium {
using namespace radray::gpu;

bool ShaderResourceBindings::Decode(uint32_t pipelineId, const radray::Rect& scissorRect, uint32_t bindingCount, uint32_t pushBytes,
    std::span<const radray::byte> bindingBytes, std::span<const radray::byte> pushData,
    const ImmediatePipelineRegistry& pipelines, const BufferRegistry& buffers, const TextureRegistry& textures,
    std::span<const TextureFormat> colors, const radray::Rect& area) {
    if (bindingCount > 256 || pushBytes % 4 || bindingBytes.size() != uint64_t(bindingCount) * sizeof(Binding) ||
        pushData.size() < pushBytes || scissorRect.X < area.X || scissorRect.Y < area.Y ||
        uint64_t(scissorRect.X) + scissorRect.Width > uint64_t(area.X) + area.Width ||
        uint64_t(scissorRect.Y) + scissorRect.Height > uint64_t(area.Y) + area.Height) return false;
    pipeline = pipelines.Find(pipelineId);
    if (!pipeline || pipeline->program->drawBasesLayout || colors.size() != pipeline->state.colors.size()) return false;
    for (size_t i = 0; i < colors.size(); ++i) {
        if (colors[i] != pipeline->state.colors[i].Format) return false;
    }
    scissor = scissorRect;
    resources.reserve(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        Resource r{};
        auto& b = r.binding;
        std::memcpy(&b, bindingBytes.data() + size_t(i) * sizeof(Binding), sizeof(Binding));
        if (b.kind == 5) {
            auto* image = textures.Find(b.id);
            if (!image || !b.sampling.mip_count || b.sampling.base_mip >= image->desc.mip_levels ||
                b.sampling.mip_count > image->desc.mip_levels - b.sampling.base_mip) return false;
            r.texture = image->texture;
        } else if (b.kind == 3 || b.kind == 4) {
            const uint32_t reqUsage = b.kind == 3 ? RN_BUFFER_UNIFORM : RN_BUFFER_TEXEL;
            r.buffer = buffers.Find(b.id);
            if (!r.buffer || !(r.buffer->usage & reqUsage) || !r.buffer->Contains(b.offset, b.size)) return false;
        } else {
            return false;
        }
        resources.push_back(std::move(r));
    }
    if (auto size = pipeline->program->pushBytes) {
        const auto copySize = std::min(size, pushBytes);
        push.clear();
        push.reserve(size);
        for (uint32_t i = 0; i < copySize; ++i) push.push_back(pushData[i]);
        for (uint32_t i = copySize; i < size; ++i) push.push_back(radray::byte{});
    }
    return true;
}

void ShaderResourceBindings::Import(radray::RDGBuilder& graph, FrameTextureImports& imports) {
    for (auto& r : resources) {
        if (r.texture) r.imageHandle = imports.Import(r.texture);
        else r.bufferHandle = r.buffer->Import(graph);
    }
}

void ShaderResourceBindings::Declare(radray::RDGRasterPassBuilder& pass) const {
    for (const auto& r : resources) {
        if (r.texture) pass.ReadTexture(r.imageHandle);
        else pass.ReadBuffer(r.bufferHandle, r.binding.kind == 3 ? BufferState::CBuffer : BufferState::ShaderRead);
    }
}

bool ShaderResourceBindings::Prepare(radray::RDGPrepareContext& ctx, TextureFormat depth) {
    auto& slot = ctx.GetSlot();
    auto& device = slot.GetDevice();
    pso = pipeline->Resolve(device, depth);
    if (!pso) return false;
    const auto& program = *pipeline->program;
    radray::array<radray::InlineVector<radray::TransientParameterBinding, 16>, 2> groups;
    for (auto& r : resources) {
        const auto& b = r.binding;
        if (b.kind == 5) {
            if (program.RequiresBinding(1, b.slot)) {
                auto* view = GetSamplingView(device, *r.texture, b.sampling);
                if (!view) return false;
                groups[1].push_back({b.slot, 0, view});
            }
            if (program.HasSampler(b.slot + 1)) {
                auto* sampler = GetTextureSampler(device, b.format, b.sampling);
                if (!sampler) return false;
                groups[1].push_back({b.slot + 1, 0, sampler});
            }
            continue;
        }
        if (b.kind == 3) {
            if (program.RequiresBinding(0, b.slot)) {
                auto slice = ctx.ResolveBuffer(r.bufferHandle);
                slice.Offset += b.offset;
                slice.Size = b.size;
                groups[0].push_back(FrameConstantBinding(b.slot, slice));
            }
        } else if (program.RequiresBinding(1, b.slot)) {
            auto slice = ctx.ResolveBuffer(r.bufferHandle);
            slice.Offset += b.offset;
            slice.Size = b.size;
            groups[1].push_back({b.slot, 0, ShaderTexelBufferBinding{slice.Buffer.Get(), {slice.Offset, slice.Size}, ToTextureFormat(rn_pixel_format(b.format))}});
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (!program.groups[i]) continue;
        auto& group = groups[i];
        EnsureOrderedBindings(group);
        sets[i] = slot.AcquireParameterSet(program.groups[i], {group.data(), group.size()}).Get();
        if (!sets[i]) return false;
    }
    return true;
}

bool ShaderResourceBindings::Bind(GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept {
    cache.BindPipeline(enc, pso);
    cache.SetScissor(enc, scissor);
    cache.SetStencil(enc, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        if (sets[i]) cache.BindParameterSet(enc, i, sets[i]);
    }
    if (!push.empty() && !enc.SetPushConstants(0, 0, {push.data(), push.size()})) return false;
    return true;
}

bool InstancedGeometryFeature::Decode(std::span<const radray::byte> bytes, std::span<const radray::byte> instanceUploads,
    radray::vector<DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit,
    const ImmediatePipelineRegistry& pipelines, const BufferRegistry& buffers, const TextureRegistry& textures,
    const StaticGeometryRegistry& staticGeometry, const TerrainResidency& terrain,
    std::span<const TextureFormat> colors, const radray::Rect& area) {
    static_assert(sizeof(WireRun)==16);
    wire::Geometry h{};
    if (!wire::ReadRasterHeader(bytes,h) || h.uploadOffset>instanceUploads.size() ||
        uint64_t(h.instanceCount)*h.instanceStride>instanceUploads.size()-h.uploadOffset) return false;
    const uint64_t bindingBytes = uint64_t(h.bindings) * sizeof(ShaderResourceBindings::Binding);
    const uint64_t runBytes = uint64_t(h.runCount) * sizeof(WireRun);
    const uint64_t pushAligned = (uint64_t(h.push) + 7u) & ~uint64_t(7u);
    const auto bindingSpan = bytes.subspan(sizeof(h), size_t(bindingBytes));
    const auto runSpan = bytes.subspan(sizeof(h) + size_t(bindingBytes), size_t(runBytes));
    const auto pushSpan = bytes.subspan(sizeof(h) + size_t(bindingBytes) + size_t(runBytes), size_t(pushAligned));
    if (!_shader.Decode(h.pipeline, h.scissor, h.bindings, h.push, bindingSpan, pushSpan, pipelines, buffers, textures, colors, area)) return false;
    _indexPool = &terrain.QuadIndexPool();
    return DecodeRuns(runSpan, h.instanceCount, h.instanceStride, h.uploadOffset, instanceUploads.size(),
        _shader.pipeline->state.vertices, staticGeometry, terrain.QuadIndices(), indirectArgs, canIndirect, indirectLimit);
}

bool InstancedGeometryFeature::DecodeRuns(std::span<const radray::byte> runs, uint32_t instanceCount, uint32_t stride,
    uint64_t offset, uint64_t uploadSize, std::span<const VertexBufferLayout> layouts,
    const StaticGeometryRegistry& geometry, const radray::IndexBufferSubAllocation& quadIndices,
    radray::vector<DrawIndexedIndirectArguments>& indirectArgs, bool canIndirect, uint32_t indirectLimit) {
    if (!quadIndices || !instanceCount || !stride || runs.empty() || runs.size() % sizeof(WireRun)) return false;
    if (std::none_of(layouts.begin(), layouts.end(), [&](const VertexBufferLayout& layout) {
        return layout.Binding == 1 && layout.StepMode == VertexStepMode::Instance && layout.ArrayStride == stride;
    })) return false;
    _instanceBytes = uint64_t(instanceCount) * stride;
    if (offset > uploadSize || _instanceBytes > uploadSize - offset) return false;
    _uploadOffset = offset;
    _quadFirstIndex = quadIndices.GetFirstIndex();
    _indirectLimit = std::max(1u, indirectLimit);
    _usesIndirect = false;
    _batches.clear();
    for (size_t i = 0; i < runs.size(); i += sizeof(WireRun)) {
        WireRun r{};
        std::memcpy(&r, runs.data() + i, sizeof(WireRun));
        if (r.reserved || !r.instanceCount || uint64_t(r.firstInstance) + r.instanceCount > instanceCount) return false;
        const StaticMesh* mesh = geometry.Find(r.meshId);
        if (!mesh || !mesh->geometry || mesh->geometry.GetFirstVertex() > INT32_MAX ||
            mesh->indexCount > quadIndices.GetIndexCount()) {
            RADRAY_ERR_LOG("InstancedGeometryFeature invalid static mesh id={}", r.meshId);
            return false;
        }
        auto* vb = mesh->geometry.GetBuffer();
        const uint64_t vbSize = vb->GetDesc().Size;
        const uint32_t firstVertex = uint32_t(mesh->geometry.GetFirstVertex());
        if (canIndirect && !_batches.empty() && _batches.back().vertexBuffer == vb) {
            auto& prev = _batches.back();
            if (!prev.indirect) {
                if (indirectArgs.size() > UINT32_MAX - 2u) return false;
                const uint32_t argOffset = uint32_t(indirectArgs.size());
                indirectArgs.push_back({prev.indexCountOrArgCount, prev.instanceCount, _quadFirstIndex, int32_t(prev.firstVertexOrArgOffset), prev.firstInstance});
                indirectArgs.push_back({mesh->indexCount, r.instanceCount, _quadFirstIndex, int32_t(firstVertex), r.firstInstance});
                prev.firstVertexOrArgOffset = argOffset;
                prev.indexCountOrArgCount = 2u;
                prev.firstInstance = 0;
                prev.instanceCount = 0;
                prev.indirect = true;
                _usesIndirect = true;
            } else {
                if (indirectArgs.size() >= UINT32_MAX) return false;
                indirectArgs.push_back({mesh->indexCount, r.instanceCount, _quadFirstIndex, int32_t(firstVertex), r.firstInstance});
                ++prev.indexCountOrArgCount;
            }
        } else {
            _batches.push_back({vb, vbSize, firstVertex, mesh->indexCount, r.firstInstance, r.instanceCount, false});
        }
    }
    _staticVertexPool = &geometry.VertexPool();
    _indexView = quadIndices.GetIndexBufferView();
    return true;
}

size_t InstancedGeometryFeature::DrawCount() const noexcept {
    size_t total = 0;
    for (const auto& b : _batches) {
        if (b.indirect) {
            total += (uint64_t(b.indexCountOrArgCount) + _indirectLimit - 1u) / _indirectLimit;
        } else {
            ++total;
        }
    }
    return total;
}

void InstancedGeometryFeature::Import(radray::RDGBuilder& graph, FrameTextureImports& imports, radray::RDGBufferHandle instanceBuffer,
    radray::RDGBufferHandle indirectBuffer) {
    _shader.Import(graph, imports);
    _instanceBuffer = instanceBuffer;
    _indirectBuffer = indirectBuffer;
}

void InstancedGeometryFeature::Declare(radray::RDGRasterPassBuilder& pass) const {
    _shader.Declare(pass);
}

bool InstancedGeometryFeature::Prepare(radray::RDGPrepareContext& ctx, TextureFormat depth) {
    if (!_shader.Prepare(ctx, depth)) return false;
    auto base = ctx.ResolveBuffer(_instanceBuffer);
    _instanceSlice = {base.Buffer, base.Offset + _uploadOffset, _instanceBytes};
    if (_usesIndirect) {
        _indirectSlice = ctx.ResolveBuffer(_indirectBuffer);
    }
    return true;
}

bool InstancedGeometryFeature::Record(GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept {
    if (!_shader.Bind(enc, cache)) return false;
    cache.BindIndexBuffer(enc, _indexView);
    const VertexBufferBinding instBinding{1, FrameVertexView(_instanceSlice)};
    enc.BindVertexBuffers({&instBinding, 1});
    Buffer* lastVb = nullptr;
    for (const auto& b : _batches) {
        if (b.vertexBuffer != lastVb) {
            const VertexBufferBinding localBinding{0, VertexBufferView{b.vertexBuffer, 0, b.vertexBufferSize}};
            enc.BindVertexBuffers({&localBinding, 1});
            lastVb = b.vertexBuffer;
        }
        if (b.indirect) {
            uint32_t done = 0;
            while (done < b.indexCountOrArgCount) {
                const uint32_t chunk = std::min(b.indexCountOrArgCount - done, _indirectLimit);
                const uint64_t byteOffset = _indirectSlice.Offset + (uint64_t(b.firstVertexOrArgOffset) + done) * sizeof(DrawIndexedIndirectArguments);
                enc.DrawIndexedIndirect(_indirectSlice.Buffer.Get(), byteOffset, chunk);
                done += chunk;
            }
        } else {
            enc.DrawIndexed(b.indexCountOrArgCount, b.instanceCount, _quadFirstIndex, int32_t(b.firstVertexOrArgOffset), b.firstInstance);
        }
    }
    return enc.IsValid();
}

}
