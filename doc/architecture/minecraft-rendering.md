> - 适用: 定位 MC 26.3 原版渲染及与绘制耦合的维护逻辑
> - 权威: 本文只记录 MC 26.3 源码事实；Raynium 当前实现归 Minecraft 接入文档
> - 锚点: `gradle.properties`, `build.gradle`, `doc/guide/dev-environment.md`

# Minecraft 26.3 渲染流程

基线为 Raynium 使用 Loom 生成的 Minecraft 26.3 源码。Raynium 当前能力见[架构地图](overview.md)，已有模组调查版本的注入点见[接入调研](integration-references.md)。

## 版本和源码位置

26.3 的 GPU 抽象主要位于 `com.mojang.renderpearl`。部分窗口、状态入口和帧图设施仍位于 `com.mojang.blaze3d`。窗口实现使用 SDL；不能直接沿用旧版模组的 GLFW 句柄和注入描述符。

调查时使用的本机解包目录为 `.gradle/mc-source-26.3/client/` 和 `common/`。它们是忽略提交的阅读缓存；在其他机器上按 [开发环境](../guide/dev-environment.md#阅读-minecraft-源码) 生成并附加对应版本源码。下面行号对应 2026-10-26 调查时生成的源码，重新生成后优先通过类和方法定位。

## 初始化

`Minecraft` 构造过程中从 `PreferredGraphicsApi.getBackendsToTry()` 取得后端候选，创建窗口和设备，安装到 `RenderSystem`，然后通过设备创建窗口 surface。

| 入口 | 当前行号 | 职责 |
| --- | --- | --- |
| `Minecraft.java` | 488 | 后端候选选择 |
| 同上 | 528 | `RenderSystem.initRenderer(device)` |
| 同上 | 552 | `device.createSurface(window.handle(), ...)` |
| `PreferredGraphicsApi.java` | 32 | 返回 OpenGL/Vulkan 后端候选 |
| `GpuBackend.java` | 类入口 | 库加载/卸载、窗口创建、设备创建 |
| `Window.java` | 类入口 | SDL 窗口与事件相关操作 |

`PreferredGraphicsApi.DEFAULT` 和 `OPENGL` 均先尝试 OpenGL，再尝试 Vulkan；`VULKAN` 的尝试顺序相反。实际设备由成功创建的后端决定。

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
| `Minecraft.renderFrame` | 1292 | 一帧调度；1353 extract，1376 render，1415 submit，1418 present |
| `GameRenderer.extract` | 436 | 准备帧状态 |
| 同文件 `render` | 463 | 世界、手持物、效果和 GUI 的组织；505 GUI 绘制 |
| 同文件 `renderLevel` | 628 | 世界渲染调用边界 |
| `LevelExtractor.extract` | 108 | 提取世界相机、实体、方块实体、天空、天气、粒子等状态 |
| `LevelRenderer.render` | 185 | 世界帧图及与渲染耦合的维护 |

## 世界渲染和维护工作

`LevelRenderer.render` 从 208 行附近建立 FrameGraph，安排清理、天空、主场景及相关特性；世界渲染存在不同透明路径，不能假定只有一个固定的不透明/透明 pass 序列。

该方法后部还执行：

- 297 行附近的 `compileSections`；
- 302 行附近的地形上传，最终调用 `SectionRenderDispatcher.uploadTerrainBuffersToGpu`；
- 与可见性、区块状态和加载完成相关的维护。

世界维护代码与 GPU 绘制位于同一方法中，方法整体取消也会跳过这些维护调用。

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

关键入口是 `SectionCompiler.compile:53` 和 `SectionRenderDispatcher.uploadTerrainBuffersToGpu:141`。

`SectionCompiler.compile` 产生 `Results` 与各层 `MeshData`。`SectionRenderDispatcher.CompileTask.doTask` 消费结果、构造 `CompiledSectionMesh`、进入 UberBuffer 并关闭 MeshData。`DefaultVertexFormat.BLOCK` 包含 Position、Color、UV0、UV2，没有 Normal 属性。

## 动态对象通路

实体、方块实体等先形成渲染状态，再通过提交节点形成绘制输入，之后由 feature dispatcher 组织几何和绘制工作：

```text
状态提取
  → 各 renderer 提交节点
  → SubmitNodeStorage
  → FeatureRenderDispatcher.prepareFrame
  → 几何准备、分组、上传与绘制
```

关键入口为 `FeatureRenderDispatcher.prepareFrame:58`。

`ModelFeatureRenderer.prepareModel` 执行 pose 设置、UV/decal 包装、`model.setupAnim(state)` 和 `model.renderToBuffer(...)`。`FeatureRenderDispatcher.prepareFrame` 还包含 staged vertex buffer 上传，`RenderTypeFeatureRenderer.Group` 会调用 `RenderType.prepare()`；这些不是纯 CPU 数据提取入口。

## RenderState 生命周期

`GameRenderState` 与 `LevelRenderState` 是可变并会复用的数据；后者的 `reset` 位于 36 行。

当前 `renderFrame` 中 extract 和 render 按顺序调用。`LevelRenderer.submitFeatures` 在提交后清空实体和方块实体 RenderState 列表；这些对象和列表不是跨帧不可变快照。Raynium 当前没有接收这些对象的 native 接口。

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

源码分别位于 `com/mojang/renderpearl/api/device/`、`frontend/`、`backend/api/`。具体可从 `FrontendGpuDevice` 和 `PipelineBuilder` 开始。

前端 shader 流程涉及 GLSL、SPIR-V、反射与绑定处理；pipeline 构建包含 executor 工作和后续 `finishCompile`。

MC 的 uniform 类别含 combined image sampler。Raynium 没有自定义 RenderPearl backend。

## 纹理、主目标与最终输出

以下是 2026-10-26 RT 调查核对的 MC 26.3 API 行为，不表示 Raynium 已有纹理传输或叠加实现。

| 类/方法 | 源码行为 |
| --- | --- |
| `MainTarget` | 颜色附件为 `GpuFormat.RGBA8_UNORM`，深度附件为 `D32_FLOAT` |
| `GameRenderer.render()` | 完成世界、手持物、后处理和 GUI；返回后 `Minecraft.renderFrame` 读取 main target 颜色 view |
| `Minecraft.renderFrame` | 在 `windowSurface.isAcquired()` 条件内调用 `blitFromTexture`，随后统一 submit/present |
| `GpuSurface` / `FrontendGpuSurface` | 公共接口不暴露 acquired image；每次 acquire 后第二次 blit 会报错，present 要求已经 blit |
| `GpuDevice` | 提供创建纹理/view 的接口，没有公共外部图像导入入口 |
| `FrontendGpuDevice.createCommandEncoder()` | 返回设备持有的同一个前端 encoder |
| `CommandEncoder.writeToTexture` | 接受 `NativeImage` 或 ByteBuffer；ByteBuffer 入口按宽高和格式读取，没有源 row-pitch 参数 |
| `DynamicTexture` | 使用 RGBA8_UNORM、COPY_DST 和 TEXTURE_BINDING，`upload()` 调用 `writeToTexture` |
| `GlCommandEncoder.writeToTexture` | 从客户端字节执行纹理上传 |
| `VulkanCommandEncoder.writeToTexture` | 先复制到 transient staging，再记录 buffer-to-image copy |
| `RenderPassDescriptor` | 颜色附件可指定空 clearValue；render area 与 scissor 限制写入范围 |
| GL/Vulkan render pass | viewport 覆盖整个附件，render area 不把全屏纹理自动缩放到局部矩形 |

原版 `Screenshot.grab(Minecraft, ...)` 使用 main render target，帧末 Tracy capture 也读取该目标。
`GameRenderer` 内部的截图发生在最终 surface blit 之前；不同读取点对应不同阶段的目标内容。

## 动画纹理、光照与 GUI

`TextureAtlas.uploadAnimationFrames` 开启 RenderPass，由 `SpriteContents.AnimationState.drawToAtlas` 绘制或插值动画帧；动画图集更新不只经过 `writeToTexture`。
`Lightmap.render` 执行 GPU pass。`GameRenderer.renderLevel` 在世界之后调用 `render3dHud`，手持物使用独立投影和深度处理。

`GuiRenderer.addElementToMesh` 取得 pipeline、纹理、scissor 并调用 `GuiElementRenderState.buildVertices`。
`GuiRenderer.prepare` 处理 picture-in-picture、物品 atlas 与文字，再区分 BEFORE_BLUR / AFTER_BLUR；原版 GUI 包含离屏预览与模糊边界。

由上述事实推演的接入候选和显示方案见[思考历史](../history/2026-10-26-rendering-design-notes.md#当时的-minecraft-接入草案)。
