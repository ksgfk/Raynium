# 已有模组 native 接入调研

第三方项目展示了两类接入边界：把 MC 场景交给独立 native renderer，或让 native 引擎实现 MC 的 GPU backend。Raynium 当前采用的职责方向见 [渲染接入设计](rendering-design.md)；第三方的具体接口、线程模型和呈现方式不自动成为 Raynium 的决定。

以下为 2026-10-26 的源码调查基线，不包含本机运行或性能验证。版本栏表示所读分支目标，不代表该项目支持的全部版本。MC 类名沿用各仓库映射，旧版 WorldRenderer、SectionBuilder、GLFW 等入口不能直接照搬到 26.3。

## 源码基线

| 项目 | 所读分支目标 | 固定提交 |
| --- | --- | --- |
| RTX Remix Minecraft | MC 1.21.1 | [f2c6fdaa763f45676f1576e63f8e054bf4ef16af](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/tree/f2c6fdaa763f45676f1576e63f8e054bf4ef16af) |
| Radiance | 主分支 MC 1.21.4 | [948b29a35328ee812b9aa566fc8de2789100291a](https://github.com/Minecraft-Radiance/Radiance/tree/948b29a35328ee812b9aa566fc8de2789100291a) |
| MCVR | Radiance 的 C++ renderer | [6719d74e48e9073b5462a60b8779430150e36070](https://github.com/Minecraft-Radiance/MCVR/tree/6719d74e48e9073b5462a60b8779430150e36070) |
| wgpu-mc / Electrum | MC 26.2-rc-1 | [65a2fe332ace938067afd143b65f59995e7d01e5](https://github.com/wgpu-mc/wgpu-mc/tree/65a2fe332ace938067afd143b65f59995e7d01e5) |
| VulkanMod | MC 1.21.11 开发分支 | [087ef24e58e5522dec5ccb44f279768786bfb6da](https://github.com/xCollateral/VulkanMod/tree/087ef24e58e5522dec5ccb44f279768786bfb6da) |
| C2ME native math | MC 26.3 | [361361de8441e0c842d1190067bc1ef4e83b88c4](https://github.com/RelativityMC/C2ME-fabric/tree/361361de8441e0c842d1190067bc1ef4e83b88c4) |

本机缓存位于 `.gradle/research/<项目目录>/`。所有源码拉取经 `http://127.0.0.1:10808`。缓存被忽略提交；下列在线链接固定到表中的 commit。

| 项目 | native 边界 | 渲染接管方式 |
| --- | --- | --- |
| RTX Remix Minecraft | Java → JNI → Remix C API | 显式提交世界场景，保留 OpenGL UI |
| Radiance / MCVR | Java → JNI → C++ Vulkan renderer | 接管世界渲染，支持 OpenGL UI 和单独的 Vulkan UI 构建 |
| wgpu-mc / Electrum | JNI 初始化与 FFM 接口并存 → Rust wgpu | 当前重写先实现 MC GPU backend |
| VulkanMod | Java → LWJGL Vulkan | renderer 主体留在 Java，替换 GPU 接口及地形等路径 |
| C2ME native math | Java FFM → native 数学库 | 局部计算加速，提供绑定和内存表达的参考 |

## RTX Remix Minecraft

它加载的 runtime 文件虽然名为 d3d9.dll，但桥接代码显式调用 Remix C API 创建网格、材质和实例。这里是主动场景提交，不应把 DLL 名称理解成 MC draw 被统一翻译成 D3D9。

该版本面向 MC 1.21.1，项目说明仍将其定位为 beta 和技术展示，内容覆盖及模组兼容性有限。参见 [项目范围](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/README.md#L3)。

### Native 数据通路

| 环节 | 源码 | 行为 |
| --- | --- | --- |
| Java 加载 | [RemixAPI.java:127](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/rtxremix-java/src/com/nvidia/rtxremix/RemixAPI.java#L127) | System.load 加载 JNI bridge。 |
| Runtime 初始化 | [remixbridge.cpp:513](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/rtxremix-java/native/remixbridge.cpp#L513) | 加载 d3d9.dll，取得 remixapi_InitializeLibrary 和函数表，显式使用 Remix API。 |
| 网格提交 | [remixbridge.cpp:1058](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/rtxremix-java/native/remixbridge.cpp#L1058) | Java buffer 数组转成 MeshInfo 后调用 CreateMesh；同文件 375 行使用 GetDirectBufferAddress。 |
| 持久实例 | [RemixInstance.java:111](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/asset/RemixInstance.java#L111) | 按 handle 和 dirty 状态 add/update；未变化且可见的实例不重复调用 native。 |
| GPU 互操作 | [GlInterop.java:96](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/gl/GlInterop.java#L96) | 等待 Vulkan、复制/合成、发出 GL 完成信号；279 行导入外部内存。 |
| 最终输出 | [RemixRenderer.java:1331](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/RemixRenderer.java#L1331) | presentFrameVk；1373 行取得 Vulkan 输出供 GL 使用，1387 行将 GL 结果交回 Remix。 |

Java 捕获、缓存和提交 mesh/material/instance；native 场景保持实例状态。隐藏实例会移除 native scene handle，但 Java 网格和实例对象可以保留，以便恢复。

帧输出顺序是：Remix Vulkan 渲染世界 → GL 等待共享图像 → 原版继续合成 UI → GL 结果复制回共享图像并发信号 → Remix present。不是 CPU 逐帧读回世界图像。

### MC 修改节点

| 目标类和方法 | 修改行为 | 关键源码 |
| --- | --- | --- |
| Window.<init> | 在 glfwCreateWindow 前设置 OpenGL 4.4 Core，满足互操作上下文要求。 | [WindowMixin.java:31](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/WindowMixin.java#L31) |
| RenderSystem.flipFrame | Redirect glfwSwapBuffers 到 Remix present。 | [RenderSystemMixin.java:23](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/RenderSystemMixin.java#L23) |
| GameRenderer.renderHand / renderWorld | 捕获第一人称模型；在覆盖效果前提交 Remix 帧，并在世界渲染尾部设置提交点。 | [GameRendererMixin.java:33](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/GameRendererMixin.java#L33) |
| WorldRenderer.render | WrapOperation 捕获原版实体绘制调用；未被捕获的对象继续原版路径。 | [WorldRendererMixin.java:351](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/WorldRendererMixin.java#L351) |
| WorldRenderer.renderLayer / renderClouds 及粒子调用 | 停止已接管地形、粒子、云的原版绘制，保留或调整世界维护流程。 | [WorldRendererMixin.java:505](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/WorldRendererMixin.java#L505) |
| SectionBuilder.build | 在原版方块遍历中生成 Remix 数据；重定向 getRenderType 跳过重复原版生成，流体另行输出；RETURN 完成区块数据并入队。 | [SectionBuilderMixin.java:86](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/SectionBuilderMixin.java#L86) |
| ChunkBuilder.BuiltChunk.<init> / getBuffer | 省去每区块各层原版 VBO；模式切换期间的遗留上传使用备用 buffer。 | [BuiltChunkBuffersMixin.java:45](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/BuiltChunkBuffersMixin.java#L45) |
| EntityRenderDispatcher.render | HEAD/RETURN 建立和结束实体捕获边界。 | [EntityRenderDispatcherMixin.java:30](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/EntityRenderDispatcherMixin.java#L30) |
| BlockEntityRenderDispatcher.render | WrapMethod，使用捕获栈与 provider 求值，避免重复绘制方块实体。 | [BlockEntityRenderDispatcherMixin.java:35](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/BlockEntityRenderDispatcherMixin.java#L35) |
| ModelPart.Cuboid.renderCuboid | 捕获 consumer 活跃时提交 cuboid 身份、姿态和颜色等，取消原版逐顶点展开。 | [CuboidMixin.java:38](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/CuboidMixin.java#L38) |
| ModelPart.rotate / renderCuboids | 为捕获矩阵栈和 consumer 提供减少分配的路径。 | [ModelPartMixin.java:42](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/ModelPartMixin.java#L42) |
| TextureManager.registerTexture | 跟踪动态纹理、皮肤与字形页。 | [TextureManagerMixin.java:39](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/TextureManagerMixin.java#L39) |
| NativeImageBackedTexture.upload / SpriteContents.upload | 捕获动态像素和图集子区域变化。 | [NativeImageBackedTextureMixin.java:37](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/NativeImageBackedTextureMixin.java#L37)；[SpriteContentsUploadMixin.java:25](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/SpriteContentsUploadMixin.java#L25) |
| SpriteAtlasTexture.upload / tickAnimatedSprites | 同步图集上传及动画更新。 | [SpriteAtlasTextureMixin.java:24](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/SpriteAtlasTextureMixin.java#L24) |
| BuiltChunk.RebuildTask.run / RenderedChunk | 任务结束释放区域快照引用，最后使用者归还池化存储。 | [RebuildTaskMixin.java:35](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/RebuildTaskMixin.java#L35)；[RenderedChunkMixin.java:26](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/mixin/RenderedChunkMixin.java#L26) |

并非所有 Mixin 都是渲染接管。ClientWorldMixin、ParticleManagerMixin 等主要添加性能采样；各种 accessor 提供状态访问。完整注册表见 [Mixin 配置](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/resources/rtx-remix-renderer.mixins.json#L6)。

初始化、退出、连接断开、区块卸载和资源重载也使用 Fabric 事件注册，见 [生命周期事件](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/src/main/java/com/nvidia/rtxremix/minecraft/RtxRemixMod.java#L480)；因此只扫描 Mixin 文件不能覆盖全部接入点。

## Radiance 与 MCVR

Radiance 是 Java 模组，MCVR 是独立 C++ renderer。Java 收集 MC 数据，native 处理渲染资源与执行。默认 OpenGL UI 保留原版 UI；另一构建变体把更多 UI 绘制接口也接到 Vulkan。

### Native 数据通路与所有权

| 环节 | 源码 | 行为 |
| --- | --- | --- |
| 加载 renderer | [RadianceClient.java:80](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/client/RadianceClient.java#L80) | 从模组资源释放 DLL/SO，再 System.load。 |
| Java 实体批量输入 | [EntityProxy.java:1338](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/client/proxy/world/EntityProxy.java#L1338) | 传递实体属性数组、顶点地址与 transferredBuffers，并区分提交成功与失败清理。 |
| C++ JNI 接收 | [com_radiance_client_proxy_world_EntityProxy.cpp:13](https://github.com/Minecraft-Radiance/MCVR/blob/6719d74e48e9073b5462a60b8779430150e36070/src/core/middleware/com_radiance_client_proxy_world_EntityProxy.cpp#L13) | 地址转成构建输入，创建 Java buffer lease，再进入 Entities::queueBuild。 |
| CPU 输入消费 | [entities.cpp:2326](https://github.com/Minecraft-Radiance/MCVR/blob/6719d74e48e9073b5462a60b8779430150e36070/src/core/render/entities.cpp#L2326) | 描述信息转换/复制；2395 行判断几何是否满足保留外部 PBR 数据的条件。 |
| 缓冲区所有权 | [java_build_buffer_lease.cpp:54](https://github.com/Minecraft-Radiance/MCVR/blob/6719d74e48e9073b5462a60b8779430150e36070/src/core/middleware/java_build_buffer_lease.cpp#L54) | NewGlobalRef 保持 transferred buffer 存活；20 行析构路径调用 Java close 并释放引用。 |
| 区块任务 | [chunks.cpp:2766](https://github.com/Minecraft-Radiance/MCVR/blob/6719d74e48e9073b5462a60b8779430150e36070/src/core/render/chunks.cpp#L2766) | 保留外部几何 lease；2833 行通过 generation 和坐标拒绝过期结果。 |
| 设备匹配 | [com_radiance_client_proxy_vulkan_RendererProxy.cpp:222](https://github.com/Minecraft-Radiance/MCVR/blob/6719d74e48e9073b5462a60b8779430150e36070/src/core/middleware/com_radiance_client_proxy_vulkan_RendererProxy.cpp#L222) | 结合 presentation target 和 OpenGL 设备 UUID 初始化 Vulkan renderer。 |
| UI 合成 | [OpenGLUiBridge.java:373](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/client/opengl_ui/OpenGLUiBridge.java#L373) | prepareFrame → compositePreparedWorld → submitCompositedFrame；611 行按 generation/frameIndex 管理共享资源。 |

描述数组通常在调用期间消费并转换为 native 对象；后续仍使用的几何数据通过 lease 保活。支持直接保留外部数据的路径与需要转换/复制的路径并存，不能概括为所有输入零复制。

UI 互操作处理设备 UUID 匹配、Windows handle/Linux FD、在途帧和 resize generation。OpenGL 与 Vulkan 使用的设备以及共享资源生命周期都属于该方案的前置条件。

### 实际启用的 Mixin

[构建配置](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/build.gradle#L22) 默认选择 opengl。[排除及新增列表](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/build.gradle#L62) 和 [最终配置生成](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/build.gradle#L140) 会过滤源 mixin JSON，再添加 opengl_ui 实现。下面标注 OpenGL UI 的条目只属于该变体，其余所列世界/数据通路在默认变体保留。

| 目标类和方法 | 修改行为 | 关键源码 |
| --- | --- | --- |
| MinecraftClient.<init>，OpenGL UI | 重定向 RenderSystem 初始化，建立 GL context、查询 GPU 身份，再初始化 native renderer。 | [OpenGLUiMinecraftClientMixins.java:54](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiMinecraftClientMixins.java#L54) |
| MinecraftClient.render / close，OpenGL UI | 处理 resize；取消原版 framebuffer 输出，在 endWrite 位置提交合成结果；退出时释放两个后端。 | [OpenGLUiMinecraftClientMixins.java:109](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiMinecraftClientMixins.java#L109) |
| Window.<init> / onFramebufferSizeChanged | 接管 GL context 初始化步骤，尺寸变化通知 GL bridge 和 native。 | [OpenGLUiWindowMixins.java:16](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiWindowMixins.java#L16) |
| RenderSystem.flipFrame | 跳过原版 glfwSwapBuffers。 | [OpenGLUiRenderSystemMixins.java:20](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiRenderSystemMixins.java#L20) |
| WorldRenderer.render | HEAD 可取消注入，重写世界编排；保留必要的区块/光照更新，收集对象并调度 ChunkProxy，末尾取消原版函数继续执行。 | [WorldRendererMixins.java:203](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/WorldRendererMixins.java#L203) |
| GameRenderer.renderWorld / renderHand，OpenGL UI | 取得 native frame context；世界结束构建实体、等待重要区块并准备共享图像；手持物独立捕获。 | [OpenGLUiGameRendererMixins.java:97](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiGameRendererMixins.java#L97) |
| SectionBuilder.build | 替换 build 结果，复用方块/流体求值并通过 PBRVertexConsumer 输出网格。 | [SectionBuilderMixins.java:49](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/SectionBuilderMixins.java#L49) |
| ChunkBuilder.BuiltChunk | 取消原版 buffer map；clear/scheduleRebuild 接入自有队列；setSectionPos 通知 native 迁移。 | [ChunkBuilderBuiltChunkMixins.java:30](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/ChunkBuilderBuiltChunkMixins.java#L30) |
| BlockModelRenderer.renderQuad | 输出 quad 并向 PBR consumer 补充发光信息。 | [BlockModelRendererMixins.java:27](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/BlockModelRendererMixins.java#L27) |
| RenderLayer.draw | 捕获世界渲染期间提交的 BuiltBuffer，成功后取消原版 draw。 | [RenderLayerMixins.java:48](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/RenderLayerMixins.java#L48) |
| EntityRenderDispatcher.renderShadow / EntityRenderer.render | 取消原版实体投影，名字标签使用单独的文字 consumer。 | [EntityRenderDispatcherMixins.java:16](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/EntityRenderDispatcherMixins.java#L16)；[EntityRendererMixins.java:21](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/EntityRendererMixins.java#L21) |
| NativeImage.uploadInternal，OpenGL UI | 为 native 排队纹理区域上传。 | [OpenGLUiNativeImageMixins.java:65](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiNativeImageMixins.java#L65) |
| SpriteAtlasTexture.upload / tick | 组织图集与动画纹理的上传批次并关联纹理身份。 | [SpriteAtlasTextureMixins.java:20](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vanilla_resource_tracker/SpriteAtlasTextureMixins.java#L20) |
| BufferAllocator.CloseableBuffer / BuiltBuffer | 暴露 allocator 关系；让中心点计算识别 PBR position 元素。 | [BufferAllocatorCloseableBufferMixins.java:9](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/BufferAllocatorCloseableBufferMixins.java#L9)；[BuiltBufferMixins.java:18](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/BuiltBufferMixins.java#L18) |

Vulkan UI 变体额外接管：

- [BufferRenderer.drawWithGlobalProgram](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/BufferRendererMixins.java#L21)：上传顶点/索引，准备 uniform，经 ShaderProxy 发出 native draw。
- [CompiledShader.compile/close](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/CompiledShaderMixins.java#L31)：接管 shader 创建和释放。
- [ShaderProgram.create/set/bind/close](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixins/vulkan_render_integration/ShaderProgramMixins.java#L120)：管理程序和 uniform 状态。

源注册表见 [radiance.mixins.json](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/resources/radiance.mixins.json#L1)。其 Distant Horizons 兼容 Mixin 还受 [MixinPlugin](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/mixin_related/MixinPlugin.java#L19) 的模组存在条件控制。

## 两种场景接入方案的关键差异

| 节点 | RTX Remix | Radiance |
| --- | --- | --- |
| 地形构建 | 嵌入原版 SectionBuilder 的方块循环，并跳过重复的原版生成 | 替换 build 结果，用 PBRVertexConsumer 生成网格 |
| 动态模型 | 在原版绘制位置捕获，Cuboid 路径保留部件身份与姿态 | 世界编排集中收集，RenderLayer.draw 等位置补充捕获 |
| 世界执行 | 较多保留原版流程，逐项停止已接管的 draw | 较大范围重写 WorldRenderer.render，自己安排收集和维护 |
| 场景更新 | 明确的 retained instance add/update/remove | 批量输入、构建任务、预构建几何及 generation 等机制 |
| UI | GL 与 Vulkan 共享图像，native present | 默认同类方案，另有 Vulkan UI 构建 |

两者共同插入的位置是初始化、资源更新、区块生命周期、网格生成、动态场景、世界绘制、UI 合成/呈现和失效释放。

对 Raynium 有直接价值的是这些接入位置和生命周期问题。其 native API 的场景模型并非必须照搬；当前目标是在 Java 转换为通用资源和帧输入，再由自己的 native 管线执行。

## wgpu-mc 与 Electrum

[当前项目说明](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/README.md#L9) 明确指出 2026 年 6 月开始针对新版 MC 渲染架构重写，当前目标先覆盖 Blaze3D backend，之后再替换地形渲染和网格生成。旧截图及旧 mixin 配置描述的是此前方案。

| 目标或环节 | 行为 | 源码 |
| --- | --- | --- |
| Minecraft.<init> | Redirect PreferredGraphicsApi.getBackendsToTry，返回 WgpuBackend。 | [MixinUseWgpuBackendNotGl.java:14](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/mixin/core/MixinUseWgpuBackendNotGl.java#L14) |
| BlockState | 增加 rustBlockStateIndex 字段和访问方法。 | [BlockStateMixin.java:7](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/mixin/world/BlockStateMixin.java#L7) |
| Registry | Mixin 仍注册，但回调全部被注释，当前无生效注入。 | [RegistryMixin.java:6](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/mixin/core/RegistryMixin.java#L6) |
| 初始化 device | WgpuNative.create_device 仍是 JNI；Java 将返回地址包装成 MemorySegment。 | [WgpuDevice.java:35](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/backend/WgpuDevice.java#L35)；[WgpuNative.java:48](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/rust/WgpuNative.java#L48) |
| 绑定生成 | Rust cbindgen 生成 C header，Java jextract 生成 WM 绑定。 | [build.rs:8](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/rust/wgpu-mc-jni/build.rs#L8)；[build.gradle:50](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/build.gradle#L50) |
| 资源调用 | FFM create_buffer_init 传入地址与长度；close 显式调用 drop_buffer。 | [WgpuBuffer.java:85](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/backend/WgpuBuffer.java#L85) |
| Rust 数据消费 | C ABI 函数中对输入执行对齐、复制并创建 wgpu buffer。 | [device.rs:1283](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/rust/wgpu-mc-jni/src/device.rs#L1283) |
| 窗口输出 | 从 GLFW 取得 HWND/X11 handle 创建 native surface，83 行调用 present。 | [WgpuSurface.java:24](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/backend/WgpuSurface.java#L24) |

当前 [Mixin 配置](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/resources/wgpu.mixins.json#L6) 只列出上述三个 Mixin。旁边的 .json.old 不属于该配置。

虽然新接口主要使用 FFM，初始化仍使用 JNI，Rust crate 名仍含 jni。不能仅按 README 中的 Panama 或目录名将其概括为纯 FFM/纯 JNI。

当前源码仍有未实现分支，完整 backend 兼容是目标而非本次验证结果。其 26.2-rc-1 代码仍使用 Blaze3D 后端包与 GLFW，和 26.3 的 RenderPearl/SDL 存在版本差异。

## VulkanMod

VulkanMod 的 renderer 主体在 Java，借助 LWJGL 进入 Vulkan。它适合参考 MC GPU 抽象、shader、资源和帧流程的替换，不等同于独立 C++ 场景 runtime 桥接。

| 目标类和方法 | 修改行为 | 源码 |
| --- | --- | --- |
| Window.<init> | 修改窗口配置，另接管 vsync 和显示更新。 | [WindowMixin.java:57](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/window/WindowMixin.java#L57) |
| RenderSystem.initRenderer | Overwrite，安装 VkGpuDevice。 | [RenderSystemMixin.java:28](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/RenderSystemMixin.java#L28) |
| Minecraft.runTick / RenderSystem.flipFrame | beginFrame，移除部分原版 clear/blit，swap 位置执行 endFrame。 | [MinecraftMixin.java:22](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/frame/MinecraftMixin.java#L22)；[RenderSystemMixin.java:12](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/frame/RenderSystemMixin.java#L12) |
| LevelRenderer | 自有 WorldRenderer；替换 cullTerrain、prepareChunkRenders、setSectionDirty 和区块 layer draw，并处理方块实体。 | [LevelRendererMixin.java:46](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/chunk/LevelRendererMixin.java#L46) |
| ClientChunkCache / ClientPacketListener | 按区块数据、光照到达和卸载维护 ChunkStatusMap。 | [ClientChunkCacheM.java:24](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/chunk/ClientChunkCacheM.java#L24)；[ClientPacketListenerM.java:16](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/chunk/ClientPacketListenerM.java#L16) |
| MainTarget / RenderTarget | 主目标接到自有 main pass，改写 blit。 | [MainTargetMixin.java:23](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/target/MainTargetMixin.java#L23)；[RenderTargetMixin.java:28](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/target/RenderTargetMixin.java#L28) |
| ShaderManager.apply | 追加编译自有 render pipelines。 | [ShaderManagerM.java:21](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/shader/ShaderManagerM.java#L21) |
| GlStateManager | 把纹理、blend、viewport、scissor 等操作转入 Vulkan 状态管理。 | [GlStateManagerM.java:29](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/render/GlStateManagerM.java#L29) |
| LWJGL GL11 / GL14 / GL15 / GL30 | 额外兼容层；这些目标不是 MC 类。 | [GL11M.java:16](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/mixin/compatibility/gl/GL11M.java#L16) |

Vulkan 提交与呈现可从 [Renderer](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/java/net/vulkanmod/vulkan/Renderer.java#L384) 继续阅读。完整清单见 [Mixin 配置](https://github.com/xCollateral/VulkanMod/blob/087ef24e58e5522dec5ccb44f279768786bfb6da/src/main/resources/vulkanmod.mixins.json#L9)。

配置中的 SectionRenderDispatcherM 目前只有注释占位，不能据其名字认为它已经重写原版 dispatcher。

## C2ME native math

此处只调查 c2me-opts-natives-math 模块，不代表整个 C2ME 的修改范围。它不接管世界渲染，可参考现代 Java 下的 native 库分发、FFM 绑定和内存表示。

| 目标类 | 修改行为 | 源码 |
| --- | --- | --- |
| PerlinNoiseSampler / LegacyPerlinNoiseSampler | 添加批量采样扩展，区域和输出 buffer 进入 native。 | [MixinPerlinNoiseSampler.java:58](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinPerlinNoiseSampler.java#L58)；[MixinLegacyPerlinNoiseSampler.java:61](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinLegacyPerlinNoiseSampler.java#L61) |
| LatticedNoiseSampler / SampleBuffer | 缓存 permutation 和采样数组的 MemorySegment 视图。 | [MixinLatticedNoiseSampler.java:37](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinLatticedNoiseSampler.java#L37)；[MixinSampleBuffer.java:49](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinSampleBuffer.java#L49) |
| CompositeNoiseSampler.Builder | Overwrite component/components，展开嵌套组合。 | [MixinCompositeNoiseSamplerBuilder.java:48](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinCompositeNoiseSamplerBuilder.java#L48) |
| BiomeAccess / 末地外岛 sampler | 覆盖对应采样方法，调用 native。 | [MixinBiomeAccess.java:49](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinBiomeAccess.java#L49)；[MixinEndOuterIslandsDensityFunctionSampler.java:70](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinEndOuterIslandsDensityFunctionSampler.java#L70) |
| 加载和绑定 | 按 OS/架构加载 native 库、检测 ISA；FunctionDescriptor 描述 ABI，invokeExact 调用。 | [NativeLoader.java:85](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/NativeLoader.java#L85)；[BindingsTemplate.java:66](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/BindingsTemplate.java#L66)；[Bindings.java:38](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/Bindings.java#L38) |

此外，组合噪声与位移噪声 sampler 提供批量采样扩展。完整启用列表见 [模块 Mixin 配置](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/resources/c2me-opts-natives-math.mixins.json#L6)。

源码中的 MixinAquiferSamplerImpl、MixinMultiNoiseUtilEntries 没有列入这份配置。native 模块是否可用还受配置与库加载结果控制，见 [模块入口](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/ModuleEntryPoint.java#L28)。不能把目录中的全部实现都当作默认启用行为。

## 阅读顺序

1. Remix 的 SectionBuilderMixin、WorldRendererMixin、CuboidMixin：理解如何在 Java 模型求值与原版 draw 之间捕获数据。
2. Radiance 的 EntityProxy → MCVR JNI EntityProxy → JavaBuildBufferLease：理解描述数据、几何内存与异步使用的生命周期。
3. 两者的共享图像合成与 present：识别保留原版 UI 所需的设备和同步边界。
4. wgpu-mc 的 backend 注入与 FFM 绑定：作为另一条低层接入路线对照。
5. VulkanMod 和 C2ME：分别补充 MC 兼容改动、通用 native 调用机制的参考。

