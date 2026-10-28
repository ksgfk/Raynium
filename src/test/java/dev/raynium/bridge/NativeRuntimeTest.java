package dev.raynium.bridge;

import dev.raynium.client.preview.VulkanPreviewImage;
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
                assertThrows(IllegalStateException.class, () -> environment.createRuntime(null));
            }
            assertEquals(List.of("Raynium native environment destroyed", "Raynium native library closed"), events);
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void runtimeRecreationKeepsEnvironmentAlive(NativeEnvironment.Backend backend) {
        try (var environment = new NativeEnvironment(payload, backend, logger)) {
            for (int iteration = 0; iteration < 2; iteration++) {
                try (var runtime = environment.createRuntime(null)) {
                    assertThrows(IllegalStateException.class, environment::close);
                    assertThrows(IllegalStateException.class, () -> environment.createRuntime(null));
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
             var runtime = environment.createRuntime(null)) {
            for (Runnable operation : List.<Runnable>of(runtime::close, environment::close,
                    () -> environment.createRuntime(null), library.bindings()::destroyEnvironment,
                    () -> library.bindings().createRuntime(null))) {
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
                assertTrue(failure.getMessage().contains("already initialized"));
            }
            try (var runtime = environment.createRuntime(null);
                 var preview = runtime.createPreview(1, 1)) {
                assertEquals(1, preview.info().width());
            }
        }
    }

    @ParameterizedTest @ValueSource(booleans = {false, true})
    void earlyD3D12EnvironmentSupportsHostAndRuntimeRecreation(boolean validation) {
        try (var environment = new NativeEnvironment(payload, NativeEnvironment.Backend.D3D12, validation, logger);
             var consumer = new VulkanConsumer()) {
            for (int iteration = 0; iteration < 2; iteration++) {
                try (var runtime = environment.createRuntime(VulkanPreviewImage.hostGpu(consumer.device.getPhysicalDevice()));
                     var preview = runtime.createPreview(17, 9);
                     var image = VulkanPreviewImage.create(consumer.device, consumer.family, preview.info())) {
                    for (long frame = 1; frame <= 2; frame++) {
                        preview.draw(frame);
                        consumer.copyAndCheck(image, frame, 17, 9);
                    }
                }
            }
        }
    }

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void nativeGuardsPreserveEnvironmentAndRuntime(NativeEnvironment.Backend backend) throws Throwable {
        try (var abi = new NativeTestSupport(gameDirectory)) {
            abi.envDesc.set(ValueLayout.JAVA_INT, 4, backend.nativeValue());
            assertEquals(0, abi.initEnv(abi.envDesc, abi.error, 256), abi.error.getString(0));
            assertEquals(1, abi.initEnv(abi.envDesc, abi.error, 256));
            assertTrue(abi.error.getString(0).contains("already initialized"));
            abi.desc.set(ValueLayout.JAVA_INT, 4, 2);
            assertEquals(1, abi.create(abi.desc, abi.output, abi.error, 256));
            abi.desc.set(ValueLayout.JAVA_INT, 4, 0);
            assertEquals(0, abi.create(abi.desc, abi.output, abi.error, 256), abi.error.getString(0));
            MemorySegment first = abi.output.get(ValueLayout.ADDRESS, 0);
            assertEquals(1, abi.destroyEnv(abi.error, 256));
            assertTrue(abi.error.getString(0).contains("active runtime"));
            assertEquals(1, abi.create(abi.desc, abi.output, abi.error, 256));
            assertEquals(MemorySegment.NULL, abi.output.get(ValueLayout.ADDRESS, 0));
            abi.destroy(first);
            assertEquals(0, abi.create(abi.desc, abi.output, abi.error, 256), abi.error.getString(0));
            abi.destroy(abi.output.get(ValueLayout.ADDRESS, 0));
            assertEquals(0, abi.destroyEnv(abi.error, 256));
        }
    }
}
