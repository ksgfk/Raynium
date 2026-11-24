package dev.raynium.bridge;

import com.mojang.blaze3d.buffers.Std140Builder;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.*;
import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.TerrainTestPass;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import dev.raynium.client.renderpearl.VanillaPipelineCatalog;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.Executors;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;
import org.joml.Matrix4f;
import org.joml.Vector4f;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeRenderPearlTest {
    @TempDir Path directory;

    static final class Sources implements ShaderSource {
        private final Map<Identifier,CachedIncludeSource> includes=new java.util.concurrent.ConcurrentHashMap<>();
        private String load(Identifier id,String path) {
            String resource="/assets/"+id.getNamespace()+"/shaders/"+path;
            try(var stream=getClass().getResourceAsStream(resource)) {
                if(stream==null) throw new IllegalStateException("Missing game resource "+resource);
                return new String(stream.readAllBytes(),StandardCharsets.UTF_8);
            } catch(IOException error) { throw new UncheckedIOException(error); }
        }
        @Override public String getShader(Identifier id,ShaderType type) { return load(id,id.getPath()+(type==ShaderType.VERTEX?".vsh":".fsh")); }
        @Override public CachedIncludeSource getInclude(Identifier id) {
            return includes.computeIfAbsent(id,key->CachedIncludeSource.create(key,load(key,"include/"+key.getPath())));
        }
        @Override public void close() { includes.values().forEach(CachedIncludeSource::close); }
    }

    private static final ShaderSource PUSH_SHADER=new ShaderSource() {
        public String getShader(Identifier id,ShaderType stage) {
            return stage==ShaderType.VERTEX?"#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}":
                "#version 450\nlayout(push_constant) uniform Push {vec4 tint;} pc;layout(location=0) out vec4 color;void main(){color=pc.tint;}";
        }
        public CachedIncludeSource getInclude(Identifier id) { return null; }
        public void close() {}
    };

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void cachedOverrideAndPushConstantsReachEveryPixel(NativeEnvironment.Backend api) throws Exception {
        var tools=Path.of(System.getProperty("raynium.shaderToolsDir","build/phase2-shader-tools"));
        boolean compile=java.nio.file.Files.isRegularFile(tools.resolve("compile_shader_override.py"));
        var previous=System.getProperty("raynium.shaderToolsDir");
        if(compile) System.setProperty("raynium.shaderToolsDir",tools.toAbsolutePath().toString());
        else {
            var file=directory.resolve(".raynium/shader-cache/v2/p_d779a21df300c379715d535179c58a0b72dd8f3e6e06f792d266448ca3c0fa91.zip");
            java.nio.file.Files.createDirectories(file.getParent());
            try(var in=getClass().getResourceAsStream("/shaders/push-color.zip")) { java.nio.file.Files.copy(Objects.requireNonNull(in),file); }
        }
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true,directory); var executor=Executors.newSingleThreadExecutor()) {
            var builder=RenderPipeline.builder().withLocation(Identifier.parse("raynium:test_push")).withVertexShader(Identifier.parse("raynium:push"))
                .withFragmentShader(Identifier.parse("raynium:push")).withCull(false).withPrimitiveTopology(PrimitiveTopology.TRIANGLES).withColorTargetState(ColorTargetState.DEFAULT);
            assertNull(device.compilePipeline(builder.withPushConstantSize(4).build(),PUSH_SHADER,executor).join().finishCompile());
            var pending=device.compilePipeline(builder.withPushConstantSize(16).build(),PUSH_SHADER,executor).join();
            var wrongThread=executor.submit(()->assertThrows(IllegalStateException.class,pending::finishCompile));
            wrongThread.get();
            try(var shader=Objects.requireNonNull(pending.finishCompile());
                var texture=device.createTexture("push",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,4,2,1,1);
                var view=device.createTextureView(texture); var read=device.createBuffer(null,GpuBuffer.USAGE_COPY_DST|GpuBuffer.USAGE_MAP_READ,32)) {
                mixedTerrainViews(device,renderer,shader);
                var encoder=device.createCommandEncoder();
                var previousPass=encoder.createRenderPass(()->"closed",view,Optional.empty());
                previousPass.setPipeline(shader);
                previousPass.close();
                try(var pass=encoder.createRenderPass(()->"push",view,Optional.of(new Vector4f(0,0,0,1)))) {
                    assertThrows(IllegalStateException.class,()->previousPass.setPipeline(shader));
                    assertThrows(IllegalStateException.class,()->RayniumGpuDevice.activeTape(previousPass));
                    pass.setPipeline(shader);
                    var push=ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder());
                    push.putFloat(4,1f).putFloat(12,1f); pass.pushConstants(push); pass.draw(3,1,0,0);
                    pass.enableScissor(2,0,2,2);
                    push.putFloat(4,0f).putFloat(8,1f); pass.pushConstants(push); pass.draw(3,1,0,0);
                }
                encoder.copyTextureToBuffer(texture,read,0,()->{},0);
                try(var done=encoder.createFence()) { encoder.submit(); assertTrue(done.awaitCompletion(-1)); }
                try(var mapped=read.slice().map(true,false)) {
                    for(int i=0;i<8;++i) assertEquals(i%4<2?0xff00ff00:0xffff0000,mapped.data().getInt(i*4));
                }
            }
            var terrain=VanillaPipelineCatalog.create().entries().stream().filter(e->e.names().contains("SOLID_TERRAIN")).findFirst().orElseThrow();
            try(var compiled=device.compilePipeline(terrain.pipeline(),PUSH_SHADER,executor).join().finishCompile()) { assertNotNull(compiled); }
        } finally {
            if(previous==null) System.clearProperty("raynium.shaderToolsDir"); else System.setProperty("raynium.shaderToolsDir",previous);
        }
    }

    static byte[] terrainQuad(int red) {
        var bytes=ByteBuffer.allocate(8*28).order(ByteOrder.nativeOrder());
        int[][] corners={{-1,-1},{1,-1},{1,1},{-1,1}};
        for(int index:new int[]{0,1,2,3,3,2,1,0}) {
            bytes.putFloat(corners[index][0]).putFloat(corners[index][1]).putFloat(.5f);
            bytes.putInt(0xff000000|red).putFloat(.5f).putFloat(.5f).putInt(0x00f000f0);
        }
        return bytes.array();
    }

    private static void mixedTerrainViews(RayniumGpuDevice device,NativeRenderer renderer,CompiledRenderPipeline shader) {
        int width=24, height=6;
        try(var texture=device.createTexture("mixed",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,width,height,1,1);
            var depth=device.createTexture("z",GpuTexture.USAGE_RENDER_ATTACHMENT,GpuFormat.D32_FLOAT,width,height,1,1);
            var atlas=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
            var light=renderer.createTexture(1,1,1,NativeTexture.Format.RGBA8);
            var color=device.createTextureView(texture); var z=device.createTextureView(depth);
            var blocks=device.createTextureView(atlas); var lighting=device.createTextureView(light);
            var sampler=device.createSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.NEAREST,FilterMode.NEAREST,1,OptionalDouble.empty());
            var read=device.createBuffer(null,GpuBuffer.USAGE_COPY_DST|GpuBuffer.USAGE_MAP_READ,width*height*4)) {
            byte[] white={-1,-1,-1,-1};
            atlas.upload(0,0,0,1,1,white); light.upload(0,0,0,1,1,white);
            var encoder=device.createCommandEncoder();
            var batch=TerrainTestPass.batch(device);
            var views=new CommandTape.TerrainView[height];
            var storageByView=new NativeFrameBuffer[height];
            for(int i=0;i<height;++i) {
                var f=NativeTestSupport.solidFrame(i+1);
                f.terrainAlbedoTexture=atlas.id(); f.terrainLightTexture=light.id();
                f.meshes=List.of(new NativeRenderer.Mesh(i+1,terrainQuad((i+1)*32),8,0));
                f.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
                f.visible=List.of(new NativeRenderer.Visible(i+1,-1,0,0,1));
                var storage=storageByView[i]=batch.acquireViewStorage();
                views[i]=batch.addView(storage,storage.marshal(f),atlas,light);
            }
            var desc=RenderPassDescriptor.builder(()->"immediate and retained")
                .withColorAttachment(color,Optional.of(new Vector4f(0,0,0,1))).withDepthAttachment(z,OptionalDouble.of(0)).build();
            try(var pass=encoder.createRenderPass(desc)) {
                pass.setPipeline(shader);
                var push=ByteBuffer.allocateDirect(160).order(ByteOrder.nativeOrder());
                push.putFloat(4,1f).putFloat(12,1f);
                pass.pushConstants(push);
                pass.enableScissor(0,0,8,height);
                pass.draw(3,1,0,0);
                for(int y=0;y<height;++y) {
                    pass.enableScissor(8,y,8,1);
                    RayniumGpuDevice.recordTerrain(pass,views[height-1-y],0,
                        new TextureViewAndSampler(blocks,sampler),new TextureViewAndSampler(lighting,sampler));
                }
                pass.enableScissor(16,0,8,height);
                pass.draw(3,1,0,0);
            }
            encoder.copyTextureToBuffer(texture,read,0,()->{},0);
            try(var done=encoder.createFence()) {
                encoder.submit();
                var next=TerrainTestPass.batch(device);
                var reused=next.acquireViewStorage();
                assertSame(storageByView[0],reused);
                var empty=NativeTestSupport.solidFrame(100);
                empty.terrainAlbedoTexture=atlas.id(); empty.terrainLightTexture=light.id();
                next.addView(reused,reused.marshal(empty),atlas,light);
                encoder.submit();
                assertTrue(done.awaitCompletion(-1));
            }
            try(var mapped=read.slice().map(true,false)) {
                var data=mapped.data();
                for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                    int expected=x>=8 && x<16?0xff000000|((height-y)*32):0xff00ff00;
                    assertEquals(expected,data.getInt(4*(y*width+x)),"pixel ("+x+","+y+")");
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void preservesFragmentCoordinatesFacingAndSignedTexels(NativeEnvironment.Backend api) throws Exception {
        var cached=directory.resolve(".raynium/shader-cache/v2/p_49cf3552c9aea8388d4a100678886d6bf2f552a31d2bf77425cc03aba0fbc5a8.zip");
        java.nio.file.Files.createDirectories(cached.getParent());
        try(var in=getClass().getResourceAsStream("/shaders/coordinates.zip")) { java.nio.file.Files.copy(Objects.requireNonNull(in),cached); }
        var layout=BindGroupLayout.builder().withUniform("Rows",UniformType.COMBINED_IMAGE_SAMPLER)
            .withUniform("Faces",UniformType.TEXEL_BUFFER,GpuFormat.R8_SINT).build();
        var id=Identifier.parse("raynium:coordinates");
        var builder=RenderPipeline.builder().withLocation(id).withVertexShader(id).withFragmentShader(id)
            .withPrimitiveTopology(PrimitiveTopology.TRIANGLES).withColorTargetState(ColorTargetState.DEFAULT);
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true,directory); var sources=new Sources(); var executor=Executors.newSingleThreadExecutor()) {
            assertNull(device.compilePipeline(builder.build(),sources,executor).join().finishCompile());
            var wrongLayout=BindGroupLayout.builder().withUniform("Rows",UniformType.COMBINED_IMAGE_SAMPLER)
                .withUniform("Faces",UniformType.TEXEL_BUFFER,GpuFormat.R8_UINT).build();
            var wrong=RenderPipeline.builder().withLocation(id).withVertexShader(id).withFragmentShader(id)
                .withBindGroupLayout(wrongLayout).withPrimitiveTopology(PrimitiveTopology.TRIANGLES).withColorTargetState(ColorTargetState.DEFAULT).build();
            assertNull(device.compilePipeline(wrong,sources,executor).join().finishCompile());
            builder.withBindGroupLayout(layout);
            var backing=ByteBuffer.allocateDirect(3);
            backing.put(1,(byte)-7).position(1).limit(2);
            var draws=ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder());
            var indexed=ByteBuffer.allocateDirect(40).order(ByteOrder.nativeOrder());
            for(int firstInstance=0;firstInstance<2;++firstInstance) {
                draws.putInt(3).putInt(1).putInt(3).putInt(firstInstance);
                indexed.putInt(3).putInt(1).putInt(0).putInt(3).putInt(firstInstance);
            }
            draws.flip(); indexed.flip();
            var indexData=ByteBuffer.allocateDirect(6).order(ByteOrder.nativeOrder()).putShort((short)0).putShort((short)1).putShort((short)2).flip();
            try(var both=device.compilePipeline(builder.withCull(false).build(),sources,executor).join().finishCompile();
                var front=device.compilePipeline(builder.withCull(true).build(),sources,executor).join().finishCompile();
                var commands=device.createBuffer(null,GpuBuffer.USAGE_INDIRECT_PARAMETERS,draws);
                var indexedCommands=device.createBuffer(null,GpuBuffer.USAGE_INDIRECT_PARAMETERS,indexed);
                var indices=device.createBuffer(null,GpuBuffer.USAGE_INDEX,indexData);
                var image=renderer.createTexture(12,2,1,NativeTexture.Format.RGBA8);
                var rows=device.createTextureView(image);
                var faces=device.createBuffer(null,GpuBuffer.USAGE_UNIFORM_TEXEL_BUFFER,backing);
                var sampler=device.createSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.NEAREST,FilterMode.NEAREST,1,OptionalDouble.empty());
                var target=device.createTexture("orientation",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,12,2,1,1);
                var view=device.createTextureView(target); var read=device.createBuffer(null,GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,96)) {
                backing.put(1,(byte)0);
                var pixels=new byte[96];
                Arrays.fill(pixels,48,96,(byte)-1);
                image.upload(0,0,0,12,2,pixels);
                var encoder=device.createCommandEncoder();
                try(var pass=encoder.createRenderPass(()->"fragment coordinates",view,Optional.of(new Vector4f(0,0,0,1)))) {
                    for(int x=0;x<12;++x) {
                        pass.setPipeline(x%4<2?both:front);
                        pass.setUniform("Rows",rows,sampler); pass.setUniform("Faces",faces);
                        pass.enableScissor(x,0,1,2);
                        if(x<4) pass.draw(3,1,3,x%2);
                        else if(x<8) pass.drawIndirect(commands.slice((x%2)*16,16),1);
                        else {
                            pass.setIndexBuffer(indices,IndexType.SHORT);
                            pass.drawIndexedIndirect(indexedCommands.slice((x%2)*20,20),1);
                        }
                    }
                }
                encoder.copyTextureToBuffer(target,read,0,()->{},0);
                try(var done=encoder.createFence()) { encoder.submit(); assertTrue(done.awaitCompletion(-1)); }
                try(var mapped=read.slice().map(true,false)) {
                    for(int y=0;y<2;++y) for(int x=0;x<12;++x) {
                        int expected=x%4==3?0xff000000:0xff00ff00|(y*255)|(x%2==0?0xff0000:0);
                        assertEquals(Integer.toHexString(expected),Integer.toHexString(mapped.data().getInt((12*y+x)*4)),"pixel ("+x+","+y+")");
                    }
                }
            }
        }
    }

    private static ByteBuffer spriteConstants(int mip) {
        var data=ByteBuffer.allocateDirect(176).order(ByteOrder.nativeOrder());
        data.position(16).limit(160);
        Std140Builder.intoBuffer(data.slice().order(ByteOrder.nativeOrder()))
            .putMat4f(new Matrix4f().ortho2D(0,2,0,2))
            .putMat4f(new Matrix4f().scale(2,2,1))
            .putFloat(0).putFloat(0).putInt(mip);
        return data;
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void samplesTheRequestedSpriteMipFromInitialUniformData(NativeEnvironment.Backend api) throws IOException {
        var mipOne=spriteConstants(1);
        var mipZero=spriteConstants(0);
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true); var sources=new Sources(); var executor=Executors.newSingleThreadExecutor();
            var shader=device.compilePipeline(RenderPipelines.ANIMATE_SPRITE_BLIT,sources,executor).join().finishCompile();
            var image=renderer.createTexture(4,4,2,NativeTexture.Format.RGBA8);
            var full=device.createTextureView(image); var sub=device.createTextureView(image,1,1);
            var target=device.createTexture("atlas",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,4,4,1,2);
            var output=device.createTextureView(target,1,1);
            var sampler=device.createSampler(AddressMode.CLAMP_TO_EDGE,AddressMode.CLAMP_TO_EDGE,FilterMode.NEAREST,FilterMode.NEAREST,1,OptionalDouble.empty());
            var original=device.createBuffer(null,GpuBuffer.USAGE_UNIFORM,mipOne);
            var relative=device.createBuffer(null,GpuBuffer.USAGE_UNIFORM,mipZero);
            var read=device.createBuffer(null,GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,16)) {
            while(mipOne.hasRemaining()) mipOne.put((byte)0);
            while(mipZero.hasRemaining()) mipZero.put((byte)0);
            var red=ByteBuffer.allocate(64).order(ByteOrder.nativeOrder());
            var green=ByteBuffer.allocate(16).order(ByteOrder.nativeOrder());
            while(red.hasRemaining()) red.putInt(0xff0000ff);
            while(green.hasRemaining()) green.putInt(0xff00ff00);
            image.upload(0,0,0,4,4,red.array()); image.upload(1,0,0,2,2,green.array());
            var encoder=device.createCommandEncoder();
            try(var pass=encoder.createRenderPass(()->"sprite mips",output,Optional.of(new Vector4f(0,0,1,1)))) {
                pass.setPipeline(shader);
                pass.setUniform("Sprite",full,sampler); pass.setUniform("SpriteAnimationInfo",original);
                pass.enableScissor(0,0,1,2); pass.draw(6,1,0,0);
                pass.setUniform("Sprite",sub,sampler); pass.setUniform("SpriteAnimationInfo",relative);
                pass.enableScissor(1,0,1,2); pass.draw(6,1,0,0);
            }
            encoder.copyTextureToBuffer(target,read,0,()->{},1);
            try(var fence=encoder.createFence()) { encoder.submit(); assertTrue(fence.awaitCompletion(-1)); }
            try(var mapped=read.slice().map(true,false)) {
                for(int i=0;i<4;++i) assertEquals(0xff00ff00,mapped.data().getInt(4*i),"sprite pixel "+i);
            }
        }
    }

    static ByteBuffer constants(boolean green) {
        var bytes=ByteBuffer.allocateDirect(256).order(ByteOrder.nativeOrder());
        for(int i=0;i<4;++i) { bytes.putFloat(20*i,1); bytes.putFloat(64+20*i,1); }
        for(int i=0;i<4;++i) bytes.putFloat(128+4*i,green && i!=1 && i!=3?0:1);
        return bytes;
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void registersVanillaPipelinesAndReplaysDrawsThroughTheFrontend(NativeEnvironment.Backend api) throws Exception {
        try(var env=new NativeEnvironment(NativeTestSupport.locate(directory),api,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeTestSupport.shaders());
            var device=new RayniumGpuDevice(runtime,renderer,true); var sources=new Sources(); var executor=Executors.newSingleThreadExecutor()) {
            var limits=device.getDeviceInfo().limits();
            assertTrue(limits.maxMemoryAllocationSize()>0);
            assertTrue(limits.maxTextureSizeForFormat(GpuFormat.RGBA8_UNORM)>=1024);
            var gate=new java.util.concurrent.CountDownLatch(1);
            var owner=Thread.currentThread();
            var witnessed=new java.util.concurrent.atomic.AtomicBoolean();
            ShaderSource checked=new ShaderSource() {
                public String getShader(Identifier id,ShaderType stage) {
                    assertNotSame(owner,Thread.currentThread());
                    witnessed.set(true); return sources.getShader(id,stage);
                }
                public CachedIncludeSource getInclude(Identifier id) { return sources.getInclude(id); }
                public void close() {}
            };
            var future=device.compilePipeline(RenderPipelines.GUI,checked,task->executor.execute(()->{
                try {
                    if(!gate.await(5,java.util.concurrent.TimeUnit.SECONDS)) throw new AssertionError("Owner blocked in compilePipeline");
                } catch(InterruptedException e) { throw new AssertionError(e); }
                task.run();
            }));
            try { assertFalse(future.isDone()); } finally { gate.countDown(); }
            try(var compiled=future.join().finishCompile()) { assertNotNull(compiled); assertTrue(witnessed.get()); }
            var variants=List.of(RenderPipelines.GUI_TEXTURED,RenderPipelines.GUI_TEXTURED_PREMULTIPLIED_ALPHA,
                RenderPipelines.OIT_ITEM_GLINT.depthBoundsPipeline(),RenderPipelines.OIT_ITEM_GLINT_SPECIAL.depthBoundsPipeline(),RenderPipelines.CLOUDS);
            try(var parallel=Executors.newFixedThreadPool(variants.size())) {
                var ready=new java.util.concurrent.CyclicBarrier(variants.size());
                java.util.concurrent.Executor startTogether=task->parallel.execute(()->{
                    try { ready.await(10,java.util.concurrent.TimeUnit.SECONDS); }
                    catch(Exception failure) { throw new AssertionError(failure); }
                    task.run();
                });
                var futures=variants.stream().map(p->device.compilePipeline(p,sources,startTogether)).toList();
                for(int i=variants.size()-1;i>=0;--i) {
                    try(var compiled=(com.mojang.renderpearl.frontend.FrontendRenderPipeline)
                        futures.get(i).get(15,java.util.concurrent.TimeUnit.SECONDS).finishCompile()) {
                        assertNotNull(compiled);
                        assertEquals(variants.get(i).getColorTargetStates(),compiled.colorTargetStates());
                        assertEquals(variants.get(i).getVertexFormatBindings(),compiled.vertexFormats());
                        if(i==2 || i==3) assertEquals(i==2?36:44,compiled.vertexFormats().getFirst().getVertexSize());
                        if(i==4) {
                            assertTrue(compiled.uniformIndices().containsKey("CloudInfo"));
                            assertTrue(compiled.uniformIndices().containsKey("CloudFaces"));
                            assertNotEquals(compiled.uniformIndices().getInt("CloudInfo"),compiled.uniformIndices().getInt("CloudFaces"));
                        }
                    }
                }
            }
            for(var entry:VanillaPipelineCatalog.create().entries()) {
                var pipe=entry.pipeline();
                if(pipe.getPolygonMode()==PolygonMode.WIREFRAME && !device.getDeviceInfo().features().wireframeFillMode()) continue;
                var pending=device.compilePipeline(pipe,sources,executor).join();
                try(var compiled=pending.finishCompile()) { assertNotNull(compiled,entry.names().toString()); }
                catch(RuntimeException error) { throw new AssertionError(entry.names().toString(),error); }
            }
            var pipeline=RenderPipeline.builder(RenderPipelines.GUI_SNIPPET).withLocation(Identifier.parse("raynium:test_gui")).withCull(false)
                .withPrimitiveTopology(PrimitiveTopology.TRIANGLES).build();
            var vertices=ByteBuffer.allocateDirect(64).order(ByteOrder.nativeOrder());
            vertices.position(8);
            for(float[] xy:new float[][]{{-1,-1},{3,-1},{-1,3}}) vertices.putFloat(xy[0]).putFloat(xy[1]).putFloat(0).putInt(-1);
            vertices.limit(vertices.position()).position(8);
            try(var shader=device.compilePipeline(pipeline,sources,executor).join().finishCompile();
                var texture=device.createTexture("draw",GpuTexture.USAGE_RENDER_ATTACHMENT|GpuTexture.USAGE_COPY_SRC,GpuFormat.RGBA8_UNORM,8,4,1,1);
                var target=device.createTextureView(texture);
                var read=device.createBuffer(null,GpuBuffer.USAGE_MAP_READ|GpuBuffer.USAGE_COPY_DST,128);
                var vb=device.createBuffer(null,GpuBuffer.USAGE_VERTEX,vertices);
                var cb=device.createBuffer(null,GpuBuffer.USAGE_UNIFORM,constants(true));
                var pool=device.createTimestampQueryPool(1)) {
                var encoder=device.createCommandEncoder();
                while(vertices.hasRemaining()) vertices.put((byte)0);
                assertSame(encoder,device.createCommandEncoder());
                try(var p=encoder.createRenderPass(()->"partial",target,Optional.of(new Vector4f(1,0,0,1)))) {
                    assertThrows(IllegalStateException.class,encoder::submit);
                    p.setPipeline(shader); p.setVertexBuffer(0,vb.slice());
                    p.setUniform("DynamicTransforms",cb); p.setUniform("Projection",cb);
                    p.enableScissor(1,1,2,2); p.draw(3,1,0,0); p.writeTimestamp(pool,0);
                }
                var calls=new ArrayList<Integer>();
                encoder.copyTextureToBuffer(texture,read,0,()->calls.add(1),0);
                try(var fence=encoder.createFence()) { encoder.submit(); assertTrue(fence.awaitCompletion(-1)); }
                assertEquals(List.of(1),calls);
                assertTrue(pool.getValue(0).isPresent());
                try(var mapped=read.slice().map(true,false)) {
                    for(int y=0;y<4;++y) for(int x=0;x<8;++x)
                        assertEquals(x>=1 && x<=2 && y>=1 && y<=2?0xff00ff00:0xff0000ff,mapped.data().getInt((8*y+x)*4));
                }
                var half=ByteBuffer.allocateDirect(6*16).order(ByteOrder.nativeOrder());
                for(float[] xy:new float[][]{{-1,-1},{0,-1},{0,1},{0,1},{-1,1},{-1,-1}})
                    half.putFloat(xy[0]).putFloat(xy[1]).putFloat(0).putInt(-1);
                try(var left=device.createBuffer(null,GpuBuffer.USAGE_VERTEX,half.flip())) {
                    try(var clear=encoder.createRenderPass(()->"reset",target,Optional.of(new Vector4f(0,0,1,1)))) {}
                    var partial=RenderPassDescriptor.builder(()->"adjacent logical passes")
                        .withColorAttachment(target,Optional.empty()).withRenderArea(new RenderPass.RenderArea(1,1,4,2)).build();
                    for(int i=0;i<2;++i) try(var p=encoder.createRenderPass(partial)) {
                        p.setPipeline(shader); p.setVertexBuffer(0,left.slice());
                        p.setUniform("DynamicTransforms",cb); p.setUniform("Projection",cb);
                        if(i==0) p.enableScissor(1,1,1,1);
                        p.draw(6,1,0,0);
                    }
                    encoder.copyTextureToBuffer(texture,read,0,()->{},0);
                    try(var fence=encoder.createFence()) { encoder.submit(); assertTrue(fence.awaitCompletion(-1)); }
                    try(var mapped=read.slice().map(true,false)) {
                        for(int y=0;y<4;++y) for(int x=0;x<8;++x)
                            assertEquals(x>=1 && x<4 && y>=1 && y<3?0xff00ff00:0xffff0000,mapped.data().getInt(4*(8*y+x)));
                    }
                }
                try(var p=encoder.createRenderPass(()->"restore default scissor",target,Optional.empty())) {
                    p.setPipeline(shader); p.setVertexBuffer(0,vb.slice());
                    p.setUniform("DynamicTransforms",cb); p.setUniform("Projection",cb);
                    p.draw(3,1,0,0);
                }
                encoder.copyTextureToBuffer(texture,read,0,()->calls.add(2),0);
                shader.close(); vb.close(); cb.close(); target.close(); texture.close();
                try(var fence=encoder.createFence()) { encoder.submit(); assertTrue(fence.awaitCompletion(-1)); }
                try(var mapped=read.slice().map(true,false)) {
                    for(int i=0;i<32;++i) assertEquals(0xff00ff00,mapped.data().getInt(i*4));
                }
            }
        }
    }
}
