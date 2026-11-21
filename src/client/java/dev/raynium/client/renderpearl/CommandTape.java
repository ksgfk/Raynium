package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.commands.GpuFence;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeTexture;
import dev.raynium.bridge.NativeBuffer;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import dev.raynium.bridge.NativeSwapchain;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.HashSet;
import static java.lang.foreign.ValueLayout.*;

/** One recording batch. Its texture owners survive application close until native replay has imported them. */
public final class CommandTape implements AutoCloseable {
    public record Region(NativeTexture texture,int mip,int layer,int x,int y,int width,int height) {
        private void validate(NativeRenderer renderer) {
            texture.checkRecordingOwner(renderer);
            if(mip<0 || mip>=texture.mipLevels() || layer<0 || layer>=texture.arrayLayers() || x<0 || y<0 ||
                width<=0 || height<=0 || (long)x+width>Math.max(1,texture.width()>>mip) ||
                (long)y+height>Math.max(1,texture.height()>>mip)) throw new IllegalArgumentException("Invalid texture region");
        }
        private boolean full() {
            return width==Math.max(1,texture.width()>>mip) && height==Math.max(1,texture.height()>>mip);
        }
        private boolean depth() { return texture.format().gpu().hasDepthAspect(); }
        private void write(MemorySegment record,long offset) {
            record.set(JAVA_INT,offset,texture.id());
            record.set(JAVA_INT,offset+4,mip);
            record.set(JAVA_INT,offset+8,layer);
            record.set(JAVA_INT,offset+12,x);
            record.set(JAVA_INT,offset+16,y);
            record.set(JAVA_INT,offset+20,width);
            record.set(JAVA_INT,offset+24,height);
        }
    }
    static final class Storage implements AutoCloseable {
        static final long INITIAL_COMMANDS=16*1024;
        static final long INITIAL_UPLOADS=64*1024;
        final Stream commands;
        final Stream uploads;
        final Stream instanceUploads;

        Storage() {
            this(INITIAL_COMMANDS,INITIAL_UPLOADS);
        }
        Storage(long initialCommands,long initialUploads) {
            this.commands=new Stream(initialCommands);
            this.uploads=new Stream(initialUploads);
            this.instanceUploads=new Stream(initialUploads);
        }
        void reset() {
            commands.reset();
            uploads.reset();
            instanceUploads.reset();
        }
        @Override public void close() {
            try {
                commands.close();
            } finally {
                uploads.close();
                instanceUploads.close();
            }
        }
    }
    static final class Stream implements AutoCloseable {
        private final long initialCapacity;
        private Arena arena;
        private MemorySegment storage=MemorySegment.NULL;
        private long used;
        private int lowUsageResets;

