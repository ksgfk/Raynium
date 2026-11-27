#include <raynium/immediate_draw.h>

#include <algorithm>
#include <cstring>

#include <raynium/raster_records.h>

namespace raynium {
ImmediateDraw::Binding ImmediateDraw::ReadBinding(const radray::byte* base, size_t index) noexcept {
    Binding b{};
    std::memcpy(&b, base + index * sizeof(Binding), sizeof(Binding));
    return b;
}

size_t ImmediateDraw::BindingCount() const noexcept {
    return _resources.size() + 1;
}

bool ImmediateDraw::Decode(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> bytes,
    rn_renderer* owner,
    std::span<const radray::gpu::TextureFormat> colors,
    const radray::Rect& area) {
    static_assert(sizeof(Binding) == 48);
    wire::Draw h{};
    if (!wire::ReadRasterHeader(bytes, h) || h.Scissor.X < area.X || h.Scissor.Y < area.Y ||
        uint64_t(h.Scissor.X) + h.Scissor.Width > uint64_t(area.X) + area.Width ||
        uint64_t(h.Scissor.Y) + h.Scissor.Height > uint64_t(area.Y) + area.Height) {
        return false;
    }
    auto* pipeline = ResolveHandle(owner, h.Pipeline);
    _pipeline = pipeline ? pipeline->Impl.get() : nullptr;
    if (!_pipeline || colors.size() != _pipeline->State.Colors.size()) return false;
    for (size_t i = 0; i < colors.size(); ++i) {
        if (colors[i] != _pipeline->State.Colors[i].Format) return false;
    }
    _kind = h.Kind;
    _count = h.Count;
    _instances = h.Instances;
    _first = h.First;
    _firstInstance = h.FirstInstance;
    _base = h.Base;
    _scissor = h.Scissor;
    if (_kind == 4 && (_count < 3 || _count > UINT32_MAX / 3)) return false;
    bytes = bytes.subspan(sizeof(h));
    _bindingBytes = bytes.data();
    _resources = FrameArray<Resource>(memory, h.Bindings);
    bool hasIndirect = false;
    for (uint32_t i = 0; i < h.Bindings; ++i) {
        auto& r = _resources[i];
        const auto b = ReadBinding(_bindingBytes, i);
        r.Kind = b.Kind;
        if (b.Kind == 5) {
            auto* image = ResolveHandle(owner, reinterpret_cast<rn_texture*>(b.Resource));
            if (!image || !b.Sampling.mip_count || b.Sampling.base_mip >= image->Desc.mip_levels ||
                b.Sampling.mip_count > image->Desc.mip_levels - b.Sampling.base_mip) {
                return false;
            }
            r.Texture = image;
        } else {
            constexpr uint32_t kUsage[]{RN_BUFFER_VERTEX, RN_BUFFER_INDEX, RN_BUFFER_INDIRECT, RN_BUFFER_UNIFORM, RN_BUFFER_TEXEL};
            r.Buffer = ResolveHandle(owner, reinterpret_cast<rn_buffer*>(b.Resource));
            if (b.Kind >= 5 || !r.Buffer || !(r.Buffer->Usage & kUsage[b.Kind]) || !BufferContainsRange(*r.Buffer, b.Offset, b.Size)) {
                return false;
            }
            if (b.Kind == 1 && ((b.Format != 2 && b.Format != 4) || b.Offset > UINT32_MAX ||
                                (_kind == 1 && (uint64_t(_first) + _count) > b.Size / b.Format))) {
                return false;
            }
            if (b.Kind == 2) {
                if ((_kind != 2 && _kind != 3) || _count > b.Size / (_kind == 2 ? 16 : 20)) return false;
                hasIndirect = true;
            }
        }
    }
    if ((_kind == 2 || _kind == 3) && !hasIndirect) return false;
    bytes = bytes.subspan(size_t(h.Bindings) * sizeof(Binding));
    _push = FramePushConstants(memory, bytes.first(h.Push), _pipeline->Program->PushBytes);
    return true;
}

void ImmediateDraw::Import(radray::RDGBuilder& graph, FrameTextureImports& imports) {
    for (auto& r : _resources) {
        if (r.Kind == 5)
            r.ImageHandle = imports.Import(r.Texture);
        else
            r.BufferHandle = ImportBuffer(*r.Buffer, graph);
    }
    if (_pipeline->Program->DrawBasesLayout) {
        const bool indirect = _kind == 2 || _kind == 3;
        if (indirect) {
            _drawBases = graph.CreateBuffer({
                .Size = uint64_t(_count) * 256,
                .Usage = radray::gpu::BufferUse::CopyDestination | radray::gpu::BufferUse::CBuffer,
            });
            size_t argIndex = 0;
            while (argIndex < _resources.size() && _resources[argIndex].Kind != 2) ++argIndex;
            const auto& args = _resources[argIndex];
            const auto argBinding = ReadBinding(_bindingBytes, argIndex);
            struct CopyBases {
                radray::RDGBufferHandle From;
                radray::RDGBufferHandle To;
                uint64_t Offset;
                uint32_t Count;
                uint32_t Stride;
            };
            graph.AddCopyPass(
                     "Draw argument bases",
                     CopyBases{args.BufferHandle, _drawBases, argBinding.Offset, _count, _kind == 2 ? 16u : 20u},
                     +[](const CopyBases& op, radray::RDGCopyPassContext& ctx) noexcept {
                         const auto src = ctx.ResolveBuffer(op.From);
                         const auto dst = ctx.ResolveBuffer(op.To);
                         for (uint32_t i = 0; i < op.Count; ++i) {
                             ctx.GetCommandBuffer().CopyBufferToBuffer(
                                 dst.Buffer.Get(),
                                 dst.Offset + uint64_t(i) * 256,
                                 src.Buffer.Get(),
                                 src.Offset + op.Offset + uint64_t(i + 1) * op.Stride - 8,
                                 8);
                         }
                         return true;
                     })
                .ReadBuffer(args.BufferHandle)
                .WriteBuffer(_drawBases);
        } else {
            class DrawBaseBuffers final {
            public:
                explicit DrawBaseBuffers(std::pmr::memory_resource* memory) : Buffers(memory) {}
                std::pmr::unordered_map<uint64_t, radray::RDGBufferHandle> Buffers;
            };
            auto& blackboard = graph.GetBlackboard();
            auto cache = blackboard.Get<DrawBaseBuffers>();
            if (!cache) cache = blackboard.Emplace<DrawBaseBuffers>(imports.Scratch());
            radray::array<uint32_t, 2> bases{_kind == 0 ? _first : uint32_t(_base), _firstInstance};
            auto [it, fresh] = cache.Get()->Buffers.try_emplace((uint64_t(bases[1]) << 32) | bases[0]);
            if (fresh) {
                it->second = graph.CreateDynamicBuffer(
                    {.MaxSize = 256, .Usage = radray::gpu::BufferUse::CBuffer},
                    radray::RDGUploadSource::Generated(
                        256,
                        bases,
                        +[](const radray::array<uint32_t, 2>& value, radray::RDGUploadWriteContext& ctx) noexcept {
                            std::memcpy(ctx.Bytes.data(), value.data(), sizeof(value));
                            return true;
                        }));
            }
            _drawBases = it->second;
        }
    }
    if (_kind == 4) {
        const uint64_t bytes = uint64_t(_count - 2) * 3 * sizeof(uint32_t);
        _fan = graph.CreateDynamicBuffer(
            {.MaxSize = bytes, .Usage = radray::gpu::BufferUse::Index},
            radray::RDGUploadSource::Generated(
                bytes,
                _count,
                +[](const uint32_t& count, radray::RDGUploadWriteContext& ctx) noexcept {
                    auto* dest = reinterpret_cast<uint32_t*>(ctx.Bytes.data());
                    for (uint32_t i = 1; i + 1 < count; ++i) {
                        *dest++ = 0;
                        *dest++ = i;
                        *dest++ = i + 1;
                    }
                    return true;
                }));
    }
}

void ImmediateDraw::Declare(radray::RDGRasterPassBuilder& pass) const {
    constexpr radray::gpu::BufferState kStates[]{
        radray::gpu::BufferState::Vertex,
        radray::gpu::BufferState::Index,
        radray::gpu::BufferState::Indirect,
        radray::gpu::BufferState::CBuffer,
        radray::gpu::BufferState::ShaderRead,
    };
    for (const auto& r : _resources) {
        if (r.Kind == 5)
            pass.ReadTexture(r.ImageHandle);
        else
            pass.ReadBuffer(r.BufferHandle, kStates[r.Kind]);
    }
    if (_drawBases.IsValid()) pass.ReadBuffer(_drawBases, radray::gpu::BufferState::CBuffer);
    if (_fan.IsValid()) pass.ReadBuffer(_fan, radray::gpu::BufferState::Index);
}

bool ImmediateDraw::Prepare(radray::RDGPrepareContext& ctx, radray::gpu::TextureFormat depth) {
    auto& slot = ctx.GetSlot();
    auto& device = slot.GetDevice();
    _pso = _pipeline->Resolve(device, depth);
    if (!_pso) return false;
    const auto& program = *_pipeline->Program;
    FrameParameterBindings bindings(slot);
    size_t vertexCount = 0;
    for (const auto& r : _resources) vertexCount += r.Kind == 0;
    _vertices = FrameArray<radray::gpu::VertexBufferBinding>(slot.GetMemoryResource(), vertexCount);
    size_t vertex = 0;
    for (size_t i = 0; i < _resources.size(); ++i) {
        const auto& r = _resources[i];
        const auto b = ReadBinding(_bindingBytes, i);
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
            continue;
        }
        if (r.Kind == 4) {
            if (program.RequiresBinding(1, b.Slot)) {
                auto slice = ctx.ResolveBuffer(r.BufferHandle);
                slice.Offset += b.Offset;
                slice.Size = b.Size;
                bindings.Group(1).push_back({
                    b.Slot,
                    0,
                    radray::gpu::ShaderTexelBufferBinding{slice.Buffer.Get(), {slice.Offset, slice.Size}, ToTextureFormat(rn_pixel_format(b.Format))},
                });
            }
            continue;
        }
        auto slice = ctx.ResolveBuffer(r.BufferHandle);
        slice.Offset += b.Offset;
        slice.Size = b.Size;
        switch (r.Kind) {
            case 0: _vertices[vertex++] = {b.Slot, FrameVertexView(slice)}; break;
            case 1:
                if (slice.Offset > UINT32_MAX) return false;
                _indices = {slice.Buffer.Get(), uint32_t(slice.Offset), b.Format};
                break;
            case 2: _indirect = slice; break;
        }
    }
    if (_drawBases.IsValid()) {
        auto slice = ctx.ResolveBuffer(_drawBases);
        const uint32_t count = (_kind == 2 || _kind == 3) ? _count : 1;
        slice.Size = 256;
        _drawBaseSets = FrameArray<radray::gpu::ShaderParameterSet*>(slot.GetMemoryResource(), count);
        for (uint32_t i = 0; i < count; ++i, slice.Offset += 256) {
            auto binding = FrameConstantBinding(0, slice);
            auto set = slot.AcquireParameterSet(program.DrawBasesLayout, {&binding, 1});
            if (!set) return false;
            _drawBaseSets[i] = set.Get();
        }
    }
    if (_fan.IsValid()) _indices = FrameIndexView(ctx.ResolveBuffer(_fan));
    if (((_kind == 1 || _kind == 3 || _kind == 4) && !_indices.Target) || ((_kind == 2 || _kind == 3) && !_indirect.Buffer)) {
        return false;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (program.Groups[i]) {
            auto& group = bindings.Group(i);
            EnsureOrderedBindings(group);
            _sets[i] = slot.AcquireParameterSet(program.Groups[i], {group.data(), group.size()}).Get();
            if (!_sets[i]) return false;
        }
    }
    return true;
}

