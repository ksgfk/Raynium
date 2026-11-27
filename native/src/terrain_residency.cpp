#include <raynium/terrain_residency.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <Eigen/Core>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/types.h>
#include <raynium/exception.h>
#include <raynium/frame_plan.h>

namespace raynium {
namespace {
template <class T>
bool Array(const T* p, uint32_t n, uint32_t limit = 1048576) {
    return n <= limit && (!n || p);
}

radray::MeshDrawCommand BuildFastTerrainCommand(
    radray::MeshDrawCommandBindingStorage& bindingsStorage,
    radray::gpu::GraphicsPipelineState* pipeline,
    const TerrainMesh& repMesh,
    const radray::IndexBufferSubAllocation& indexAlloc) {
    const auto indexView = indexAlloc.GetIndexBufferView();
    const auto& range0 = repMesh.Layout.Ranges[0];
    radray::MeshDrawCommand cmd{};
    cmd.Pipeline = pipeline;
    cmd.IndexBuffer = indexView;
    cmd.IndexCount = range0.Count;
    cmd.FirstIndex = indexAlloc.GetFirstIndex() + range0.First;
    cmd.VertexOffset = int32_t(repMesh.Geometry.GetFirstVertex());
    cmd.ValidElementMask = 1u;
    cmd.StencilReference = 0u;
    const auto& vi = repMesh.Geometry.GetVertexInput();
    radray::MeshDrawCommandBindings bindings{};
    for (const auto& stream : vi.Streams) {
        bindings.VertexBuffers.push_back({stream.Binding, {stream.Buffer.Get(), stream.Offset, stream.Size}});
    }
    for (const auto& layout : vi.Layouts) {
        bindings.VertexStreamLayouts.push_back({layout.Binding, layout.ArrayStride, layout.StepMode});
        if (layout.StepMode == radray::gpu::VertexStepMode::Instance) {
            bindings.InstanceStreamBinding = layout.Binding;
        }
    }
    bindingsStorage.Store(cmd, bindings);
    return cmd;
}

const TerrainWireframe* WireIndices(
    TerrainMesh& mesh,
    uint32_t mask,
    radray::IndexBufferPool& pool,
    std::pmr::vector<GeometryUpload>& uploads,
    uint64_t& uploadBytes) {
    for (auto& wire : mesh.Wireframes) {
        if (wire.Mask == mask) return &wire;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < mesh.Layout.Ranges.size(); ++i) {
        if (mask & (1u << i)) count += mesh.Layout.Ranges[i].Count;
    }
    auto alloc = pool.Allocate(count);
    if (!alloc) {
        RADRAY_ERR_LOG("Cannot allocate terrain wireframe indices");
        return nullptr;
    }
    auto bytes = FrameArray<radray::byte>(*uploads.get_allocator().resource(), alloc->GetByteSize());
    if (!WriteWireframeIndices(bytes, mesh.Layout, mask)) return nullptr;
    uploadBytes += bytes.size();
    uploads.push_back({alloc->GetGpuBuffer(), alloc->GetByteOffset(), radray::RDGUploadSource::BorrowedBytes(bytes)});
    return &mesh.Wireframes.emplace_back(TerrainWireframe{mask, std::move(*alloc)});
}
}  // namespace

void TerrainEyeCrossingCache::IncludePlane(uint32_t axis, double delta) noexcept {
    // Keep a conservative margin when camera and mesh origins have very different magnitudes.
    constexpr double kMargin = 1e-5;
    if (delta <= kMargin) Lower[axis] = std::max(Lower[axis], std::min(0.0, delta + kMargin));
    if (delta >= -kMargin) Upper[axis] = std::min(Upper[axis], std::max(0.0, delta - kMargin));
}

namespace {
struct FaceMask {
    uint32_t Bits{};
    bool MayCross{};
};

template <bool TrackPlanes>
FaceMask EvaluateFaces(
    const TerrainHotSlot& hot,
    const radray::array<int32_t, 3>& block,
    const radray::array<double, 3>& eye,
    bool valid,
    std::span<const rn_instance> instances,
    uint32_t requested,
    TerrainEyeCrossingCache* cache) noexcept {
    if (hot.Layer == RN_MATERIAL_TRANSLUCENT || !(hot.Flags & kTerrainHotFlagFiniteBounds) || !valid) {
        return {requested, false};
    }
    FaceMask result{hot.GroupBits[6] & requested, false};
    if (result.Bits == requested) return result;
    const bool standard = hot.SectionBoundsAxes == 7 && instances.size() == 1 &&
                          std::abs(eye[0]) <= 1.5 && std::abs(eye[1]) <= 1.5 && std::abs(eye[2]) <= 1.5;
    for (const auto& instance : instances) {
        for (uint32_t a = 0; a < 3; ++a) {
            const uint32_t positive = hot.GroupBits[2 * a] & requested;
            const uint32_t negative = hot.GroupBits[2 * a + 1] & requested;
            if (!(positive | negative)) continue;
            const int64_t diff = int64_t(block[a]) - instance.origin[a];
            if (standard) {
                if (diff <= -4) {
                    result.Bits |= negative;
                    continue;
                }
                if (diff >= 20) {
                    result.Bits |= positive;
                    continue;
                }
            }
            const double position = double(diff) + eye[a];
            if (!std::isfinite(position)) return {requested, false};
            // For bounds inside a section, neither plane can cross the one-block eye box at this distance.
            if (hot.SectionBoundsAxes & (1u << a)) {
                if (position < -2) {
                    result.Bits |= negative;
                    continue;
                }
                if (position > 18) {
                    result.Bits |= positive;
                    continue;
                }
            }
            if (positive) {
                const double plane = (double(hot.MinBound[a]) - 0.001) - position;
                if (plane <= 0) result.Bits |= positive;
                if constexpr (TrackPlanes) {
                    if (std::abs(plane) <= 1.00001) {
                        cache->IncludePlane(a, plane);
                        result.MayCross = true;
                    }
                }
            }
            if (negative) {
                const double plane = (double(hot.MaxBound[a]) + 0.001) - position;
                if (plane >= 0) result.Bits |= negative;
                if constexpr (TrackPlanes) {
                    if (std::abs(plane) <= 1.00001) {
                        cache->IncludePlane(a, plane);
                        result.MayCross = true;
                    }
                }
            }
        }
        if (result.Bits == requested) break;
    }
    return result;
}
}  // namespace

uint32_t ComputeHotSlotVisibleMask(
    const TerrainHotSlot& hot,
    const radray::array<int32_t, 3>& block,
    const radray::array<double, 3>& eye,
    bool valid,
    std::span<const rn_instance> instances,
    uint32_t requested) noexcept {
    return EvaluateFaces<false>(hot, block, eye, valid, instances, hot.FullMask & requested, nullptr).Bits;
}

uint32_t TerrainEyeCrossingCache::Capture(
    const rn_visible_item& item,
    const TerrainHotSlot& hot,
    std::span<const rn_instance> instances) {
    const auto requested = uint8_t(item.element_mask & hot.ValidMask);
    const auto result = EvaluateFaces<true>(hot, BaseBlock, Center, EyeValid, instances, requested, this);
    if (result.MayCross) {
        CrossingItems.push_back({uint32_t(item.mesh), item.first_instance, item.instance_count, requested, uint8_t(result.Bits)});
    }
    return result.Bits;
}

bool AreProjectedBoundsDisjoint(const ProjectedTerrainBounds& a, const ProjectedTerrainBounds& b) noexcept {
    return a.Valid && b.Valid && (a.Right < b.Left || b.Right < a.Left || a.Top < b.Bottom || b.Top < a.Bottom);
}

ProjectedTerrainBounds ProjectTerrainBounds(
    const TerrainHotSlot& hot,
    const rn_instance& instance,
    const rn_terrain_view_desc& frame) noexcept {
    if (!(hot.Flags & kTerrainHotFlagFiniteBounds)) return {};
    const auto view = Eigen::Map<const Eigen::Matrix4f>(frame.view);
    const auto projection = Eigen::Map<const Eigen::Matrix4f>(frame.projection);
    const float inf = std::numeric_limits<float>::infinity();
    ProjectedTerrainBounds bounds{inf, -inf, inf, -inf, true};
    for (unsigned corner = 0; corner < 8; ++corner) {
        Eigen::Vector4f position;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const int64_t delta = int64_t(instance.origin[axis]) - frame.camera_block[axis];
            if (delta < INT32_MIN || delta > INT32_MAX) return {};
            position[axis] = ((corner & (1u << axis) ? hot.MaxBound[axis] : hot.MinBound[axis]) + float(delta)) - frame.camera_fraction[axis];
        }
        position.w() = 1;
        const Eigen::Vector4f clip = projection * (view * position);
        if (!clip.allFinite() || clip.w() <= 0.001f) return {};
        const float x = clip.x() / clip.w();
        const float y = clip.y() / clip.w();
        if (!std::isfinite(x) || !std::isfinite(y)) return {};
        constexpr float kMargin = 0.001f;
        bounds.Left = std::min(bounds.Left, x - kMargin);
        bounds.Right = std::max(bounds.Right, x + kMargin);
        bounds.Bottom = std::min(bounds.Bottom, y - kMargin);
        bounds.Top = std::max(bounds.Top, y + kMargin);
    }
    return bounds;
}

