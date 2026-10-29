package dev.raynium.bridge;

import java.lang.foreign.*;
import java.util.List;
import static java.lang.foreign.ValueLayout.*;

/** Owns shared terrain inputs and resident geometry, independently of preview output size. */
public final class NativeTerrain implements AutoCloseable {
    public record Mesh(long id, byte[] vertices, int[] groups) {
        public Mesh(long id, byte[] vertices) { this(id,vertices,new int[]{0,0,0,0,0,0,vertices.length/28}); }
    }
    public record Draw(long id, int x, int y, int z, float visibility, int groupMask) {
        public Draw(long id,int x,int y,int z,float visibility) { this(id,x,y,z,visibility,127); }
    }
    public record Frame(long epoch, long inputSequence, boolean direct, boolean atlasChanged, boolean rgss, int anisotropy, List<Mesh> updates, List<Draw> draws,
                        long[] removed, float[] projection, float[] view, int[] cameraBlock,
                        float[] cameraFraction, float[] fogColor, float[] fogDistances) {}
    private final NativeRuntime owner;
    private final NativeBindings bindings;
    private final List<NativePreview.Info> images;
    private final boolean splitMips;
    private MemorySegment handle;

    private NativeTerrain(NativeRuntime owner, NativeBindings bindings, MemorySegment handle,
                          List<NativePreview.Info> images, boolean splitMips) {
        this.owner=owner; this.bindings=bindings; this.handle=handle; this.images=List.copyOf(images); this.splitMips=splitMips;
    }
    static NativeTerrain create(NativeRuntime owner, NativeBindings bindings, MemorySegment runtime,
                                int width, int height, int mips, boolean split) {
        try (Arena arena=Arena.ofConfined()) {
            var desc=arena.allocate(24,4);
            desc.set(JAVA_INT,0,24); desc.set(JAVA_INT,4,width); desc.set(JAVA_INT,8,height);
            desc.set(JAVA_INT,12,mips); desc.set(JAVA_INT,16,split?1:0);
            int count=split?mips+1:2;
            if (count<2 || count>16) throw new IllegalArgumentException("Invalid mip count");
            var output=arena.allocate(ADDRESS); var info=arena.allocate(count*128L,8); var error=arena.allocate(1024);
            int status=(int)bindings.createTerrain.invokeExact(runtime,desc,output,info,count,error,1024);
            check(status,error,"create terrain");
            var handle=output.get(ADDRESS,0);
            var images=new java.util.ArrayList<NativePreview.Info>();
            try {
                for(int i=0;i<count;i++) images.add(NativePreview.Info.read(info.asSlice(i*128L,128)));
            } catch(RuntimeException failure) {
                int cleanup=(int)bindings.destroyTerrain.invokeExact(handle,error,1024);
                check(cleanup,error,"destroy unreadable terrain"); throw failure;
            }
            return new NativeTerrain(owner,bindings,handle,images,split);
        } catch(RuntimeException | Error e) { throw e; }
        catch(Throwable e) { throw new IllegalStateException("Cannot create terrain",e); }
    }
    public List<NativePreview.Info> images() { checkOpen(); return images; }
    public boolean splitMips() { return splitMips; }
    public void draw(NativePreview preview, long sequence, Frame f) {
        checkOpen();
        try(Arena arena=Arena.ofConfined()) {
            var frame=arena.allocate(248,8); frame.set(JAVA_INT,0,248); frame.set(JAVA_INT,4,(f.direct?1:0)|(f.atlasChanged?2:0)|(f.rgss?4:0)|(Boolean.getBoolean("raynium.terrain.profile")?8:0));
            frame.set(JAVA_LONG,8,f.epoch); frame.set(JAVA_LONG,16,f.inputSequence);
            var updates=arena.allocate(Math.max(1,f.updates.size())*56L,8);
            for(int i=0;i<f.updates.size();i++) {
                Mesh mesh=f.updates.get(i);
                if(mesh.vertices.length==0 || mesh.vertices.length%112!=0) throw new IllegalArgumentException("Expected complete 28-byte quads");
                var bytes=arena.allocateFrom(JAVA_BYTE,mesh.vertices);
                updates.set(JAVA_LONG,i*56L,mesh.id); updates.set(ADDRESS,i*56L+8,bytes);
                updates.set(JAVA_INT,i*56L+16,mesh.vertices.length/28);
                if(mesh.groups.length!=7) throw new IllegalArgumentException("Expected seven face groups");
                MemorySegment.copy(MemorySegment.ofArray(mesh.groups),0,updates,i*56L+24,28);
            }
            var draws=arena.allocate(Math.max(1,f.draws.size())*32L,8);
            for(int i=0;i<f.draws.size();i++) {
                Draw d=f.draws.get(i); long offset=i*32L;
                draws.set(JAVA_LONG,offset,d.id); draws.set(JAVA_INT,offset+8,d.x); draws.set(JAVA_INT,offset+12,d.y);
                draws.set(JAVA_INT,offset+16,d.z); draws.set(JAVA_FLOAT,offset+20,d.visibility); draws.set(JAVA_INT,offset+24,d.groupMask);
            }
            frame.set(ADDRESS,24,updates); frame.set(ADDRESS,32,draws); frame.set(ADDRESS,40,arena.allocateFrom(JAVA_LONG,f.removed));
            frame.set(JAVA_INT,48,f.updates.size()); frame.set(JAVA_INT,52,f.draws.size()); frame.set(JAVA_INT,56,f.removed.length); frame.set(JAVA_INT,60,f.anisotropy);
            floats(frame,64,f.projection,16); floats(frame,128,f.view,16);
            if(f.cameraBlock.length!=3) throw new IllegalArgumentException("Invalid camera block");
            MemorySegment.copy(MemorySegment.ofArray(f.cameraBlock),0,frame,192,12);
            floats(frame,204,f.cameraFraction,3); floats(frame,216,f.fogColor,4); floats(frame,232,f.fogDistances,4);
            var error=arena.allocate(1024);
            int status=(int)bindings.drawTerrain.invokeExact(preview.handle(),handle,sequence,frame,error,1024);
            check(status,error,"draw terrain");
        } catch(RuntimeException | Error e) { throw e; }
        catch(Throwable e) { throw new IllegalStateException("Cannot draw terrain",e); }
    }
    private static void floats(MemorySegment target,long offset,float[] values,int count) {
        if(values.length!=count) throw new IllegalArgumentException("Invalid frame vector/matrix");
        MemorySegment.copy(MemorySegment.ofArray(values),0,target,offset,count*4L);
    }
    private static void check(int status,MemorySegment error,String operation) {
        if(status!=0) throw new IllegalStateException(operation+": "+error.getString(0));
    }
    private void checkOpen() {
        owner.checkOpen(); if(handle.equals(MemorySegment.NULL)) throw new IllegalStateException("Terrain closed");
    }
    @Override public void close() {
        owner.checkThread(); if(handle.equals(MemorySegment.NULL)) return;
        try(Arena arena=Arena.ofConfined()) {
            var error=arena.allocate(1024);
            int status=(int)bindings.destroyTerrain.invokeExact(handle,error,1024);
            check(status,error,"destroy terrain"); handle=MemorySegment.NULL; owner.releasedTerrain(this);
        } catch(RuntimeException | Error e) { throw e; }
        catch(Throwable e) { throw new IllegalStateException("Cannot destroy terrain",e); }
    }
}
