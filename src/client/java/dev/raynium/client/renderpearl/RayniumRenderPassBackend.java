package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.*;
import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.backend.api.*;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.bridge.NativeBuffer;
import dev.raynium.bridge.NativeTexture;
import java.lang.foreign.MemorySegment;
import java.nio.*;
import java.util.*;
import java.util.function.Supplier;
import org.lwjgl.PointerBuffer;
import static java.lang.foreign.ValueLayout.*;

final class RayniumRenderPassBackend {
    private static final int INITIAL_UNIFORMS=16;
    private static final int INITIAL_BINDINGS=16;
    private CommandTape tape;
    private RenderPass.RenderArea area;
    private RenderPass.RenderArea scissor;
    private RayniumPipeline pipeline;
    private Object[] uniforms=new Object[INITIAL_UNIFORMS];
    private int uniformCount;
    private final GpuBufferSlice[] vertices=new GpuBufferSlice[16];
    private GpuBuffer indexBuffer;
    private IndexType indexType;
    private byte[] push=new byte[0];
    private int pushLength;
    private int[] bindingKind=new int[INITIAL_BINDINGS];
    private int[] bindingSlot=new int[INITIAL_BINDINGS];
    private NativeBuffer[] bindingBuffer=new NativeBuffer[INITIAL_BINDINGS];
    private NativeTexture[] bindingImage=new NativeTexture[INITIAL_BINDINGS];
    private int[] bindingFormat=new int[INITIAL_BINDINGS];
    private long[] bindingOffset=new long[INITIAL_BINDINGS];
    private long[] bindingLength=new long[INITIAL_BINDINGS];
    private int[] bindingMip=new int[INITIAL_BINDINGS];
    private int[] bindingLevels=new int[INITIAL_BINDINGS];
    private float[] bindingLod=new float[INITIAL_BINDINGS];
    private int[] bindingAnisotropy=new int[INITIAL_BINDINGS];
    private int bindingCount;


    static final class Lease implements RenderPassBackend {
        private RayniumRenderPassBackend recorder;
        Lease(RayniumRenderPassBackend recorder) { this.recorder=recorder; }
        RayniumRenderPassBackend recorder() {
            if(recorder==null) throw new IllegalStateException("Render pass is closed");
            return recorder;
        }
        void close() {
            recorder().reset();
            recorder=null;
        }
        @Override public void setPipeline(BackendRenderPipeline value) { recorder().setPipeline(value); }
        @Override public void setUniform(int index,Object value) { recorder().setUniform(index,value); }
        @Override public void pushConstants(ByteBuffer value) { recorder().pushConstants(value); }
        @Override public void pushDebugGroup(Supplier<String> label) { recorder().pushDebugGroup(label); }
        @Override public void popDebugGroup() { recorder().popDebugGroup(); }
        @Override public void enableScissor(int x,int y,int width,int height) { recorder().enableScissor(x,y,width,height); }
        @Override public void disableScissor() { recorder().disableScissor(); }
        @Override public void setVertexBuffer(int slot,GpuBufferSlice buffer) { recorder().setVertexBuffer(slot,buffer); }
        @Override public void setIndexBuffer(GpuBuffer buffer,IndexType type) { recorder().setIndexBuffer(buffer,type); }
        @Override public void writeTimestamp(GpuQueryPool pool,int index) { recorder().writeTimestamp(pool,index); }
        @Override public void draw(int count,int instances,int first,int firstInstance) { recorder().draw(count,instances,first,firstInstance); }
        @Override public void drawIndexed(int count,int instances,int first,int base,int firstInstance) { recorder().drawIndexed(count,instances,first,base,firstInstance); }
        @Override public void drawIndirect(GpuBufferSlice commands,int count) { recorder().drawIndirect(commands,count); }
        @Override public void drawIndexedIndirect(GpuBufferSlice commands,int count) { recorder().drawIndexedIndirect(commands,count); }
        @Override public void multiDraw(IntBuffer args,int instances,int firstInstance,int count) { recorder().multiDraw(args,instances,firstInstance,count); }
        @Override public void multiDrawIndexed(IntBuffer args,int instances,int firstInstance,int count) { recorder().multiDrawIndexed(args,instances,firstInstance,count); }
        @Override public void multiDraw(IntBuffer first,IntBuffer counts,int count) { recorder().multiDraw(first,counts,count); }
        @Override public void multiDrawIndexed(PointerBuffer offsets,IntBuffer counts,IntBuffer bases,int count) { recorder().multiDrawIndexed(offsets,counts,bases,count); }
    }

    private int usedBindings,vertexMask;