bool TerrainResidency::GroupDisjointClassicItems(std::span<const rn_instance> instances, const rn_terrain_view_desc& frame) {
    auto& items = _fastClassicTranslucent;
    if (items.size() < 3 || (_translucentVertices->GetPageCount() < 2 && _indices->GetPageCount() < 2)) return false;
    const auto samePage = [](const auto& a, const auto& b) { return a.VertexPageId == b.VertexPageId && a.IndexPageId == b.IndexPageId; };
    _classicBounds.clear();
    for (const auto& item : items) {
        _classicBounds.push_back(
            item.Arg.InstanceCount == 1 ? ProjectTerrainBounds(_hotSlots[item.SlotId], instances[item.Arg.FirstInstance], frame)
                                        : ProjectedTerrainBounds{});
    }
    bool reordered = false;
    for (size_t i = 1; i + 1 < items.size(); ++i) {
        if (samePage(items[i - 1], items[i])) continue;
        for (size_t j = i + 1; j < std::min(items.size(), i + 8); ++j) {
            if (!samePage(items[i - 1], items[j])) continue;
            if (!std::all_of(_classicBounds.begin() + i, _classicBounds.begin() + j, [&](const auto& crossed) {
                    return AreProjectedBoundsDisjoint(_classicBounds[j], crossed);
                })) {
                break;
            }
            std::rotate(items.begin() + i, items.begin() + j, items.begin() + j + 1);
            std::rotate(_classicBounds.begin() + i, _classicBounds.begin() + j, _classicBounds.begin() + j + 1);
            reordered = true;
            break;
        }
    }
    return reordered;
}

void TerrainEyeCrossingCache::Reset(const radray::array<int32_t, 3>& block, const radray::array<double, 3>& eye, bool valid) noexcept {
    BaseBlock = block;
    Center = eye;
    EyeValid = valid;
    CrossingItems.clear();
    Lower.fill(-1);
    Upper.fill(1);
    LastDelta = {};
}

bool TerrainEyeCrossingCache::Matches(
    const radray::array<int32_t, 3>& block,
    const radray::array<double, 3>& eye,
    bool valid,
    std::span<const TerrainHotSlot> slots,
    std::span<const rn_instance> instances) noexcept {
    if (valid != EyeValid) return false;
    if (!valid || (block == BaseBlock && eye == Center)) return true;
    radray::array<double, 3> delta;
    bool insidePlanes = true;
    for (size_t a = 0; a < 3; ++a) {
        delta[a] = double(int64_t(block[a]) - BaseBlock[a]) + eye[a] - Center[a];
        if (!(std::abs(delta[a]) < 1.0)) return false;
        if (delta[a] != 0 && !(delta[a] > Lower[a] && delta[a] < Upper[a])) insidePlanes = false;
    }
    if (insidePlanes || LastDelta == delta) return true;
    for (const auto& item : CrossingItems) {
        const auto mask = ComputeHotSlotVisibleMask(
            slots[item.Mesh], block, eye, true, instances.subspan(item.FirstInstance, item.InstanceCount), item.RequestedMask);
        if (mask != item.EffectiveMask) return false;
    }
    LastDelta = delta;
    return true;
}

void EmitHotSlotSolidIndirectArgs(
    const TerrainHotSlot& hot,
    const TerrainMeshLayout* wideFallbackLayout,
    uint32_t quadBaseFirstIndex,
    uint32_t mask,
    uint32_t firstInstance,
    uint32_t instanceCount,
    radray::vector<radray::gpu::DrawIndexedIndirectArguments>& outArgs) {
    mask &= hot.ValidMask;
    if (!mask || !instanceCount) return;
    if (instanceCount == 1 && mask == hot.ValidMask) {
        outArgs.push_back({hot.IndexCount, 1, quadBaseFirstIndex, int32_t(hot.FirstVertex), firstInstance});
        return;
    }
    const bool wide = (hot.Flags & kTerrainHotFlagWideCounts) != 0;
    if (wide && !wideFallbackLayout) return;
    const auto endIndex = [&](uint32_t range) {
        if (wide) {
            const auto& r = wideFallbackLayout->Ranges[range];
            return r.First + r.Count;
        }
        return range == 6 ? hot.IndexCount : uint32_t(hot.RangeEndQuads[range]) * 6;
    };
    while (mask) {
        const uint32_t first = std::countr_zero(mask);
        const uint32_t count = instanceCount == 1 ? std::countr_one(mask >> first) : 1;
        const uint32_t begin = first ? endIndex(first - 1) : 0;
        outArgs.push_back({endIndex(first + count - 1) - begin, instanceCount, quadBaseFirstIndex + begin, int32_t(hot.FirstVertex), firstInstance});
        mask &= ~(((1u << count) - 1) << first);
    }
}

void TerrainLayerDrawData::Clear() noexcept {
    Prepared.Clear();
    IndirectReps.clear();
    FastCommands.clear();
    FastBindings.Clear();
    IndirectSize = 0;
    RecordCount = 0;
    SelectedGroups = 0;
    IndicesInstances = 0;
}

void TerrainViewDrawData::Clear() noexcept {
    for (auto& layer : Layers) layer.Clear();
    for (auto& reps : OitIndirectReps) reps.clear();
    Oit = false;
    Wireframe = false;
    HasAnyDraws = false;
    TotalIndirectArguments = 0;
}

bool TerrainViewCacheSlot::MatchesVisible(const rn_terrain_view_desc& view) const noexcept {
    return Epoch == view.epoch && view.visible_version && VisibleVersion == view.visible_version && Visible.size() == view.visible_count;
}

bool TerrainViewCacheSlot::MatchesOrigins(const rn_terrain_view_desc& view) const noexcept {
    return Epoch == view.epoch && view.origins_version && OriginsVersion == view.origins_version && Instances.size() == view.instance_count;
}

bool TerrainViewCacheSlot::MatchesInstances(const rn_terrain_view_desc& view) const noexcept {
    return Epoch == view.epoch && view.instances_version && InstancesVersion == view.instances_version && Instances.size() == view.instance_count;
}

TerrainResidency::TerrainResidency(
    radray::DeviceContext& device,
    radray::unique_ptr<radray::VertexBufferPool> vertices,
    radray::unique_ptr<radray::VertexBufferPool> translucentVertices,
    radray::unique_ptr<radray::IndexBufferPool> quadPool,
    radray::unique_ptr<radray::IndexBufferPool> indices)
    : _device(device),
      _vertices(std::move(vertices)),
      _translucentVertices(std::move(translucentVertices)),
      _quadPool(std::move(quadPool)),
      _indices(std::move(indices)) {
    _state.MeshesBySlot.reserve(4096);
    _hotSlots.reserve(4096);
    _fastClassicTranslucent.reserve(1024);
}

TerrainViewCacheSlot& TerrainResidency::View(size_t index) {
    return *_views[index];
}

const radray::VertexBufferPool& TerrainResidency::VertexPool() const noexcept {
    return *_vertices;
}

const radray::VertexBufferPool& TerrainResidency::TranslucentVertexPool() const noexcept {
    return *_translucentVertices;
}

const radray::IndexBufferPool& TerrainResidency::QuadIndexPool() const noexcept {
    return *_quadPool;
}

const radray::IndexBufferPool& TerrainResidency::TranslucentIndexPool() const noexcept {
    return *_indices;
}

const radray::IndexBufferSubAllocation& TerrainResidency::QuadIndices() const noexcept {
    return _quadIndices;
}

TerrainResidency::RetiredTerrainAllocations& TerrainResidency::Retired(radray::FrameSlot& slot) {
    if (!_retired) _retired = slot.Emplace<RetiredTerrainAllocations>();
    return *_retired;
}

void TerrainResidency::RetireMesh(TerrainMesh& mesh, radray::FrameSlot& slot) {
    auto& retired = Retired(slot);
    if (mesh.Geometry) retired.Vertices.push_back(std::move(mesh.Geometry));
    if (mesh.Indices) retired.Indices.push_back(std::move(mesh.Indices));
    for (auto& wire : mesh.Wireframes) retired.Indices.push_back(std::move(wire.Indices));
    mesh = {};
}

