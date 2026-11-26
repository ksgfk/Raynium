package dev.raynium.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandles;
import java.util.Optional;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import static org.junit.jupiter.api.Assertions.*;

class NativeBindingsTest {
    @ParameterizedTest @ValueSource(ints={19,20,21,22,23,24,25,27})
    void rejectsIncompatibleDllBeforeLookingUpVersionSpecificExports(int version) {
        try(var arena=Arena.ofConfined()) {
            var versionAddress=Linker.nativeLinker().upcallStub(MethodHandles.constant(int.class,version),
                FunctionDescriptor.of(ValueLayout.JAVA_INT),arena);
            var failure=assertThrows(IllegalStateException.class,()->new NativeBindings(name-> {
                assertEquals("rn_get_abi_version",name,"Incompatible DLL must not resolve newer ABI symbols");
                return Optional.of(versionAddress);
            }));
            assertTrue(failure.getMessage().contains("version mismatch: "+version));
        }
    }
}
