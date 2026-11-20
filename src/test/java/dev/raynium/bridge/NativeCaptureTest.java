package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodHandles;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.locks.LockSupport;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.lwjgl.sdl.SDLVideo;
import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativeCaptureTest {
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rawPollOnlyConsumesAfterSuccessfulCopy(NativeEnvironment.Backend backend) throws Throwable {
        try (var environment = new NativeEnvironment(NativeTestSupport.locate(gameDirectory), backend,
                     NativeTestSupport.recordingLogger(new ArrayList<>()));
             var window = new NativeSwapchainTest.Window();
             var runtime = environment.createRuntime();
             var renderer = runtime.createRenderer(NativeTestSupport.shaders());
             var chain = renderer.createSwapchain(window.hwnd, 127, 65, NativeSwapchain.PresentMode.IMMEDIATE);
             var arena = Arena.ofConfined()) {
            var handleField = MethodHandles.privateLookupIn(NativeRenderer.class, MethodHandles.lookup())
                    .findVarHandle(NativeRenderer.class, "handle", MemorySegment.class);
            var handle = (MemorySegment) handleField.get(renderer);
            var poll = renderer.bindings.pollCapture;
            long id = 8100L;
            var info = arena.allocate(32, 8);
            info.set(JAVA_INT, 0, 32);
            try(var texture=renderer.createTexture(127,65,1,1,NativeTexture.Format.RGBA8,NativeTexture.ATTACHMENT|NativeTexture.READBACK);
                var tape=new dev.raynium.client.renderpearl.CommandTape(renderer)) {
                var region=new dev.raynium.client.renderpearl.CommandTape.Region(texture,0,0,0,0,127,65);
                tape.clearColor(region,1,0,0,1); tape.captureColor(region,id);
                texture.close();
                assertTrue(tape.submit().success());
            }
            long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
            do {
                assertEquals(0, (int) poll.invokeExact(handle, id, info, MemorySegment.NULL, 0L));
                if (info.get(JAVA_INT, 4) == 1) break;
                LockSupport.parkNanos(1_000_000);
            } while (System.nanoTime() < deadline);
            assertEquals(1, info.get(JAVA_INT, 4));
            assertEquals(0, (int) poll.invokeExact(handle, id, info, MemorySegment.NULL, 0L));
            assertEquals(1, info.get(JAVA_INT, 4));
            long size = info.get(JAVA_LONG, 16);
            assertEquals(127L * 65 * 4, size);
            var pixels = arena.allocate(size);
            assertEquals(1, (int) poll.invokeExact(handle, id, info, pixels, 0L));
            assertEquals(1, (int) poll.invokeExact(handle, id, info, pixels, size - 1));
            assertEquals(0, (int) poll.invokeExact(handle, id, info, pixels, size));
            assertEquals(255, Byte.toUnsignedInt(pixels.get(JAVA_BYTE, 0)));
            assertEquals(1, (int) poll.invokeExact(handle, id, info, MemorySegment.NULL, 0L));
        }
    }

    @TempDir Path gameDirectory;
}