void TerrainResidency::BeginSlotViews(std::span<const rn_terrain_view_desc> views, radray::FrameSlot& slot) {
    _retired = nullptr;
    if (!views.empty()) {
        const bool wireframe = std::any_of(views.begin(), views.end(), [](const auto& view) { return (view.flags & RN_FRAME_WIREFRAME) != 0; });
        if (_state.WireframeActive && !wireframe) {
            ++_layerGeometryVersion[0];
            ++_layerGeometryVersion[1];
            for (auto& mesh : _state.MeshesBySlot) {
                if (!mesh.Active || mesh.Wireframes.empty()) continue;
                for (auto& wire : mesh.Wireframes) {
                    Retired(slot).Indices.push_back(std::move(wire.Indices));
                }
                mesh.Wireframes.clear();
            }
        }
        _state.WireframeActive = wireframe;
        _views.resize(views.size());
        for (auto& view : _views) {
            if (!view) view = radray::make_unique<TerrainViewCacheSlot>();
        }
    }
    _quadPool->TrimEmptyPages();
    // Retired allocations from two frames ago were released during FrameScheduler::BeginFrame.
    if (_vertices && (_vertices->GetPageCount() > 1 || (_state.ResidentMeshCount == 0 && _vertices->GetPageCount() > 0))) {
        _vertices->TrimEmptyPages();
    }
    if (_translucentVertices && (_translucentVertices->GetPageCount() > 1 || (_state.ResidentMeshCount == 0 && _translucentVertices->GetPageCount() > 0))) {
        _translucentVertices->TrimEmptyPages();
    }
    _indices->TrimEmptyPages();
}

bool SlotPromotedBuffer::Prepare(
    radray::gpu::Device& device,
    bool unchanged,
    uint64_t bytes,
    radray::gpu::BufferUses usage,
    radray::IntrusivePtr<radray::GpuBuffer>& output,
    bool& upload,
    bool hasDraws) {
    constexpr uint64_t kMinCapacity = 4096;
    uint32_t nextStable = 0;
    if (bytes && unchanged && bytes == LastBytes) {
        nextStable = StableFrames;
        if (hasDraws && nextStable < 2) ++nextStable;
    }
    const bool persistent = hasDraws && nextStable == 2;
    const uint64_t capacity = Buffer ? Buffer->GetDesc().Size : 0;
    const bool grow = persistent && bytes > capacity;
    const bool needUpload = persistent && (StableFrames < 2 || grow);
    if (grow) {
        const uint64_t deviceLimit = device.GetCapabilities().Limits.MaxBufferSize;
        const uint64_t limit = deviceLimit ? deviceLimit : std::numeric_limits<uint64_t>::max();
        if (bytes > limit) {
            RADRAY_ERR_LOG("Terrain buffer of {} bytes exceeds device limit {}", bytes, limit);
            return false;
        }
        const uint64_t desired = std::max(bytes, kMinCapacity);
        const uint64_t newCapacity = desired > std::bit_floor(limit) ? limit : std::bit_ceil(desired);
        auto created = radray::GpuBuffer::Create(device, {.Size = newCapacity, .Usage = usage | radray::gpu::BufferUse::CopyDestination});
        if (!created) {
            RADRAY_ERR_LOG("Cannot promote terrain buffer to {} bytes", newCapacity);
            return false;
        }
        Buffer = created.Release();
    } else if (!bytes || (capacity > kMinCapacity && bytes < capacity / 4)) {
        Buffer = {};
    }
    LastBytes = bytes;
    StableFrames = nextStable;
    output = persistent ? Buffer : nullptr;
    upload = needUpload;
    return true;
}

bool TerrainResidency::EnsureQuadIndices(
    uint32_t vertexCount,
    std::pmr::vector<GeometryUpload>& uploads,
    radray::FrameSlot* retireSlot,
    uint64_t* uploadBytesOut,
    bool* grewOut) {
    if (grewOut) *grewOut = false;
    if (vertexCount <= _quadVertexCapacity) return true;
    uint32_t capacity = 65536;
    while (capacity < vertexCount) {
        if (capacity > UINT32_MAX / 2) {
            RADRAY_ERR_LOG("Shared quad index capacity overflow");
            return false;
        }
        capacity *= 2;
    }
    auto allocation = _quadPool->Allocate(capacity / 4 * 6);
    if (!allocation) {
        RADRAY_ERR_LOG("Cannot allocate shared quad indices");
        return false;
    }
    auto& owner = *allocation;
    if (uploadBytesOut) *uploadBytesOut += owner.GetByteSize();
    uploads.push_back({
        owner.GetGpuBuffer(),
        owner.GetByteOffset(),
        radray::RDGUploadSource::Generated(
            owner.GetByteSize(),
            capacity,
            +[](const uint32_t& vertices, radray::RDGUploadWriteContext& context) noexcept {
                return WriteQuadIndices(context.Bytes, vertices);
            }),
    });
    if (_quadIndices && retireSlot) Retired(*retireSlot).Indices.push_back(std::move(_quadIndices));
    _quadIndices = std::move(owner);
    _quadVertexCapacity = capacity;
    ++_layerGeometryVersion[0];
    ++_layerGeometryVersion[1];
    if (grewOut) *grewOut = true;
    return true;
}

bool TerrainResidency::ResyncQuadMeshes() {
    ++_layerGeometryVersion[0];
    ++_layerGeometryVersion[1];
    for (uint32_t slotId = 1; slotId < _state.MeshesBySlot.size(); ++slotId) {
        auto& mesh = _state.MeshesBySlot[slotId];
        if (!mesh.Active || mesh.Layout.Layer == RN_MATERIAL_TRANSLUCENT) continue;
        if (!ValidateAndSyncHotSlot(slotId)) return false;
    }
    return true;
}

bool TerrainResidency::ValidateAndSyncHotSlot(uint32_t slotId) {
    auto& mesh = _state.MeshesBySlot[slotId];
    const auto& index = mesh.Layout.Layer == RN_MATERIAL_TRANSLUCENT ? mesh.Indices : _quadIndices;
    if (!index) {
        RADRAY_ERR_LOG("Terrain mesh references missing indices");
        return false;
    }
    if (mesh.Layout.Ranges.empty() || mesh.Layout.Ranges.size() >= 32) {
        RADRAY_ERR_LOG("Invalid terrain range layout");
        return false;
    }
    const uint64_t vertex = mesh.Geometry.GetFirstVertex();
    if (vertex > INT32_MAX) {
        RADRAY_ERR_LOG("Invalid terrain vertex offset");
        return false;
    }
    if (mesh.VertexPageId > UINT16_MAX || index.GetPageIndex() > UINT16_MAX) {
        RADRAY_ERR_LOG("Terrain geometry pool page index overflow");
        return false;
    }
    mesh.FullMask = (1u << mesh.Layout.Ranges.size()) - 1;
    mesh.ValidMask = 0;
    for (uint32_t n = 0; n < mesh.Layout.Ranges.size(); ++n) {
        const auto& range = mesh.Layout.Ranges[n];
        const uint64_t first = uint64_t(index.GetFirstIndex()) + range.First;
        if (uint64_t(range.First) + range.Count > index.GetIndexCount() || first > UINT32_MAX) {
            RADRAY_ERR_LOG("Invalid terrain index range");
            return false;
        }
        if (range.Count) mesh.ValidMask |= 1u << n;
    }
    if (slotId >= _hotSlots.size()) _hotSlots.resize(size_t(slotId) + 1);
    TerrainHotSlot hot{};
    for (uint32_t a = 0; a < 3; ++a) {
        hot.MinBound[a] = float(mesh.Layout.Minimum[a]);
        hot.MaxBound[a] = float(mesh.Layout.Maximum[a]);
        if (hot.MinBound[a] >= 0 && hot.MaxBound[a] <= 16) hot.SectionBoundsAxes |= uint8_t(1u << a);
    }
    hot.FirstVertex = uint32_t(vertex);
    hot.VertexPageId = uint16_t(mesh.VertexPageId);
    hot.IndexPageId = uint16_t(index.GetPageIndex());
    hot.Layer = uint8_t(mesh.Layout.Layer);
    hot.FullMask = uint8_t(mesh.FullMask);
    hot.ValidMask = uint8_t(mesh.ValidMask);
    hot.Flags = kTerrainHotFlagActive | kTerrainHotFlagHasIndices | (mesh.Layout.FiniteBounds ? kTerrainHotFlagFiniteBounds : 0u);
    hot.IndexCount = mesh.Layout.VertexCount / 4 * 6;
    if (mesh.Layout.Layer == RN_MATERIAL_TRANSLUCENT) {
        hot.FirstIndex = index.GetFirstIndex() + mesh.Layout.Ranges[0].First;
        hot.GroupBits[6] = uint8_t(mesh.Layout.GroupBits[6]);
    } else {
        for (uint32_t g = 0; g < 7; ++g) hot.GroupBits[g] = uint8_t(mesh.Layout.GroupBits[g]);
        for (size_t r = 0; r < std::min<size_t>(6, mesh.Layout.Ranges.size()); ++r) {
            const auto& range = mesh.Layout.Ranges[r];
            const uint32_t endQuads = (range.First + range.Count) / 6;
            if (endQuads > UINT16_MAX)
                hot.Flags |= kTerrainHotFlagWideCounts;
            else
                hot.RangeEndQuads[r] = uint16_t(endQuads);
        }
    }
    _hotSlots[slotId] = hot;
    return true;
}

