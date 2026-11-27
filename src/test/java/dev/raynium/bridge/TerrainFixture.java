package dev.raynium.bridge;

import dev.raynium.client.terrain.MeshPayload;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

final class TerrainFixture extends NativeRenderer.Frame {
    record Mesh(long id, byte[] vertices, int vertexCount, int layer) {}
    record Index(long meshId, int[] values) {}
    record Visible(long mesh, int mask, int sortKey, int first, int count) {}
    record Instance(int x, int y, int z, int fadeStartMs) {}
    List<Mesh> meshes = List.of();
    List<Index> indices = List.of();
    List<Visible> visible = List.of();
    List<Instance> instances = List.of();
    long[] removedMeshes = {};
    long terrainAlbedoTexture, terrainLightTexture;
    MemorySegment marshal(NativeFrameBuffer buffer) {
        var payloads = new ArrayList<MeshPayload>(meshes.size());
        try {
            long bytes = 0;
            for (var mesh : meshes) {
                var payload = new MeshPayload(epoch, ByteBuffer.wrap(mesh.vertices), mesh.layer);
                payloads.add(payload);
                if (payload.vertexCount != mesh.vertexCount)
                    throw new IllegalArgumentException("Vertex count mismatch");
                if (payload.uploadLayout().originalQuadGroups != null)
                    bytes += NativeFrameBuffer.alignedBytes(payload.uploadLayout().originalQuadGroups.length);
            }
            buffer.begin(Math.max(visible.size(), instances.size()), removedMeshes.length, meshes.size(), indices.size(), bytes);
            buffer.terrain(terrainAlbedoTexture, terrainLightTexture, 1);
            for (int i = 0; i < meshes.size(); ++i) {
                var payload = payloads.get(i);
                var layout = payload.uploadLayout();
                buffer.meshPooled(meshes.get(i).id, payload.pin(), payload.vertexCount, payload.mode,
                                  layout.groupQuadCounts, payload.layoutFlags, layout.minBounds, layout.maxBounds, layout.originalQuadGroups, payload);
            }
            try (var scratch = Arena.ofConfined()) {
                for (var index : indices)
                    buffer.indexDirect(index.meshId, scratch.allocateFrom(ValueLayout.JAVA_INT, index.values), index.values.length, scratch);
            }
            for (var item : visible)
                buffer.visible(item.mesh, item.mask, item.sortKey, item.first, item.count);
            for (var item : instances)
                buffer.instance(item.x, item.y, item.z, item.fadeStartMs);
            for (long id : removedMeshes)
                buffer.removeMesh(id);
            return buffer.finish(this);
        } finally {
            for (var payload : payloads)
                payload.releaseVertices();
        }
    }
}
