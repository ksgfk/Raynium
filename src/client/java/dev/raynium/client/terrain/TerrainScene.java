package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeRenderer;
import java.util.ArrayList;

/** Converts MC quad streams and face groups to generic indexed submeshes. */
public final class TerrainScene {
    private TerrainScene() {}
    public static NativeRenderer.Mesh mesh(MeshPayload payload) {
        var ranges=new ArrayList<NativeRenderer.Submesh>(); int first=0;
        for(int count:payload.groupVertices) {
            int indices=count/4*6;
            if(indices>0) ranges.add(new NativeRenderer.Submesh(first,indices,0));
            first+=indices;
        }
        return new NativeRenderer.Mesh(payload.id,payload.vertices,1,payload.vertices.length/28,28,
            NativeRenderer.POSITION_COLOR_UV_LIGHT,ranges);
    }
    public static int[] quadIndices(int vertices) {
        if(vertices<4 || vertices%4!=0) throw new IllegalArgumentException("Expected complete quads");
        var indices=new int[vertices/4*6];
        for(int v=0,i=0;v<vertices;v+=4) {
            indices[i++]=v; indices[i++]=v+1; indices[i++]=v+2;
            indices[i++]=v+2; indices[i++]=v+3; indices[i++]=v;
        }
        return indices;
    }
}