void ImportGeometryUpload(GeometryUpload& upload, radray::RDGBuilder& graph) {
    const auto dst = graph.ImportPersistentOwned(upload.Target);
    if (upload.CopyRanges.empty()) {
        graph.UploadBuffer(dst, upload.Offset, std::move(upload.Source));
    } else {
        const auto size = upload.Source.GetMaxSize();
        const auto src = graph.CreateDynamicBuffer(
            {.MaxSize = size, .Usage = radray::gpu::BufferUse::CopySource, .Alignment = alignof(rn_instance)},
            std::move(upload.Source));
        struct Copies {
            radray::RDGBufferHandle Src;
            radray::RDGBufferHandle Dst;
            std::span<const radray::gpu::BufferRange> Ranges;
        };
        graph.AddCopyPass(
                 "Raynium terrain instance copies",
                 Copies{src, dst, upload.CopyRanges},
                 +[](const Copies& copies, radray::RDGCopyPassContext& ctx) noexcept {
                     radray::RDGBufferSlice from;
                     radray::RDGBufferSlice to;
                     {
                         RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.InstanceCopies.ResolveBuffers");
                         from = ctx.ResolveBuffer(copies.Src);
                         to = ctx.ResolveBuffer(copies.Dst);
                     }
                     auto srcOffset = from.Offset;
                     {
                         RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.InstanceCopies.RecordRanges");
                         for (const auto& range : copies.Ranges) {
                             ctx.GetCommandBuffer().CopyBufferToBuffer(to.Buffer.Get(), to.Offset + range.Offset, from.Buffer.Get(), srcOffset, range.Size);
                             srcOffset += range.Size;
                         }
                     }
                     RADRAY_PROFILE_PLOT("Raynium.Terrain.InstanceCopies.RangeCount", double(copies.Ranges.size()));
                     RADRAY_PROFILE_PLOT("Raynium.Terrain.InstanceCopies.Bytes", double(srcOffset - from.Offset));
                     return true;
                 })
            .ReadBuffer(src)
            .WriteBuffer(dst);
    }
    graph.MarkOutput(dst);
}

namespace {
bool UpdateInstancePool(const rn_terrain_view_desc& view, TerrainViewCacheSlot& cache, SlotUploads slot) {
    const bool reset = view.instance_base_version == 0;
    if (view.instance_capacity > TerrainResidency::kMaxMeshSlot || !Array(view.instance_slots, view.instance_count) ||
        !view.instances_version ||
        (!reset && (!cache.PooledInstances || cache.Epoch != view.epoch ||
                    cache.InstancesVersion != view.instance_base_version || view.instance_capacity < cache.Instances.size()))) {
        RADRAY_ERR_LOG("Invalid terrain instance pool base or extent");
        return false;
    }
    std::span ids{view.instance_slots, view.instance_count};
    for (uint32_t id : ids) {
        if (id >= view.instance_capacity) {
            RADRAY_ERR_LOG("Terrain instance update exceeds pool extent");
            return false;
        }
    }
    const size_t previousSize = cache.Instances.size();
    if (reset) cache.Instances.clear();
    cache.Instances.resize(view.instance_capacity);
    for (size_t i = 0; i < ids.size(); ++i) cache.Instances[ids[i]] = view.instances[i];
    if (cache.Instances.empty()) {
        cache.InstancePool = nullptr;
        return true;
    }
    const auto bytes = std::as_bytes(std::span(cache.Instances));
    const bool grow = !cache.InstancePool || cache.InstancePool->GetDesc().Size < bytes.size();
    if (grow) {
        uint64_t capacity = std::max<uint64_t>(4096, std::bit_ceil(bytes.size()));
        const uint64_t limit = slot.Frame.GetDevice().GetCapabilities().Limits.MaxBufferSize;
        if (limit) capacity = std::min(capacity, limit);
        if (capacity < bytes.size()) {
            RADRAY_ERR_LOG("Terrain instance pool exceeds device buffer limit");
            return false;
        }
        auto buffer = radray::GpuBuffer::Create(
            slot.Frame.GetDevice(),
            {.Size = capacity, .Usage = radray::gpu::BufferUse::Vertex | radray::gpu::BufferUse::CopyDestination});
        if (!buffer) {
            RADRAY_ERR_LOG("Cannot allocate terrain instance pool");
            return false;
        }
        cache.InstancePool = buffer.Release();
    }
    const auto upload = [&](radray::gpu::BufferRange range) {
        slot.Geometry.push_back({
            cache.InstancePool,
            range.Offset,
            radray::RDGUploadSource::BorrowedBytes(bytes.subspan(range.Offset, range.Size)),
        });
    };
    if (reset || grow || ids.size() > cache.Instances.size() / 2) {
        upload({0, bytes.size()});
        return true;
    }
    if (ids.empty() && previousSize == cache.Instances.size()) return true;
    auto& scratch = slot.Frame.GetScratchMemoryResource();
    std::pmr::vector<uint32_t> sorted(&scratch);
    sorted.reserve(ids.size());
    for (auto id : ids) {
        if (id < previousSize) sorted.push_back(id);
    }
    std::sort(sorted.begin(), sorted.end());
    std::pmr::vector<radray::gpu::BufferRange> ranges(&scratch);
    const auto append = [&](size_t first, size_t end) {
        const size_t beginByte = first * sizeof(rn_instance);
        const size_t endByte = end * sizeof(rn_instance);
        constexpr size_t kMaxGapBytes = 64;
        if (!ranges.empty() && beginByte <= ranges.back().Offset + ranges.back().Size + kMaxGapBytes) {
            ranges.back().Size = endByte - ranges.back().Offset;
        } else {
            ranges.push_back({beginByte, endByte - beginByte});
        }
    };
    for (auto id : sorted) append(id, id + 1);
    if (previousSize < cache.Instances.size()) append(previousSize, cache.Instances.size());
    if (ranges.size() == 1) {
        upload(ranges.front());
    } else {
        const auto copies = FreezeFrameArray(slot.Frame.GetMemoryResource(), ranges);
        size_t packedSize = 0;
        for (const auto& range : copies) packedSize += range.Size;
        struct Source {
            std::span<const radray::byte> Mirror;
            std::span<const radray::gpu::BufferRange> Ranges;
        };
        auto source = radray::RDGUploadSource::Generated(
            packedSize,
            Source{bytes, copies},
            +[](const Source& src, radray::RDGUploadWriteContext& ctx) noexcept {
                auto* out = ctx.Bytes.data();
                for (const auto& range : src.Ranges) {
                    std::memcpy(out, src.Mirror.data() + range.Offset, range.Size);
                    out += range.Size;
                }
                return true;
            });
        slot.Geometry.push_back({cache.InstancePool, 0, std::move(source), copies});
    }
    return true;
}
}  // namespace

