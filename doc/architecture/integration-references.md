> - 适用: 比较现有模组的场景接入、呈现与 native 加载做法
> - 权威: 本文保存注明调查日期和版本的源码证据，不定义 Raynium 契约或当前能力
> - 锚点: `gradle.properties`, `src/client/resources/raynium.client.mixins.json`, `doc/architecture/minecraft-integration.md`

# 已有模组 native 接入调研

第三方项目包含两类接入边界：把 MC 场景交给独立 native renderer，或让 native 引擎实现 MC 的 GPU backend。Raynium 当前实现见[架构地图](overview.md)；本页记录第三方源码行为，不定义 Raynium 方案。

以下为 2026-10-26 的源码调查基线，不包含本机运行或性能验证。版本栏表示所读分支目标，不代表该项目支持的全部版本。MC 类名沿用各仓库映射，旧版 WorldRenderer、SectionBuilder、GLFW 等入口不能直接照搬到 26.3。

## 源码基线

| 项目 | 所读分支目标 | 仓库入口 |
| --- | --- | --- |
| RTX Remix Minecraft | MC 1.21.1 | [仓库](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft) |
| Radiance | 主分支 MC 1.21.4 | [仓库](https://github.com/Minecraft-Radiance/Radiance) |
| MCVR | Radiance 的 C++ renderer | [仓库](https://github.com/Minecraft-Radiance/MCVR) |
| wgpu-mc / Electrum | MC 26.2-rc-1 | [仓库](https://github.com/wgpu-mc/wgpu-mc) |
| VulkanMod | MC 1.21.11 开发分支 | [仓库](https://github.com/xCollateral/VulkanMod) |
| C2ME native math | MC 26.3 | [仓库](https://github.com/RelativityMC/C2ME-fabric) |

调查时的缓存约定位于 `.gradle/research/<项目目录>/`，缓存不随仓库分发。下文链接指向仓库入口，附带路径与行号用于定位调查时的源码，不保证仓库当前内容与调查时相同。以下结论仅来自所列版本源码，不代表已运行第三方项目或验证其性能。

| 项目 | native 边界 | 渲染接管方式 |
| --- | --- | --- |
| RTX Remix Minecraft | Java → JNI → Remix C API | 显式提交世界场景，保留 OpenGL UI |
| Radiance / MCVR | Java → JNI → C++ Vulkan renderer | 接管世界渲染，支持 OpenGL UI 和单独的 Vulkan UI 构建 |
| wgpu-mc / Electrum | JNI 初始化与 FFM 接口并存 → Rust wgpu | 当前重写先实现 MC GPU backend |
| VulkanMod | Java → LWJGL Vulkan | renderer 主体留在 Java，替换 GPU 接口及地形等路径 |
| C2ME native math | Java FFM → native 数学库 | 局部数学计算加速，不接管世界渲染 |

## RTX Remix Minecraft

它加载的 runtime 文件虽然名为 d3d9.dll，但桥接代码显式调用 Remix C API 创建网格、材质和实例。这里是主动场景提交，不应把 DLL 名称理解成 MC draw 被统一翻译成 D3D9。

该版本面向 MC 1.21.1，项目说明仍将其定位为 beta 和技术展示，内容覆盖及模组兼容性有限。参见 [项目范围](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`README.md:3`）。

### Native 数据通路

| 环节 | 源码 | 行为 |
| --- | --- | --- |
| Java 加载 | [RemixAPI.java:127](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`rtxremix-java/src/com/nvidia/rtxremix/RemixAPI.java:127`） | System.load 加载 JNI bridge。 |
| Runtime 初始化 | [remixbridge.cpp:513](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`rtxremix-java/native/remixbridge.cpp:513`） | 加载 d3d9.dll，取得 remixapi_InitializeLibrary 和函数表，显式使用 Remix API。 |
| 网格提交 | [remixbridge.cpp:1058](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`rtxremix-java/native/remixbridge.cpp:1058`） | Java buffer 数组转成 MeshInfo 后调用 CreateMesh；同文件 375 行使用 GetDirectBufferAddress。 |
| 持久实例 | [RemixInstance.java:111](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/asset/RemixInstance.java:111`） | 按 handle 和 dirty 状态 add/update；未变化且可见的实例不重复调用 native。 |
| GPU 互操作 | [GlInterop.java:96](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/gl/GlInterop.java:96`） | 等待 Vulkan、复制/合成、发出 GL 完成信号；279 行导入外部内存。 |
| 最终输出 | [RemixRenderer.java:1331](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/RemixRenderer.java:1331`） | presentFrameVk；1373 行取得 Vulkan 输出供 GL 使用，1387 行将 GL 结果交回 Remix。 |

Java 捕获、缓存和提交 mesh/material/instance；native 场景保持实例状态。隐藏实例会移除 native scene handle，但 Java 网格和实例对象可以保留，以便恢复。

帧输出顺序是：Remix Vulkan 渲染世界 → GL 等待共享图像 → 原版继续合成 UI → GL 结果复制回共享图像并发信号 → Remix present。不是 CPU 逐帧读回世界图像。

### MC 修改节点

| 目标类和方法 | 修改行为 | 关键源码 |
| --- | --- | --- |
| Window.<init> | 在 glfwCreateWindow 前设置 OpenGL 4.4 Core，满足互操作上下文要求。 | [WindowMixin.java:31](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/WindowMixin.java:31`） |
| RenderSystem.flipFrame | Redirect glfwSwapBuffers 到 Remix present。 | [RenderSystemMixin.java:23](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/RenderSystemMixin.java:23`） |
| GameRenderer.renderHand / renderWorld | 捕获第一人称模型；在覆盖效果前提交 Remix 帧，并在世界渲染尾部设置提交点。 | [GameRendererMixin.java:33](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/GameRendererMixin.java:33`） |
| WorldRenderer.render | WrapOperation 捕获原版实体绘制调用；未被捕获的对象继续原版路径。 | [WorldRendererMixin.java:351](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/WorldRendererMixin.java:351`） |
| WorldRenderer.renderLayer / renderClouds 及粒子调用 | 停止已接管地形、粒子、云的原版绘制，保留或调整世界维护流程。 | [WorldRendererMixin.java:505](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/WorldRendererMixin.java:505`） |
| SectionBuilder.build | 在原版方块遍历中生成 Remix 数据；重定向 getRenderType 跳过重复原版生成，流体另行输出；RETURN 完成区块数据并入队。 | [SectionBuilderMixin.java:86](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/SectionBuilderMixin.java:86`） |
| ChunkBuilder.BuiltChunk.<init> / getBuffer | 省去每区块各层原版 VBO；模式切换期间的遗留上传使用备用 buffer。 | [BuiltChunkBuffersMixin.java:45](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/BuiltChunkBuffersMixin.java:45`） |
| EntityRenderDispatcher.render | HEAD/RETURN 建立和结束实体捕获边界。 | [EntityRenderDispatcherMixin.java:30](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/EntityRenderDispatcherMixin.java:30`） |
| BlockEntityRenderDispatcher.render | WrapMethod，使用捕获栈与 provider 求值，避免重复绘制方块实体。 | [BlockEntityRenderDispatcherMixin.java:35](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/BlockEntityRenderDispatcherMixin.java:35`） |
| ModelPart.Cuboid.renderCuboid | 捕获 consumer 活跃时提交 cuboid 身份、姿态和颜色等，取消原版逐顶点展开。 | [CuboidMixin.java:38](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/CuboidMixin.java:38`） |
| ModelPart.rotate / renderCuboids | 为捕获矩阵栈和 consumer 提供减少分配的路径。 | [ModelPartMixin.java:42](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/ModelPartMixin.java:42`） |
| TextureManager.registerTexture | 跟踪动态纹理、皮肤与字形页。 | [TextureManagerMixin.java:39](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/TextureManagerMixin.java:39`） |
| NativeImageBackedTexture.upload / SpriteContents.upload | 捕获动态像素和图集子区域变化。 | [NativeImageBackedTextureMixin.java:37](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/NativeImageBackedTextureMixin.java:37`）；[SpriteContentsUploadMixin.java:25](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/SpriteContentsUploadMixin.java:25`） |
| SpriteAtlasTexture.upload / tickAnimatedSprites | 同步图集上传及动画更新。 | [SpriteAtlasTextureMixin.java:24](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/SpriteAtlasTextureMixin.java:24`） |
| BuiltChunk.RebuildTask.run / RenderedChunk | 任务结束释放区域快照引用，最后使用者归还池化存储。 | [RebuildTaskMixin.java:35](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/RebuildTaskMixin.java:35`）；[RenderedChunkMixin.java:26](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/mixin/RenderedChunkMixin.java:26`） |

并非所有 Mixin 都是渲染接管。ClientWorldMixin、ParticleManagerMixin 等主要添加性能采样；各种 accessor 提供状态访问。完整注册表见 [Mixin 配置](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/resources/rtx-remix-renderer.mixins.json:6`）。

初始化、退出、连接断开、区块卸载和资源重载也使用 Fabric 事件注册，见 [生命周期事件](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`src/main/java/com/nvidia/rtxremix/minecraft/RtxRemixMod.java:480`）；因此只扫描 Mixin 文件不能覆盖全部接入点。

## Radiance 与 MCVR

Radiance 是 Java 模组，MCVR 是独立 C++ renderer。Java 收集 MC 数据，native 处理渲染资源与执行。默认 OpenGL UI 保留原版 UI；另一构建变体把更多 UI 绘制接口也接到 Vulkan。

### Native 数据通路与所有权

| 环节 | 源码 | 行为 |
| --- | --- | --- |
| 加载 renderer | [RadianceClient.java:80](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/client/RadianceClient.java:80`） | 从模组资源释放 DLL/SO，再 System.load。 |
| Java 实体批量输入 | [EntityProxy.java:1338](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/client/proxy/world/EntityProxy.java:1338`） | 传递实体属性数组、顶点地址与 transferredBuffers，并区分提交成功与失败清理。 |
| C++ JNI 接收 | [com_radiance_client_proxy_world_EntityProxy.cpp:13](https://github.com/Minecraft-Radiance/MCVR)（`src/core/middleware/com_radiance_client_proxy_world_EntityProxy.cpp:13`） | 地址转成构建输入，创建 Java buffer lease，再进入 Entities::queueBuild。 |
| CPU 输入消费 | [entities.cpp:2326](https://github.com/Minecraft-Radiance/MCVR)（`src/core/render/entities.cpp:2326`） | 描述信息转换/复制；2395 行判断几何是否满足保留外部 PBR 数据的条件。 |
| 缓冲区所有权 | [java_build_buffer_lease.cpp:54](https://github.com/Minecraft-Radiance/MCVR)（`src/core/middleware/java_build_buffer_lease.cpp:54`） | NewGlobalRef 保持 transferred buffer 存活；20 行析构路径调用 Java close 并释放引用。 |
| 区块任务 | [chunks.cpp:2766](https://github.com/Minecraft-Radiance/MCVR)（`src/core/render/chunks.cpp:2766`） | 保留外部几何 lease；2833 行通过 generation 和坐标拒绝过期结果。 |
| 设备匹配 | [com_radiance_client_proxy_vulkan_RendererProxy.cpp:222](https://github.com/Minecraft-Radiance/MCVR)（`src/core/middleware/com_radiance_client_proxy_vulkan_RendererProxy.cpp:222`） | 结合 presentation target 和 OpenGL 设备 UUID 初始化 Vulkan renderer。 |
| UI 合成 | [OpenGLUiBridge.java:373](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/client/opengl_ui/OpenGLUiBridge.java:373`） | prepareFrame → compositePreparedWorld → submitCompositedFrame；611 行按 generation/frameIndex 管理共享资源。 |

描述数组通常在调用期间消费并转换为 native 对象；后续仍使用的几何数据通过 lease 保活。支持直接保留外部数据的路径与需要转换/复制的路径并存，不能概括为所有输入零复制。

UI 互操作处理设备 UUID 匹配、Windows handle/Linux FD、在途帧和 resize generation。OpenGL 与 Vulkan 使用的设备以及共享资源生命周期都属于该方案的前置条件。

### 实际启用的 Mixin

[构建配置](https://github.com/Minecraft-Radiance/Radiance)（`build.gradle:22`） 默认选择 opengl。[排除及新增列表](https://github.com/Minecraft-Radiance/Radiance)（`build.gradle:62`） 和 [最终配置生成](https://github.com/Minecraft-Radiance/Radiance)（`build.gradle:140`） 会过滤源 mixin JSON，再添加 opengl_ui 实现。下面标注 OpenGL UI 的条目只属于该变体，其余所列世界/数据通路在默认变体保留。

| 目标类和方法 | 修改行为 | 关键源码 |
| --- | --- | --- |
| MinecraftClient.<init>，OpenGL UI | 重定向 RenderSystem 初始化，建立 GL context、查询 GPU 身份，再初始化 native renderer。 | [OpenGLUiMinecraftClientMixins.java:54](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiMinecraftClientMixins.java:54`） |
| MinecraftClient.render / close，OpenGL UI | 处理 resize；取消原版 framebuffer 输出，在 endWrite 位置提交合成结果；退出时释放两个后端。 | [OpenGLUiMinecraftClientMixins.java:109](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiMinecraftClientMixins.java:109`） |
| Window.<init> / onFramebufferSizeChanged | 接管 GL context 初始化步骤，尺寸变化通知 GL bridge 和 native。 | [OpenGLUiWindowMixins.java:16](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiWindowMixins.java:16`） |
| RenderSystem.flipFrame | 跳过原版 glfwSwapBuffers。 | [OpenGLUiRenderSystemMixins.java:20](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiRenderSystemMixins.java:20`） |
| WorldRenderer.render | HEAD 可取消注入，重写世界编排；保留必要的区块/光照更新，收集对象并调度 ChunkProxy，末尾取消原版函数继续执行。 | [WorldRendererMixins.java:203](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/WorldRendererMixins.java:203`） |
| GameRenderer.renderWorld / renderHand，OpenGL UI | 取得 native frame context；世界结束构建实体、等待重要区块并准备共享图像；手持物独立捕获。 | [OpenGLUiGameRendererMixins.java:97](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiGameRendererMixins.java:97`） |
| SectionBuilder.build | 替换 build 结果，复用方块/流体求值并通过 PBRVertexConsumer 输出网格。 | [SectionBuilderMixins.java:49](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/SectionBuilderMixins.java:49`） |
| ChunkBuilder.BuiltChunk | 取消原版 buffer map；clear/scheduleRebuild 接入自有队列；setSectionPos 通知 native 迁移。 | [ChunkBuilderBuiltChunkMixins.java:30](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/ChunkBuilderBuiltChunkMixins.java:30`） |
| BlockModelRenderer.renderQuad | 输出 quad 并向 PBR consumer 补充发光信息。 | [BlockModelRendererMixins.java:27](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/BlockModelRendererMixins.java:27`） |
| RenderLayer.draw | 捕获世界渲染期间提交的 BuiltBuffer，成功后取消原版 draw。 | [RenderLayerMixins.java:48](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/RenderLayerMixins.java:48`） |
| EntityRenderDispatcher.renderShadow / EntityRenderer.render | 取消原版实体投影，名字标签使用单独的文字 consumer。 | [EntityRenderDispatcherMixins.java:16](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/EntityRenderDispatcherMixins.java:16`）；[EntityRendererMixins.java:21](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/EntityRendererMixins.java:21`） |
| NativeImage.uploadInternal，OpenGL UI | 为 native 排队纹理区域上传。 | [OpenGLUiNativeImageMixins.java:65](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiNativeImageMixins.java:65`） |
| SpriteAtlasTexture.upload / tick | 组织图集与动画纹理的上传批次并关联纹理身份。 | [SpriteAtlasTextureMixins.java:20](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vanilla_resource_tracker/SpriteAtlasTextureMixins.java:20`） |
| BufferAllocator.CloseableBuffer / BuiltBuffer | 暴露 allocator 关系；让中心点计算识别 PBR position 元素。 | [BufferAllocatorCloseableBufferMixins.java:9](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/BufferAllocatorCloseableBufferMixins.java:9`）；[BuiltBufferMixins.java:18](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/BuiltBufferMixins.java:18`） |

Vulkan UI 变体额外接管：

- [BufferRenderer.drawWithGlobalProgram](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/BufferRendererMixins.java:21`）：上传顶点/索引，准备 uniform，经 ShaderProxy 发出 native draw。
- [CompiledShader.compile/close](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/CompiledShaderMixins.java:31`）：接管 shader 创建和释放。
- [ShaderProgram.create/set/bind/close](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixins/vulkan_render_integration/ShaderProgramMixins.java:120`）：管理程序和 uniform 状态。

源注册表见 [radiance.mixins.json](https://github.com/Minecraft-Radiance/Radiance)（`src/main/resources/radiance.mixins.json:1`）。其 Distant Horizons 兼容 Mixin 还受 [MixinPlugin](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/mixin_related/MixinPlugin.java:19`） 的模组存在条件控制。

## 两种场景接入方案的关键差异

| 节点 | RTX Remix | Radiance |
| --- | --- | --- |
| 地形构建 | 嵌入原版 SectionBuilder 的方块循环，并跳过重复的原版生成 | 替换 build 结果，用 PBRVertexConsumer 生成网格 |
| 动态模型 | 在原版绘制位置捕获，Cuboid 路径保留部件身份与姿态 | 世界编排集中收集，RenderLayer.draw 等位置补充捕获 |
| 世界执行 | 较多保留原版流程，逐项停止已接管的 draw | 较大范围重写 WorldRenderer.render，自己安排收集和维护 |
| 场景更新 | 明确的 retained instance add/update/remove | 批量输入、构建任务、预构建几何及 generation 等机制 |
| UI | GL 与 Vulkan 共享图像，native present | 默认同类方案，另有 Vulkan UI 构建 |

两者共同插入的位置是初始化、资源更新、区块生命周期、网格生成、动态场景、世界绘制、UI 合成/呈现和失效释放。

## wgpu-mc 与 Electrum

[该版本项目说明](https://github.com/wgpu-mc/wgpu-mc)（`README.md:9`） 记载 2026 年 6 月开始针对新版 MC 渲染架构重写。下表记录调查时的实际代码；旧截图及旧 mixin 配置不属于同一实现基线。

| 目标或环节 | 行为 | 源码 |
| --- | --- | --- |
| Minecraft.<init> | Redirect PreferredGraphicsApi.getBackendsToTry，返回 WgpuBackend。 | [MixinUseWgpuBackendNotGl.java:14](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/mixin/core/MixinUseWgpuBackendNotGl.java:14`） |
| BlockState | 增加 rustBlockStateIndex 字段和访问方法。 | [BlockStateMixin.java:7](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/mixin/world/BlockStateMixin.java:7`） |
| Registry | Mixin 仍注册，但回调全部被注释，当前无生效注入。 | [RegistryMixin.java:6](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/mixin/core/RegistryMixin.java:6`） |
| 初始化 device | WgpuNative.create_device 仍是 JNI；Java 将返回地址包装成 MemorySegment。 | [WgpuDevice.java:35](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/backend/WgpuDevice.java:35`）；[WgpuNative.java:48](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/rust/WgpuNative.java:48`） |
| 绑定生成 | Rust cbindgen 生成 C header，Java jextract 生成 WM 绑定。 | [build.rs:8](https://github.com/wgpu-mc/wgpu-mc)（`rust/wgpu-mc-jni/build.rs:8`）；[build.gradle:50](https://github.com/wgpu-mc/wgpu-mc)（`fabric/build.gradle:50`） |
| 资源调用 | FFM create_buffer_init 传入地址与长度；close 显式调用 drop_buffer。 | [WgpuBuffer.java:85](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/backend/WgpuBuffer.java:85`） |
| Rust 数据消费 | C ABI 函数中对输入执行对齐、复制并创建 wgpu buffer。 | [device.rs:1283](https://github.com/wgpu-mc/wgpu-mc)（`rust/wgpu-mc-jni/src/device.rs:1283`） |
| 窗口输出 | 从 GLFW 取得 HWND/X11 handle 创建 native surface，83 行调用 present。 | [WgpuSurface.java:24](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/backend/WgpuSurface.java:24`） |

当前 [Mixin 配置](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/resources/wgpu.mixins.json:6`） 只列出上述三个 Mixin。旁边的 .json.old 不属于该配置。

虽然新接口主要使用 FFM，初始化仍使用 JNI，Rust crate 名仍含 jni。不能仅按 README 中的 Panama 或目录名将其概括为纯 FFM/纯 JNI。

调查时的源码仍有未实现分支，本次没有完整 backend 兼容验证结果。其 26.2-rc-1 代码仍使用 Blaze3D 后端包与 GLFW，和 26.3 的 RenderPearl/SDL 存在版本差异。

## VulkanMod

VulkanMod 的 renderer 主体在 Java，借助 LWJGL 进入 Vulkan；它没有独立的 C++ 场景 runtime 桥接层。

| 目标类和方法 | 修改行为 | 源码 |
| --- | --- | --- |
| Window.<init> | 修改窗口配置，另接管 vsync 和显示更新。 | [WindowMixin.java:57](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/window/WindowMixin.java:57`） |
| RenderSystem.initRenderer | Overwrite，安装 VkGpuDevice。 | [RenderSystemMixin.java:28](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/RenderSystemMixin.java:28`） |
| Minecraft.runTick / RenderSystem.flipFrame | beginFrame，移除部分原版 clear/blit，swap 位置执行 endFrame。 | [MinecraftMixin.java:22](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/frame/MinecraftMixin.java:22`）；[RenderSystemMixin.java:12](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/frame/RenderSystemMixin.java:12`） |
| LevelRenderer | 自有 WorldRenderer；替换 cullTerrain、prepareChunkRenders、setSectionDirty 和区块 layer draw，并处理方块实体。 | [LevelRendererMixin.java:46](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/chunk/LevelRendererMixin.java:46`） |
| ClientChunkCache / ClientPacketListener | 按区块数据、光照到达和卸载维护 ChunkStatusMap。 | [ClientChunkCacheM.java:24](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/chunk/ClientChunkCacheM.java:24`）；[ClientPacketListenerM.java:16](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/chunk/ClientPacketListenerM.java:16`） |
| MainTarget / RenderTarget | 主目标接到自有 main pass，改写 blit。 | [MainTargetMixin.java:23](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/target/MainTargetMixin.java:23`）；[RenderTargetMixin.java:28](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/target/RenderTargetMixin.java:28`） |
| ShaderManager.apply | 追加编译自有 render pipelines。 | [ShaderManagerM.java:21](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/shader/ShaderManagerM.java:21`） |
| GlStateManager | 把纹理、blend、viewport、scissor 等操作转入 Vulkan 状态管理。 | [GlStateManagerM.java:29](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/render/GlStateManagerM.java:29`） |
| LWJGL GL11 / GL14 / GL15 / GL30 | 额外兼容层；这些目标不是 MC 类。 | [GL11M.java:16](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/mixin/compatibility/gl/GL11M.java:16`） |

Vulkan 提交与呈现可从 [Renderer](https://github.com/xCollateral/VulkanMod)（`src/main/java/net/vulkanmod/vulkan/Renderer.java:384`） 继续阅读。完整清单见 [Mixin 配置](https://github.com/xCollateral/VulkanMod)（`src/main/resources/vulkanmod.mixins.json:9`）。

配置中的 SectionRenderDispatcherM 目前只有注释占位，不能据其名字认为它已经重写原版 dispatcher。

## C2ME native math

此处只调查 c2me-opts-natives-math 模块，不代表整个 C2ME 的修改范围。该模块实现 native 库分发、FFM 绑定和数学计算，不接管世界渲染。

| 目标类 | 修改行为 | 源码 |
| --- | --- | --- |
| PerlinNoiseSampler / LegacyPerlinNoiseSampler | 添加批量采样扩展，区域和输出 buffer 进入 native。 | [MixinPerlinNoiseSampler.java:58](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinPerlinNoiseSampler.java:58`）；[MixinLegacyPerlinNoiseSampler.java:61](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinLegacyPerlinNoiseSampler.java:61`） |
| LatticedNoiseSampler / SampleBuffer | 缓存 permutation 和采样数组的 MemorySegment 视图。 | [MixinLatticedNoiseSampler.java:37](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinLatticedNoiseSampler.java:37`）；[MixinSampleBuffer.java:49](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinSampleBuffer.java:49`） |
| CompositeNoiseSampler.Builder | Overwrite component/components，展开嵌套组合。 | [MixinCompositeNoiseSamplerBuilder.java:48](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinCompositeNoiseSamplerBuilder.java:48`） |
| BiomeAccess / 末地外岛 sampler | 覆盖对应采样方法，调用 native。 | [MixinBiomeAccess.java:49](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinBiomeAccess.java:49`）；[MixinEndOuterIslandsDensityFunctionSampler.java:70](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/mixin/MixinEndOuterIslandsDensityFunctionSampler.java:70`） |
| 加载和绑定 | 按 OS/架构加载 native 库、检测 ISA；FunctionDescriptor 描述 ABI，invokeExact 调用。 | [NativeLoader.java:85](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/NativeLoader.java:85`）；[BindingsTemplate.java:66](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/BindingsTemplate.java:66`）；[Bindings.java:38](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/Bindings.java:38`） |

此外，组合噪声与位移噪声 sampler 提供批量采样扩展。完整启用列表见 [模块 Mixin 配置](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/resources/c2me-opts-natives-math.mixins.json:6`）。

源码中的 MixinAquiferSamplerImpl、MixinMultiNoiseUtilEntries 没有列入这份配置。native 模块是否可用还受配置与库加载结果控制，见 [模块入口](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/ModuleEntryPoint.java:28`）。不能把目录中的全部实现都当作默认启用行为。

## 构建与加载参考

以下来源均固定到已调查提交，描述其加载和构建机制，不表示 Raynium 要照搬其目录或全局状态。

| 项目 | 构建和打包 | Java 调用与加载 |
| --- | --- | --- |
| RTX Remix Minecraft | Gradle 的 `compileNativeWin64` 调批处理生成 bridge DLL；`packageJar` 加入按架构组织的 native payload | 解包 native 目录；`System.load` 加载 `remixbridge.dll`，Java `native` 方法进入 JNI |
| Radiance | Java 编译以 `javac -h` 生成 JNI 头；Gradle 消费已安装到 resources 的 native 库，并检查变体 | 从资源复制 `core.dll` 到游戏目录，随后 `System.load`；Java proxy 中声明 native 方法 |
| C2ME native math | Gradle CMake 插件按 target 构建，把平台名放进资源文件名 | 解包到文件，通过 `SymbolLookup.libraryLookup`、`Linker.downcallHandle` 使用 FFM |
| wgpu-mc | Gradle 调 Cargo，复制 DLL 与 C 头，使用 jextract 生成 Java 绑定 | 从 JAR 资源复制 DLL 并 `System.load`；当前代码中可见 jextract 的 `WM.*` 调用，也保留 JNI 声明，不能仅凭 DLL 名判定全项目都是 JNI |

具体源码入口：

- Remix：[native 构建任务 451–460](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`build.gradle:451–460`）、[分发打包 489–546](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`build.gradle:489–546`）、[加载 bridge 114–134](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`rtxremix-java/src/com/nvidia/rtxremix/RemixAPI.java:114–134`）、[JNI 声明](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（`rtxremix-java/src/com/nvidia/rtxremix/RemixBridge.java:28–57`）。
- Radiance：[JNI 头生成 204–208](https://github.com/Minecraft-Radiance/Radiance)（`build.gradle:204–208`）、[资源处理 301–308](https://github.com/Minecraft-Radiance/Radiance)（`build.gradle:301–308`）、[DLL 复制与加载 61–86](https://github.com/Minecraft-Radiance/Radiance)（`src/main/java/com/radiance/client/RadianceClient.java:61–86`）。
- C2ME：[CMake 和资源任务](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/build.gradle`）、[FFM 加载 85–104](https://github.com/RelativityMC/C2ME-fabric)（`c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/NativeLoader.java:85–104`）。
- wgpu-mc：[Cargo 与文件复制任务](https://github.com/wgpu-mc/wgpu-mc)（`rust/build.gradle`）、[Java 资源与 jextract 配置](https://github.com/wgpu-mc/wgpu-mc)（`fabric/build.gradle:35–65`）、[DLL 加载](https://github.com/wgpu-mc/wgpu-mc)（`fabric/src/main/java/dev/birb/wgpu/rust/WgpuNative.java:35–48`）。

Raynium 当前 FFM、CMake 和 payload 的具体契约见[互操作与加载](interop.md)；调查后的选型与阅读取舍见[思考历史](../history/2026-10-26-rendering-design-notes.md#阅读与调查取舍)。