    private void reset() {
        Arrays.fill(uniforms,0,uniformCount,null);
        for(int mask=vertexMask;mask!=0;mask&=mask-1) vertices[Integer.numberOfTrailingZeros(mask)]=null;
        int end=Math.max(bindingCount,usedBindings);
        Arrays.fill(bindingBuffer,0,end,null);
        Arrays.fill(bindingImage,0,end,null);
        uniformCount=bindingCount=usedBindings=vertexMask=pushLength=0;
        pipeline=null; indexBuffer=null; indexType=null;
        tape=null; area=scissor=null;
    }

    Lease open(CommandTape tape,RenderPassDescriptor descriptor) {
        if(this.tape!=null) throw new IllegalStateException("Render pass already open");
        this.tape=tape;
        this.area=this.scissor=descriptor.renderArea();
        return new Lease(this);
    }
    void setPipeline(BackendRenderPipeline value) {
        if(!(value instanceof RayniumPipeline selected) || selected.isClosed()) throw new IllegalArgumentException("Invalid render pipeline");
        pipeline=selected;
        int count=selected.routes.size();
        if(uniformCount>0) Arrays.fill(uniforms,0,uniformCount,null);
        if(uniforms.length<count) uniforms=new Object[Math.max(count,uniforms.length*2)];
        uniformCount=count;
        pushLength=0;
    }
    void setUniform(int index,Object value) {
        Objects.checkIndex(index,uniformCount);
        uniforms[index]=value;
    }
    void pushConstants(ByteBuffer value) {
        int size=pipeline.pushBytes;
        if(value.remaining()<size) throw new BufferUnderflowException();
        if(push.length<size) push=new byte[size];
        if(size>0) value.get(value.position(),push,0,size);
        pushLength=size;
    }
    void pushDebugGroup(Supplier<String> label) {}
    void popDebugGroup() {}
    void enableScissor(int x,int y,int width,int height) {
        if(width<=0 || height<=0 || x<area.x() || y<area.y() || (long)x+width>(long)area.x()+area.width() ||
            (long)y+height>(long)area.y()+area.height()) throw new IllegalArgumentException("Invalid scissor");
        scissor=new RenderPass.RenderArea(x,y,width,height);
    }
    void disableScissor() { scissor=area; }
    void setVertexBuffer(int slot,GpuBufferSlice buffer) { vertices[slot]=buffer; vertexMask|=1<<slot; }
    void setIndexBuffer(GpuBuffer buffer,IndexType type) { indexBuffer=buffer; indexType=type; }
    void writeTimestamp(GpuQueryPool pool,int index) { tape.writeTimestamp((RayniumQueryPool)pool,index); }

    CommandTape tape() { return tape; }

    void terrain(CommandTape.TerrainView view,int stage,TextureViewAndSampler atlas,TextureViewAndSampler light,GpuTextureView[] inputs) {
        tape.terrain(view,stage,scissor,atlas,light,inputs);
    }

    private void stageInstancedUniforms() {
        usedBindings=Math.max(usedBindings,bindingCount);
        bindingCount=0;
        var routes=pipeline.instancedRoutes;
        var map=pipeline.instancedUniformMap;
        for(int i=0;i<routes.size();++i) {
            var route=routes.get(i);
            var value=uniforms[map[i]];
            if(value==null) throw new IllegalStateException("Unbound shader resource "+i);
            switch(route.type()) {
                case UNIFORM_BUFFER -> stageBuffer(3,route.binding(),(GpuBufferSlice)value,0,GpuBuffer.USAGE_UNIFORM);
                case TEXEL_BUFFER -> stageBuffer(4,route.binding(),(GpuBufferSlice)value,NativeTexture.Format.from(route.format()).ordinal(),GpuBuffer.USAGE_UNIFORM_TEXEL_BUFFER);
                case COMBINED_IMAGE_SAMPLER -> stageTexture(route,(TextureViewAndSampler)value);
            }
        }
    }

    private void writeBindings(MemorySegment record,long at) {
        for(int i=0;i<bindingCount;++i) {
            int id;
            var image=bindingImage[i];
            if(image!=null) { id=image.id(); tape.retain(image); }
            else { var buffer=bindingBuffer[i]; id=buffer.id(); tape.retain(buffer); }
            record.set(JAVA_INT,at,bindingKind[i]); record.set(JAVA_INT,at+4,bindingSlot[i]); record.set(JAVA_INT,at+8,id);
            record.set(JAVA_INT,at+12,bindingFormat[i]); record.set(JAVA_LONG,at+16,bindingOffset[i]); record.set(JAVA_LONG,at+24,bindingLength[i]);
            record.set(JAVA_INT,at+32,bindingMip[i]); record.set(JAVA_INT,at+36,bindingLevels[i]);
            record.set(JAVA_FLOAT,at+40,bindingLod[i]); record.set(JAVA_INT,at+44,bindingAnisotropy[i]);
            at+=48;
        }
    }

