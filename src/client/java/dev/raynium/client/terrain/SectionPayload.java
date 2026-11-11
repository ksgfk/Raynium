package dev.raynium.client.terrain;

/** One adopted compiled section; all nonempty layers share its residency transition. */
public final class SectionPayload {
    public final long epoch;
    public final MeshPayload[] layers;
    public final long vertexBytes;
    public long uploadedEpoch,stagedFrame,lastActiveFrame,sectionKey;
    MeshPayload.IndexSnapshot uploadedIndices;
    public SectionPayload(long epoch,MeshPayload... layers) {
        this.epoch=epoch;
        this.layers=new MeshPayload[3];
        long bytes=0;
        for(var layer:layers) if(layer!=null) {
            if(layer.epoch!=epoch || this.layers[layer.mode]!=null) throw new IllegalArgumentException("Invalid section layers");
            this.layers[layer.mode]=layer; bytes+=layer.vertices.length;
        }
        vertexBytes=bytes;
    }
    long uploadBytes(MeshPayload.IndexSnapshot indices) { return vertexBytes+(indices==null?0:indices.values().length*4L); }
}
