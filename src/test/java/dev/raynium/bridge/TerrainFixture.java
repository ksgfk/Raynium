package dev.raynium.bridge;

import dev.raynium.client.terrain.MeshPayload;
import java.lang.foreign.*;
import java.nio.*;
import java.util.*;

final class TerrainFixture extends NativeRenderer.Frame {
    record Mesh(long id,byte[] vertices,int vertexCount,int layer) {}
    record Index(long meshId,int[] values) {}
    record Visible(long mesh,int mask,int sortKey,int first,int count) {}
    record Instance(int x,int y,int z,int fadeStartMs) {}
    List<Mesh> meshes=List.of();
    List<Index> indices=List.of();
    List<Visible> visible=List.of();
    List<Instance> instances=List.of();
    long[] removedMeshes={};
    long terrainAlbedoTexture,terrainLightTexture;
    MemorySegment marshal(NativeFrameBuffer buffer) {
        var payloads=new ArrayList<MeshPayload>(meshes.size());
        try {
            long bytes=0;
            for(var mesh:meshes) {
                var payload=new MeshPayload(epoch,ByteBuffer.wrap(mesh.vertices),mesh.layer);
                payloads.add(payload);
                if(payload.vertexCount!=mesh.vertexCount) throw new IllegalArgumentException("Vertex count mismatch");
                if(payload.originalQuadGroups!=null) bytes+=NativeFrameBuffer.alignedBytes(payload.originalQuadGroups.length);
            }
            buffer.begin(Math.max(visible.size(),instances.size()),removedMeshes.length,meshes.size(),indices.size(),bytes);
            buffer.terrain(terrainAlbedoTexture,terrainLightTexture,1);
            for(int i=0;i<meshes.size();++i) buffer.meshPooled(meshes.get(i).id,payloads.get(i));
            try(var scratch=Arena.ofConfined()) {
                for(var index:indices) buffer.indexDirect(index.meshId,scratch.allocateFrom(ValueLayout.JAVA_INT,index.values),index.values.length,scratch);
            }
            for(var item:visible) buffer.visible(item.mesh,item.mask,item.sortKey,item.first,item.count);
            for(var item:instances) buffer.instance(item.x,item.y,item.z,item.fadeStartMs);
            for(long id:removedMeshes) buffer.removeMesh(id);
            return buffer.finish(this);
        } finally { for(var payload:payloads) payload.releaseVertices(); }
    }
}
