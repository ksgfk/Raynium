package dev.raynium.client.terrain;

import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.OptionsRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.util.Util;
import org.joml.Matrix4f;
import java.util.*;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicLong;

public final class TerrainCapture {
    public static final boolean ENABLED=Boolean.parseBoolean(System.getProperty("raynium.preview.enabled","true"))
        && !System.getProperty("raynium.preview.content","terrain").equals("uv");
    private static final AtomicLong EPOCH=new AtomicLong(1);
    private static final ConcurrentLinkedQueue<Long> REMOVED=new ConcurrentLinkedQueue<>();
    private static Matrix4f projection;
    private static Snapshot snapshot;
    private static volatile boolean active;
    public static boolean active() { return ENABLED && active; }
    public static void activate(boolean value) { active=value; if(!value) { snapshot=null; REMOVED.clear(); } }
    public record Draw(long id,int x,int y,int z,float visibility,int groupMask) {}
    public record Visible(MeshPayload payload, Draw draw) {}
    public record Snapshot(long epoch, List<Visible> visible, float[] projection, float[] view,
                           int[] block, float[] fraction, float[] fog, float[] distances, boolean rgss, int anisotropy) {}
    public static long epoch() { return EPOCH.get(); }
    public static void invalidate() { EPOCH.incrementAndGet(); snapshot=null; }
    public static void beginFrame() { snapshot=null; projection=null; }
    public static void projection(Matrix4f value) { if(active()) projection=new Matrix4f(value); }
    public static void removed(MeshPayload payload) { if(active() && payload!=null) REMOVED.add(payload.id); }
    public static long[] drainRemoved() {
        var removed=new ArrayList<Long>(); Long value;
        while((value=REMOVED.poll())!=null) removed.add(value);
        return removed.stream().mapToLong(Long::longValue).toArray();
    }
    public static Snapshot snapshot() { return snapshot; }
    public static void capture(LevelRenderer renderer, CameraRenderState camera, OptionsRenderState options) {
        if(!active() || projection==null) return;
        long epoch=epoch(), now=Util.getMillis(), fade=Util.toMillis(options.chunkSectionFadeInTime);
        var visible=new ArrayList<Visible>();
        var eye=new org.joml.Vector4f(0,0,1,0);
        new Matrix4f(projection).mul(camera.viewRotationMatrix).invert().transform(eye);
        boolean cull=!Boolean.getBoolean("raynium.terrain.disableFaceCull") && Float.isFinite(eye.w) && Math.abs(eye.w)>1e-8f;
        if(cull) eye.div(eye.w);
        var dispatcher=renderer.sectionRenderDispatcher();
        if(dispatcher==null) return;
        dispatcher.lock();
        try {
            for(var section:renderer.visibleSections()) {
                if(!(section.getSectionMesh() instanceof PayloadOwner owner)) continue;
                var payload=owner.raynium$payload();
                if(payload==null || payload.epoch!=epoch) continue;
                var origin=section.getRenderOrigin();
                int mask=cull ? payload.visibleGroups(camera.pos.x-origin.getX()+eye.x,camera.pos.y-origin.getY()+eye.y,camera.pos.z-origin.getZ()+eye.z) : 127;
                visible.add(new Visible(payload,new Draw(payload.id,origin.getX(),origin.getY(),origin.getZ(),section.getVisibility(now,fade),mask)));
            }
        } finally { dispatcher.unlock(); }
        visible.sort(Comparator.comparingDouble(v -> {
            var draw=v.draw(); double x=draw.x()+8.0-camera.pos.x,y=draw.y()+8.0-camera.pos.y,z=draw.z()+8.0-camera.pos.z;
            return x*x+y*y+z*z;
        }));
        var p=camera.pos; var b=camera.blockPos; var f=camera.fogData;
        snapshot=new Snapshot(epoch,List.copyOf(visible),projection.get(new float[16]),camera.viewRotationMatrix.get(new float[16]),
            new int[]{b.getX(),b.getY(),b.getZ()},new float[]{(float)(p.x-b.getX()),(float)(p.y-b.getY()),(float)(p.z-b.getZ())},
            new float[]{f.color.x,f.color.y,f.color.z,f.color.w},new float[]{f.environmentalStart,f.environmentalEnd,f.renderDistanceStart,f.renderDistanceEnd},options.textureFiltering==net.minecraft.client.TextureFilteringMethod.RGSS,
            options.textureFiltering==net.minecraft.client.TextureFilteringMethod.ANISOTROPIC?options.maxAnisotropyValue:1);
    }
}