        Stream(long initialCapacity) {
            this.initialCapacity=initialCapacity;
        }
        MemorySegment append(long bytes) {
            long size=Math.addExact(used,bytes);
            if(size>storage.byteSize()) {
                long doubled=storage.byteSize()<=Long.MAX_VALUE/2?storage.byteSize()*2:Long.MAX_VALUE;
                long capacity=Math.max(size,Math.max(initialCapacity,doubled));
                var nextArena=Arena.ofConfined();
                MemorySegment replacement;
                try {
                    replacement=nextArena.allocate(capacity,8);
                } catch(Throwable failure) {
                    nextArena.close();
                    throw failure;
                }
                if(used>0) replacement.asSlice(0,used).copyFrom(storage.asSlice(0,used));
                if(arena!=null) arena.close();
                arena=nextArena;
                storage=replacement;
            }
            var result=storage.asSlice(used,bytes);
            used=size;
            return result;
        }
        MemorySegment slice(long offset,long bytes) { return contents().asSlice(offset,bytes); }
        MemorySegment contents() { return used==0?MemorySegment.NULL:storage.asSlice(0,used); }
        void reset() {
            if(storage.byteSize()>16L*1024*1024 && used<storage.byteSize()/4) {
                if(++lowUsageResets==120) close();
            } else {
                lowUsageResets=0;
            }
            used=0;
        }
        @Override public void close() {
            used=0;
            lowUsageResets=0;
            storage=MemorySegment.NULL;
            if(arena!=null) {
                var owned=arena;
                arena=null;
                owned.close();
            }
        }
    }
    private static final java.util.concurrent.atomic.AtomicLong NEXT_TAPE_ID=new java.util.concurrent.atomic.AtomicLong(1);
    private final long tapeId=NEXT_TAPE_ID.getAndIncrement();
    private final NativeRenderer renderer;
    private final BatchSubmission submission;
    private final java.util.ArrayDeque<Storage> freeStorage;
    private Storage storage;
    private final java.util.ArrayList<NativeTexture> references=new java.util.ArrayList<>();
    private final java.util.ArrayList<NativeBuffer> bufferReferences=new java.util.ArrayList<>();
    private final java.util.ArrayList<Runnable> callbacks=new java.util.ArrayList<>();
    private final java.util.ArrayList<RayniumQueryPool> queryReferences=new java.util.ArrayList<>();
    private final java.util.ArrayList<RayniumPipeline> pipelines=new java.util.ArrayList<>();
    private final java.util.ArrayList<Runnable> submittedCallbacks=new java.util.ArrayList<>();
    public void onSubmitted(Runnable callback) { checkRecording(); submittedCallbacks.add(callback); }
    private final java.util.ArrayList<MemorySegment> views=new java.util.ArrayList<>();
    private final java.util.ArrayList<dev.raynium.bridge.NativeFrameBuffer> viewStorage=new java.util.ArrayList<>();
    private final java.util.ArrayDeque<dev.raynium.bridge.NativeFrameBuffer> freeViews;
    private final java.util.ArrayList<net.minecraft.client.model.geom.ModelPart> modelReferences=new java.util.ArrayList<>();
    public void retainModel(net.minecraft.client.model.geom.ModelPart part) { modelReferences.add(part); }
    private final HashSet<Long> captures=new HashSet<>();
    private boolean finalized,rasterOpen;
    private dev.raynium.bridge.TerrainFrameEvent terrainEvent;

    public dev.raynium.bridge.TerrainFrameEvent measureTerrain() {
        if(terrainEvent==null) terrainEvent=dev.raynium.bridge.TerrainFrameEvent.start();
        return terrainEvent;
    }
    private NativeSwapchain surface;
    private java.util.function.IntConsumer presentation;