bool TerrainResidency::PrepareFrame(
    rn_renderer* renderer,
    const rn_terrain_view_desc& f,
    const MeshPassResources& resources,
    SlotUploads slot,
    TerrainFrame& frame) {
    RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Prepare");

    constexpr uint32_t kAllowedFlags = RN_FRAME_RGSS | RN_FRAME_IMPROVED_TRANSPARENCY | RN_FRAME_WIREFRAME |
                                       RN_FRAME_REUSE_VISIBLE | RN_FRAME_REUSE_INSTANCES | RN_FRAME_INSTANCE_POOL;
    if (f.struct_size != sizeof(f) || (f.flags & ~kAllowedFlags) || !f.epoch || f.epoch < _state.Epoch ||
        !Array(f.indices, f.index_count, 65536) || !Array(f.meshes, f.mesh_count, 65536) ||
        !Array(f.removed_meshes, f.removed_mesh_count) || !Array(f.visible, f.visible_count) || !Array(f.instances, f.instance_count)) {
        RADRAY_ERR_LOG("Invalid frame layout, epoch or array: sequence={} epoch={} resident_epoch={}", f.output_sequence, f.epoch, _state.Epoch);
        return false;
    }
    const bool wireframe = (f.flags & RN_FRAME_WIREFRAME) != 0;
    const bool oit = (f.flags & RN_FRAME_IMPROVED_TRANSPARENCY) != 0;
    const bool pooled = (f.flags & RN_FRAME_INSTANCE_POOL) != 0;
    const bool reuseVisible = (f.flags & RN_FRAME_REUSE_VISIBLE) != 0;
    const bool reuseInstances = (f.flags & RN_FRAME_REUSE_INSTANCES) != 0;
    if (wireframe && !resources.SupportsWireframe()) {
        RADRAY_ERR_LOG("Terrain wireframe unsupported on this device");
        return false;
    }
    if (reuseInstances && !reuseVisible) {
        RADRAY_ERR_LOG("RN_FRAME_REUSE_INSTANCES requires RN_FRAME_REUSE_VISIBLE");
        return false;
    }
    auto& cache = slot.Cache;
    auto& draws = cache.Draws;
    const bool warm = cache.Epoch == f.epoch;
    if (reuseVisible) {
        if (pooled != cache.PooledInstances || !warm || f.epoch != _state.Epoch ||
            f.visible_count || f.mesh_count || f.removed_mesh_count) {
            RADRAY_ERR_LOG(
                "Invalid RN_FRAME_REUSE_VISIBLE usage: cached={} epoch={} vis={} meshes={} removed={}",
                warm, f.epoch, f.visible_count, f.mesh_count, f.removed_mesh_count);
            return false;
        }
        if (reuseInstances && (f.instance_count || (pooled && (!f.instance_base_version ||
                                                               f.instance_base_version != f.instances_version ||
                                                               f.instance_capacity != cache.Instances.size())))) {
            RADRAY_ERR_LOG("Invalid RN_FRAME_REUSE_INSTANCES usage: cached={} instances={}", warm, f.instance_count);
            return false;
        }
    }
    const bool hasVisibleItems = reuseVisible ? !cache.Visible.empty() : (f.visible_count != 0);
    if (hasVisibleItems) {
        if (f.terrain_anisotropy > 16 ||
            !ResolveHandle(renderer, f.terrain_albedo_texture) || !ResolveHandle(renderer, f.terrain_light_texture)) {
            RADRAY_ERR_LOG("Invalid terrain texture binding: anisotropy={}", f.terrain_anisotropy);
            return false;
        }
    }

    auto& state = _state;
    frame = {};
    frame.Parameters = f;
    frame.Draws = &draws;
    frame.Geometry = &slot.Geometry;
    auto& scalar = frame.Parameters;
    scalar.indices = nullptr;
    scalar.meshes = nullptr;
    scalar.removed_meshes = nullptr;
    scalar.visible = nullptr;
    scalar.instances = nullptr;
    scalar.instance_slots = nullptr;

    if (f.epoch != state.Epoch) {
        for (auto& owner : state.MeshesBySlot) {
            if (owner.Active) {
                RetireMesh(owner, slot.Frame);
            }
        }
        state.MeshesBySlot.clear();
        _hotSlots.clear();
        state.ResidentMeshCount = 0;
        for (auto& v : _layerGeometryVersion) ++v;
    }
    state.Epoch = f.epoch;

    uint64_t uploadBytes = 0;
    if (f.removed_mesh_count) {
        for (uint32_t i = 0; i < f.removed_mesh_count; ++i) {
            const uint64_t rawId = f.removed_meshes[i];
            if (!rawId || rawId >= state.MeshesBySlot.size()) continue;
            auto& owner = state.MeshesBySlot[size_t(rawId)];
            if (!owner.Active) continue;
            ++_layerGeometryVersion[owner.Layout.Layer];
            ++_partitionVersion;
            if (rawId < _hotSlots.size()) _hotSlots[size_t(rawId)] = {};
            --state.ResidentMeshCount;
            RetireMesh(owner, slot.Frame);
        }
    }

    uint32_t needed = _quadVertexCapacity;
    uint32_t maxMeshSlot = 0;
    for (const auto& x : std::span{f.meshes, f.mesh_count}) {
        if (!x.id || x.id > kMaxMeshSlot || !x.vertices || !x.layout || !x.vertex_count || x.vertex_count % 4 ||
            x.vertex_count > 1048576 || x.layer > RN_MATERIAL_TRANSLUCENT) {
            RADRAY_ERR_LOG("Invalid terrain mesh: id={} vertices={} layer={}", x.id, x.vertex_count, uint32_t(x.layer));
            return false;
        }
        maxMeshSlot = std::max(maxMeshSlot, uint32_t(x.id));
        if (x.layer != RN_MATERIAL_TRANSLUCENT) needed = std::max(needed, x.vertex_count);
    }
    if (maxMeshSlot >= state.MeshesBySlot.size()) {
        state.MeshesBySlot.resize(size_t(maxMeshSlot) + 1);
    }
    if (maxMeshSlot >= _hotSlots.size()) {
        _hotSlots.resize(size_t(maxMeshSlot) + 1);
    }
    bool quadGrew = false;
    if (!EnsureQuadIndices(needed, slot.Geometry, &slot.Frame, &uploadBytes, &quadGrew)) return false;

    if (f.mesh_count || f.index_count || quadGrew) {
        if (f.mesh_count) ++_partitionVersion;
        const uint64_t syncStamp = ++_syncStamp;
        _changedSlotsScratch.clear();
        const auto markChanged = [&](uint32_t slotId) {
            auto& m = state.MeshesBySlot[slotId];
            if (m.DirtyStamp != syncStamp) {
                m.DirtyStamp = syncStamp;
                _changedSlotsScratch.push_back(slotId);
            }
        };
        if (quadGrew) {
            for (uint32_t slotId = 1; slotId < state.MeshesBySlot.size(); ++slotId) {
                const auto& mesh = state.MeshesBySlot[slotId];
                if (mesh.Active && mesh.Layout.Layer != RN_MATERIAL_TRANSLUCENT) markChanged(slotId);
            }
        }

        for (const auto& x : std::span{f.meshes, f.mesh_count}) {
            TerrainMesh mesh;
            mesh.Active = true;
            const std::span<const radray::byte> vertexBytes{static_cast<const radray::byte*>(x.vertices), size_t(x.vertex_count) * 28};
            auto& pool = x.layer == RN_MATERIAL_TRANSLUCENT ? *_translucentVertices : *_vertices;
            if (!BuildPreanalyzedTerrainMeshLayout(*x.layout, x.vertex_count, x.layer, mesh.Layout)) return false;
            auto allocation = pool.Allocate(x.vertex_count);
            if (!allocation) {
                RADRAY_ERR_LOG("Cannot allocate terrain vertices");
                return false;
            }
            mesh.Geometry = std::move(*allocation);
            mesh.VertexPageId = mesh.Geometry.GetPageIndex();
            uploadBytes += vertexBytes.size();
            slot.Geometry.push_back({
                mesh.Geometry.GetGpuBuffer(),
                mesh.Geometry.GetByteOffset(),
                radray::RDGUploadSource::BorrowedBytes(vertexBytes),
            });
            const uint32_t slotId = uint32_t(x.id);
            auto& previous = state.MeshesBySlot[slotId];
            if (previous.Active) {
                ++_layerGeometryVersion[previous.Layout.Layer];
                if (x.layer == RN_MATERIAL_TRANSLUCENT && previous.Layout.Layer == x.layer) mesh.Indices = std::move(previous.Indices);
                RetireMesh(previous, slot.Frame);
            } else {
                ++state.ResidentMeshCount;
            }
            ++_layerGeometryVersion[x.layer];
            previous = std::move(mesh);
            markChanged(slotId);
        }

        for (const auto& x : std::span{f.indices, f.index_count}) {
            if (!x.mesh_id || x.mesh_id >= state.MeshesBySlot.size()) {
                RADRAY_ERR_LOG("Invalid transparent index update: mesh={} count={}", x.mesh_id, x.count);
                return false;
            }
            auto& mesh = state.MeshesBySlot[size_t(x.mesh_id)];
            if (!mesh.Active || mesh.Layout.Layer != RN_MATERIAL_TRANSLUCENT || x.reserved ||
                !Array(x.indices, x.count, 16u << 20) || x.count != mesh.Layout.VertexCount / 4 * 6) {
                RADRAY_ERR_LOG("Invalid transparent index update: mesh={} count={}", x.mesh_id, x.count);
                return false;
            }
            auto allocation = _indices->Allocate(x.count);
            if (!allocation) {
                RADRAY_ERR_LOG("Cannot allocate terrain indices");
                return false;
            }
            auto& owner = *allocation;
            uploadBytes += owner.GetByteSize();
            slot.Geometry.push_back({
                owner.GetGpuBuffer(),
                owner.GetByteOffset(),
                radray::RDGUploadSource::BorrowedBytes(std::as_bytes(std::span{x.indices, x.count})),
            });
            if (mesh.Indices) Retired(slot.Frame).Indices.push_back(std::move(mesh.Indices));
            mesh.Indices = std::move(owner);
            ++_layerGeometryVersion[RN_MATERIAL_TRANSLUCENT];
            markChanged(uint32_t(x.mesh_id));
        }

        for (uint32_t slotId : _changedSlotsScratch) {
            if (!ValidateAndSyncHotSlot(slotId)) return false;
        }
    }

    std::span<const rn_visible_item> activeVisible;
    std::span<const rn_instance> activeInstances;
    const bool sameMode = pooled == cache.PooledInstances;
    const bool visibleIdentical = reuseVisible || (sameMode && cache.MatchesVisible(f));
    const bool originsIdentical = pooled
                                      ? f.instance_base_version && f.origins_version && f.origins_version == cache.OriginsVersion
                                      : sameMode && (reuseInstances || cache.MatchesOrigins(f));
    const bool instancesIdentical = !pooled && sameMode && (reuseInstances || cache.MatchesInstances(f));
    if (!visibleIdentical) {
        if (f.visible_count)
            cache.Visible.assign(f.visible, f.visible + f.visible_count);
        else
            cache.Visible.clear();
        cache.PartitionVersion = 0;
    }
    if (!reuseVisible) cache.VisibleVersion = f.visible_version;
    if (pooled) {
        if (!UpdateInstancePool(f, cache, slot)) return false;
        frame.PersistentInstances = cache.InstancePool;
        cache.InstanceBuffer = {};
    } else if (!instancesIdentical) {
        cache.InstancePool = nullptr;
        if (f.instance_count)
            cache.Instances.assign(f.instances, f.instances + f.instance_count);
        else
            cache.Instances.clear();
    }
    cache.PooledInstances = pooled;
    if (!reuseInstances || pooled) {
        cache.OriginsVersion = f.origins_version;
        cache.InstancesVersion = f.instances_version;
    }
    activeVisible = cache.Visible;
    activeInstances = cache.Instances;

    frame.Instances = activeInstances;
    radray::array<size_t, 3> visibleCounts{};
    uint64_t totalBuiltBatches = 0;
    uint64_t totalBuiltRecords = 0;
    uint64_t uploadedIndirectBytes = 0;

    {
        const auto eye = ComputeTerrainEye(f);
        const radray::array<int32_t, 3> camBlock{f.camera_block[0], f.camera_block[1], f.camera_block[2]};
        const radray::array<double, 3> fracEye{
            double(f.camera_fraction[0]) + eye.Offset[0],
            double(f.camera_fraction[1]) + eye.Offset[1],
            double(f.camera_fraction[2]) + eye.Offset[2],
        };
        const bool fracEyeFinite = eye.Valid && std::isfinite(fracEye[0]) && std::isfinite(fracEye[1]) && std::isfinite(fracEye[2]);

        bool classicReordered = false;
        const bool canReuseCache = warm && draws.Oit == oit && draws.Wireframe == wireframe && visibleIdentical && cache.PartitionVersion == _partitionVersion;
        if (visibleIdentical && cache.PartitionVersion == _partitionVersion && cache.MaxInstanceEnd > activeInstances.size()) {
            RADRAY_ERR_LOG("Cached visible instance range exceeds current instance count");
            return false;
        }
        radray::array<bool, 3> layerHit{false, false, false};
        if (canReuseCache) {
            for (uint32_t layer = 0; layer < 2; ++layer) {
                layerHit[layer] = originsIdentical && cache.Layers[layer].Valid &&
                                  cache.Layers[layer].GeometryVersion == _layerGeometryVersion[layer] &&
                                  cache.Layers[layer].Crossing.Matches(camBlock, fracEye, fracEyeFinite, _hotSlots, activeInstances);
            }
            layerHit[2] = cache.Layers[2].Valid && cache.Layers[2].GeometryVersion == _layerGeometryVersion[2];
        }

        if (!warm || draws.Oit != oit || draws.Wireframe != wireframe) {
            draws.Clear();
            layerHit = {false, false, false};
        } else {
            for (uint32_t layer = 0; layer < 3; ++layer) {
                if (layerHit[layer]) continue;
                draws.Layers[layer].Clear();
                if (layer == RN_MATERIAL_TRANSLUCENT) {
                    for (auto& reps : draws.OitIndirectReps) reps.clear();
                }
            }
        }
        draws.Oit = oit;
        draws.Wireframe = wireframe;

        for (uint32_t layer = 0; layer < 3; ++layer) {
            if (layerHit[layer]) visibleCounts[layer] = cache.Layers[layer].VisibleCount;
        }

        const bool anyDirty = !layerHit[0] || !layerHit[1] || !layerHit[2];
        if (anyDirty) {
            for (uint32_t layer = 0; layer < 2; ++layer) {
                if (layerHit[layer]) continue;
                auto& crossing = cache.Layers[layer].Crossing;
                crossing.Reset(camBlock, fracEye, fracEyeFinite);
                if (wireframe) _wirePages[layer].clear();
                for (auto& bucket : _fastPageBuckets[layer]) {
                    bucket.Args.clear();
                    bucket.RepSlotId = 0;
                    bucket.HasNonZeroFirstInstance = false;
                }
            }
            if (!layerHit[2]) {
                if (oit) {
                    for (auto& bucket : _fastOitBuckets) bucket.Args.clear();
                    _fastOitBuckets.clear();
                    _oitPageLookup.clear();
                } else {
                    _fastClassicTranslucent.clear();
                }
            }

            const uint32_t quadBaseFirstIndex = _quadIndices ? _quadIndices.GetFirstIndex() : 0u;
            const auto* hotData = _hotSlots.data();
            const size_t hotCount = _hotSlots.size();
            const auto* meshesData = state.MeshesBySlot.data();
            const auto* instancesData = activeInstances.data();
            const size_t instanceCountTotal = activeInstances.size();
            size_t lastOitBucket = 0;

            radray::array<radray::InlineVector<uint32_t, 16>, 2> activePages;
            bool preparedItems = true;
            const auto processItem = [&](const rn_visible_item& item, const TerrainHotSlot& hot, uint32_t layer) {
                const uint32_t requested = item.element_mask == UINT32_MAX ? uint32_t(hot.FullMask) : item.element_mask;
                if (layer < 2u) {
                    const uint32_t mask = cache.Layers[layer].Crossing.Capture(item, hot, {instancesData + item.first_instance, item.instance_count});
                    if (!mask) return;
                    draws.Layers[layer].SelectedGroups += std::popcount(mask);
                    ++visibleCounts[layer];
                    uint32_t page = hot.VertexPageId;
                    auto& mesh = state.MeshesBySlot[item.mesh];
                    const TerrainWireframe* wire = nullptr;
                    if (wireframe) {
                        if (!mesh.Layout.OriginalQuadGroups.empty()) {
                            wire = WireIndices(mesh, requested, *_indices, slot.Geometry, uploadBytes);
                            if (!wire) {
                                preparedItems = false;
                                return;
                            }
                        }
                        const uint64_t indexPage = wire ? uint64_t(wire->Indices.GetPageIndex()) + 1 : 0;
                        auto& pages = _wirePages[layer];
                        page = pages.try_emplace((indexPage << 16) | hot.VertexPageId, uint32_t(pages.size())).first->second;
                    }
                    auto& buckets = _fastPageBuckets[layer];
                    if (page >= buckets.size()) buckets.resize(size_t(page) + 1);
                    auto& bucket = buckets[page];
                    if (bucket.Args.empty()) {
                        activePages[layer].push_back(page);
                        bucket.RepSlotId = uint32_t(item.mesh);
                        bucket.WireMask = wire ? requested : 0;
                    }
                    bucket.HasNonZeroFirstInstance |= (item.first_instance != 0u);
                    if (wire) {
                        bucket.Args.push_back({
                            wire->Indices.GetIndexCount(),
                            item.instance_count,
                            wire->Indices.GetFirstIndex(),
                            int32_t(hot.FirstVertex),
                            item.first_instance,
                        });
                        return;
                    }
                    const TerrainMeshLayout* wideLayout = (hot.Flags & kTerrainHotFlagWideCounts) ? &meshesData[size_t(item.mesh)].Layout : nullptr;
                    EmitHotSlotSolidIndirectArgs(hot, wideLayout, quadBaseFirstIndex, mask, item.first_instance, item.instance_count, bucket.Args);
                } else {
                    const uint32_t mask = requested & uint32_t(hot.ValidMask);
                    if (!mask) return;
                    draws.Layers[2].SelectedGroups += std::popcount(mask);
                    ++visibleCounts[2];
                    const radray::gpu::DrawIndexedIndirectArguments arg{
                        hot.IndexCount,
                        item.instance_count,
                        hot.FirstIndex,
                        int32_t(hot.FirstVertex),
                        item.first_instance,
                    };
                    if (oit) {
                        FastOitBucket* target = nullptr;
                        if (lastOitBucket < _fastOitBuckets.size() &&
                            _fastOitBuckets[lastOitBucket].VertexPageId == hot.VertexPageId &&
                            _fastOitBuckets[lastOitBucket].IndexPageId == hot.IndexPageId) {
                            target = &_fastOitBuckets[lastOitBucket];
                        } else {
                            const uint32_t key = (uint32_t(hot.VertexPageId) << 16) | hot.IndexPageId;
                            const auto [it, inserted] = _oitPageLookup.try_emplace(key, _fastOitBuckets.size());
                            lastOitBucket = it->second;
                            if (inserted) {
                                _fastOitBuckets.push_back({hot.VertexPageId, hot.IndexPageId, uint32_t(item.mesh), false, {}});
                                _fastOitBuckets.back().Args.reserve(256);
                            }
                            target = &_fastOitBuckets[lastOitBucket];
                        }
                        target->HasNonZeroFirstInstance |= (item.first_instance != 0u);
                        target->Args.push_back(arg);
                    } else {
                        const uint64_t sortKey = (uint64_t(item.sort_key) << 32) | uint64_t(hot.VertexPageId);
                        _fastClassicTranslucent.push_back({sortKey, item.sort_key, uint32_t(item.mesh), hot.VertexPageId, hot.IndexPageId, arg});
                    }
                }
            };

            if (cache.PartitionVersion != _partitionVersion) {
                for (auto& list : cache.LayerItems) list.clear();
                cache.MaxInstanceEnd = 0;
                for (size_t i = 0; i < activeVisible.size(); ++i) {
                    const auto& item = activeVisible[i];
                    if (!item.mesh || item.mesh >= hotCount || !item.instance_count ||
                        uint64_t(item.first_instance) + item.instance_count > instanceCountTotal) {
                        RADRAY_ERR_LOG("Invalid visible mesh or instance range: mesh={} first={} count={}", item.mesh, item.first_instance, item.instance_count);
                        return false;
                    }
                    const auto& hot = hotData[size_t(item.mesh)];
                    if (!(hot.Flags & kTerrainHotFlagActive) || !(hot.Flags & kTerrainHotFlagHasIndices) ||
                        !item.element_mask || (item.element_mask != UINT32_MAX && (item.element_mask & ~uint32_t(hot.FullMask)))) {
                        RADRAY_ERR_LOG("Invalid visible terrain submesh: mesh={} mask={}", item.mesh, item.element_mask);
                        return false;
                    }
                    cache.MaxInstanceEnd = std::max(cache.MaxInstanceEnd, uint64_t(item.first_instance) + item.instance_count);
                    cache.LayerItems[hot.Layer].push_back(uint32_t(i));
                    if (layerHit[hot.Layer]) continue;
                    processItem(item, hot, hot.Layer);
                }
                cache.PartitionVersion = _partitionVersion;
            } else {
                for (uint32_t layer = 0; layer < 3; ++layer) {
                    if (layerHit[layer]) continue;
                    for (uint32_t idx : cache.LayerItems[layer]) {
                        const auto& item = activeVisible[idx];
                        processItem(item, hotData[size_t(item.mesh)], layer);
                    }
                }
            }

            if (!preparedItems) return false;
            {
                RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.BuildArguments");
                for (uint32_t layer = 0; layer < 2; ++layer) {
                    if (layerHit[layer]) continue;
                    auto& layerData = draws.Layers[layer];
                    auto& prepared = layerData.Prepared;
                    size_t totalArgs = 0;
                    const size_t activeBuckets = activePages[layer].size();
                    for (uint32_t page : activePages[layer]) totalArgs += _fastPageBuckets[layer][page].Args.size();
                    if (totalArgs > UINT32_MAX) {
                        RADRAY_ERR_LOG("Terrain indirect argument count overflow");
                        return false;
                    }
                    if (totalArgs) {
                        prepared.IndexedArguments.reserve(totalArgs);
                        prepared.Batches.reserve(activeBuckets);
                        layerData.FastCommands.reserve(activeBuckets);
                        for (uint32_t page : activePages[layer]) {
                            const auto& bucket = _fastPageBuckets[layer][page];
                            const auto& repMesh = meshesData[bucket.RepSlotId];
                            const auto* indices = &_quadIndices;
                            if (bucket.WireMask) {
                                for (const auto& wire : repMesh.Wireframes) {
                                    if (wire.Mask == bucket.WireMask) {
                                        indices = &wire.Indices;
                                        break;
                                    }
                                }
                            }
                            const uint32_t batchIdx = uint32_t(prepared.Batches.size());
                            const uint32_t firstArg = uint32_t(prepared.IndexedArguments.size());
                            prepared.IndexedArguments.insert(prepared.IndexedArguments.end(), bucket.Args.begin(), bucket.Args.end());
                            prepared.Batches.push_back({
                                .CommandIndex = batchIdx,
                                .Indexed = true,
                                .IndexBuffer = indices->GetIndexBufferView(),
                                .FirstArgument = firstArg,
                                .ArgumentCount = uint32_t(bucket.Args.size()),
                                .HasNonZeroFirstInstance = bucket.HasNonZeroFirstInstance,
                            });
                            layerData.FastCommands.push_back(BuildFastTerrainCommand(
                                layerData.FastBindings,
                                wireframe ? resources.WireframePipeline() : resources.Pipeline(layer),
                                repMesh,
                                *indices));
                        }
                        layerData.IndirectReps.resize(activeBuckets);
                        for (size_t b = 0; b < activeBuckets; ++b) {
                            layerData.IndirectReps[b] = {
                                .SortKey = 0u,
                                .CachedCommand = &layerData.FastCommands[b],
                                .FirstInstance = 0u,
                                .InstanceCount = 1u,
                                .ElementMask = 1u,
                            };
                        }
                    }
                    layerData.RecordCount = uint32_t(prepared.IndexedArguments.size());
                    auto& cached = cache.Layers[layer];
                    cached.GeometryVersion = _layerGeometryVersion[layer];
                    cached.VisibleCount = visibleCounts[layer];
                    cached.Valid = true;
                }

                if (!layerHit[2]) {
                    auto& layerData = draws.Layers[2];
                    auto& prepared = layerData.Prepared;
                    if (oit) {
                        size_t totalArgs = 0;
                        for (const auto& bucket : _fastOitBuckets) totalArgs += bucket.Args.size();
                        if (totalArgs > UINT32_MAX) {
                            RADRAY_ERR_LOG("Terrain indirect argument count overflow");
                            return false;
                        }
                        if (totalArgs) {
                            const size_t batchCount = _fastOitBuckets.size();
                            prepared.IndexedArguments.reserve(totalArgs);
                            prepared.Batches.reserve(batchCount);
                            layerData.FastCommands.reserve(batchCount * 3);
                            for (size_t b = 0; b < batchCount; ++b) {
                                const auto& bucket = _fastOitBuckets[b];
                                const auto& repMesh = meshesData[bucket.RepSlotId];
                                const uint32_t firstArg = uint32_t(prepared.IndexedArguments.size());
                                prepared.IndexedArguments.insert(prepared.IndexedArguments.end(), bucket.Args.begin(), bucket.Args.end());
                                prepared.Batches.push_back({
                                    .CommandIndex = uint32_t(b),
                                    .Indexed = true,
                                    .IndexBuffer = repMesh.Indices.GetIndexBufferView(),
                                    .FirstArgument = firstArg,
                                    .ArgumentCount = uint32_t(bucket.Args.size()),
                                    .HasNonZeroFirstInstance = bucket.HasNonZeroFirstInstance,
                                });
                                layerData.FastCommands.push_back(BuildFastTerrainCommand(layerData.FastBindings, resources.Pipeline(OitBounds), repMesh, repMesh.Indices));
                                layerData.FastCommands.push_back(BuildFastTerrainCommand(layerData.FastBindings, resources.Pipeline(OitTransmittance), repMesh, repMesh.Indices));
                                layerData.FastCommands.push_back(BuildFastTerrainCommand(layerData.FastBindings, resources.Pipeline(OitAccumulate), repMesh, repMesh.Indices));
                            }
                            for (uint32_t s = 0; s < 3; ++s) {
                                draws.OitIndirectReps[s].resize(batchCount);
                                for (size_t b = 0; b < batchCount; ++b) {
                                    draws.OitIndirectReps[s][b] = {
                                        .SortKey = 0u,
                                        .CachedCommand = &layerData.FastCommands[b * 3 + s],
                                        .FirstInstance = 0u,
                                        .InstanceCount = 1u,
                                        .ElementMask = 1u,
                                    };
                                }
                            }
                        }
                    } else if (!_fastClassicTranslucent.empty()) {
                        if (_fastClassicTranslucent.size() >= 2) {
                            bool strictlyDescending = true;
                            for (size_t i = 1; i < _fastClassicTranslucent.size(); ++i) {
                                if (_fastClassicTranslucent[i - 1].RawSortKey <= _fastClassicTranslucent[i].RawSortKey) {
                                    strictlyDescending = false;
                                    break;
                                }
                            }
                            if (strictlyDescending) std::reverse(_fastClassicTranslucent.begin(), _fastClassicTranslucent.end());
                            bool sorted = true;
                            for (size_t i = 1; i < _fastClassicTranslucent.size(); ++i) {
                                if (_fastClassicTranslucent[i - 1].SortKey > _fastClassicTranslucent[i].SortKey) {
                                    sorted = false;
                                    break;
                                }
                            }
                            if (!sorted) {
                                RADRAY_PROFILE_SCOPE_N("Raynium.Terrain.Sort");
                                std::stable_sort(
                                    _fastClassicTranslucent.begin(),
                                    _fastClassicTranslucent.end(),
                                    [](const FastClassicTranslucentItem& a, const FastClassicTranslucentItem& b) noexcept {
                                        return a.SortKey < b.SortKey;
                                    });
                            }
                        }
                        classicReordered = GroupDisjointClassicItems(activeInstances, f);
                        if (_fastClassicTranslucent.size() > UINT32_MAX) {
                            RADRAY_ERR_LOG("Terrain indirect argument count overflow");
                            return false;
                        }
                        prepared.IndexedArguments.reserve(_fastClassicTranslucent.size());
                        _fastBatchRepSlots.clear();
                        uint16_t prevVPage = UINT16_MAX;
                        uint16_t prevIPage = UINT16_MAX;
                        for (const auto& item : _fastClassicTranslucent) {
                            const uint32_t argIdx = uint32_t(prepared.IndexedArguments.size());
                            prepared.IndexedArguments.push_back(item.Arg);
                            if (!prepared.Batches.empty() && item.VertexPageId == prevVPage && item.IndexPageId == prevIPage) {
                                auto& batch = prepared.Batches.back();
                                ++batch.ArgumentCount;
                                batch.HasNonZeroFirstInstance |= (item.Arg.FirstInstance != 0u);
                            } else {
                                const uint32_t batchIdx = uint32_t(prepared.Batches.size());
                                const auto& repMesh = meshesData[item.SlotId];
                                prepared.Batches.push_back({
                                    .CommandIndex = batchIdx,
                                    .Indexed = true,
                                    .IndexBuffer = repMesh.Indices.GetIndexBufferView(),
                                    .FirstArgument = argIdx,
                                    .ArgumentCount = 1u,
                                    .HasNonZeroFirstInstance = (item.Arg.FirstInstance != 0u),
                                });
                                _fastBatchRepSlots.push_back(item.SlotId);
                                prevVPage = item.VertexPageId;
                                prevIPage = item.IndexPageId;
                            }
                        }
                        const size_t batchCount = prepared.Batches.size();
                        layerData.FastCommands.reserve(batchCount);
                        for (size_t b = 0; b < batchCount; ++b) {
                            const auto& repMesh = meshesData[_fastBatchRepSlots[b]];
                            layerData.FastCommands.push_back(BuildFastTerrainCommand(
                                layerData.FastBindings,
                                wireframe ? resources.WireframePipeline() : resources.Pipeline(Translucent),
                                repMesh,
                                repMesh.Indices));
                        }
                        layerData.IndirectReps.resize(batchCount);
                        for (size_t b = 0; b < batchCount; ++b) {
                            layerData.IndirectReps[b] = {
                                .SortKey = 0u,
                                .CachedCommand = &layerData.FastCommands[b],
                                .FirstInstance = 0u,
                                .InstanceCount = 1u,
                                .ElementMask = 1u,
                            };
                        }
                    }
                    layerData.RecordCount = uint32_t(prepared.IndexedArguments.size());
                    cache.Layers[2] = {_layerGeometryVersion[2], {}, visibleCounts[2], !classicReordered};
                }
            }
        }

        draws.TotalIndirectArguments = 0;
        draws.HasAnyDraws = false;
        for (uint32_t layer = 0; layer < 3; ++layer) {
            auto& layerData = draws.Layers[layer];
            if (layerData.RecordCount) {
                if (layerData.RecordCount > UINT32_MAX - draws.TotalIndirectArguments) {
                    RADRAY_ERR_LOG("Terrain indirect argument count overflow");
                    return false;
                }
                layerData.IndirectSize = uint64_t(layerData.RecordCount) * sizeof(radray::gpu::DrawIndexedIndirectArguments);
                draws.TotalIndirectArguments += layerData.RecordCount;
                draws.HasAnyDraws = true;
            }
            totalBuiltRecords += layerData.RecordCount;
            totalBuiltBatches += layerData.Prepared.Batches.size();
        }

        cache.Epoch = f.epoch;
        if (!pooled && !cache.InstanceBuffer.Prepare(
                           _device.GetDevice(),
                           instancesIdentical,
                           frame.Instances.size_bytes(),
                           radray::gpu::BufferUse::Vertex,
                           frame.PersistentInstances,
                           frame.UploadInstances,
                           draws.HasAnyDraws)) {
            return false;
        }
        if (slot.UseIndirect) {
            for (uint32_t layer = 0; layer < 3; ++layer) {
                auto bytes = draws.Layers[layer].IndirectSize;
                if (!cache.ArgumentBuffers[layer].Prepare(
                        _device.GetDevice(),
                        layerHit[layer],
                        bytes,
                        radray::gpu::BufferUse::Indirect,
                        frame.PersistentIndirect[layer],
                        frame.UploadIndirect[layer])) {
                    return false;
                }
                if (!frame.PersistentIndirect[layer] || frame.UploadIndirect[layer]) uploadedIndirectBytes += bytes;
            }
        }
    }

    frame.HasTranslucent = draws.Layers[RN_MATERIAL_TRANSLUCENT].RecordCount != 0;
    RADRAY_PROFILE_PLOT("Raynium.Visible.Solid", double(visibleCounts[0]));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Cutout", double(visibleCounts[1]));
    RADRAY_PROFILE_PLOT("Raynium.Visible.Translucent", double(visibleCounts[2]));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.ArgumentBytes", double(uint64_t(draws.TotalIndirectArguments) * sizeof(radray::gpu::DrawIndexedIndirectArguments)));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.UploadedArgumentBytes", double(uploadedIndirectBytes));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltRecords", double(totalBuiltRecords));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.BuiltBatches", double(totalBuiltBatches));
    RADRAY_PROFILE_PLOT("Raynium.Terrain.OitParameterBuilds", double(frame.HasTranslucent && oit ? 1 : 0));
