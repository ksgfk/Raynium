package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.slf4j.Logger;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

@Tag("native")
@Execution(ExecutionMode.SAME_THREAD)
class NativeRuntimeTest {
    @TempDir Path gameDirectory;
    private NativeLibraryLocator.Payload payload;
    private final List<String> events = new ArrayList<>();
    private final Logger logger = NativeTestSupport.recordingLogger(events);

    @BeforeEach
    void locatePayload() throws Exception {
        payload = NativeTestSupport.locate(gameDirectory);
    }

    @ParameterizedTest
    @EnumSource(NativeRuntime.Backend.class)
    void createsDestroysAndRecreates(NativeRuntime.Backend backend) {
        for (int iteration = 0; iteration < 2; iteration++) {
            try (var runtime = new NativeRuntime(payload, backend, logger)) {
                events.clear();
            }
            assertEquals(List.of("Raynium runtime destroyed", "Raynium native library closed"), events,
                    "runtime destruction must precede DLL unload");
        }
    }

    @ParameterizedTest
    @EnumSource(NativeRuntime.Backend.class)
    void closeIsIdempotent(NativeRuntime.Backend backend) {
        try (var runtime = new NativeRuntime(payload, backend, logger)) {
            runtime.close();
            int afterFirstClose = events.size();
            runtime.close();
            assertEquals(afterFirstClose, events.size());
        }
    }

    @ParameterizedTest
    @EnumSource(NativeRuntime.Backend.class)
    void rejectsCloseFromAnotherThread(NativeRuntime.Backend backend) throws InterruptedException {
        try (var runtime = new NativeRuntime(payload, backend, logger)) {
            var rejection = new AtomicReference<Throwable>();
            Thread wrongThread = new Thread(() -> {
                try { runtime.close(); }
                catch (Throwable failure) { rejection.set(failure); }
            }, "wrong-runtime-owner");
            wrongThread.start();
            wrongThread.join();
            assertInstanceOf(IllegalStateException.class, rejection.get());
            assertFalse(events.contains("Raynium runtime destroyed"));
        }
    }

    @Test
    void failedVulkanCreationPreservesOwnerAndAllowsRecreation() throws Throwable {
        try (var abi = new NativeTestSupport(gameDirectory)) {
            abi.desc.set(ValueLayout.JAVA_INT, 4, 2);
            assertEquals(0, abi.create(abi.desc, abi.output, abi.error, 256), abi.error.getString(0));
            MemorySegment first = abi.output.get(ValueLayout.ADDRESS, 0);
            assertEquals(1, abi.create(abi.desc, abi.output, abi.error, 256), "occupied Vulkan environment returns FAILED");
            assertEquals(MemorySegment.NULL, abi.output.get(ValueLayout.ADDRESS, 0));
            assertTrue(abi.error.getString(0).contains("Vulkan"));
            abi.destroy(first);
            assertEquals(0, abi.create(abi.desc, abi.output, MemorySegment.NULL, 0), "Vulkan recreation after failure cleanup");
        }
    }
}
