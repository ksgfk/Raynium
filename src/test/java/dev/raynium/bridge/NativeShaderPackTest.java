package dev.raynium.bridge;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.zip.CRC32;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;
import static java.lang.foreign.ValueLayout.*;

@Tag("native")
class NativeShaderPackTest {
    @TempDir Path gameDirectory;
    private static byte[] bytes() throws IOException {
        try(var input=NativeShaderPackTest.class.getResourceAsStream("/assets/raynium/shaderpacks/default.rnsp")) {
            assertNotNull(input); return input.readAllBytes();
        }
    }
    private static int record(byte[] data,String name,int target) {
        var b=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        for(int i=0;i<b.getInt(12);i++) {
            int offset=24+i*96,end=offset;
            while(data[end]!=0) end++;
            if(new String(data,offset,end-offset,java.nio.charset.StandardCharsets.US_ASCII).equals(name) && b.getInt(offset+64)==target) return offset;
        }
        throw new AssertionError("Missing program "+name);
    }
    private static void checksum(byte[] data,int record) {
        var buffer=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int offset=buffer.getInt(record+68),size=buffer.getInt(record+72);
        var crc=new CRC32(); crc.update(data,offset,size); buffer.putInt(record+76,(int)crc.getValue());
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rejectsMalformedPacksAndAllowsRetry(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=environment.createRuntime()) {
            byte[] original=bytes();
            for(String mutation:new String[]{"version","truncated","range","overlap","duplicate","missing","checksum","identity","schema","toolchain","binding","vertex","constants"}) {
                byte[] changed=original.clone(); var b=ByteBuffer.wrap(changed).order(ByteOrder.LITTLE_ENDIAN);
                int record=record(changed,"terrain",backend==NativeEnvironment.Backend.VULKAN?1:0),offset=b.getInt(record+68);
                switch(mutation) {
                    case "version" -> b.putInt(4,2);
                    case "truncated" -> changed=Arrays.copyOf(changed,changed.length-1);
                    case "range" -> b.putInt(record+68,Integer.MAX_VALUE);
                    case "overlap" -> { b.putInt(24+96+68,b.getInt(24+68)); b.putInt(24+96+72,b.getInt(24+72)); b.putInt(24+96+76,b.getInt(24+76)); }
                    case "duplicate" -> b.putInt(24+96+64,0);
                    case "missing" -> { changed[record]='x'; }
                    case "checksum" -> changed[changed.length-1]^=1;
                    case "identity" -> changed[record+80]^=1;
                    case "schema" -> { b.putShort(offset+4,(short)7); checksum(changed,record); }
                    case "toolchain" -> { b.putLong(offset+80,0); checksum(changed,record); }
                    case "binding" -> { int bindings=b.getInt(offset+24); b.putInt(offset+bindings+12,99); checksum(changed,record); }
                    case "vertex" -> { int inputs=b.getInt(offset+48); b.putInt(offset+inputs+20,1); checksum(changed,record); }
                    case "constants" -> { int types=b.getInt(offset+32); int count=b.getInt(offset+36)/52;
                        for(int i=0;i<count;i++) if(b.getInt(offset+types+i*52+24)==192) { b.putInt(offset+types+i*52+24,208); break; }
                        checksum(changed,record); }
                    default -> throw new AssertionError(mutation);
                }
                var pack=NativeShaderPack.fromBytes(changed);
                var failure=assertThrows(IllegalStateException.class,()->runtime.createRenderer(pack),mutation);
                assertFalse(failure.getMessage().isBlank());
            }
            byte[] source=bytes(); var immutable=NativeShaderPack.fromBytes(source); Arrays.fill(source,(byte)0);
            for(int i=0;i<2;i++) try(var renderer=runtime.createRenderer(immutable)) {
                // Creation's confined FFM arena is already closed; subsequent GPU resource creation must still work.
                assertTrue(runtime.deviceInfo().maxTextureSize()>0);
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void everyRequiredProgramValidatesItsOwnInterface(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var runtime=environment.createRuntime()) {
            for(String program:new String[]{"terrain","terrain_cutout","terrain_translucent","terrain_oit_bounds",
                "terrain_oit_transmittance","terrain_oit_accumulate","oit_bounds_cull","oit_composite","gui_color","gui_textured","gui_text"}) {
                for(boolean missing:new boolean[]{true,false}) {
                    byte[] data=bytes(); var b=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
                    int record=record(data,program,backend==NativeEnvironment.Backend.VULKAN?1:0);
                    if(missing) data[record]='x';
                    else {
                        int offset=b.getInt(record+68),bindings=b.getInt(offset+24);
                        b.putInt(offset+bindings+12,99); checksum(data,record);
                    }
                    var failure=assertThrows(IllegalStateException.class,()->runtime.createRenderer(NativeShaderPack.fromBytes(data)),program);
                    assertFalse(failure.getMessage().isBlank());
                }
            }
            try(var renderer=runtime.createRenderer(NativeTestSupport.shaders())) { assertNotNull(renderer); }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void descriptorFailureClearsOutputAndRejectsAbi10Layout(NativeEnvironment.Backend backend) throws Throwable {
        try(var abi=new NativeTestSupport(gameDirectory); var arena=Arena.ofConfined()) {
            abi.envDesc.set(JAVA_INT,4,backend==NativeEnvironment.Backend.D3D12?1:2);
            assertEquals(0,abi.initEnv(abi.envDesc));
            assertEquals(0,abi.create(abi.desc,abi.output));
            var runtime=abi.output.get(ADDRESS,0); var desc=arena.allocate(24,8);
            for(int size:new int[]{8,24}) {
                desc.set(JAVA_INT,0,size); abi.output.set(ADDRESS,0,MemorySegment.ofAddress(1));
                assertEquals(1,abi.createRenderer(runtime,desc,abi.output));
                assertEquals(MemorySegment.NULL,abi.output.get(ADDRESS,0));
            }
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rejectsUnknownOrConflictingSubmissionFlags(NativeEnvironment.Backend backend) throws Throwable {
        try(var abi=new NativeTestSupport(gameDirectory); var arena=Arena.ofConfined()) {
            abi.envDesc.set(JAVA_INT,4,backend==NativeEnvironment.Backend.D3D12?1:2);
            assertEquals(0,abi.initEnv(abi.envDesc));
            assertEquals(0,abi.create(abi.desc,abi.output));
            var runtime=abi.output.get(ADDRESS,0); var desc=arena.allocate(24,8); var shaders=NativeTestSupport.shaders();
            desc.set(JAVA_INT,0,24); desc.set(ADDRESS,8,shaders.copyTo(arena)); desc.set(JAVA_LONG,16,shaders.size());
            for(int flags:new int[]{3,4,-1}) {
                desc.set(JAVA_INT,4,flags); abi.output.set(ADDRESS,0,MemorySegment.ofAddress(1));
                assertEquals(1,abi.createRenderer(runtime,desc,abi.output));
                assertEquals(MemorySegment.NULL,abi.output.get(ADDRESS,0));

            }
        }
    }
}
