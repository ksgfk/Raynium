package dev.raynium.bridge;

import dev.raynium.client.preview.VulkanPreviewImage;
import java.nio.file.Path;
import java.util.ArrayList;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import static org.junit.jupiter.api.Assertions.*;

@Tag("native")
class NativePreviewTest {
    @TempDir Path gameDirectory;

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void sharesUvWithVulkanAcrossFramesAndRecreation(NativeEnvironment.Backend backend) throws Exception {
        try (var environment = new NativeEnvironment(NativeTestSupport.locate(gameDirectory), backend,
                     NativeTestSupport.recordingLogger(new ArrayList<>()));
             var consumer = new VulkanConsumer();
             var runtime = environment.createRuntime(VulkanPreviewImage.hostGpu(consumer.device.getPhysicalDevice()))) {
            assertThrows(IllegalStateException.class, () -> runtime.createPreview(0, 9));
            if (backend == NativeEnvironment.Backend.VULKAN) {
                try (var preview = runtime.createPreview(17, 9)) {
                    var i = preview.info();
                    var invalid = new NativePreview.Info(i.handleType(), i.width(), i.height(), i.memoryHandle(),
                            i.readyHandle(), i.consumedHandle(), i.allocationSize(), -1, i.nodeMask(), i.luid(),
                            i.deviceUuid(), i.driverUuid(), i.nativeFormat(), i.nativeUsage(), i.nativeFlags(),
                            i.nativeTiling(), i.allocationOffset(), i.dedicated());
                    // Fails after VkImage creation; consumer validation at close detects leaked objects.
                    assertThrows(IllegalStateException.class,
                            () -> VulkanPreviewImage.create(consumer.device, consumer.family, invalid));
                }
            }
            for (int[] size : new int[][]{{257, 131}, {1, 1}, {319, 177}}) {
                try (var preview = runtime.createPreview(size[0], size[1]);
                     var image = VulkanPreviewImage.create(consumer.device, consumer.family, preview.info())) {
                    assertEquals(backend == NativeEnvironment.Backend.D3D12 ? 1 : 2, preview.info().handleType());
                    assertThrows(IllegalStateException.class, runtime::close);
                    assertThrows(IllegalStateException.class, () -> runtime.createPreview(16, 16));
                    assertThrows(IllegalStateException.class, () -> preview.draw(0));
                    assertThrows(IllegalStateException.class, () -> preview.draw(2));
                    for (long frame = 1; frame <= 8; frame++) {
                        preview.draw(frame);
                        consumer.copyAndCheck(image, frame, size[0], size[1]);
                    }
                    assertThrows(IllegalStateException.class, () -> preview.draw(8));
                }
            }
        }
    }
}
