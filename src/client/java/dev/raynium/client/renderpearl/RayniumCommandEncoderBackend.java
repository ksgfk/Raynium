package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.*;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.backend.api.CommandEncoderBackend;
import com.mojang.renderpearl.backend.api.RenderPassBackend;
import dev.raynium.bridge.NativeBuffer;
import dev.raynium.bridge.NativeFrameBuffer;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeTexture;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Objects;
import org.joml.Vector4fc;

final class RayniumCommandEncoderBackend implements CommandEncoderBackend, AutoCloseable {
    private final NativeRenderer renderer;
    private final RayniumTransientMemory memory;
    private final RayniumRenderPassBackend recorder=new RayniumRenderPassBackend();
    private final ArrayDeque<NativeFrameBuffer> freeViews = new ArrayDeque<>();
    private final ArrayDeque<CommandTape.Storage> freeStorage = new ArrayDeque<>();
    private CommandTape tape;
    private RayniumRenderPassBackend.Lease activePass;

    RayniumCommandEncoderBackend(NativeRenderer renderer) {
        this.renderer=renderer;
        tape=new CommandTape(renderer,freeViews,freeStorage);
        memory=new RayniumTransientMemory(renderer,this::tape);
    }

    CommandTape tape() { return tape; }

    @Override public void submit() {
        if(activePass!=null) throw new IllegalStateException("Close the render pass before submission");
        boolean measuring=dev.raynium.client.benchmark.BenchmarkSession.isMeasuring();
        long start=measuring?System.nanoTime():0;
        memory.seal(tape);
        long sealed=measuring?System.nanoTime():0;
        var result=tape.submit(measuring);
        if(measuring) dev.raynium.client.benchmark.BenchmarkSession.nativeBatch(result,sealed-start,System.nanoTime()-sealed);
        tape=new CommandTape(renderer,freeViews,freeStorage);
        renderer.pollCompleted();
    }

    @Override public TransientMemory transientMemory() { return memory; }

    @Override public RenderPassBackend createRenderPass(RenderPassDescriptor descriptor) {
        if(activePass!=null) throw new IllegalStateException("A render pass is already open");
        tape.beginRaster(descriptor);
        activePass=recorder.open(tape,descriptor);
        return activePass;
    }

    @Override public void submitRenderPass() {
        if(activePass==null) throw new IllegalStateException("No open render pass");
        tape.endRaster();
        activePass.close();
        activePass=null;
    }

    private static NativeTexture image(GpuTexture texture) {
        if(texture instanceof NativeTexture nativeTexture) return nativeTexture;
        throw new IllegalArgumentException("Texture belongs to a different backend");
    }

    private static NativeBuffer buffer(GpuBuffer resource) {
        if(resource instanceof NativeBuffer nativeBuffer) return nativeBuffer;
        throw new IllegalArgumentException("Buffer belongs to a different backend");
    }

    private static CommandTape.Region full(NativeTexture image,int mip) {
        return new CommandTape.Region(image,mip,0,0,0,image.getWidth(mip),image.getHeight(mip));
    }

    @Override public void clearColorTexture(GpuTexture target,Vector4fc color) {
        var texture=image(target);
        for(int mip=0;mip<texture.mipLevels();++mip) tape.clearColor(full(texture,mip),color.x(),color.y(),color.z(),color.w());
    }

    @Override public void clearDepthTexture(GpuTexture target,double clearDepth) {
        var texture=image(target);
        for(int mip=0;mip<texture.mipLevels();++mip) tape.clearDepth(full(texture,mip),(float)clearDepth);
    }

    @Override public void clearColorAndDepthTextures(GpuTexture color,Vector4fc clearColor,GpuTexture depth,double clearDepth) {
        tape.validateAttachment(full(image(color),0),false);
        validateDepth(full(image(depth),0),clearDepth);
        clearColorTexture(color,clearColor);
        clearDepthTexture(depth,clearDepth);
    }

    private void validateDepth(CommandTape.Region region,double depth) {
        tape.validateAttachment(region,true);
        if(!(depth>=0 && depth<=1)) throw new IllegalArgumentException("Invalid depth clear value");
    }