#ifdef RADRAY_ENABLE_PROFILER
    const radray::array<const char*, 3> groups{"Raynium.Terrain.Solid.SelectedGroups", "Raynium.Terrain.Cutout.SelectedGroups", "Raynium.Terrain.Translucent.SelectedGroups"};
    const radray::array<const char*, 3> records{"Raynium.Terrain.Solid.Records", "Raynium.Terrain.Cutout.Records", "Raynium.Terrain.Translucent.Records"};
    const radray::array<const char*, 3> batches{"Raynium.Terrain.Solid.Batches", "Raynium.Terrain.Cutout.Batches", "Raynium.Terrain.Translucent.Batches"};
    for (size_t layer = 0; layer < 3; ++layer) {
        RADRAY_PROFILE_PLOT(groups[layer], double(draws.Layers[layer].SelectedGroups));
        RADRAY_PROFILE_PLOT(records[layer], double(draws.Layers[layer].RecordCount));
        RADRAY_PROFILE_PLOT(batches[layer], double(draws.Layers[layer].Prepared.Batches.size()));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes", double(state.ResidentMeshCount));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexPages", double(_vertices->GetPageCount() + _translucentVertices->GetPageCount()));
    RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexPages", double(_indices->GetPageCount() + _quadPool->GetPageCount()));
