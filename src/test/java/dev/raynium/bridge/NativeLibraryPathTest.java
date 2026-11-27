package dev.raynium.bridge;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledOnOs;
import org.junit.jupiter.api.condition.OS;

@EnabledOnOs(OS.WINDOWS)
class NativeLibraryPathTest {
    @Test
    void normalizesAbsoluteDrivePathsBeforeAddingExtendedPrefix() {
        assertEquals("\\\\?\\C:\\game folder\\native\\rayniumruntime.dll",
                     NativeLibrary.windowsLibraryPath(Path.of("C:\\game folder\\cache\\..\\native\\rayniumruntime.dll")));
    }

    @Test
    void preservesUncAndAlreadyExtendedPaths() {
        assertEquals("\\\\?\\UNC\\server\\share\\rayniumruntime.dll",
                     NativeLibrary.windowsLibraryPath(Path.of("\\\\server\\share\\rayniumruntime.dll")));
        assertEquals("\\\\?\\C:\\game\\rayniumruntime.dll",
                     NativeLibrary.windowsLibraryPath(Path.of("\\\\?\\C:\\game\\rayniumruntime.dll")));
    }
}