    boolean instancedGeometry(String mode,long uploadOffset,int instanceCount,int instanceStride,int[] runMeshIds,int[] runFirstInstances,int[] runInstanceCounts,int runCount) {
        if(pipeline==null || !pipeline.supportsInstanced(mode)) return false;
        if(instanceCount<=0 || runCount<=0) return true;
        stageInstancedUniforms();
        var record=tape.beginInstancedGeometry(pipeline,uploadOffset,instanceCount,instanceStride,runCount,
            scissor.x(),scissor.y(),scissor.width(),scissor.height(),bindingCount,push,pushLength);
        writeBindings(record,56);
        long runAt=56+48L*bindingCount;
        for(int i=0;i<runCount;++i) {
            record.set(JAVA_INT,runAt,runMeshIds[i]);
            record.set(JAVA_INT,runAt+4,runFirstInstances[i]);
            record.set(JAVA_INT,runAt+8,runInstanceCounts[i]);
            record.set(JAVA_INT,runAt+12,0);
            runAt+=16;
        }
        return true;
    }

    private void growBindings() {
        int capacity=bindingKind.length*2;
        bindingKind=Arrays.copyOf(bindingKind,capacity);
        bindingSlot=Arrays.copyOf(bindingSlot,capacity);
        bindingBuffer=Arrays.copyOf(bindingBuffer,capacity);
        bindingImage=Arrays.copyOf(bindingImage,capacity);
        bindingFormat=Arrays.copyOf(bindingFormat,capacity);
        bindingOffset=Arrays.copyOf(bindingOffset,capacity);
        bindingLength=Arrays.copyOf(bindingLength,capacity);
        bindingMip=Arrays.copyOf(bindingMip,capacity);
        bindingLevels=Arrays.copyOf(bindingLevels,capacity);
        bindingLod=Arrays.copyOf(bindingLod,capacity);
        bindingAnisotropy=Arrays.copyOf(bindingAnisotropy,capacity);
    }
    private void stageBuffer(int kind,int slot,GpuBufferSlice slice,int format,int usage) {
        stageBuffer(kind,slot,slice.buffer(),slice.offset(),slice.length(),format,usage);
    }
    private void stageBuffer(int kind,int slot,GpuBuffer resourceBuffer,long offset,long length,int format,int usage) {
        if(!(resourceBuffer instanceof NativeBuffer resource)) throw new IllegalArgumentException("Invalid GPU buffer");
        tape.validateBuffer(resource,offset,length,usage);
        if(bindingCount==bindingKind.length) growBindings();
        int n=bindingCount++;
        bindingKind[n]=kind; bindingSlot[n]=slot;
        bindingBuffer[n]=resource; bindingImage[n]=null;
        bindingFormat[n]=format; bindingOffset[n]=offset; bindingLength[n]=length;
        bindingMip[n]=0; bindingLevels[n]=0; bindingLod[n]=0; bindingAnisotropy[n]=0;
    }
    private void stageTexture(RayniumPrograms.Route route,TextureViewAndSampler pair) {
        var view=pair.view(); var sampler=pair.sampler();
        if(view.isClosed() || sampler.isClosed() || !(view.texture() instanceof NativeTexture image) ||
            route.cube()!=((image.flags()&NativeTexture.CUBE)!=0)) throw new IllegalArgumentException("Invalid sampled texture");
        tape.validateTexture(image);
        int bits=RayniumSampler.flags(sampler);
        if(bindingCount==bindingKind.length) growBindings();
        int n=bindingCount++;
        bindingKind[n]=5; bindingSlot[n]=route.binding();
        bindingBuffer[n]=null; bindingImage[n]=image;
        bindingFormat[n]=bits; bindingOffset[n]=0; bindingLength[n]=0;
        bindingMip[n]=view.baseMipLevel(); bindingLevels[n]=view.mipLevels();
        bindingLod[n]=(float)sampler.getMaxLod().orElse(1000); bindingAnisotropy[n]=sampler.getMaxAnisotropy();
    }
    private void drawCommand(int kind,int count,int instances,int first,int baseVertex,int firstInstance,GpuBufferSlice indirect) {
        if(pipeline==null || pipeline.isClosed()) throw new IllegalStateException("Missing render pipeline");
        if(count<0 || instances<0 || first<0 || firstInstance<0) throw new IllegalArgumentException("Invalid draw arguments");
        if(count==0 || instances==0) return;
        if(indirect==null && pipeline.topology==PrimitiveTopology.POINTS) {
            if(instances!=1 || firstInstance!=0) throw new IllegalArgumentException("Instanced point expansion");
            firstInstance=Math.addExact(first,baseVertex);
            if(firstInstance<0) throw new IllegalArgumentException("Negative point offset");
            instances=count; count=6; first=baseVertex=kind=0;
        } else if(indirect==null && pipeline.topology==PrimitiveTopology.TRIANGLE_FAN) {
            if(count<3) return;
            baseVertex=Math.addExact(baseVertex,first);
            kind=4; first=0;
        }
        usedBindings=Math.max(usedBindings,bindingCount);
        bindingCount=0;
        for(int i=0;i<vertices.length;++i) if(vertices[i]!=null) stageBuffer(0,i,vertices[i],0,GpuBuffer.USAGE_VERTEX);
        if(kind==1 || kind==3) {
            if(indexBuffer==null) throw new IllegalStateException("Index buffer required");
            stageBuffer(1,0,indexBuffer,0,indexBuffer.size(),indexType.bytes,GpuBuffer.USAGE_INDEX);
        }
        if(indirect!=null) stageBuffer(2,0,indirect,0,GpuBuffer.USAGE_INDIRECT_PARAMETERS);
        for(int i=0;i<uniformCount;++i) {
            var route=pipeline.routes.get(i); var value=uniforms[i];
            if(value==null) throw new IllegalStateException("Unbound shader resource "+i);
            switch(route.type()) {
                case UNIFORM_BUFFER -> stageBuffer(3,route.binding(),(GpuBufferSlice)value,0,GpuBuffer.USAGE_UNIFORM);
                case TEXEL_BUFFER -> stageBuffer(4,route.binding(),(GpuBufferSlice)value,NativeTexture.Format.from(route.format()).ordinal(),GpuBuffer.USAGE_UNIFORM_TEXEL_BUFFER);
                case COMBINED_IMAGE_SAMPLER -> stageTexture(route,(TextureViewAndSampler)value);
            }
        }
        var record=tape.beginDraw(pipeline,kind,count,instances,first,firstInstance,baseVertex,scissor.x(),scissor.y(),scissor.width(),scissor.height(),bindingCount,push,pushLength);
        long at=64;
        for(int i=0;i<bindingCount;++i) {
            int id;
            var image=bindingImage[i];
            if(image!=null) { id=image.id(); tape.retain(image); }
            else { var buffer=bindingBuffer[i]; id=buffer.id(); tape.retain(buffer); }
            record.set(JAVA_INT,at,bindingKind[i]); record.set(JAVA_INT,at+4,bindingSlot[i]); record.set(JAVA_INT,at+8,id);
            record.set(JAVA_INT,at+12,bindingFormat[i]); record.set(JAVA_LONG,at+16,bindingOffset[i]); record.set(JAVA_LONG,at+24,bindingLength[i]);
            record.set(JAVA_INT,at+32,bindingMip[i]); record.set(JAVA_INT,at+36,bindingLevels[i]);
            record.set(JAVA_FLOAT,at+40,bindingLod[i]); record.set(JAVA_INT,at+44,bindingAnisotropy[i]);
            at+=48;
        }
    }
    void draw(int count,int instances,int first,int firstInstance) { drawCommand(0,count,instances,first,0,firstInstance,null); }
    void drawIndexed(int count,int instances,int first,int base,int firstInstance) { drawCommand(1,count,instances,first,base,firstInstance,null); }
    void drawIndirect(GpuBufferSlice commands,int count) { drawCommand(2,count,1,0,0,0,commands); }
    void drawIndexedIndirect(GpuBufferSlice commands,int count) { drawCommand(3,count,1,0,0,0,commands); }
    void multiDraw(IntBuffer args,int instances,int firstInstance,int count) {
        Objects.checkFromIndexSize(0,Math.multiplyExact(2,count),args.remaining());
        for(int i=0;i<count;++i) { int p=args.position()+2*i; draw(args.get(p+1),instances,args.get(p),firstInstance); }
    }
    void multiDrawIndexed(IntBuffer args,int instances,int firstInstance,int count) {
        Objects.checkFromIndexSize(0,Math.multiplyExact(3,count),args.remaining());
        for(int i=0;i<count;++i) { int p=args.position()+3*i; drawIndexed(args.get(p+1),instances,args.get(p),args.get(p+2),firstInstance); }
    }
    void multiDraw(IntBuffer first,IntBuffer counts,int count) {
        Objects.checkFromIndexSize(0,count,Math.min(first.remaining(),counts.remaining()));
        for(int i=0;i<count;++i) draw(counts.get(counts.position()+i),1,first.get(first.position()+i),0);
    }
    void multiDrawIndexed(PointerBuffer offsets,IntBuffer counts,IntBuffer bases,int count) {
        Objects.checkFromIndexSize(0,count,Math.min(offsets.remaining(),Math.min(counts.remaining(),bases.remaining())));
        for(int i=0;i<count;++i) drawIndexed(counts.get(counts.position()+i),1,Math.toIntExact(offsets.get(offsets.position()+i)/indexType.bytes),bases.get(bases.position()+i),0);
    }
}
