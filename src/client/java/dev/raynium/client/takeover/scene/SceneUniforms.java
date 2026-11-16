package dev.raynium.client.takeover.scene;

import dev.raynium.bridge.NativeSceneFrame;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import net.minecraft.client.renderer.state.GameRenderState;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.joml.Vector3f;
import net.minecraft.world.level.CardinalLighting;

/** std140 payloads shared by all feature batches using this view. */
public final class SceneUniforms {
    private NativeSceneFrame frame;
    private final ByteBuffer scratch=ByteBuffer.allocate(256).order(ByteOrder.nativeOrder());
    private final int[] values=new int[5];
    private final Matrix4f view=new Matrix4f(),viewScratch=new Matrix4f();
    public SceneUniforms() {}
    public SceneUniforms(NativeSceneFrame frame,GameRenderState state,Matrix4fc projection,Matrix4fc view,CardinalLighting.Type lighting) {
        reset(frame,state,projection,view,lighting);
    }
    public void reset(NativeSceneFrame frame,GameRenderState state,Matrix4fc projection,Matrix4fc view,CardinalLighting.Type lighting) {
        this.frame=frame; this.view.set(view); Arrays.fill(values,-1);
        zero(); matrix(projection,scratch,0); save("Projection");
        var camera=state.levelRenderState.cameraRenderState; var fog=camera.fogData;
        zero(); scratch.putFloat(0,fog.color.x).putFloat(4,fog.color.y).putFloat(8,fog.color.z).putFloat(12,fog.color.w);
        scratch.putFloat(16,fog.environmentalStart).putFloat(20,fog.environmentalEnd).putFloat(24,fog.renderDistanceStart).putFloat(28,fog.renderDistanceEnd);
        scratch.putFloat(32,fog.skyEnd).putFloat(36,fog.cloudEnd); save("Fog");
        zero(); globals(scratch,state); save("Globals");
        zero(); vector(new Vector3f(.2f,1,-.7f).normalize(),0);
        vector(new Vector3f(-.2f,lighting==CardinalLighting.Type.NETHER?-1:1,.7f).normalize(),16); save("Lighting");
    }
    public static void globals(ByteBuffer scratch,GameRenderState state) {
        var camera=state.levelRenderState.cameraRenderState;
        int x=(int)Math.floor(camera.pos.x),y=(int)Math.floor(camera.pos.y),z=(int)Math.floor(camera.pos.z);
        scratch.putInt(0,x).putInt(4,y).putInt(8,z).putFloat(12,(float)state.optionsRenderState.glintStrength);
        scratch.putFloat(16,(float)(x-camera.pos.x)).putFloat(20,(float)(y-camera.pos.y)).putFloat(24,(float)(z-camera.pos.z));
        scratch.putFloat(28,state.shouldRenderLevel?((float)(state.levelRenderState.gameTime%24000)+state.levelRenderState.worldPartialTicks)/24000:0);
        scratch.putFloat(32,state.windowRenderState.width).putFloat(36,state.windowRenderState.height);
        scratch.putInt(40,state.optionsRenderState.menuBackgroundBlurriness)
            .putInt(44,state.optionsRenderState.textureFiltering==net.minecraft.client.TextureFilteringMethod.RGSS?1:0);
    }
    public void itemsLighting() {
        var pose=new Matrix4f().scaling(1,-1,1).rotateYXZ(1.0821041f,3.2375858f,0)
            .rotateYXZ((float)(-Math.PI/8),(float)(Math.PI*3/4),0);
        zero(); vector(pose.transformDirection(new Vector3f(.2f,1,-.7f).normalize()),0);
        vector(pose.transformDirection(new Vector3f(-.2f,1,.7f).normalize()),16); save("Lighting");
    }
    public void noFog() {
        zero(); for(int at=16;at<=36;at+=4) scratch.putFloat(at,Float.MAX_VALUE); save("Fog");
    }
    public void offscreen(com.mojang.blaze3d.platform.Lighting.Entry lighting) {
        noFog();
        switch(lighting) {
            case ITEMS_3D -> itemsLighting();
            case ITEMS_FLAT -> {
                var pose=new Matrix4f().rotationY((float)(-Math.PI/8)).rotateX((float)(Math.PI*3/4));
                zero(); vector(pose.transformDirection(new Vector3f(.2f,1,-.7f).normalize()),0);
                vector(pose.transformDirection(new Vector3f(-.2f,1,.7f).normalize()),16); save("Lighting");
            }
            case ENTITY_IN_UI,PLAYER_SKIN -> {
                zero(); vector(new Vector3f(.2f,-1,1).normalize(),0); vector(new Vector3f(-.2f,-1,0).normalize(),16); save("Lighting");
            }
            case LEVEL -> {}
        }
    }
    // Minecraft enables JOML's direct-buffer fast path; heap scratch must use explicit stores.
    public static void matrix(Matrix4fc matrix,ByteBuffer target,int offset) {
        for(int column=0;column<4;column++) for(int row=0;row<4;row++) target.putFloat(offset+(column*4+row)*4,matrix.get(column,row));
    }
    private void vector(Vector3f vector,int offset) { scratch.putFloat(offset,vector.x).putFloat(offset+4,vector.y).putFloat(offset+8,vector.z); }
    private void zero() { scratch.clear(); for(int i=0;i<256;i+=8) scratch.putLong(i,0); }
    private static int slot(String name) {
        return switch(name) { case "Projection"->0; case "Fog"->1; case "Globals"->2; case "Lighting"->3; case "CloudInfo"->4;
            default->throw new IllegalArgumentException("Unmapped scene constant: "+name); };
    }
    private void save(String name) { values[slot(name)]=frame.copy(MemorySegment.ofBuffer(scratch)); }
    public void constant(String name,MemorySegment bytes) { values[slot(name)]=frame.copy(bytes); }
    public int transforms(Matrix4fc modelView,Matrix4fc texture) {
        zero(); matrix(modelView,scratch,0); matrix(texture,scratch,64);
        for(int i=0;i<4;i++) scratch.putFloat(128+i*4,1);
        return frame.copy(MemorySegment.ofBuffer(scratch));
    }
    public int transforms(Matrix4fc modelView,org.joml.Vector4fc color) {
        zero(); matrix(modelView,scratch,0);
        for(int i=0;i<4;i++) scratch.putFloat(64+i*20,1);
        scratch.putFloat(128,color.x()).putFloat(132,color.y()).putFloat(136,color.z()).putFloat(140,color.w());
        return frame.copy(MemorySegment.ofBuffer(scratch));
    }
    public int transforms(Matrix4fc modelView,org.joml.Vector4fc color,org.joml.Vector3fc offset,Matrix4fc texture) {
        zero(); matrix(modelView,scratch,0); matrix(texture,scratch,64);
        scratch.putFloat(128,color.x()).putFloat(132,color.y()).putFloat(136,color.z()).putFloat(140,color.w());
        scratch.putFloat(144,offset.x()).putFloat(148,offset.y()).putFloat(152,offset.z());
        return frame.copy(MemorySegment.ofBuffer(scratch));
    }
    /** Mutable scratch copy, valid until the next view call. */
    public Matrix4f view() { return viewScratch.set(view); }
    public void bind(ScenePrograms.Program program,int transforms) {
        for(var entry:program.constants().entrySet()) {
            int blob=entry.getKey().equals("DynamicTransforms")?transforms:values[slot(entry.getKey())];
            if(blob<0) throw new IllegalArgumentException("Unmapped scene constant: "+entry.getKey()+" in "+program.names().getFirst());
            program.constant(frame,entry.getKey(),blob);
        }
    }
}
