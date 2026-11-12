package dev.raynium.client.terrain;

import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.chunk.CompiledSectionMesh;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.util.Util;
import org.joml.Matrix4f;
import java.util.Arrays;
import java.util.Collection;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicLong;

public final class TerrainCapture {
    private static final net.minecraft.client.renderer.chunk.ChunkSectionLayer[] LAYERS=net.minecraft.client.renderer.chunk.ChunkSectionLayer.values();
    private static final AtomicLong EPOCH=new AtomicLong(1);
    private static final ConcurrentLinkedQueue<SectionPayload> REMOVED=new ConcurrentLinkedQueue<>();
    private static final Buffer BUFFER=new Buffer();
    private static final Matrix4f capturedProjection=new Matrix4f();
    private static boolean hasProjection,hasSnapshot;
    private static volatile boolean active;

    /** Borrowed on the render thread until beginFrame, invalidation or the next capture. */
    static final class Buffer {
        SectionPayload[] payloads=new SectionPayload[1024];
        int[] originX=new int[1024],originY=new int[1024],originZ=new int[1024];
        float[] visibility=new float[1024];
        int[] elementMask=new int[1024],distKey=new int[1024];
        MeshPayload.IndexSnapshot[] indices=new MeshPayload.IndexSnapshot[1024];
        int count;
        long epoch;
        final float[] projection=new float[16],view=new float[16],fraction=new float[3],fog=new float[4],distances=new float[4];
        final int[] block=new int[3];
        boolean rgss,improvedTransparency;
        int anisotropy;
        void put(int i,SectionPayload payload,int x,int y,int z,float fade,int mask,int distance) {
            if(i>=payloads.length) {
                int capacity=Math.max(i+1,payloads.length*2);
                payloads=Arrays.copyOf(payloads,capacity); indices=Arrays.copyOf(indices,capacity);
                originX=Arrays.copyOf(originX,capacity); originY=Arrays.copyOf(originY,capacity); originZ=Arrays.copyOf(originZ,capacity);
                visibility=Arrays.copyOf(visibility,capacity); elementMask=Arrays.copyOf(elementMask,capacity); distKey=Arrays.copyOf(distKey,capacity);
            }
            payloads[i]=payload; indices[i]=payload.layers[2]==null?null:payload.layers[2].indices; originX[i]=x; originY[i]=y; originZ[i]=z;
            visibility[i]=fade; elementMask[i]=mask; distKey[i]=distance;
        }
        void finish(int size) { if(size<count) { Arrays.fill(payloads,size,count,null); Arrays.fill(indices,size,count,null); } count=size; }
        void clear() { finish(0); }
    }
    public static boolean active() { return active; }
    public static void activate(boolean value) { active=value; if(!value) { clear(); REMOVED.clear(); } }
    public static long epoch() { return EPOCH.get(); }
    private static void clear() { BUFFER.clear(); hasSnapshot=hasProjection=false; }
    public static void invalidate() { EPOCH.incrementAndGet(); clear(); REMOVED.clear(); }
    public static void beginFrame() { clear(); }
    public static void projection(Matrix4f value) { if(active()) { capturedProjection.set(value); hasProjection=true; } }
    public static void removed(SectionPayload payload) { if(active() && payload!=null) REMOVED.add(payload); }
    public static void drainRemoved(Collection<SectionPayload> target) {
        SectionPayload value;
        while((value=REMOVED.poll())!=null) target.add(value);
    }
    static Buffer snapshot() { return hasSnapshot?BUFFER:null; }
    public static void capture(LevelRenderer renderer,CameraRenderState camera,OptionsRenderState options,boolean improvedTransparency) {
        if(!active() || !hasProjection) return;
        long epoch=epoch(),now=Util.getMillis(),fade=Util.toMillis(options.chunkSectionFadeInTime);
        var dispatcher=renderer.sectionRenderDispatcher();
        if(dispatcher==null) return;
        int count=0;
        dispatcher.lock();
        try {
            var sections=renderer.visibleSections();
            for(int i=0;i<sections.size();i++) {
                var section=sections.get(i); var mesh=section.getSectionMesh();
                if(mesh==CompiledSectionMesh.EMPTY || mesh==CompiledSectionMesh.UNCOMPILED || !(mesh instanceof PayloadOwner owner)) continue;
                var payload=owner.raynium$payload();
                if(payload==null || payload.epoch!=epoch) continue;
                var origin=section.getRenderOrigin(); int x=origin.getX(),y=origin.getY(),z=origin.getZ();
                int mask=0;
                for(int layer=0;layer<3;layer++) {
                    var part=payload.layers[layer]; if(part==null) continue;
                    var draw=mesh.getSectionDraw(LAYERS[layer]);
                    if(draw==null || !(mesh instanceof CompiledSectionMesh compiled) ||
                        !compiled.isVertexBufferUploaded(LAYERS[layer]) || !compiled.isIndexBufferUploaded(LAYERS[layer])) continue;
                    mask|=1<<(layer*8);
                }
                double dx=x+8.0-camera.pos.x,dy=y+8.0-camera.pos.y,dz=z+8.0-camera.pos.z;
                int distance=distanceKey(dx*dx+dy*dy+dz*dz);
                BUFFER.put(count++,payload,x,y,z,section.getVisibility(now,fade),mask,distance);
            }
        } finally { dispatcher.unlock(); }
        BUFFER.finish(count); BUFFER.epoch=epoch; BUFFER.improvedTransparency=improvedTransparency;
        capturedProjection.get(BUFFER.projection); camera.viewRotationMatrix.get(BUFFER.view);
        var p=camera.pos; var b=camera.blockPos; var f=camera.fogData;
        BUFFER.block[0]=b.getX(); BUFFER.block[1]=b.getY(); BUFFER.block[2]=b.getZ();
        BUFFER.fraction[0]=(float)(p.x-b.getX()); BUFFER.fraction[1]=(float)(p.y-b.getY()); BUFFER.fraction[2]=(float)(p.z-b.getZ());
        BUFFER.fog[0]=f.color.x; BUFFER.fog[1]=f.color.y; BUFFER.fog[2]=f.color.z; BUFFER.fog[3]=f.color.w;
        BUFFER.distances[0]=f.environmentalStart; BUFFER.distances[1]=f.environmentalEnd;
        BUFFER.distances[2]=f.renderDistanceStart; BUFFER.distances[3]=f.renderDistanceEnd;
        BUFFER.rgss=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.RGSS;
        BUFFER.anisotropy=options.textureFiltering==net.minecraft.client.TextureFilteringMethod.ANISOTROPIC?options.maxAnisotropyValue:1;
        hasSnapshot=true;
    }
    static int distanceKey(double squared) {
        float distance=(float)squared;
        return Float.floatToRawIntBits(Float.isNaN(distance)?Float.POSITIVE_INFINITY:Math.max(0,distance));
    }
}