bool ImmediateDraw::Record(radray::gpu::GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache) const noexcept {
    cache.BindPipeline(enc, _pso);
    cache.SetScissor(enc, _scissor);
    cache.SetStencil(enc, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        if (_sets[i]) cache.BindParameterSet(enc, i, _sets[i]);
    }
    if (!_push.empty() && !enc.SetPushConstants(0, 0, {_push.data(), _push.size()})) return false;
    if (!_vertices.empty()) enc.BindVertexBuffers({_vertices.data(), _vertices.size()});
    if (_indices.Target) cache.BindIndexBuffer(enc, _indices);
    if (!_drawBaseSets.empty()) {
        if (_kind == 2 || _kind == 3) {
            for (uint32_t i = 0; i < _count; ++i) {
                enc.BindShaderParameterSet(2, _drawBaseSets[i]);
                if (_kind == 2)
                    enc.DrawIndirect(_indirect.Buffer.Get(), _indirect.Offset + uint64_t(i) * 16, 1);
                else
                    enc.DrawIndexedIndirect(_indirect.Buffer.Get(), _indirect.Offset + uint64_t(i) * 20, 1);
            }
            return enc.IsValid();
        }
        enc.BindShaderParameterSet(2, _drawBaseSets.front());
    }
    switch (_kind) {
        case 0: enc.Draw(_count, _instances, _first, _firstInstance); break;
        case 1: enc.DrawIndexed(_count, _instances, _first, _base, _firstInstance); break;
        case 2: enc.DrawIndirect(_indirect.Buffer.Get(), _indirect.Offset, _count); break;
        case 3: enc.DrawIndexedIndirect(_indirect.Buffer.Get(), _indirect.Offset, _count); break;
        case 4: enc.DrawIndexed(3 * (_count - 2), _instances, 0, _base, _firstInstance); break;
    }
    return enc.IsValid();
}
}  // namespace raynium