    public CommandTape(NativeRenderer renderer) {
        this(renderer,null,null);
    }
    CommandTape(NativeRenderer renderer,java.util.ArrayDeque<dev.raynium.bridge.NativeFrameBuffer> freeViews,java.util.ArrayDeque<Storage> freeStorage) {
        this.renderer=renderer;
        this.freeViews=freeViews;
        this.freeStorage=freeStorage;
        var pooled=freeStorage==null?null:freeStorage.pollFirst();
        this.storage=pooled==null?new Storage():pooled;
        this.submission=new BatchSubmission(renderer::awaitSubmission);
    }
    private void checkRecording() {
        if(finalized) throw new IllegalStateException("Command tape already finalized");
    }
    private MemorySegment command(int opcode,int size,Region region) {
        var record=storage.commands.append(size);
        retain(region.texture);
        record.set(JAVA_INT,0,opcode);
        record.set(JAVA_INT,4,size);
        region.write(record,8);
        return record;
    }
    void validateAttachment(Region target,boolean depth) {
        checkRecording();
        target.validate(renderer);
        if((target.texture.flags()&NativeTexture.ATTACHMENT)==0 || target.depth()!=depth)
            throw new IllegalArgumentException("Invalid clear attachment");
    }
    public void clearColor(Region target,float red,float green,float blue,float alpha) {
        validateAttachment(target,false);
        var record=command(1,56,target);
        record.set(JAVA_FLOAT,36,red);
        record.set(JAVA_FLOAT,40,green);
        record.set(JAVA_FLOAT,44,blue);
        record.set(JAVA_FLOAT,48,alpha);
        record.set(JAVA_INT,52,0);
    }
    public void clearDepth(Region target,float depth) {
        validateAttachment(target,true);
        if(!(depth>=0 && depth<=1)) throw new IllegalArgumentException("Invalid depth");
        var record=command(2,56,target);
        record.set(JAVA_FLOAT,36,depth);
        record.set(JAVA_FLOAT,40,0f);
        record.set(JAVA_FLOAT,44,0f);
        record.set(JAVA_FLOAT,48,0f);
        record.set(JAVA_INT,52,0);
    }
    public void captureColor(Region source,long id) {
        checkRecording();
        source.validate(renderer);
        if(id<=0 || source.texture.format()!=NativeTexture.Format.RGBA8 ||
           (source.texture.flags()&NativeTexture.READBACK)==0 || !captures.add(id))
            throw new IllegalArgumentException("Invalid capture source or ID");
        var record=command(3,48,source);
        record.set(JAVA_INT,36,0);
        record.set(JAVA_LONG,40,id);
    }
    public void uploadTexture(Region target,int pitch,MemorySegment pixels) {
        checkRecording();
        target.validate(renderer);
        if(target.depth()) throw new UnsupportedOperationException("Depth texture uploads");
        long row=(long)target.width*target.texture.format().bytesPerPixel();
        long size=(long)pitch*(target.height-1)+row;
        if(pitch<row || pixels.byteSize()<size) throw new IllegalArgumentException("Invalid upload row pitch or size");
        long offset=storage.uploads.used;
        storage.uploads.append(size).copyFrom(pixels.asSlice(0,size));
        var record=command(4,48,target);
        record.set(JAVA_INT,36,pitch);
        record.set(JAVA_LONG,40,offset);
    }
    public void copyTexture(Region source,Region destination) {
        checkRecording();
        source.validate(renderer);
        destination.validate(renderer);
        if(source.texture.format()!=destination.texture.format() || (source.texture.flags()&NativeTexture.READBACK)==0 ||
            source.width!=destination.width || source.height!=destination.height)
            throw new IllegalArgumentException("Invalid texture copy");
        if(source.texture.format().gpu().hasStencilAspect() || (source.depth() && !(source.full() && destination.full()))) throw new UnsupportedOperationException("Partial depth texture copy");
        if(source.texture==destination.texture && source.mip==destination.mip && source.layer==destination.layer &&
            source.x<destination.x+destination.width && destination.x<source.x+source.width &&
            source.y<destination.y+destination.height && destination.y<source.y+source.height)
            throw new IllegalArgumentException("Overlapping texture copy");
        var record=command(5,64,source);
        retain(destination.texture);
        destination.write(record,36);
    }
    public void blitToSurface(NativeTexture source,int baseMip,NativeSwapchain target) {
        blitToSurface(source,baseMip,target,null);
    }
    void blitToSurface(NativeTexture source,int baseMip,NativeSwapchain target,java.util.function.IntConsumer presentation) {
        checkRecording();
        target.checkRecordingOwner(renderer);
        source.checkRecordingOwner(renderer);
        if(surface!=null || baseMip<0 || baseMip>=source.mipLevels() || source.arrayLayers()!=1 ||
           !source.format().supportsFloatBlit() || (source.flags()&NativeTexture.READBACK)==0)
            throw new IllegalArgumentException("Invalid surface blit");
        var record=storage.commands.append(16);
        retain(source);
        record.set(JAVA_INT,0,6);
        record.set(JAVA_INT,4,16);
        record.set(JAVA_INT,8,source.id());
        record.set(JAVA_INT,12,baseMip);
        surface=target;
        this.presentation=presentation;
    }
    static void validatePass(com.mojang.renderpearl.api.commands.RenderPassDescriptor desc) {
        var area=desc.renderArea();
        if(area.x()<0 || area.y()<0 || area.width()<=0 || area.height()<=0 ||
           (long)area.x()+area.width()>Integer.MAX_VALUE || (long)area.y()+area.height()>Integer.MAX_VALUE)
            throw new IllegalArgumentException("Invalid render area");
        var colors=desc.colorAttachments();
        int count=colors.size();
        int present=0;
        for(int i=0;i<count;++i) {
            var a=colors.get(i);
            if(a==null) continue;
            var image=validateAttachmentView(a.textureView(),area,false);
            for(int j=0;j<i;++j) {
                var prev=colors.get(j);
                if(prev!=null && prev.textureView().texture()==image) throw new IllegalArgumentException("Invalid render pass attachment");
            }
            ++present;
        }
        var depth=desc.depthAttachment();
        if(depth!=null) {
            var depthImage=validateAttachmentView(depth.textureView(),area,true);
            for(int i=0;i<count;++i) {
                var prev=colors.get(i);
                if(prev!=null && prev.textureView().texture()==depthImage) throw new IllegalArgumentException("Invalid render pass attachment");
            }
            if(depth.clearValue().isPresent() && !(depth.clearValue().getAsDouble()>=0 && depth.clearValue().getAsDouble()<=1))
                throw new IllegalArgumentException("Invalid depth clear value");
            ++present;
        }
        if(present==0) throw new IllegalArgumentException("Render pass has no attachments");
    }
    private static NativeTexture validateAttachmentView(com.mojang.renderpearl.api.textures.GpuTextureView view,
            com.mojang.renderpearl.api.commands.RenderPass.RenderArea area,boolean depth) {
        if(view.isClosed() || !(view.texture() instanceof NativeTexture image) ||
           (image.flags()&NativeTexture.ATTACHMENT)==0 || depth!=image.getFormat().hasDepthAspect() ||
           (long)area.x()+area.width()>view.getWidth(0) || (long)area.y()+area.height()>view.getHeight(0))
            throw new IllegalArgumentException("Invalid render pass attachment");
        return image;
    }
    private NativeTexture putAttachment(MemorySegment record,long at,com.mojang.renderpearl.api.textures.GpuTextureView view,boolean clear) {
        var texture=(NativeTexture)view.texture();
        retain(texture);
        record.set(JAVA_INT,at,texture.id());
        record.set(JAVA_INT,at+4,view.baseMipLevel());
        record.set(JAVA_INT,at+8,0);
        record.set(JAVA_INT,at+12,clear?1:0);
        return texture;
    }
    void beginRaster(com.mojang.renderpearl.api.commands.RenderPassDescriptor desc) {
        checkRecording();
        if(rasterOpen) throw new IllegalStateException("A render pass is already open");
        validatePass(desc);
        for(var a:desc.colorAttachments()) if(a!=null) ((NativeTexture)a.textureView().texture()).checkRecordingOwner(renderer);
        if(desc.depthAttachment()!=null) ((NativeTexture)desc.depthAttachment().textureView().texture()).checkRecordingOwner(renderer);
        int count=desc.colorAttachments().size(),depth=desc.depthAttachment()==null?0:1;
        int size=32+32*(count+depth);
        var record=storage.commands.append(size);
        record.set(JAVA_INT,0,12);
        record.set(JAVA_INT,4,size);
        record.set(JAVA_INT,8,count);
        record.set(JAVA_INT,12,depth);
        var area=desc.renderArea();
        record.set(JAVA_INT,16,area.x()); record.set(JAVA_INT,20,area.y());
        record.set(JAVA_INT,24,area.width()); record.set(JAVA_INT,28,area.height());
        for(int i=0;i<count;++i) {
            var a=desc.colorAttachments().get(i);
            int at=32+32*i;
            if(a==null) {
                record.set(JAVA_INT,at,0);
                continue;
            }
            var clearValue=a.clearValue();
            putAttachment(record,at,a.textureView(),clearValue.isPresent());
            if(clearValue.isPresent()) {
                var c=clearValue.get();
                record.set(JAVA_FLOAT,at+16,c.x()); record.set(JAVA_FLOAT,at+20,c.y());
                record.set(JAVA_FLOAT,at+24,c.z()); record.set(JAVA_FLOAT,at+28,c.w());
            } else {
                record.set(JAVA_FLOAT,at+16,0f); record.set(JAVA_FLOAT,at+20,0f);
                record.set(JAVA_FLOAT,at+24,0f); record.set(JAVA_FLOAT,at+28,0f);
            }
        }
        if(depth!=0) {
            var a=desc.depthAttachment();
            int at=32+32*count;
            putAttachment(record,at,a.textureView(),a.clearValue().isPresent());
            record.set(JAVA_FLOAT,at+16,(float)a.clearValue().orElse(0));
            record.set(JAVA_FLOAT,at+20,0f); record.set(JAVA_FLOAT,at+24,0f); record.set(JAVA_FLOAT,at+28,0f);
        }
        rasterOpen=true;
    }
    void endRaster() {
        checkRecording();
        if(!rasterOpen) throw new IllegalStateException("No render pass is open");
        var record=storage.commands.append(8);
        record.set(JAVA_INT,0,13);
        record.set(JAVA_INT,4,8);
        rasterOpen=false;
    }
    public record TerrainView(CommandTape batch,int id) {}
    public dev.raynium.bridge.NativeFrameBuffer acquireViewStorage() {
        checkRecording();
        var storage=freeViews==null?null:freeViews.pollFirst();
        return storage==null?new dev.raynium.bridge.NativeFrameBuffer():storage;
    }
    public int viewCount() { return views.size(); }
    public TerrainView addView(dev.raynium.bridge.NativeFrameBuffer storage,MemorySegment parameters,NativeTexture atlas,NativeTexture light) {
        checkRecording(); atlas.checkRecordingOwner(renderer); light.checkRecordingOwner(renderer);
        retain(atlas); retain(light);
        var ref=new TerrainView(this,views.size());
        views.add(parameters); viewStorage.add(storage);
        return ref;
    }
    void terrain(TerrainView view,int stage,com.mojang.renderpearl.api.commands.RenderPass.RenderArea scissor,
                 com.mojang.renderpearl.util.TextureViewAndSampler atlas,com.mojang.renderpearl.util.TextureViewAndSampler light,
                 com.mojang.renderpearl.api.textures.GpuTextureView[] inputs) {
        checkRecording();
        if(!rasterOpen || view.batch()!=this || view.id()<0 || view.id()>=views.size()) throw new IllegalStateException("Terrain view belongs to another batch");
        for(var input:inputs) {
            if(input==null || input.isClosed()) throw new IllegalArgumentException("Invalid terrain stage input");
            ((NativeTexture)input.texture()).checkRecordingOwner(renderer);
        }
        if(inputs.length!=(stage==4?1:stage==5?3:0)) throw new IllegalArgumentException("Invalid terrain stage inputs");
        var material=java.util.List.of(atlas,light);
        for(var pair:material) {
            if(pair.view().isClosed() || pair.sampler().isClosed() || !(pair.view().texture() instanceof NativeTexture image))
                throw new IllegalArgumentException("Invalid terrain material");
            validateTexture(image);
        }
        var record=storage.commands.append(104);
        record.set(JAVA_INT,0,15);
        record.set(JAVA_INT,4,104);
        record.set(JAVA_INT,8,view.id());
        record.set(JAVA_INT,12,stage);
        record.set(JAVA_INT,16,scissor.x());
        record.set(JAVA_INT,20,scissor.y());
        record.set(JAVA_INT,24,scissor.width());
        record.set(JAVA_INT,28,scissor.height());
        for(int i=0;i<material.size();++i) {
            var pair=material.get(i);
            var image=(NativeTexture)pair.view().texture();
            long at=56+24L*i;
            record.set(JAVA_INT,at,image.id());
            record.set(JAVA_INT,at+4,RayniumSampler.flags(pair.sampler()));
            record.set(JAVA_INT,at+8,pair.view().baseMipLevel());
            record.set(JAVA_INT,at+12,pair.view().mipLevels());
            record.set(JAVA_FLOAT,at+16,(float)pair.sampler().getMaxLod().orElse(1000));
            record.set(JAVA_INT,at+20,pair.sampler().getMaxAnisotropy());
            retain(image);
        }
        for(int i=0;i<inputs.length;++i) {
            var image=(NativeTexture)inputs[i].texture();
            record.set(JAVA_INT,32+i*4L,image.id()); record.set(JAVA_INT,44+i*4L,inputs[i].baseMipLevel());
            retain(image);
        }
        for(int i=inputs.length;i<3;++i) {
            record.set(JAVA_INT,32+i*4L,0); record.set(JAVA_INT,44+i*4L,0);
        }
    }
    public long reserveInstanceUpload(long bytes,int alignment) {
        checkRecording();
        if(bytes<=0 || alignment<=0 || (alignment&(alignment-1))!=0) throw new IllegalArgumentException("Invalid aligned upload reservation");
        long mask=(long)alignment-1L;
        long aligned=(storage.instanceUploads.used+mask)&~mask;
        if(aligned>storage.instanceUploads.used) storage.instanceUploads.append(aligned-storage.instanceUploads.used);
        storage.instanceUploads.append(bytes);
        return aligned;
    }
    public MemorySegment instanceUploadSlice(long offset,long bytes) {
        checkRecording();
        return storage.instanceUploads.slice(offset,bytes);
    }
    MemorySegment beginInstancedGeometry(RayniumPipeline pipeline,long uploadOffset,int instanceCount,int instanceStride,int runCount,
            int x,int y,int width,int height,int bindings,byte[] push,int pushLength) {
        checkRecording();
        if(!rasterOpen || pipeline.owner!=renderer || pipeline.isClosed() || pipeline.instancedId==0)
            throw new IllegalStateException("Invalid instanced geometry pipeline");
        if(instanceCount<=0 || instanceStride<=0 || runCount<=0 || uploadOffset<0 || pushLength<0 || pushLength>push.length)
            throw new IllegalArgumentException("Invalid instanced geometry arguments");
        int headerAndData=Math.addExact(56,Math.addExact(Math.multiplyExact(48,bindings),Math.multiplyExact(16,runCount)));
        int size=(Math.addExact(headerAndData,pushLength)+7)&~7;
        var record=storage.commands.append(size);
        retain(pipeline);
        record.set(JAVA_INT,0,16);
        record.set(JAVA_INT,4,size);
        record.set(JAVA_INT,8,pipeline.instancedId);
        record.set(JAVA_INT,12,bindings);
        record.set(JAVA_INT,16,runCount);
        record.set(JAVA_INT,20,instanceCount);
        record.set(JAVA_INT,24,instanceStride);
        record.set(JAVA_INT,28,pushLength);
        record.set(JAVA_LONG,32,uploadOffset);
        record.set(JAVA_INT,40,x);
        record.set(JAVA_INT,44,y);
        record.set(JAVA_INT,48,width);
        record.set(JAVA_INT,52,height);
        if(pushLength>0) MemorySegment.copy(push,0,record,JAVA_BYTE,headerAndData,pushLength);
        return record;
    }
    MemorySegment beginDraw(RayniumPipeline pipeline,int kind,int count,int instances,int first,int firstInstance,int baseVertex,
            int x,int y,int width,int height,int bindings,byte[] push,int pushLength) {
        checkRecording();
        if(!rasterOpen || pipeline.owner!=renderer || pipeline.isClosed()) throw new IllegalStateException("Invalid draw pipeline");
        if(pushLength<0 || pushLength>push.length) throw new IllegalArgumentException("Invalid push constant length");
        int size=(Math.addExact(64+Math.multiplyExact(48,bindings),pushLength)+7)&~7;
        var record=storage.commands.append(size);
        retain(pipeline);
        record.set(JAVA_INT,0,14);
        record.set(JAVA_INT,4,size);
        record.set(JAVA_INT,8,pipeline.id);
        record.set(JAVA_INT,12,kind);
        record.set(JAVA_INT,16,count);
        record.set(JAVA_INT,20,instances);
        record.set(JAVA_INT,24,first);
        record.set(JAVA_INT,28,firstInstance);
        record.set(JAVA_INT,32,baseVertex);
        record.set(JAVA_INT,36,x);
        record.set(JAVA_INT,40,y);
        record.set(JAVA_INT,44,width);
        record.set(JAVA_INT,48,height);
        record.set(JAVA_INT,52,bindings);
        record.set(JAVA_INT,56,pushLength);
        record.set(JAVA_INT,60,0);
        if(pushLength>0) MemorySegment.copy(push,0,record,JAVA_BYTE,64+48L*bindings,pushLength);
        return record;
    }
    void validateBuffer(NativeBuffer buffer,long offset,long length,int uses) { buffer.checkRange(renderer,offset,length,uses); }
    void validateTexture(NativeTexture image) { image.checkRecordingOwner(renderer); }
    void retain(NativeTexture image) {
        if(image.lastRetainedTapeId()==tapeId) return;
        image.retainRecording();
        image.setLastRetainedTapeId(tapeId);
        references.add(image);
    }
    void retain(NativeBuffer buffer) {
        if(buffer.lastRetainedTapeId()==tapeId) return;
        buffer.retainRecording();
        buffer.setLastRetainedTapeId(tapeId);
        bufferReferences.add(buffer);
    }
    private void retain(RayniumPipeline pipeline) {
        if(pipeline.lastRetainedTapeId==tapeId) return;
        pipeline.retain();
        pipeline.lastRetainedTapeId=tapeId;
        pipelines.add(pipeline);
    }
    private void retain(RayniumQueryPool pool) {
        if(pool.lastRetainedTapeId==tapeId) return;
        pool.retainRecording();
        pool.lastRetainedTapeId=tapeId;
        queryReferences.add(pool);
    }
    public void writeBuffer(NativeBuffer target,long offset,MemorySegment bytes) {
        checkRecording();
        long size=bytes.byteSize();
        target.checkRange(renderer,offset,size,GpuBuffer.USAGE_COPY_DST);
        long upload=storage.uploads.used;
        storage.uploads.append(size).copyFrom(bytes);
        var record=storage.commands.append(40);
        record.set(JAVA_INT,0,8);
        record.set(JAVA_INT,4,40);
        record.set(JAVA_INT,8,target.id());
        record.set(JAVA_INT,12,0);
        record.set(JAVA_LONG,16,offset);
        record.set(JAVA_LONG,24,size);
        record.set(JAVA_LONG,32,upload);
        retain(target);
    }
    public void copyBuffer(NativeBuffer source,long srcOffset,NativeBuffer target,long dstOffset,long size) {
        checkRecording();
        source.checkRange(renderer,srcOffset,size,GpuBuffer.USAGE_COPY_SRC);
        target.checkRange(renderer,dstOffset,size,GpuBuffer.USAGE_COPY_DST);
        if(source==target && srcOffset<dstOffset+size && dstOffset<srcOffset+size)
            throw new IllegalArgumentException("Overlapping buffer copy");
        var record=storage.commands.append(40);
        record.set(JAVA_INT,0,9);
        record.set(JAVA_INT,4,40);
        record.set(JAVA_INT,8,source.id());
        record.set(JAVA_INT,12,target.id());
        record.set(JAVA_LONG,16,srcOffset);
        record.set(JAVA_LONG,24,dstOffset);
        record.set(JAVA_LONG,32,size);
        retain(source);
        retain(target);
    }
    public void onCompleted(Runnable callback) {
        checkRecording();
        callbacks.add(java.util.Objects.requireNonNull(callback));
    }
    public void bufferToTexture(NativeBuffer source,long offset,int sourcePitch,Region destination) {
        transferTextureBuffer(source,offset,sourcePitch,destination,true);
    }
    public void textureToBuffer(Region source,NativeBuffer destination,long offset,Runnable callback) {
        transferTextureBuffer(destination,offset,Math.multiplyExact(source.width,source.texture.format().bytesPerPixel()),source,false);
        if(callback!=null) onCompleted(callback);
    }
    private void transferTextureBuffer(NativeBuffer buffer,long offset,int pitch,Region image,boolean toImage) {
        checkRecording();
        image.validate(renderer);
        if(image.texture.format().gpu().hasStencilAspect() || (image.depth() && (toImage || !image.full())))
            throw new UnsupportedOperationException("Unsupported depth or stencil buffer transfer");
        if(!toImage && (image.texture.flags()&NativeTexture.READBACK)==0) throw new IllegalArgumentException("Missing copy source usage");
        long row=(long)image.width*image.texture.format().bytesPerPixel();
        if(pitch<row) throw new IllegalArgumentException("Invalid texture row pitch");
        buffer.checkRange(renderer,offset,(long)pitch*(image.height-1)+row,toImage?GpuBuffer.USAGE_COPY_SRC:GpuBuffer.USAGE_COPY_DST);
        var record=command(toImage?10:11,56,image);
        record.set(JAVA_INT,36,buffer.id());
        record.set(JAVA_LONG,40,offset);
        record.set(JAVA_LONG,48,pitch);
        retain(buffer);
    }
    public void writeTimestamp(RayniumQueryPool pool,int index) {
        checkRecording();
        pool.validateWrite(renderer,index);
        var record=storage.commands.append(16);
        record.set(JAVA_INT,0,7);
        record.set(JAVA_INT,4,16);
        record.set(JAVA_INT,8,pool.id());
        record.set(JAVA_INT,12,index);
        retain(pool);
        pool.written(index,submission);
    }
    public GpuFence createFence() {
        checkRecording();
        return submission.createFence();
    }
    public NativeRenderer.SubmitResult submit() { return submit(false); }
    NativeRenderer.SubmitResult submit(boolean measure) {
        checkRecording();
        if(rasterOpen) throw new IllegalStateException("Close the render pass before submission");
        try {
            NativeRenderer.SubmitResult result;
            try {
                long start=terrainEvent==null?0:System.nanoTime();
                result=renderer.submitFrame(storage.commands.contents(),storage.uploads.contents(),storage.instanceUploads.contents(),surface,views,measure);
                if(terrainEvent!=null) {
                    terrainEvent.ffm=System.nanoTime()-start;
                    terrainEvent.sequence=result.sequence();
                    terrainEvent.commit();
                    terrainEvent=null;
                }
            } catch(RuntimeException | Error failure) {
                submission.failed();
                throw failure;
            }
            if(result.sequence()>0) {
                submission.submitted(result.sequence());
                renderer.onCompleted(result.sequence(),callbacks);
                submittedCallbacks.forEach(Runnable::run);
            } else submission.failed();
            if(!result.success()) renderer.terminalFailure(result.deviceLost());
            if(presentation!=null) presentation.accept(result.outcome());
            return result;
        } finally {
            release();
        }
    }
    private void release() {
        finalized=true;
        modelReferences.clear();
        var owned=storage;
        storage=null;
        if(owned!=null) {
            if(freeStorage==null) owned.close();
            else {
                owned.reset();
                freeStorage.addFirst(owned);
            }
        }
        for(int i=0;i<references.size();++i) references.get(i).releaseRecording();
        references.clear();
        for(int i=0;i<queryReferences.size();++i) queryReferences.get(i).releaseRecording();
        queryReferences.clear();
        for(int i=0;i<bufferReferences.size();++i) bufferReferences.get(i).releaseRecording();
        bufferReferences.clear();
        for(int i=0;i<pipelines.size();++i) pipelines.get(i).release();
        pipelines.clear();
        // Native has copied the view data into its frame slot before the submit call returns.
        for(int i=0;i<viewStorage.size();++i) viewStorage.get(i).clearBorrowed();
        if(freeViews==null) viewStorage.forEach(dev.raynium.bridge.NativeFrameBuffer::close);
        else freeViews.addAll(viewStorage);
        viewStorage.clear(); views.clear(); submittedCallbacks.clear();
        callbacks.clear();
    }
    @Override public void close() {
        if(!finalized) {
            submission.failed();
            release();
        }
    }
}
