package dev.raynium.bridge;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.lwjgl.sdl.SDLError;
import org.lwjgl.sdl.SDLEvents;
import org.lwjgl.sdl.SDLInit;
import org.lwjgl.sdl.SDLProperties;
import org.lwjgl.sdl.SDLVideo;
import org.lwjgl.system.MemoryStack;
@Tag("native")
class NativeSwapchainTest {
    @TempDir
    Path gameDirectory;
    static final class Window implements AutoCloseable {
        final long handle, hwnd;
        Window() {
            if (!SDLInit.SDL_Init(SDLInit.SDL_INIT_VIDEO))
                throw new IllegalStateException(SDLError.SDL_GetError());
            handle = SDLVideo.SDL_CreateWindow("Raynium swapchain test", 640, 360, SDLVideo.SDL_WINDOW_RESIZABLE | SDLVideo.SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (handle == 0) {
                SDLInit.SDL_Quit();
                throw new IllegalStateException(SDLError.SDL_GetError());
            }
            hwnd = SDLProperties.SDL_GetPointerProperty(SDLVideo.SDL_GetWindowProperties(handle), SDLVideo.SDL_PROP_WINDOW_WIN32_HWND_POINTER, 0);
            if (hwnd == 0) {
                close();
                throw new IllegalStateException("SDL window has no HWND");
            }
            pump();
        }
        void pump() {
            SDLEvents.SDL_PumpEvents();
        }
        void zeroClientArea() throws Throwable {
            try (var arena = Arena.ofConfined(); var stack = MemoryStack.stackPush()) {
                var top = stack.mallocInt(1);
                var left = stack.mallocInt(1);
                var bottom = stack.mallocInt(1);
                var right = stack.mallocInt(1);
                assertTrue(SDLVideo.SDL_GetWindowBordersSize(handle, top, left, bottom, right));
                var symbols = SymbolLookup.libraryLookup("user32.dll", arena);
                var linker = Linker.nativeLinker();
                var integer = ValueLayout.JAVA_INT;
                var address = ValueLayout.ADDRESS;
                var resize = linker.downcallHandle(symbols.find("SetWindowPos").orElseThrow(),
                                                   FunctionDescriptor.of(integer, address, address, integer, integer, integer, integer, integer));
                var window = MemorySegment.ofAddress(hwnd);
                assertNotEquals(0, (int)resize.invokeExact(window, MemorySegment.NULL, 0, 0,
                                                           left.get(0) + right.get(0), top.get(0) + bottom.get(0), 0x16));
                pump();
                var bounds = arena.allocate(16, 4);
                var query = linker.downcallHandle(symbols.find("GetClientRect").orElseThrow(),
                                                  FunctionDescriptor.of(integer, address, address));
                assertNotEquals(0, (int)query.invokeExact(window, bounds));
                int width = bounds.get(integer, 8) - bounds.get(integer, 0), height = bounds.get(integer, 12) - bounds.get(integer, 4);
                assertTrue(width == 0 || height == 0, "Expected zero client area, got " + width + "x" + height);
            }
        }
        @Override
        public void close() {
            SDLVideo.SDL_DestroyWindow(handle);
            SDLInit.SDL_Quit();
        }
    }
    @ParameterizedTest
    @EnumSource(NativeEnvironment.Backend.class)
    void presentsResizesSkipsAndRecreatesAfterFailedPreparation(NativeEnvironment.Backend backend) throws Throwable {
        try (var environment = new NativeEnvironment(NativeTestSupport.locate(gameDirectory), backend, NativeTestSupport.validation(), NativeTestSupport.recordingLogger(new ArrayList<>()));
             var window = new Window(); var runtime = environment.createRuntime()) {
            var info = runtime.deviceInfo();
            assertEquals(backend.nativeValue(), info.backend());
            assertFalse(info.name().isBlank());
            assertFalse(info.vendor().isBlank());
            assertTrue(info.transfer().hostCoherentAtomSize() >= 1);
            if (backend == NativeEnvironment.Backend.D3D12)
                assertEquals(1, info.transfer().hostCoherentAtomSize());
            assertTrue(info.maxTextureSize() >= 1920);
            assertTrue(info.maxColorAttachments() >= 2);
            assertTrue(info.uniformOffsetAlignment() > 0);
            assertTrue(info.maxAnisotropy() >= 1);
            for (int generation = 0; generation < 2; generation++) {
                try (var renderer = runtime.createRenderer(NativeTestSupport.shaders(), NativeTestSupport.submissionMode());
                     var chain = renderer.createSwapchain(window.hwnd, 640, 360, NativeSwapchain.PresentMode.IMMEDIATE)) {
                    assertThrows(IllegalStateException.class, runtime::close);
                    assertThrows(IllegalStateException.class, renderer::close);
                    assertThrows(IllegalStateException.class, () -> renderer.createSwapchain(window.hwnd, 640, 360, NativeSwapchain.PresentMode.FIFO));
                    var wrongThread = new AtomicReference<Throwable>();
                    var worker = new Thread(() -> {try { NativeTestSupport.blit(renderer,chain); } catch(Throwable failure) {wrongThread.set(failure);} });
                    worker.start();
                    worker.join();
                    assertInstanceOf(IllegalStateException.class, wrongThread.get());
                    int presented = 0;
                    long started = System.nanoTime();
                    int attempts = 0;
                    while (presented < 120 && attempts++ < 240) {
                        window.pump();
                        if (NativeTestSupport.blit(renderer, chain) == 0)
                            presented++;
                    }
                    assertEquals(120, presented);
                    System.out.printf("Swapchain backend=%s requested=IMMEDIATE attempts=%d presented=%d fps=%.1f%n",
                                      backend, attempts, presented, presented * 1e9 / (System.nanoTime() - started));
                    for (var mode : NativeSwapchain.PresentMode.values())
                        for (int[] size : new int[][] {{640, 360}, {1281, 721}, {1920, 1080}}) {
                            assertTrue(SDLVideo.SDL_SetWindowSize(window.handle, size[0], size[1]));
                            window.pump();
                            chain.resize(size[0], size[1], mode);
                            assertEquals(0, NativeTestSupport.blit(renderer, chain));
                        }
                    assertTrue(SDLVideo.SDL_HideWindow(window.handle));
                    window.pump();
                    assertEquals(1, NativeTestSupport.blit(renderer, chain));
                    assertTrue(SDLVideo.SDL_ShowWindow(window.handle));
                    assertTrue(SDLVideo.SDL_MinimizeWindow(window.handle));
                    assertTrue(SDLVideo.SDL_SyncWindow(window.handle));
                    window.pump();
                    assertEquals(1, NativeTestSupport.blit(renderer, chain));
                    assertTrue(SDLVideo.SDL_RestoreWindow(window.handle));
                    assertTrue(SDLVideo.SDL_SyncWindow(window.handle));
                    window.pump();
                    assertEquals(0, NativeTestSupport.blit(renderer, chain));
                    window.zeroClientArea();
                    assertEquals(1, NativeTestSupport.blit(renderer, chain));
                    assertTrue(SDLVideo.SDL_SetWindowSize(window.handle, 640, 360));
                    assertTrue(SDLVideo.SDL_SyncWindow(window.handle));
                    window.pump();
                    chain.resize(640, 360, NativeSwapchain.PresentMode.IMMEDIATE);
                    assertEquals(0, NativeTestSupport.blit(renderer, chain));
                }
            }
        }
    }
}