#ifdef RADRAY_ENABLE_PROFILER
    if (f.output_sequence % 120 == 0) {
        const uint64_t vertexReserved = _vertices->GetCapacityBytes() + _translucentVertices->GetCapacityBytes();
        const uint64_t indexReserved = _indices->GetCapacityBytes() + _quadPool->GetCapacityBytes();
        const auto vertexFree = _vertices->GetFreeBytes() + _translucentVertices->GetFreeBytes();
        const auto indexFree = _indices->GetFreeBytes() + _quadPool->GetFreeBytes();
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexReservedBytes", double(vertexReserved));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexReservedBytes", double(indexReserved));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexFreeBytes", double(vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexFreeBytes", double(indexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.VertexAllocatedBytes", double(vertexReserved - vertexFree));
        RADRAY_PROFILE_PLOT("Raynium.Geometry.IndexAllocatedBytes", double(indexReserved - indexFree));
    }
#endif
    RADRAY_PROFILE_PLOT("Raynium.Uploads.Bytes", double(uploadBytes));
    return true;
}

void TerrainResidency::Reset() noexcept try {
    _state.MeshesBySlot.clear();
    _hotSlots.clear();
    _state.ResidentMeshCount = 0;
    _state.Epoch = 0;
    _state.WireframeActive = false;
    _views.clear();
    _changedSlotsScratch.clear();
    for (auto& layerBuckets : _fastPageBuckets) layerBuckets.clear();
    _fastOitBuckets.clear();
    _fastClassicTranslucent.clear();
    _fastBatchRepSlots.clear();
    _quadIndices = {};
    _quadVertexCapacity = 0;
} catch (...) {
    AbortUnexpectedException("TerrainResidency::Reset");
}

uint64_t TerrainResidency::ResidentBytes() const noexcept {
    uint64_t bytes = _quadIndices ? _quadIndices.GetByteSize() : 0;
    for (const auto& mesh : _state.MeshesBySlot) {
        if (!mesh.Active) continue;
        bytes += mesh.Geometry.GetByteSize();
        if (mesh.Indices) bytes += mesh.Indices.GetByteSize();
        for (const auto& wire : mesh.Wireframes) bytes += wire.Indices.GetByteSize();
    }
    return bytes;
}

void TerrainResidency::MeasureGeometry(rn_frame_metrics& metrics) const noexcept {
    metrics.vertex_capacity = _vertices->GetCapacityBytes() + _translucentVertices->GetCapacityBytes();
    metrics.index_capacity = _indices->GetCapacityBytes() + _quadPool->GetCapacityBytes();
    metrics.geometry_allocated = _vertices->GetAllocatedBytes() + _translucentVertices->GetAllocatedBytes() +
                                 _indices->GetAllocatedBytes() + _quadPool->GetAllocatedBytes();
}
}  // namespace raynium
