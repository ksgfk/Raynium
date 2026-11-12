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
    void environmentAllowsRepeatedInitializationAndDestruction() throws Throwable {
        assertEquals(0, abi.initEnv(abi.envDesc));
        assertEquals(0, abi.destroyEnv());
        assertEquals(0, abi.destroyEnv());
        assertEquals(0, abi.initEnv(abi.envDesc));
        assertEquals(0, abi.destroyEnv());
    }

    @Test
    void environmentRejectsNullDescriptor() throws Throwable {
        assertEquals(1, abi.initEnv(MemorySegment.NULL));
    }

    @ParameterizedTest @ValueSource(ints = {0, 8, 16})
    void environmentRejectsIncorrectDescriptorSize(int size) throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 0, size);
        assertEquals(1, abi.initEnv(abi.envDesc));
    }

    @Test
    void environmentRejectsUnknownFlagsAndAllowsRetry() throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 8, 2);
        assertEquals(1, abi.initEnv(abi.envDesc));

        abi.envDesc.set(ValueLayout.JAVA_INT, 8, 0);
        assertEquals(0, abi.initEnv(abi.envDesc));
    }

    @Test
    void runtimeRequiresEnvironment() throws Throwable {
        reject(abi.desc);
    }

    @Test
    void rejectsNullDescriptor() throws Throwable {
        reject(MemorySegment.NULL);
    }

    @Test
    void rejectsNullOutput() throws Throwable {
        assertEquals(1, abi.create(abi.desc, MemorySegment.NULL));
    }

    @ParameterizedTest
    @ValueSource(ints = {4, 12})
    void rejectsIncorrectDescriptorSize(int size) throws Throwable {
        abi.desc.set(ValueLayout.JAVA_INT, 0, size);
        reject(abi.desc);
    }

    @Test
    void rejectsUnknownBackend() throws Throwable {
        abi.envDesc.set(ValueLayout.JAVA_INT, 4, 99);
        assertEquals(1, abi.initEnv(abi.envDesc));
    }

    @Test
    void acceptsNullDestroy() {
        abi.destroy(MemorySegment.NULL);
    }

    @ParameterizedTest @ValueSource(ints = {4, 1})
    void rejectsNonzeroReservedField(int flags) throws Throwable {
        abi.desc.set(ValueLayout.JAVA_INT, 4, flags);
        reject(abi.desc);
    }

    @Test
    void queryRejectsNullInfo() throws Throwable {
        assertEquals(1, abi.query(MemorySegment.NULL, 240));
    }

    @Test
    void queryRejectsShortBufferWithoutWritingInfo() throws Throwable {
        abi.info.fill((byte) 0x7f);
        byte[] before = abi.info.toArray(ValueLayout.JAVA_BYTE);
        assertEquals(1, abi.query(abi.info, 239));

        assertArrayEquals(before, abi.info.toArray(ValueLayout.JAVA_BYTE));
    }

    @Test
    void queryReportsAbi() throws Throwable {
        assertEquals(0, abi.query(abi.info, 240));
        assertEquals(240, abi.info.get(ValueLayout.JAVA_INT, 0));
        assertEquals(NativeManifest.ABI_VERSION, abi.info.get(ValueLayout.JAVA_INT, 4));
    }

    private void reject(MemorySegment desc) throws Throwable {
        abi.output.set(ValueLayout.ADDRESS, 0, MemorySegment.ofAddress(1));
        assertEquals(1, abi.create(desc, abi.output), "FAILED status");
        assertEquals(MemorySegment.NULL, abi.output.get(ValueLayout.ADDRESS, 0), "failure clears output");
    }
}
