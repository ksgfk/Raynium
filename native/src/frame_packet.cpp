#include <raynium/frame_packet.h>
#include <cstring>
#include <span>

namespace raynium {
namespace {
template<class T>
bool ValidArray(const T* data, uint64_t count, uint64_t limit) {
    return count <= limit && count <= PTRDIFF_MAX / sizeof(T) && (!count || data);
}

template<class T>
T* Copy(std::pmr::memory_resource& arena, const T* data, size_t count) {
    if (!count) return nullptr;
    auto* copy = static_cast<T*>(arena.allocate(count * sizeof(T), alignof(T)));
    std::memcpy(copy, data, count * sizeof(T));
    return copy;
}

bool FreezeView(std::pmr::memory_resource& arena, rn_terrain_view_desc& view, bool borrow) {
    if (view.struct_size != sizeof(view) || !view.epoch ||
        !ValidArray(view.indices, view.index_count, 65536) || !ValidArray(view.meshes, view.mesh_count, 65536) ||
        !ValidArray(view.removed_meshes, view.removed_mesh_count, 1048576) ||
        !ValidArray(view.visible, view.visible_count, 1048576) || !ValidArray(view.instances, view.instance_count, 1048576)) return false;
    constexpr uint32_t allowed = RN_FRAME_RGSS | RN_FRAME_IMPROVED_TRANSPARENCY | RN_FRAME_WIREFRAME |
                                 RN_FRAME_REUSE_VISIBLE | RN_FRAME_REUSE_INSTANCES;
    const bool reuseVisible = (view.flags & RN_FRAME_REUSE_VISIBLE) != 0;
    if ((view.flags & ~allowed) || (reuseVisible && (view.visible_count || view.mesh_count || view.removed_mesh_count)) ||
        ((view.flags & RN_FRAME_REUSE_INSTANCES) && (!reuseVisible || view.instance_count))) return false;
    for (const auto& item : std::span(view.visible, view.visible_count)) {
        if (!item.mesh || !item.instance_count || uint64_t(item.first_instance) + item.instance_count > view.instance_count)
            return false;
    }
    for (const auto& mesh : std::span(view.meshes, view.mesh_count)) {
        if (!mesh.vertices || !mesh.vertex_count || mesh.vertex_count % 4 || mesh.vertex_count > 1048576) return false;
        if (mesh.layout && !ValidArray(mesh.layout->original_quad_groups, mesh.layout->original_quad_groups_bytes,
                                      (uint64_t(mesh.vertex_count / 4) * 3 + 7) / 8)) return false;
    }
    for (const auto& update : std::span(view.indices, view.index_count)) {
        if (update.reserved || !ValidArray(update.indices, update.count, 16u << 20)) return false;
    }
    if (borrow) return true;
    auto* meshes = Copy(arena, view.meshes, view.mesh_count);
    view.meshes = meshes;
    for (auto& mesh : std::span(meshes, view.mesh_count)) {
        mesh.vertices = Copy(arena, static_cast<const radray::byte*>(mesh.vertices), size_t(mesh.vertex_count) * 28);
        if (mesh.layout) {
            auto* layout = Copy(arena, mesh.layout, 1);
            layout->original_quad_groups = Copy(arena, layout->original_quad_groups, layout->original_quad_groups_bytes);
            mesh.layout = layout;
        }
    }
    auto* indices = Copy(arena, view.indices, view.index_count);
    view.indices = indices;
    for (auto& update : std::span(indices, view.index_count)) update.indices = Copy(arena, update.indices, update.count);
    view.removed_meshes = Copy(arena, view.removed_meshes, view.removed_mesh_count);
    view.visible = Copy(arena, view.visible, view.visible_count);
    view.instances = Copy(arena, view.instances, view.instance_count);
    return true;
}
}

radray::unique_ptr<FramePacket> FramePacket::Create(const rn_batch_desc& input) {
    auto desc = input;
    if (desc.struct_size != sizeof(desc) || desc.reserved || desc.view_reserved ||
        (desc.flags & ~(RN_BATCH_MEASURE | RN_BATCH_BORROW_POOLED)) ||
        !ValidArray(desc.views, desc.view_count, 65536) || !ValidArray(desc.dirty_ranges, desc.dirty_count, UINT32_MAX)) return {};
    auto storage = radray::make_unique<Storage>();
    auto& arena = storage->arena;
    const bool borrow = (desc.flags & RN_BATCH_BORROW_POOLED) != 0;
    auto freeze = [&](const void*& data, uint64_t size) {
        const auto* bytes = static_cast<const radray::byte*>(data);
        if (!ValidArray(bytes, size, PTRDIFF_MAX)) return false;
        if (!borrow) data = Copy(arena, bytes, size);
        return true;
    };
    if (!freeze(desc.commands, desc.command_bytes) || !freeze(desc.uploads, desc.upload_bytes) ||
        !freeze(desc.instance_uploads, desc.instance_upload_bytes)) return {};
    auto* views = Copy(arena, desc.views, desc.view_count);
    desc.views = views;
    for (auto& view : std::span(views, desc.view_count)) if (!FreezeView(arena, view, borrow)) return {};
    desc.dirty_ranges = Copy(arena, desc.dirty_ranges, desc.dirty_count);
    desc.flags &= ~RN_BATCH_BORROW_POOLED;
    radray::vector<uint64_t> captures;
    radray::vector<TimestampTarget> queries;
    if (!InspectCommandStream(desc, captures, queries)) return {};
    return radray::unique_ptr<FramePacket>(new FramePacket(std::move(storage), desc, std::move(captures), std::move(queries)));
}
}
