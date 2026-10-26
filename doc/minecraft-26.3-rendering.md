# Minecraft 26.3 渲染流程

基线为 Raynium 使用 Loom 生成的 Minecraft 26.3 源码。后续 Raynium/native 的职责划分见 [渲染接入设计](rendering-design.md)，已有模组的旧版注入点见 [接入调研](native-mod-research.md)。

## 版本和源码位置

26.3 的 GPU 抽象主要位于 `com.mojang.renderpearl`。部分窗口、状态入口和帧图设施仍位于 `com.mojang.blaze3d`。窗口实现使用 SDL；不能直接沿用旧版模组的 GLFW 句柄和注入描述符。

本机解包源码根目录为 `.gradle/mc-source-26.3/client/` 和 `common/`。它们是忽略提交的阅读缓存；在其他机器上按 [开发环境](../README.md) 生成并附加对应版本源码。下面行号对应本机当前生成结果，重新生成后优先通过类和方法定位。

## 初始化

`Minecraft` 构造过程中从 `PreferredGraphicsApi.getBackendsToTry()` 取得后端候选，创建窗口和设备，安装到 `RenderSystem`，然后通过设备创建窗口 surface。

| 入口 | 当前行号 | 职责 |
| --- | --- | --- |
| [Minecraft.java](../.gradle/mc-source-26.3/client/net/minecraft/client/Minecraft.java) | 488 | 后端候选选择 |
| 同上 | 528 | `RenderSystem.initRenderer(device)` |
| 同上 | 552 | `device.createSurface(window.handle(), ...)` |
| [PreferredGraphicsApi.java](../.gradle/mc-source-26.3/client/net/minecraft/client/PreferredGraphicsApi.java) | 32 | 返回 OpenGL/Vulkan 后端候选 |
| [GpuBackend.java](../.gradle/mc-source-26.3/client/com/mojang/renderpearl/api/device/GpuBackend.java) | 类入口 | 库加载/卸载、窗口创建、设备创建 |
| [Window.java](../.gradle/mc-source-26.3/client/com/mojang/blaze3d/platform/Window.java) | 类入口 | SDL 窗口与事件相关操作 |

如果以后选择自定义 RenderPearl backend，必须在依赖原设备的 GPU 资源建立之前接入。当前场景数据接入方向不要求先完整实现该 backend。

## 一帧的主流程

```text
Minecraft.renderFrame
  配置 surface / 获取窗口图像
  GameRenderer.extract
    相机、世界、手持物、UI 等渲染状态准备
  处理待执行 GPU 工作
  GameRenderer.render
    光照贴图
    GameRenderer.renderLevel
      LevelRenderer.render
        动态对象准备
        世界 FrameGraph
        区块重建与上传等维护
    手持物、画面效果等
    GuiRenderer.render
  最终画面输出到 surface
  CommandEncoder.submit
  GpuSurface.present
  帧结束处理
```

| 类与方法 | 行号 | 阅读重点 |
| --- | --- | --- |
| [Minecraft.renderFrame](../.gradle/mc-source-26.3/client/net/minecraft/client/Minecraft.java) | 1292 | 一帧调度；1353 extract，1376 render，1415 submit，1418 present |
| [GameRenderer.extract](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/GameRenderer.java) | 436 | 准备帧状态 |
| 同文件 `render` | 463 | 世界、手持物、效果和 GUI 的组织；505 GUI 绘制 |
| 同文件 `renderLevel` | 628 | 世界渲染调用边界 |
| [LevelExtractor.extract](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/extract/LevelExtractor.java) | 108 | 提取世界相机、实体、方块实体、天空、天气、粒子等状态 |
| [LevelRenderer.render](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/LevelRenderer.java) | 185 | 世界帧图及与渲染耦合的维护 |

## 世界渲染和维护工作

`LevelRenderer.render` 从 208 行附近建立 FrameGraph，安排清理、天空、主场景及相关特性；世界渲染存在不同透明路径，不能假定只有一个固定的不透明/透明 pass 序列。

该方法后部还执行：

- 297 行附近的 `compileSections`；
- 302 行附近的地形上传，最终调用 `SectionRenderDispatcher.uploadTerrainBuffersToGpu`；
- 与可见性、区块状态和加载完成相关的维护。

