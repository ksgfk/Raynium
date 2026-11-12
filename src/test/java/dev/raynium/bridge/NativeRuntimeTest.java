package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;
import org.slf4j.Logger;

import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
@Execution(ExecutionMode.SAME_THREAD)
class NativeRuntimeTest {
    @TempDir Path gameDirectory;
    private NativeLibraryLocator.Payload payload;
    private final List<String> events = new ArrayList<>();
    private final Logger logger = NativeTestSupport.recordingLogger(events);

    @BeforeEach void locatePayload() throws Exception {
        payload = NativeTestSupport.locate(gameDirectory);
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void environmentWithoutRuntimeClosesAndCanBeRecreated(NativeEnvironment.Backend backend) {
        for (int iteration = 0; iteration < 2; iteration++) {
            try (var environment = new NativeEnvironment(payload, backend, logger)) {
                events.clear();
                environment.close();
                environment.close();
                assertThrows(IllegalStateException.class, () -> environment.createRuntime());
            }
            assertEquals(List.of("Raynium native environment destroyed", "Raynium native library closed"), events);
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void runtimeRecreationKeepsEnvironmentAlive(NativeEnvironment.Backend backend) {
        try (var environment = new NativeEnvironment(payload, backend, logger)) {
            for (int iteration = 0; iteration < 2; iteration++) {
                try (var runtime = environment.createRuntime()) {
                    assertThrows(IllegalStateException.class, environment::close);
                    assertThrows(IllegalStateException.class, () -> environment.createRuntime());
                    events.clear();
                    runtime.close();
                    runtime.close();
                }
                assertEquals(List.of("Raynium runtime destroyed"), events);
            }
        }
        assertEquals(List.of("Raynium runtime destroyed", "Raynium native environment destroyed",
                "Raynium native library closed"), events);
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void rejectsLifecycleCallsFromAnotherThread(NativeEnvironment.Backend backend) throws InterruptedException {
        try (var environment = new NativeEnvironment(payload, backend, logger);
             var library = new NativeLibrary(payload, logger);
             var runtime = environment.createRuntime()) {
            for (Runnable operation : List.<Runnable>of(runtime::close, environment::close,
                    () -> environment.createRuntime(), library.bindings()::destroyEnvironment,
                    () -> library.bindings().createRuntime())) {
                var rejection = new AtomicReference<Throwable>();
                var thread = new Thread(() -> {
                    try { operation.run(); }
                    catch (Throwable failure) { rejection.set(failure); }
                });
                thread.start();
                thread.join();
                assertInstanceOf(IllegalStateException.class, rejection.get());
            }
            assertFalse(events.contains("Raynium runtime destroyed"));
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void duplicateEnvironmentCannotDestroyOriginalOwner(NativeEnvironment.Backend backend) {
        try (var environment = new NativeEnvironment(payload, backend, logger)) {
            for (var requested : NativeEnvironment.Backend.values()) {
                var failure = assertThrows(IllegalStateException.class,
                        () -> new NativeEnvironment(payload, requested, logger));
                assertTrue(failure.getMessage().contains("rn_init_env"));
            }
            try (var runtime = environment.createRuntime();
                 var renderer = runtime.createRenderer(NativeTestSupport.shaders())) {
                assertTrue(runtime.deviceInfo().maxTextureSize()>0);
            }
        }
    }

    @ParameterizedTest @ValueSource(booleans = {false, true})
    void earlyD3D12EnvironmentSupportsRuntimeRecreation(boolean validation) {
        try(var environment=new NativeEnvironment(payload,NativeEnvironment.Backend.D3D12,validation,logger);
            var window=new NativeSwapchainTest.Window()) {
            for(int i=0;i<2;i++) try(var runtime=environment.createRuntime();
                var renderer=runtime.createRenderer(NativeTestSupport.shaders());
                var chain=renderer.createSwapchain(window.hwnd,640,360,NativeSwapchain.PresentMode.IMMEDIATE)) {
                assertEquals(NativeSwapchain.Presentation.PRESENTED,renderer.present(chain,NativeTestSupport.solidFrame(1)));
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void nativeGuardsPreserveEnvironmentAndRuntime(NativeEnvironment.Backend backend) throws Throwable {
        try (var abi = new NativeTestSupport(gameDirectory)) {
            abi.envDesc.set(ValueLayout.JAVA_INT, 4, backend.nativeValue());
            assertEquals(0, abi.initEnv(abi.envDesc));
            assertEquals(1, abi.initEnv(abi.envDesc));

            abi.desc.set(ValueLayout.JAVA_INT, 4, 2);
            assertEquals(1, abi.create(abi.desc, abi.output));
            abi.desc.set(ValueLayout.JAVA_INT, 4, 0);
            assertEquals(0, abi.create(abi.desc, abi.output));
            MemorySegment first = abi.output.get(ValueLayout.ADDRESS, 0);
            assertEquals(1, abi.destroyEnv());

            assertEquals(1, abi.create(abi.desc, abi.output));
            assertEquals(MemorySegment.NULL, abi.output.get(ValueLayout.ADDRESS, 0));
            abi.destroy(first);
            assertEquals(0, abi.create(abi.desc, abi.output));
            abi.destroy(abi.output.get(ValueLayout.ADDRESS, 0));
            assertEquals(0, abi.destroyEnv());
        }
    }
}