    @Override public void clearColorAndDepthTextures(GpuTexture color,Vector4fc value,GpuTexture depth,double clearDepth,
                                                     int x,int y,int width,int height,int mip) {
        var colorRegion=new CommandTape.Region(image(color),mip,0,x,y,width,height);
        var depthRegion=new CommandTape.Region(image(depth),mip,0,x,y,width,height);
        validateDepth(depthRegion,clearDepth);
        tape.clearColor(colorRegion,value.x(),value.y(),value.z(),value.w());
        tape.clearDepth(depthRegion,(float)clearDepth);
    }

    @Override public void writeToBuffer(GpuBufferSlice destination,ByteBuffer data) {
        if(data.remaining()>destination.length()) throw new IllegalArgumentException("Write exceeds destination slice");
        if(data.hasRemaining()) tape.writeBuffer(buffer(destination.buffer()),destination.offset(),MemorySegment.ofBuffer(data));
    }

    @Override public void copyToBuffer(GpuBufferSlice source,GpuBufferSlice target) {
        if(source.length()>target.length()) throw new IllegalArgumentException("Copy exceeds destination slice");
        if(source.length()>0) tape.copyBuffer(buffer(source.buffer()),source.offset(),buffer(target.buffer()),target.offset(),source.length());
    }

    @Override public void writeToTexture(GpuTexture destination,ByteBuffer data,int mip,int layer,int x,int y,int width,int height) {
        var texture=image(destination);
        tape.uploadTexture(new CommandTape.Region(texture,mip,layer,x,y,width,height),
            Math.multiplyExact(width,texture.format().bytesPerPixel()),MemorySegment.ofBuffer(data));
    }

    @Override public void copyBufferToTexture(GpuBufferSlice source,int srcX,int srcY,int srcWidth,int srcHeight,
            GpuTexture destination,int dstX,int dstY,int width,int height,int mip,int layer) {
        Objects.checkFromIndexSize(srcX,width,srcWidth);
        Objects.checkFromIndexSize(srcY,height,srcHeight);
        var texture=image(destination);
        int bytes=texture.format().bytesPerPixel();
        int pitch=Math.multiplyExact(srcWidth,bytes);
        long start=(long)srcY*pitch+(long)srcX*bytes;
        long size=(long)(height-1)*pitch+(long)width*bytes;
        Objects.checkFromIndexSize(start,size,source.length());
        tape.bufferToTexture(buffer(source.buffer()),Math.addExact(source.offset(),start),pitch,
            new CommandTape.Region(texture,mip,layer,dstX,dstY,width,height));
    }

    @Override public void copyTextureToBuffer(GpuTexture source,GpuBuffer destination,long offset,Runnable callback,int mip) {
        tape.textureToBuffer(full(image(source),mip),buffer(destination),offset,callback);
    }

    @Override public void copyTextureToBuffer(GpuTexture source,GpuBuffer destination,long offset,Runnable callback,int mip,int x,int y,int width,int height) {
        tape.textureToBuffer(new CommandTape.Region(image(source),mip,0,x,y,width,height),buffer(destination),offset,callback);
    }

    @Override public void copyTextureToTexture(GpuTexture source,GpuTexture destination,int mip,int dstX,int dstY,int srcX,int srcY,int width,int height) {
        tape.copyTexture(new CommandTape.Region(image(source),mip,0,srcX,srcY,width,height),
            new CommandTape.Region(image(destination),mip,0,dstX,dstY,width,height));
    }

    @Override public GpuFence createFence() { return tape.createFence(); }

    @Override public void writeTimestamp(GpuQueryPool pool,int index) {
        if(!(pool instanceof RayniumQueryPool queries)) throw new IllegalArgumentException("Query pool belongs to a different backend");
        tape.writeTimestamp(queries,index);
    }

    @Override public void close() {
        if(activePass!=null) { activePass.close(); activePass=null; }
        memory.close();
        tape.close();
        freeViews.forEach(NativeFrameBuffer::close);
        freeViews.clear();
        freeStorage.forEach(CommandTape.Storage::close);
        freeStorage.clear();
    }
}
