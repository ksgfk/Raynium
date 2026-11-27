#include <raynium/immediate_pass.h>

#include <algorithm>
#include <cstring>

#include <raynium/raster_records.h>

namespace raynium {
RasterPassPlan* RasterPassPlan::Decode(
    std::pmr::memory_resource& memory,
    std::span<const radray::byte> record,
    rn_renderer* owner,
    uint32_t maxColors) {
    struct Wire {
        rn_texture* Id;
        uint32_t Mip;
        uint32_t Layer;
        uint32_t Clear;
        uint32_t Reserved;
        radray::gpu::ColorClearValue Value;
    };
    static_assert(sizeof(Wire) == 40);
    wire::Pass header{};
    if (!wire::ReadRasterHeader(record, header) || header.Colors > maxColors) return nullptr;
    auto result = FrameArray<RasterPassPlan>(memory, 1).data();
    result->_area = header.Area;
    result->_colors = FrameArray<Attachment>(memory, header.Colors);

    for (uint32_t i = 0; i < header.Colors + header.Depth; ++i) {
        Wire wire{};
        std::memcpy(&wire, record.data() + sizeof(header) + i * sizeof(Wire), sizeof(wire));
        const bool depth = i == header.Colors;
        if (!wire.Id && !depth) continue;
        auto* texture = ResolveHandle(owner, wire.Id);
        if (!texture || !(texture->Desc.flags & RN_TEXTURE_ATTACHMENT) ||
            wire.Clear > 1 || wire.Mip >= texture->Desc.mip_levels || wire.Layer >= texture->Desc.array_layers ||
            depth != radray::gpu::IsDepthStencilFormat(texture->Texture->GetDesc().Format) ||
            (depth && wire.Clear && !(wire.Value.Value[0] >= 0 && wire.Value.Value[0] <= 1))) {
            return nullptr;
        }
        for (const auto& previous : result->_colors) {
            if (previous.Texture == texture) return nullptr;
        }
        const uint32_t w = std::max(1u, texture->Desc.width >> wire.Mip);
        const uint32_t h = std::max(1u, texture->Desc.height >> wire.Mip);
        if (result->_width && (result->_width != w || result->_height != h)) return nullptr;
        result->_width = w;
        result->_height = h;
        (depth ? result->_depth : result->_colors[i]) = {texture, {}, wire.Mip, wire.Layer, wire.Clear != 0, wire.Value};
    }
    if (uint64_t(header.Area.X) + header.Area.Width > result->_width ||
        uint64_t(header.Area.Y) + header.Area.Height > result->_height) {
        return nullptr;
    }
    result->_colorFormats = FrameArray<radray::gpu::TextureFormat>(memory, header.Colors);
    for (size_t i = 0; i < header.Colors; ++i) {
        auto* texture = result->_colors[i].Texture;
        result->_colorFormats[i] = texture ? texture->Texture->GetDesc().Format : radray::gpu::TextureFormat::UNKNOWN;
    }
    return result;
}

bool RasterPassPlan::TryFoldClear(
    const rn_texture* texture,
    uint32_t mip,
    uint32_t layer,
    const radray::gpu::ColorClearValue& value) noexcept {
    if (_area.X || _area.Y || _area.Width != _width || _area.Height != _height) return false;
    const auto fold = [&](Attachment& a) {
        if (a.Texture != texture || a.Mip != mip || a.Layer != layer || a.Clear) return false;
        a.Clear = true;
        a.Value = value;
        return true;
    };
    if (radray::gpu::IsDepthStencilFormat(texture->Texture->GetDesc().Format)) {
        return !(radray::gpu::GetTextureFormatAspects(texture->Texture->GetDesc().Format) & radray::gpu::TextureAspect::Stencil) &&
               fold(_depth);
    }
    for (auto& color : _colors) {
        if (fold(color)) return true;
    }
    return false;
}

bool RasterPassPlan::AddTerrain(
    std::pmr::memory_resource& memory,
    std::pmr::vector<RasterOp>& ops,
    std::span<const radray::byte> bytes,
    std::span<TerrainView> views,
    rn_renderer* owner) {
    auto* terrain = FrameArray<TerrainFeature>(memory, 1).data();
    if (!terrain->Decode(bytes, views, owner, _area)) return false;
    const uint32_t layer = std::min(terrain->Stage, uint32_t(Translucent));
    if (terrain->View->Draws->Layers[layer].RecordCount) {
        if (layer == Translucent)
            _translucentVertices = terrain->View->TranslucentVertexPool;
        else
            _terrainVertices = terrain->View->VertexPool;
        if (layer != Translucent) _quadIndices = terrain->View->QuadIndexPool;
        if (layer == Translucent || terrain->View->Draws->Wireframe) _translucentIndices = terrain->View->TranslucentIndexPool;
    }
    ops.emplace_back(terrain);
    return true;
}

bool RasterPassPlan::AddDraw(
    std::pmr::memory_resource& memory,
    std::pmr::vector<RasterOp>& ops,
    std::span<const radray::byte> bytes,
    rn_renderer* owner) {
    auto* draw = FrameArray<ImmediateDraw>(memory, 1).data();
    if (!draw->Decode(memory, bytes, owner, {_colorFormats.data(), _colorFormats.size()}, _area)) return false;
    ops.emplace_back(draw);
    return true;
}

bool RasterPassPlan::AddInstancedGeometry(
    std::pmr::memory_resource& memory,
    std::pmr::vector<RasterOp>& ops,
    std::span<const radray::byte> bytes,
    std::span<const radray::byte> instanceUploads,
    std::pmr::vector<radray::gpu::DrawIndexedIndirectArguments>& indirectArgs,
    bool canIndirect,
    uint32_t indirectLimit,
    rn_renderer* owner,
    const StaticGeometryRegistry& staticGeometry,
    const TerrainResidency& terrain) {
    auto* feature = FrameArray<InstancedGeometryFeature>(memory, 1).data();
    if (!feature->Decode(
            memory,
            bytes,
            instanceUploads,
            indirectArgs,
            canIndirect,
            indirectLimit,
            owner,
            staticGeometry,
            terrain,
            {_colorFormats.data(), _colorFormats.size()},
            _area)) {
        return false;
    }
    _staticVertices = feature->StaticVertexPool();
    _quadIndices = feature->IndexPool();
    _needInstanceBuffer = true;
    _needGeometryIndirect |= feature->UsesIndirect();
    ops.emplace_back(feature);
    return true;
}

void RasterPassPlan::WriteTimestamp(std::pmr::vector<RasterOp>& ops, uint32_t physicalIndex) {
    ops.emplace_back(physicalIndex);
    _containsTimestamp = true;
}

void RasterPassPlan::Finish(std::pmr::memory_resource& memory, const std::pmr::vector<RasterOp>& ops) {
    _commands = FreezeFrameArray(memory, ops);
}

void RasterPassPlan::Measure(rn_frame_metrics& metrics) const {
    for (const auto& command : _commands) {
        if (auto* draw = command.Get<ImmediateDraw>()) {
            ++metrics.scene_calls;
            metrics.bind_calls += draw->BindingCount();
        } else if (auto* staticGeometry = command.Get<InstancedGeometryFeature>()) {
            metrics.scene_calls += staticGeometry->DrawCount();
            metrics.bind_calls += staticGeometry->BindingCount();
        } else if (auto* feature = command.Get<TerrainFeature>()) {
            const auto& view = *feature->View;
            const auto& layer = view.Draws->Layers[std::min(feature->Stage, uint32_t(Translucent))];
            metrics.terrain_records += layer.RecordCount;
            if (view.UseIndirect) {
                for (const auto& b : layer.Prepared.Batches) {
                    metrics.terrain_calls += (uint64_t(b.ArgumentCount) + view.IndirectLimit - 1) / view.IndirectLimit;
                }
            } else {
                metrics.terrain_calls += layer.RecordCount;
            }
        }
    }
}

void RasterPassPlan::Replay(
    radray::RDGBuilder& graph,
    FrameTextureImports& imports,
    const FrameTimestamps& timestamps,
    radray::RDGBufferHandle instanceBuffer,
    radray::RDGBufferHandle geometryIndirectBuffer) {
    const bool full = _area.Width == _width && _area.Height == _height;
    // Initialization passes have to precede the raster node in the graph.
    for (size_t i = 0; i < _colors.size(); ++i) {
        if (_colors[i].Texture) _colors[i].Handle = imports.Import(_colors[i].Texture, _colors[i].Clear && full);
    }
    radray::RDGTextureHandle depth;
    if (_depth.Texture) {
        depth = imports.Import(
            _depth.Texture,
            _depth.Clear && full &&
                !(radray::gpu::GetTextureFormatAspects(_depth.Texture->Texture->GetDesc().Format) & radray::gpu::TextureAspect::Stencil));
    }
    for (auto& command : _commands) {
        if (auto* draw = command.Get<ImmediateDraw>())
            draw->Import(graph, imports);
        else if (auto* terrain = command.Get<TerrainFeature>())
            terrain->Import(graph, imports);
        else if (auto* staticGeometry = command.Get<InstancedGeometryFeature>()) {
            staticGeometry->Import(graph, imports, instanceBuffer, geometryIndirectBuffer);
        }
    }
    struct Data {
        RasterPassPlan* Pass;
        const FrameTimestamps* Queries;
    };
    auto pass = graph.AddRasterPass(
        "RenderPearl raster pass",
        Data{this, &timestamps},
        +[](const Data& data, radray::RDGRasterPassContext& context) noexcept {
            auto& encoder = context.GetEncoder();
            encoder.SetViewport({0, 0, float(data.Pass->_width), float(data.Pass->_height), 0, 1});
            encoder.SetScissor(data.Pass->_area);
            ImmediatePassStateCache stateCache{};
            stateCache.SeedScissor(data.Pass->_area);
            for (const auto& command : data.Pass->_commands) {
                if (auto idx = command.Get<uint32_t>()) {
                    data.Queries->WriteInline(*encoder.GetCommandBuffer(), *idx);
                } else if (auto* draw = command.Get<ImmediateDraw>()) {
                    if (!draw->Record(encoder, stateCache)) return false;
                } else if (auto* staticGeometry = command.Get<InstancedGeometryFeature>()) {
                    if (!staticGeometry->Record(encoder, stateCache)) return false;
                } else {
                    if (!command.Get<TerrainFeature>()->Record(encoder)) return false;
                    encoder.SetViewport({0, 0, float(data.Pass->_width), float(data.Pass->_height), 0, 1});
                    const auto scissor = command.Get<TerrainFeature>()->Scissor;
                    encoder.SetScissor(scissor);
                    stateCache.Invalidate();
                    stateCache.SeedScissor(scissor);
                }
            }
            return encoder.IsValid();
        },
        +[](Data& data, radray::RDGPrepareContext& ctx) noexcept {
            const auto& depthTex = data.Pass->_depth.Texture;
            const auto format = depthTex ? depthTex->Texture->GetDesc().Format : radray::gpu::TextureFormat::UNKNOWN;
            for (auto& cmd : data.Pass->_commands) {
                if (auto* draw = cmd.Get<ImmediateDraw>(); draw && !draw->Prepare(ctx, format)) return false;
                if (auto* terrain = cmd.Get<TerrainFeature>(); terrain && !terrain->Prepare(ctx)) return false;
                if (auto* staticGeometry = cmd.Get<InstancedGeometryFeature>(); staticGeometry && !staticGeometry->Prepare(ctx, format)) {
                    return false;
                }
            }
            return true;
        });
    for (auto& command : _commands) {
        if (auto* draw = command.Get<ImmediateDraw>())
            draw->Declare(pass);
        else if (auto* terrain = command.Get<TerrainFeature>())
            terrain->Declare(pass);
        else if (auto* staticGeometry = command.Get<InstancedGeometryFeature>())
            staticGeometry->Declare(pass);
    }
    if (_terrainVertices) _terrainVertices->AddPassReadAccess(graph, pass);
    if (_translucentVertices) _translucentVertices->AddPassReadAccess(graph, pass);
    if (_staticVertices) _staticVertices->AddPassReadAccess(graph, pass);
    if (_quadIndices) _quadIndices->AddPassReadAccess(graph, pass);
    if (_translucentIndices) _translucentIndices->AddPassReadAccess(graph, pass);
    if (_needInstanceBuffer && instanceBuffer.IsValid()) pass.ReadBuffer(instanceBuffer, radray::gpu::BufferState::Vertex);
    if (_needGeometryIndirect && geometryIndirectBuffer.IsValid()) pass.ReadBuffer(geometryIndirectBuffer, radray::gpu::BufferState::Indirect);
    pass.SetRenderArea(_area);
    for (uint32_t i = 0; i < _colors.size(); ++i) {
        if (_colors[i].Handle.IsValid()) {
            const auto& a = _colors[i];
            pass.WriteColor(
                i,
                _colors[i].Handle,
                a.Clear ? radray::gpu::LoadAction::Clear : radray::gpu::LoadAction::Load,
                radray::gpu::StoreAction::Store,
                a.Value,
                radray::RDGAttachmentSlice{a.Mip, a.Layer, 1});
            graph.MarkOutput(_colors[i].Handle);
        }
    }
    if (depth.IsValid()) {
        const auto& a = _depth;
        const auto aspects = radray::gpu::GetTextureFormatAspects(a.Texture->Texture->GetDesc().Format);
        pass.SetDepthStencil(
            depth,
            radray::gpu::TextureState::DepthWrite,
            a.Clear ? radray::gpu::LoadAction::Clear : radray::gpu::LoadAction::Load,
            radray::gpu::StoreAction::Store,
            aspects & radray::gpu::TextureAspect::Stencil ? radray::gpu::LoadAction::Load : radray::gpu::LoadAction::DontCare,
            aspects & radray::gpu::TextureAspect::Stencil ? radray::gpu::StoreAction::Store : radray::gpu::StoreAction::Discard,
            {a.Value.Value[0], 0},
            radray::RDGAttachmentSlice{a.Mip, a.Layer, 1});
        graph.MarkOutput(depth);
    }
    if (_containsTimestamp) {
        pass.NeverCull();
        graph.AddOrderBarrier(pass.GetHandle());
    }
}
}  // namespace raynium
