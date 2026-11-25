#include <raynium/batch_envelope.h>
#include <cstring>
#include <span>

namespace raynium {
namespace {
template<class T>
bool ValidArray(const T* data, uint64_t count, uint64_t limit) {
    return count <= limit && count <= PTRDIFF_MAX / sizeof(T) && (!count || data);
}

template<class T>
T* Copy(std::pmr::memory_resource& arena, const T* data, size_t count, size_t alignment = alignof(T)) {
    if (!count) return nullptr;
    auto* copy = static_cast<T*>(arena.allocate(count * sizeof(T), alignment));
    std::memcpy(copy, data, count * sizeof(T));
    return copy;
}

bool FreezeView(std::pmr::memory_resource& arena, rn_terrain_view_desc& view, bool borrow) {
    if (view.struct_size != sizeof(view) ||
        !ValidArray(view.indices, view.index_count, 65536) || !ValidArray(view.meshes, view.mesh_count, 65536) ||
        !ValidArray(view.removed_meshes, view.removed_mesh_count, 1048576) ||
        !ValidArray(view.visible, view.visible_count, 1048576) || !ValidArray(view.instances, view.instance_count, 1048576)) return false;
    if (borrow) return true;
    auto* meshes = Copy(arena, view.meshes, view.mesh_count);
    view.meshes = meshes;
    for (auto& mesh : std::span(meshes, view.mesh_count)) {
        if (mesh.vertices && mesh.vertex_count && mesh.vertex_count <= 1048576) {
            mesh.vertices = Copy(arena, static_cast<const radray::byte*>(mesh.vertices), size_t(mesh.vertex_count) * 28, 8);
        } else {
            mesh.vertices = nullptr;
        }
        if (mesh.layout) {
            auto* layout = Copy(arena, mesh.layout, 1);
            const uint64_t maxWf = (uint64_t(mesh.vertex_count / 4) * 3 + 7) / 8;
            if (layout->original_quad_groups && layout->original_quad_groups_bytes <= maxWf) {
                layout->original_quad_groups = Copy(arena, layout->original_quad_groups, layout->original_quad_groups_bytes);
            } else if (layout->original_quad_groups_bytes > maxWf) {
                layout->original_quad_groups = nullptr;
            }
            mesh.layout = layout;
        }
    }
    auto* indices = Copy(arena, view.indices, view.index_count);
    view.indices = indices;
    for (auto& update : std::span(indices, view.index_count)) {
        if (update.indices && update.count <= (16u << 20)) {
            update.indices = Copy(arena, update.indices, update.count);
        } else {
            update.indices = nullptr;
        }
    }
    view.removed_meshes = Copy(arena, view.removed_meshes, view.removed_mesh_count);
    view.visible = Copy(arena, view.visible, view.visible_count);
    view.instances = Copy(arena, view.instances, view.instance_count);
    return true;
}
}

bool BatchEnvelope::Freeze(const rn_batch_desc& input) {
    auto desc = input;
    if (desc.struct_size != sizeof(desc) || desc.reserved || desc.view_reserved || desc.capture_reserved || desc.query_reserved ||
        (desc.flags & ~(RN_BATCH_MEASURE | RN_BATCH_BORROW_POOLED)) ||
        !ValidArray(desc.views, desc.view_count, 65536) || !ValidArray(desc.dirty_ranges, desc.dirty_count, UINT32_MAX) ||
        !ValidArray(desc.captures, desc.capture_count, 4096) || !ValidArray(desc.queries, desc.query_count, UINT32_MAX)) return {};
    const bool borrow = (desc.flags & RN_BATCH_BORROW_POOLED) != 0;
    auto freeze = [&](const void*& data, uint64_t size) {
        const auto* bytes = static_cast<const radray::byte*>(data);
        if (!ValidArray(bytes, size, PTRDIFF_MAX)) return false;
        if (!borrow) data = Copy(arena, bytes, size, 8);
        return true;
    };
    if (!freeze(desc.commands, desc.command_bytes) || !freeze(desc.uploads, desc.upload_bytes) ||
        !freeze(desc.instance_uploads, desc.instance_upload_bytes)) return {};
    auto* views = Copy(arena, desc.views, desc.view_count);
    desc.views = views;
    for (auto& view : std::span(views, desc.view_count)) if (!FreezeView(arena, view, borrow)) return {};
    desc.dirty_ranges = Copy(arena, desc.dirty_ranges, desc.dirty_count);
    desc.flags &= ~RN_BATCH_BORROW_POOLED;
    desc.captures=Copy(arena,desc.captures,desc.capture_count);
    desc.queries=Copy(arena,desc.queries,desc.query_count);
    batch=desc;
    return true;
}
void BatchEnvelope::Reset() {
    resources.textureUploads.clear();
    resources.bufferUploads.clear();
    resources.removedModelMeshes.clear();
    shadows.clear();
    batch={};
    arena.release();
}
}
