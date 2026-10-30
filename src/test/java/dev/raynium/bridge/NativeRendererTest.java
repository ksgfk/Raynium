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
class NativeRendererTest {
    @TempDir Path gameDirectory;

    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void failedConsumerWaitRetainsOwners(NativeEnvironment.Backend backend) throws Exception {
        try(var environment=new NativeEnvironment(NativeTestSupport.locate(gameDirectory),backend,NativeTestSupport.recordingLogger(new ArrayList<>()));
            var consumer=new VulkanConsumer();
            var runtime=environment.createRuntime(VulkanPreviewImage.hostGpu(consumer.device.getPhysicalDevice()));
            var renderer=runtime.createRenderer();
            var target=renderer.createTarget(17,9);
            var image=VulkanPreviewImage.create(consumer.device,consumer.family,target.info())) {
            var scene=new dev.raynium.client.preview.UvScene(renderer);
            var rejection=new java.util.concurrent.atomic.AtomicReference<Throwable>();
            var wrongThread=new Thread(()->{ try { scene.render(target,1); } catch(Throwable failure) { rejection.set(failure); } });
            wrongThread.start(); wrongThread.join(); assertInstanceOf(IllegalStateException.class,rejection.get());
            scene.render(target,1);
            assertThrows(IllegalStateException.class,target::close);
            assertEquals(17,target.info().width());
            assertThrows(IllegalStateException.class,renderer::close);
            assertThrows(IllegalStateException.class,runtime::close);
            assertThrows(IllegalStateException.class,environment::close);
            consumer.copyAndCheck(image,1,17,9);
            scene.render(target,2); consumer.copyAndCheck(image,2,17,9);
        }
    }
    @ParameterizedTest @EnumSource(NativeEnvironment.Backend.class)
    void sharesUvWithVulkanAcrossFramesAndRecreation(NativeEnvironment.Backend backend) throws Exception {
        try (var environment = new NativeEnvironment(NativeTestSupport.locate(gameDirectory), backend,
                     NativeTestSupport.recordingLogger(new ArrayList<>()));
             var consumer = new VulkanConsumer();
             var runtime = environment.createRuntime(VulkanPreviewImage.hostGpu(consumer.device.getPhysicalDevice()));
             var renderer=runtime.createRenderer()) {
            var scene=new dev.raynium.client.preview.UvScene(renderer);
            assertThrows(IllegalStateException.class, () -> renderer.createTarget(0, 9));
            if (backend == NativeEnvironment.Backend.VULKAN) {
                try (var preview = renderer.createTarget(17, 9)) {
                    var i = preview.info();
                    var invalid = new SharedImageInfo(i.handleType(), i.width(), i.height(), i.memoryHandle(),
                            i.readyHandle(), i.consumedHandle(), i.allocationSize(), -1, i.nodeMask(), i.luid(),
                            i.deviceUuid(), i.driverUuid(), i.nativeFormat(), i.nativeUsage(), i.nativeFlags(),
                            i.nativeTiling(), i.allocationOffset(), i.dedicated());
                    // Fails after VkImage creation; consumer validation at close detects leaked objects.
                    assertThrows(IllegalStateException.class,
                            () -> VulkanPreviewImage.create(consumer.device, consumer.family, invalid));
                }
            }
            for (int[] size : new int[][]{{257, 131}, {1, 1}, {319, 177}}) {
                try (var preview = renderer.createTarget(size[0], size[1]);
                     var image = VulkanPreviewImage.create(consumer.device, consumer.family, preview.info())) {
                    assertEquals(backend == NativeEnvironment.Backend.D3D12 ? 1 : 2, preview.info().handleType());
                    assertThrows(IllegalStateException.class, runtime::close);
                    assertThrows(IllegalStateException.class, () -> renderer.createTarget(16, 16));
                    assertThrows(IllegalStateException.class, () -> scene.render(preview,0));
                    assertThrows(IllegalStateException.class, () -> scene.render(preview,2));
                    for (long frame = 1; frame <= 8; frame++) {
                        scene.render(preview,frame);
                        consumer.copyAndCheck(image, frame, size[0], size[1]);
                    }
                    assertThrows(IllegalStateException.class, () -> scene.render(preview,8));
                }
            }
        }
    }
}
