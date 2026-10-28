package dev.raynium.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

@Tag("native")
@Execution(ExecutionMode.SAME_THREAD)
class NativeAbiTest {
    @TempDir Path gameDirectory;
    private NativeTestSupport abi;

    @BeforeEach
    void openLibrary() throws Exception {
        abi = new NativeTestSupport(gameDirectory);
    }

    @AfterEach
    void closeLibrary() {
        if (abi != null) abi.close();
    }

    @Test
    void environmentRejectsInvalidDiagnosticBuffers() throws Throwable {
        assertEquals(1, abi.initEnv(abi.envDesc, MemorySegment.NULL, 1));
        assertEquals(1, abi.destroyEnv(MemorySegment.NULL, 1));
    }

    @Test
    void environmentClearsDiagnosticsAndAllowsOmittingThem() throws Throwable {
        abi.error.fill((byte) 0x7f);
        assertEquals(0, abi.initEnv(abi.envDesc, abi.error, 1));
        assertEquals((byte) 0, abi.error.get(ValueLayout.JAVA_BYTE, 0));
        assertEquals((byte) 0x7f, abi.error.get(ValueLayout.JAVA_BYTE, 1));
        assertEquals(0, abi.destroyEnv(MemorySegment.NULL, 0));
        assertEquals(0, abi.destroyEnv(MemorySegment.NULL, 0));
        assertEquals(0, abi.initEnv(abi.envDesc, MemorySegment.NULL, 0));
        assertEquals(0, abi.destroyEnv(abi.error, 1));
        assertEquals((byte) 0x7f, abi.error.get(ValueLayout.JAVA_BYTE, 1));
    }

    @Test
    void environmentRejectsNullDescriptor() throws Throwable {
        assertEquals(1, abi.initEnv(MemorySegment.NULL, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("rn_env_desc"));
    }

    @ParameterizedTest @ValueSource(ints = {0, 8, 16})
    void environmentRejectsIncorrectDescriptorSize(int size) throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 0, size);
        assertEquals(1, abi.initEnv(abi.envDesc, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("expected 12 bytes"));
    }

    @Test
    void environmentRejectsUnknownFlagsAndAllowsRetry() throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 8, 2);
        assertEquals(1, abi.initEnv(abi.envDesc, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("environment flags"));
        abi.envDesc.set(ValueLayout.JAVA_INT, 8, 0);
        assertEquals(0, abi.initEnv(abi.envDesc, abi.error, 256));
    }

    @Test
    void runtimeRequiresEnvironment() throws Throwable {
        reject(abi.desc, abi.error, 256);
        assertTrue(abi.error.getString(0).contains("not initialized"));
    }

    @Test
    void rejectsNullDescriptor() throws Throwable {
        reject(MemorySegment.NULL, abi.error, 256);
        assertTrue(abi.error.getString(0).contains("desc is null"));
    }

    @Test
    void rejectsNullOutput() throws Throwable {
        assertEquals(1, abi.create(abi.desc, MemorySegment.NULL, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("out_runtime is null"));
    }

    @Test
    void rejectsNullDiagnosticWithNonzeroCapacity() throws Throwable {
        reject(abi.desc, MemorySegment.NULL, 1);
    }

    @ParameterizedTest
    @ValueSource(ints = {4, 12})
    void rejectsIncorrectDescriptorSize(int size) throws Throwable {
        abi.desc.set(ValueLayout.JAVA_INT, 0, size);
        reject(abi.desc, abi.error, 256);
        assertTrue(abi.error.getString(0).contains("struct_size; expected 20 bytes"));
    }

    @Test
    void rejectsUnknownBackendWithDiagnostic() throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 4, 99);
        assertEquals(1, abi.initEnv(abi.envDesc, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("Unknown RHI backend"));
    }

    @ParameterizedTest
    @ValueSource(ints = {1, 5})
    void truncatesDiagnosticWithinCapacity(int capacity) throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 4, 99);
        abi.error.fill((byte) 0x7f);
        assertEquals(1, abi.initEnv(abi.envDesc, abi.error, capacity));
        assertEquals(capacity == 1 ? "" : "Unkn", abi.error.getString(0));
        assertEquals((byte) 0x7f, abi.error.get(ValueLayout.JAVA_BYTE, capacity), "byte after buffer must remain untouched");
    }

    @Test
    void allowsOmittingDiagnosticOnFailure() throws Throwable {
        abi.desc.set(ValueLayout.JAVA_INT, 4, 99);
        reject(abi.desc, MemorySegment.NULL, 0);
    }

    @Test
    void acceptsNullDestroy() {
        abi.destroy(MemorySegment.NULL);
    }

    @ParameterizedTest @ValueSource(ints = {4, 1})
    void rejectsUnknownFlagsOrInvalidHostNode(int flags) throws Throwable {
        abi.desc.set(ValueLayout.JAVA_INT, 4, flags);
        reject(abi.desc, abi.error, 256);
        assertTrue(abi.error.getString(0).contains("flags or host node mask"));
    }

    @Test
    void queryRejectsNullInfoWithDiagnostic() throws Throwable {
        assertEquals(1, abi.query(MemorySegment.NULL, 240, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("info is null"));
    }

    @Test
    void queryRejectsShortBufferWithoutWritingInfo() throws Throwable {
        abi.info.fill((byte) 0x7f);
        byte[] before = abi.info.toArray(ValueLayout.JAVA_BYTE);
        assertEquals(1, abi.query(abi.info, 239, abi.error, 256));
        assertTrue(abi.error.getString(0).contains("capacity"));
        assertArrayEquals(before, abi.info.toArray(ValueLayout.JAVA_BYTE));
    }

    @Test
    void queryRejectsNullDiagnosticWithNonzeroCapacity() throws Throwable {
        abi.info.fill((byte) 0x7f);
        byte[] before = abi.info.toArray(ValueLayout.JAVA_BYTE);
        assertEquals(1, abi.query(abi.info, 240, MemorySegment.NULL, 1));
        assertArrayEquals(before, abi.info.toArray(ValueLayout.JAVA_BYTE));
    }

    @Test
    void queryAllowsOmittingDiagnosticOnSuccess() throws Throwable {
        assertEquals(0, abi.query(abi.info, 240, MemorySegment.NULL, 0));
        assertEquals(240, abi.info.get(ValueLayout.JAVA_INT, 0));
        assertEquals(NativeManifest.ABI_VERSION, abi.info.get(ValueLayout.JAVA_INT, 4));
    }

    @Test
    void queryTruncatesAndClearsDiagnostic() throws Throwable {
        abi.error.fill((byte) 0x7f);
        assertEquals(1, abi.query(MemorySegment.NULL, 240, abi.error, 5));
        assertEquals("rn_q", abi.error.getString(0));
        assertEquals((byte) 0x7f, abi.error.get(ValueLayout.JAVA_BYTE, 5));
        assertEquals(0, abi.query(abi.info, 240, abi.error, 5));
        assertEquals("", abi.error.getString(0));
    }

    private void reject(MemorySegment desc, MemorySegment diagnostic, int capacity) throws Throwable {
        abi.output.set(ValueLayout.ADDRESS, 0, MemorySegment.ofAddress(1));
        assertEquals(1, abi.create(desc, abi.output, diagnostic, capacity), "FAILED status");
        assertEquals(MemorySegment.NULL, abi.output.get(ValueLayout.ADDRESS, 0), "failure clears output");
    }
}