因此，对整个方法做 HEAD cancellation 会一并跳过这些工作。采用 native 世界管线时，需要分别明确哪些维护继续保留、哪些由 Java 适配层接替、哪些原版 GPU 工作停止执行。

`LevelExtractor` 与 `LevelRenderer` 的可见性、区块和渲染状态仍有关联，也不能把它视为完全独立的通用场景导出器。

## 地形通路

```text
区块标脏与重建调度
  → SectionCompiler.compile
  → Results / MeshData
  → SectionRenderDispatcher
  → 地形 GPU buffer 上传
  → 世界地形绘制
```

关键入口是 [SectionCompiler.compile:53](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/chunk/SectionCompiler.java) 和 [SectionRenderDispatcher.uploadTerrainBuffersToGpu:141](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/chunk/SectionRenderDispatcher.java)。

Raynium 可以在 Java 网格生成出口转换或直接生成通用网格数据，并追踪其更新和删除。是否保留原版网格布局、调度器和可见性结构尚未确定。第三方旧版代码中的 `SectionBuilder` 不能直接套到本版本。

## 动态对象通路

实体、方块实体等先形成渲染状态，再通过提交节点形成绘制输入，之后由 feature dispatcher 组织几何和绘制工作：

```text
状态提取
  → 各 renderer 提交节点
  → SubmitNodeStorage
  → FeatureRenderDispatcher.prepareFrame
  → 几何准备、分组、上传与绘制
```

关键入口为 [FeatureRenderDispatcher.prepareFrame:58](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/feature/FeatureRenderDispatcher.java)。

捕获位置决定 Java 侧保留多少模型语义：较早捕获可以保留模型部件与变换，较晚捕获更接近通用网格和材质数据。需要结合模组兼容性、动态几何规模以及 runtime 输入契约选择。

## RenderState 生命周期

[GameRenderState](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/state/GameRenderState.java) 与 [LevelRenderState](../.gradle/mc-source-26.3/client/net/minecraft/client/renderer/state/level/LevelRenderState.java) 是可变并会复用的数据；后者的 `reset` 位于 36 行。

当前 `renderFrame` 中 extract 和 render 按顺序调用，这不意味着其对象可以被 native 异步永久持有。异步提交需要复制、稳定快照或明确的所有权交接。具体机制见 [设计中的所有权问题](rendering-design.md#所有权与执行约束)。

## RenderPearl 边界

| 类型 | 职责 |
| --- | --- |
| `GpuBackend` | 窗口和设备启动 |
| `FrontendGpuDevice` | 公共设备接口的前端组织、编码器与 pipeline 构建 |
| `GpuDeviceBackend` | 底层设备与资源实现 |
| `CommandEncoderBackend` | 命令编码实现 |
| `RenderPassBackend` | pass 内状态与绘制 |
| `GpuSurfaceBackend` | 窗口图像和呈现 |
| `PipelineBuilder` | shader 编译、反射和 pipeline 构建组织 |

源码分别位于 `com/mojang/renderpearl/api/device/`、`frontend/`、`backend/api/`。具体可从 [FrontendGpuDevice](../.gradle/mc-source-26.3/client/com/mojang/renderpearl/frontend/FrontendGpuDevice.java) 和 [PipelineBuilder](../.gradle/mc-source-26.3/client/com/mojang/renderpearl/frontend/shaders/PipelineBuilder.java) 开始。

前端 shader 流程涉及 GLSL、SPIR-V、反射与绑定处理；pipeline 构建包含 executor 工作和后续 `finishCompile`。若适配到 RadRay，需要把这些 CPU 调用整理为符合 RHI 串行契约的执行顺序。

MC 的 uniform 类别含 combined image sampler，而当前 RadRay RHI 的纹理和 sampler 分开描述。完整 backend 适配还需要逐项处理绑定、拓扑、实例步进、格式与设备能力等差异，不能只包一层同名方法。

## 对当前设计的意义

当前目标是 Java 准备通用场景输入、native 执行管线。26.3 的工作应优先围绕资源更新、地形网格出口、动态场景收集、原版 draw 抑制和生命周期展开。

UI 与呈现仍是独立待定问题。现有模组采用的 GL/Vulkan 共享图像路线需要外部资源互操作支持；完整 RenderPearl 适配则需要覆盖原版 GPU 接口和 shader 契约。两者不能从“native 使用 Vulkan”这一点直接推出。

