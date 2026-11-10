> - 适用: 执行或评审「切断 Minecraft 26.3 原版渲染调用路径、由 Raynium 接管出图」的工作；本文按可直接执行的步骤、验收条件和风险规避编写
> - 权威: 本文为历史计划记录，不代表当前实现契约；当前契约以 [地形预览](../architecture/terrain-preview.md)、[Minecraft 渲染](../architecture/minecraft-rendering.md)、[Minecraft 接入](../architecture/minecraft-integration.md)、[互操作与加载](../architecture/interop.md) 和 [Native runtime](../architecture/native-runtime.md) 为准
> - 锚点: `src/client/java/dev/raynium/client/mixin/*.java`, `src/client/resources/raynium.client.mixins.json`, `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/java/dev/raynium/bridge/*.java`, `native/include/raynium/bridge.h`, `tools/native.py`, `build.gradle`

# 原版渲染切断与 Raynium 完全接管：执行计划

- 日期：2026-11-10；2026-11-11 在提交前修订为可执行版本
- 状态：方向和阶段划分已由用户确认，待实施。本文写入时没有修改任何源码，没有启动过游戏，没有运行过任何接管实验
- 范围：Minecraft 26.3 客户端的启动、帧循环、渲染对象构造、资源加载、设备信息读取、窗口事件与关闭路径；Raynium 的 Mixin 层、桥接层、C ABI 与构建脚本
- 来源：本会话内的静态阅读，材料为 Loom 生成的 Minecraft 26.3 客户端源码（阅读方式见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)）、当前工作树源码、锁定的 RadRay 源码、Fabric Loader 0.20.1 与 Fabric API 0.165.0+26.3 的 jar 内容。会话内容未提交
- 修订说明（2026-11-11）：相对 11-10 的初稿有两处方案变化，都由助手提出，尚待用户确认。其一，阶段 1 不再删除右上角预览路径，改为开关关闭时保持现状，删除推迟到阶段 2。其二，阶段 1 内部顺序从「按 A–F 分组」改为「按启动执行顺序的检查点」；构造期、资源期、设备信息三组切点仍全部在阶段 1 内完成

## 0. 读法

- 执行者先读第 1、2、3 节，再按第 4 节的步骤顺序执行。每一步做完先过该步的验收，再进入下一步。
- 遇到 2.3 列出的情况，停止并向用户报告，不要自行绕过。
- 【已读】表示结论来自读到的源码或 jar 内容；【未验证】表示推断，执行时要先核实。两者都没有经过运行验证。
- Minecraft 行号取自本机 Loom 生成的 26.3 源码，只作定位辅助，以类名、方法名和调用目标为准。
- `Takeover`、`TakeoverResources` 等 Raynium 侧新类名是建议名，可以调整；Minecraft 与 RadRay 的名称是实际名称。

## 1. 目标与已定决定

### 1.1 用户决定（按会话顺序）

1. 不采用「逐 feature 渐进替换」，因为原版路径同时运行会干扰 benchmark。
2. 一步到位接管：启用后完全阻止 Minecraft 的图形初始化，由 Raynium 出图。
3. 不把 Raynium 实现成 RenderPearl 的一个后端。
4. 不要求删除对 RenderPearl 类型的引用，只要切断调用路径。
5. 第一步切断原版渲染调用路径，后续逐项用 Raynium 接入。
6. 不使用临时的空 `GpuDevice` 脚手架。原话是「先做完CDE再继续下一步」；本文理解为构造期、资源期、设备信息三组切点必须在阶段 1 内逐点切完，阶段 1 整体达标后才进入阶段 2。

这一方向与 [2026-10-26 渲染方向记录](2026-10-26-rendering-design-notes.md) 中「从窗口接管改为保留原版」的取舍相反。

### 1.2 阶段 1 的目标

- 开关开启时：Minecraft 不创建任何图形设备，窗口由 Raynium 交换链呈现纯色。游戏逻辑、区块编译、遮挡、GUI 与世界的状态抽取照常运行；可以进入世界、重载资源、改变窗口、正常退出。
- 开关关闭时：行为与当前完全一致，即原版渲染加右上角预览。
- 阶段 1 不画任何内容，画面从阶段 2 开始逐项接入。

### 1.3 现状【已读】

- Raynium 当前把 SOLID 地形画进四分之一尺寸的共享 RT，再拷到窗口右上角；原版渲染照常运行，并且要求 Minecraft 自身使用 Vulkan。
- 链路为 Java 25 FFM → C ABI 10（13 个导出）→ `rayniumruntime.dll` → RadRay。ABI 里没有交换链、呈现或设备能力查询。
- runtime 在 `CLIENT_STARTED` 创建，适配器按 `PreviewOverlay.hostGpu` 取得的 Minecraft Vulkan 设备身份匹配；`NativeBindings.createRuntime` 在 host 为 null 时不做匹配。
- RadRay RHI 有 `Device::CreateSwapChain`，`SwapChainDescriptor.NativeHandler` 在 D3D12 与 Vulkan（Windows）后端都按 HWND 使用；RadRay runtime 模块没有使用交换链。
- Fabric Loader 0.20.1 内嵌 `mixinextras-fabric-0.5.5`，其中有 `WrapOperation`、`WrapWithCondition`、`ModifyExpressionValue`、`ModifyReturnValue`、`Expression`、`Local`。
- `lwjgl-sdl` 3.4.3 提供 `SDLVideo.SDL_GetWindowProperties`、`SDLProperties.SDL_GetPointerProperty` 和属性名常量 `SDL_PROP_WINDOW_WIN32_HWND_POINTER`。

## 2. 执行规则

### 2.1 硬约束

1. 不安装任何 `GpuDevice` 实现，`RenderSystem.DEVICE` 保持 null。不实现 `CommandEncoder`、`RenderPass`、`GpuSurface`、`GpuBackend`。
2. 允许实现的 RenderPearl 接口只有 `GpuTexture`、`GpuTextureView`、`GpuSampler`、`GpuBuffer` 四个，并且只作身份句柄（见 3.3）。
3. 不捕获 `IllegalStateException` 或 `NullPointerException` 来越过绊线；看到绊线就按调用栈补切点。
4. 不修改 `external/RadRay/`。它是脚本管理的检出，`tools/fetch_radray.py` 发现工作树有改动会停止。
5. 不修改、不删除现有测试来让构建通过。开关关闭时的行为不得变化。
6. 未获用户指示时不提交、不建分支、不写 `doc/history/`。当前事实文档只在行为落地的步骤同步（S1 与 S8）。
7. 开工前核对版本：`gradle.properties` 中 Minecraft 26.3、Fabric Loader 0.20.1、Fabric API 0.165.0+26.3；`python tools/fetch_radray.py status` 正常。任一项不符，本文的行号、注入目标和 jar 结论都不能直接使用。

### 2.2 Mixin 技术规则

1. 接管用的 Mixin 只放在 `dev.raynium.client.mixin.takeover` 包，由 Mixin 配置插件在开关开启时整体应用。Mixin 内部不再判断开关。不得把接管行为写进两种模式共用的 Mixin。
2. 首选 MixinExtras 的 `@WrapOperation`、`@WrapWithCondition`、`@ModifyExpressionValue`、`@ModifyReturnValue`，`@Inject` 也可以用。禁止 `@Overwrite`。`@Redirect` 只在确认同一条指令没有其它 Mixin 目标时使用。
3. 所有注入保持 `require = 1`（配置里已有 `defaultRequire: 1`）。禁止 `require = 0` 和 `expect = 0`，目标漂移必须在启动时报错。
4. 定位以「类 + 方法 + 调用目标描述符」为准。lambda 和匿名类先用 `javap -p -c` 在 Loom 产出的 Minecraft jar 上查出真实的合成名称。
5. 整方法替换（在方法头取消并重写）只在两个条件都满足时使用：方法里除设备操作外没有其它副作用，或副作用已在替换里原样重做；并且没有其它模组的 Mixin 以该方法为目标（Fabric API 的已知目标见第 8 条，其余用字节码检查确认）。
6. `Minecraft.<init>` 里 `if (device == null)` 的改写：首选 MixinExtras 表达式（`@Definition` 绑定局部变量，`@Expression` 写 `device == null`，配 `@ModifyExpressionValue`）。不可行时，改用配置插件的 `postApply` 对该方法做精确匹配的字节码修改，匹配不到就让启动失败。两者都不可行，按 2.3 停止。【未验证：两种手段在本环境是否可用】
7. 接收者为 null 的调用：3.3 的配方会让处理函数收到值为 null 的接收者参数。处理函数不得解引用它，不得调用 `original`。S2 的第 6 项是这一机制的第一次实测。【未验证】
8. Fabric API 0.165.0+26.3 在相关类上的已知注入目标【已读，来自 jar 字节码】：
   - `fabric-rendering-v1`：`LevelRenderer` 的 `render`、`prepareChunkRenders`、`prepareChunkRendersIndirect`、`executeSolid`、`submitFeatures`、`executeClassicTransparency`、`executeOit`、`submitBlockOutline` 与 `lambda$addMainPass$0`；`GameRenderer` 的 `<init>`、`extract`；`GuiRenderer` 的 `<init>`、`preparePictureInPicture`、`preparePictureInPictureState`、`close`、`addElementToMesh`；`LevelExtractor` 的 `extract`、`extractBlockOutline`、`allChanged`；`FeatureRenderDispatcher` 的 `<init>`。
   - `fabric-renderer-api-v1`：`TextureAtlas.upload`（方法返回处）、`SectionCompiler.compile`、`LevelRenderer.submitBlockDestroyAnimation`。
   - `fabric-lifecycle-events-v1`：`Minecraft` 的 `tick`、`run`、`exitWorldAndClose`、`updateLevelInEngines`。
   - `fabric-screen-api-v1`：`Minecraft` 的 `<init>`、`exitWorldAndClose`、`doWorldLoad`。
   - `fabric-client-gametest-api-v1` 在 `Minecraft` 与 `Window` 上有注入，具体目标没有读。

### 2.3 停止并上报的情况

1. 只有引入 `GpuDevice`、`CommandEncoder`、`RenderPass`、`GpuSurface` 或 `GpuBackend` 的实现才能继续。
2. 需要修改 RadRay 上游源码。
3. 与其它模组的 Mixin 冲突，用 2.2 的可叠加注入方式解决不了。
4. 版本前提（2.1 第 7 条）不成立。
5. 同一步验收在三次不同思路的修正后仍不通过。
6. 需要删除现有功能或测试，或需要改变开关关闭时的行为。
7. 某一步的「预期失败点」之外出现了本文没有列出的绊线，并且该调用点不属于任何已列出的类别。

### 2.4 证据与记录

- 每步的命令、`latest.log` 副本、类加载日志和结论放在 `build/verification/takeover/<步骤>/`（忽略目录）。
- 维护一张调用点审计表 `build/verification/takeover/sites.csv`：列出 Minecraft 源码中每个 `RenderSystem.getDevice()`、`tryGetDevice()`、`getDynamicUniforms()` 调用点，标为「上游已切（写明切断它的根）」「已改写（写明 Mixin 名）」或「保留路径不可达（写明理由）」。调查时统计到 53 个文件调用 `getDevice()`【已读】。
- 汇报时区分「本次实际运行的结果」和「沿用本文的静态结论」。

## 3. 固定设计

### 3.1 总开关与 Mixin 分组

- JVM 属性 `raynium.takeover.enabled`，默认 false。Gradle 属性 `-Ptakeover=true` 传入它，并把运行目录切到 `runs/takeover/`（`.gitignore` 已忽略 `runs/`）。
- 接管运行使用独立目录的原因：开发期会反复在启动中崩溃，Minecraft 在上次启动不干净时会改写 `options.txt` 里的全屏和图形后端选项（`Minecraft.<init>` 461–485 行）【已读】，这会破坏 `run/` 里预览模式依赖的 Vulkan 设置。
- `raynium.client.mixins.json` 增加配置插件，按下表决定是否应用：

| 分组 | 成员 | 应用条件 |
| --- | --- | --- |
| 公共 | `MinecraftBenchmarkMixin`、`SectionCompilerMixin`、`SectionResultsMixin`、`CompiledSectionMeshMixin` | 总是 |
| 预览 | `MinecraftPreviewMixin`、`VulkanFeatureSetsMixin`、`VulkanCommandEncoderAccessor`、`LevelTerrainMixin`、`GameTerrainMixin`、`TerrainAtlasMixin`、`TerrainLightmapMixin` | 开关关闭 |
| 接管 | `dev.raynium.client.mixin.takeover` 包内全部 | 开关开启 |

- 配置插件在游戏类加载前运行，只能读系统属性，不能引用 Minecraft 类。
- 开关开启时没有 native payload 就让启动失败并说明原因，不回退到原版。
- 开关开启时 `TerrainCapture` 保持未激活，公共分组里的三个地形 Mixin 因此不复制顶点【已读：它们以 `TerrainCapture.active()` 为条件】。
- 阶段 1 不支持在接管模式下运行 `tools/benchmark.py`；该脚本不传 `-Ptakeover`，行为不变。

### 3.2 绊线

`RenderSystem.getDevice()` 在 `DEVICE` 为 null 时抛出 `IllegalStateException`，消息为 `Can't getDevice() before it was initialized`；`getDynamicUniforms()` 同理【已读】。任何没有切到的路径会在第一次执行时带着调用栈失败。阶段 1 的开发方式就是沿启动顺序前进：让它失败，按调用栈对照第 4 节的表补上，再前进。

### 3.3 身份句柄与改写配方

身份句柄是只记录参数、不执行任何 GPU 操作的对象，用来保持 Minecraft 对象的字段非空，使 `slice()`、`close()`、尺寸查询等 CPU 行为照常工作。每个句柄带一个唯一编号，阶段 2 起用它映射到 Raynium 资源。

| 句柄 | 实现的接口 | 记录的内容 | 特殊行为 |
| --- | --- | --- | --- |
| `TakeoverTexture` | `GpuTexture` | 标签、用途、格式、宽、高、层数、mip 数 | `close()` 置关闭标志 |
| `TakeoverTextureView` | `GpuTextureView` | 所属纹理、起始 mip、mip 数 | 同上 |
| `TakeoverSampler` | `GpuSampler` | 两个寻址模式、两个过滤模式、各向异性、最大 LOD | 同上 |
| `TakeoverBuffer` | `GpuBuffer` | 用途、字节数 | `map(...)` 抛 `IllegalStateException`，消息以 `takeover:` 开头 |

`TakeoverBuffer` 不保留创建时传入的 `ByteBuffer`，那块内存属于调用方的栈或临时分配。

| 配方 | 作用对象 | 改写结果 |
| --- | --- | --- |
| K1 创建换句柄 | 对 `createTexture`、`createTextureView`、`createSampler`、`createBuffer` 的调用（注意各有重载） | 返回对应的身份句柄 |
| K2 取设备给空 | 对 `RenderSystem.getDevice()` 的调用 | 返回 null |
| K3 编码器操作置空 | `device.createCommandEncoder()` 以及其后的 `writeToTexture`、`writeToBuffer`、`clearColorTexture`、`copyBufferToTexture` 等 | 前者返回 null，后者不执行。阶段 2 在这里接收像素 |
| K4 读设备信息 | 对 `getDeviceInfo()` 的调用 | 返回 3.4 的 `DeviceInfo` |
| K5 整方法守卫 | 整个方法 | 在方法头取消，按 2.2 第 5 条的条件使用 |
| K6 调用点跳过 | 某一次方法调用 | 不执行该调用，其余代码照常 |

K2 的使用条件：同一方法里，保留路径上对这个局部变量的每一次使用都已被 K1、K3 或 K4 覆盖。被切断路径上的使用不处理，它们一旦被执行会以 `NullPointerException` 暴露。

### 3.4 DeviceInfo 取值

`DeviceInfo`、`DeviceLimits`、`DeviceFeatures`、`HintsAndWorkarounds` 都是 record，可以直接构造【已读】。接管模式下构造一次并缓存。

| 字段 | 取值 | 理由 |
| --- | --- | --- |
| `name`、`vendorName`、`driverInfo` | 来自新增的 ABI 设备信息查询 | 日志、F3、崩溃报告 |
| `isZZeroToOne` | true | 原版 Vulkan 后端为 true【已读】；Raynium 现有地形管线按这一约定加反向 Z 编写。改成 false 会改变 `Camera` 的剔除矩阵和 `Projection` 的结果 |
| `backendName` | `Raynium` | `GpuWarnlistManager` 只在名称等于 `OpenGL` 时匹配警告名单【已读】 |
| `timestampPeriod` | 1.0 | 阶段 1 没有计时查询 |
| `limits.maxAnisotropy` | RadRay `MaxSamplerAnisotropy` 取整，至少 1 | `Options.maxAnisotropyValue` |
| `limits.minUniformOffsetAlignment` | RadRay `CBufferOffsetAlignment`，至少 1 | `Lighting`、`TextureAtlas` 用它做取整，不能为 0 |
| `limits.maxTextureSize` | RadRay `MaxTexture2DDimension` | 图集拼接上限、窗口最大尺寸 |
| `limits.maxMemoryAllocationSize` | `Long.MAX_VALUE` | 原版 Vulkan 后端在取不到时也用这个值【已读】 |
| `limits.maxColorAttachments` | RadRay `MaxColorAttachments` | 仅被切断路径读取 |
| `limits.maxMultiDrawDirectInterleavedDrawCount`、`limits.maxDrawIndirectDrawCount` | 0 | 使 `LevelRenderer` 判定间接多重绘制不可用 |
| `features` 全部 8 项 | false | `persistentMapping` 为 false 使 `StagingBuffer.create` 选 CPU 变体；`wireframeFillMode` 为 false 使线框调试键提示不可用 |
| `hintsAndWorkarounds` 全部 4 项 | false | `writeToBufferIsSlow` 为 false 同样指向 CPU 变体 |
| `underlyingExtensions` | 空集合 | 仅日志与崩溃报告 |
| `type` | 读 `DeviceType` 枚举后选对应值 | 【未验证：枚举成员没有读】 |

### 3.5 表面调用到交换链的映射

`Minecraft.windowSurface` 保持 null。`renderFrame` 里对它的调用逐个改写，原版的「需要重新配置」状态机因此直接驱动 Raynium 交换链：

| `renderFrame` 中的调用 | 行 | 接管后的结果 |
| --- | --- | --- |
| `windowSurface.isAcquired()` | 1293 | false，使方法体执行 |
| `windowSurface.isSuboptimal()` | 1302 | 上一次呈现是否返回「需要重建」 |
| `windowSurface.supportedPresentModes()` | 1305 | 固定集合：`IMMEDIATE`、`FIFO` |
| `windowSurface.configure(config)` | 1310 | 用 `config` 的宽、高和呈现模式调用交换链重建。失败照常向上抛出，不伪装成功 |
| `windowSurface.acquireNextTexture()` | 1321 | 不执行；Raynium 在呈现时自己获取后备缓冲 |
| `windowSurface.isAcquired()` | 1394、1417 | false，跳过原版的 blit 与 present |

垂直同步开启映射到 FIFO，关闭映射到 Immediate。`DebugEntryFps`:22 读取的 `currentConfiguration()` 返回由当前交换链尺寸和模式构造的 `GpuSurface.Configuration`。崩溃报告对 `windowSurface` 已有空判断（2581 行）【已读】。

### 3.6 帧入口与关卡维护

`renderFrame` 里的更新、拾取、`gameRenderer.extract(...)`、限帧和帧率统计全部保留。两处替换：

- 1376 行 `gameRenderer.render()` 换成接管的帧入口。
- 1415 行 `RenderSystem.getDevice().createCommandEncoder().submit()` 用 K2、K3 处理，并在 `submit()` 的位置执行 Raynium 的呈现。

帧入口按顺序做：

1. 如果 `windowRenderState` 的宽高与 `mainRenderTarget` 的宽高不同，调用 `gameRenderer.resize(w, h)`。原版在 `GameRenderer.render` 开头做同样的事（467–469 行），它会经 `LevelRenderer.resize` 使遮挡图失效【已读】。目标对象的尺寸字段经 K5 保持更新（见 S3）。
2. 如果 `gameRenderState.shouldRenderLevel` 为真，执行关卡维护。

关卡维护是原本夹在 `LevelRenderer.render` 里、不属于绘制的工作，顺序与原版一致【已读】：

1. `repositionCamera(cameraState)`（原 198 行）。
2. `finalizeGizmoCollection()`（原在 `submitFeatures` 内，341 行）。渲染线程的 gizmo 收集器每帧被写入，只有这个方法会排空它；不调用会累积。
3. `compileSections(cameraState)`（原 298 行）。
4. 持有 `sectionRenderDispatcher` 的锁，调用 `uploadTerrainBuffersToGpu()`（原 300–311 行；接管后的实现见 3.7）。
5. `sectionOcclusionGraph.update(cameraState, fov, chunkLoadingRenderState)`（原 314 行）。
6. `playerCompiledSectionCallback` 的判断与调用（原 316–322 行）。

阶段 1 不调用 `submitFeatures`，`submitNodeStorage` 与 `handAndScreenSubmitNodeStorage` 因此始终为空，不需要排空。`RenderSystem.isRenderingLevel` 只在提交节点时被读取（`SubmitNodeCollection`:180），阶段 1 不设置；阶段 5 接入提交节点时要恢复。

### 3.7 地形编译与上传的替代路径

原版行为【已读】：

- 编译任务把每层网格追加到 `StagingBuffer`。写满时 `tryAppend` 返回 false，工作线程自旋等待渲染线程调用 `uploadTerrainBuffersToGpu`。
- 新网格的接纳只发生在 `RenderSection.checkSectionMesh`，它由 `UberGpuBuffer.uploadStagedAllocations` 执行的上传回调触发。
- `SectionRenderDispatcher.runTask` 捕获 `NullPointerException` 后把任务重新入队，不报错。

所以只是不调用上传会让编译线程永久自旋、网格永远不被接纳；工作线程路径上的空指针会变成静默的无限重试。

接管后的做法：保留原版的线程模型，把上传换成不需要设备的排空。

1. `StagingBuffer.create` 经 K4 选中 CPU 变体。它在构造时分配约 98 MiB 堆外内存，不触设备【已读】。
2. 在 `UberGpuBuffer` 上新增一个排空方法，对每个待上传项：如果它的键不在「已跳过」集合里，执行它的上传回调；然后关闭该项；最后清空待上传表和「已跳过」集合。不分配堆，不调用 `uploader.copyTo`。
3. `uploadTerrainBuffersToGpu` 内：取设备用 K2；`device.createCommandEncoder()` 返回 null；两处 `uploadStagedAllocations(...)` 调用改为调用上面的排空方法并返回 false。方法里原有的 `try (uploader = stagingBuffer.startUploading(...))` 保留，`Uploader.close()` 会在所有句柄关闭后把暂存区写指针归零【已读】。
4. 这个方法的两个调用方都继续有效：3.6 的关卡维护，以及 `addSectionBuffersToUberBuffer` 在渲染线程上追加失败时的直接调用（395 行）。

已知代价：每个网格仍被复制进暂存区一次，没有人读取。阶段 1 接受这一开销；阶段 2 接入地形时决定是否跳过这次复制。

### 3.8 关闭顺序

- Fabric 的 `CLIENT_STOPPING` 在 `Minecraft.exitWorldAndClose` 开头触发，早于 `Minecraft.close()`【已读】。此后是否还会调用 `renderFrame` 没有核实，所以接管模式下不在这个事件里释放 native 对象。
- 在 `Minecraft.close()` 的 `windowSurface.close()`（1154 行）处按顺序关闭：交换链 → runtime → native 环境。这与原版「所有渲染对象关闭之后才关闭表面」的位置一致。
- `close()` 里在它之前的各项依靠身份句柄自然安全：`gameRenderer.close()`、`levelRenderer.close()`、`shaderManager.close()`、纹理管理器等都只是对句柄调用 `close()`【已读各方法体】。`timerQuery.close()` 见 S4。
- 其后的 `RenderSystem.shutdownRenderer()` 对管线缓存、动态数据和设备都有空判断，但对采样器缓存没有，所以采样器缓存必须在 S2 填满 32 个句柄【已读】。

### 3.9 诊断与自动化入口

接管模式提供以下入口，全部只在开关开启时存在：

| JVM 属性 | Gradle 属性 | 作用 |
| --- | --- | --- |
| `raynium.takeover.statsSeconds` | — | 统计日志间隔，默认 5，0 为关闭 |
| `raynium.takeover.exitAfterSeconds` | `-PtakeoverExitAfter=<秒>` | 初次资源加载完成后计时，到时在客户端线程调用 `Minecraft.stop()` 正常退出 |
| `raynium.takeover.selftest` | `-PtakeoverSelftest=true` | 进入世界后执行 S7 的脚本化自检，然后正常退出 |
| — | `-PquickPlayWorld=<存档目录名>` | 追加启动参数 `--quickPlaySingleplayer <名称>` |
| — | `-PclassLoadLog=<绝对路径>` | 追加 `-Xlog:class+load=info:file=<路径>` |

检查点日志（logger 名 `raynium.takeover`，每个只打印一次）：

| 标记 | 打印位置 |
| --- | --- |
| `Raynium takeover CP1` | 交换链创建并呈现第一帧纯色之后，附适配器名、后端、尺寸 |
| `Raynium takeover CP2` | `GameRenderer.<init>` 返回 |
| `Raynium takeover CP3` | `LevelRenderer.<init>` 返回 |
| `Raynium takeover CP4` | `renderFrame` 第一次完成呈现 |
| `Raynium takeover CP5` | `Minecraft.onGameLoadFinished` 返回 |
| `Raynium takeover CP6` | 第一次执行关卡维护之后 |
| `Raynium takeover CP7` | native 环境关闭之后 |

统计日志一行，字段固定：`frames`（帧入口次数）、`presented`、`skipped`、`fps`、`level`（是否有世界）、`screen` 与 `overlay`（当前界面和覆盖层的类名或 none）、`visibleRenderable`（`LevelExtractor.countRenderedSections()`）、`compileQueue`（`SectionRenderDispatcher.getCompileQueueSize()`）、`heapUsedMiB`。两个取数方法都是已有的公开方法【已读】。`screen` 字段用来发现看不见的模态界面。

全程禁止出现的日志内容（各步写明的预期失败点除外）：`Can't getDevice() before it was initialized`、`Can't getDynamicUniforms() before device was initialized`、以 `takeover:` 开头的异常消息、`Shutdown failure!`、`Caught error loading resourcepacks`、`Failed to load required shader programs`、`Batching sections`。

## 4. 阶段 1 执行步骤

运行方式：

- 开关关闭：`.\gradlew.bat runClientNative`。开关开启：`.\gradlew.bat runClientNative -Ptakeover=true`，按需追加 3.9 的属性。
- 接管模式的日志在 `runs/takeover/logs/latest.log`，崩溃报告在 `runs/takeover/crash-reports/`。每次运行都要设超时，超时后结束进程并保留日志。
- S2、S3、S4 各自以一个「预期失败点」结束：到达该步的检查点之后，游戏会在下一步表格的第一项处触发绊线。失败点不在下一步的表格里，说明表格有遗漏，先把它补进审计表再处理；无法归类时按 2.3 第 7 条上报。
- 每步结束都重跑一次开关关闭的 `.\gradlew.bat build`，确认没有影响另一种模式。

### S0 基线与护栏

改动：

1. 记录基线：`.\gradlew.bat build` 与 `.\gradlew.bat nativeTest` 的结果存入证据目录。
2. 新增 Mixin 配置插件和 3.1 的分组，新增空的 `takeover` 包。
3. `RayniumClient`：开关开启时不创建 `PreviewOverlay`，不做 `PreviewHost` 转型（预览分组没有应用时这个转型会失败），也不在 `CLIENT_STARTED` 调用 `PreviewOverlay.hostGpu` 或创建 runtime（前者依赖预览分组的 accessor；runtime 改在 S2 创建）；没有 payload 时启动失败。
4. `build.gradle`：加入 3.1 与 3.9 的 Gradle 属性。
5. 准备 `runs/takeover/`：复制 `run/options.txt`，确认其中 `onboardAccessibility:false`、`tutorialStep:none`、`fullscreen:false`（`tools/benchmark.py` 给基准目录写的是同样的值【已读】）；复制一个已经在本版本打开过的存档到 `runs/takeover/saves/`。

验收：

- 开关关闭：build 与 nativeTest 通过；`runClientNative` 的日志含 `Raynium shared preview ready:` 和 `Raynium SOLID terrain inputs ready`。
- 开关开启：此时接管分组是空的，原版渲染照常，只是没有预览。日志写明开关状态和已应用的分组；带 `-PquickPlayWorld` 能直接进入世界并看到原版画面；带 `-PtakeoverExitAfter=30` 能正常退出，退出码为 0。
- 这一步的意义是在画面变空之前，用看得见的画面确认进世界流程、存档和选项文件都没有问题。

### S1 native 交换链与设备信息（ABI 11）

这一步不依赖 Minecraft，可以独立验收。

改动：

1. `bridge.h` 把 `RN_ABI_VERSION` 改为 11，新增 5 个导出：

| 导出 | 语义 |
| --- | --- |
| `rn_query_device_info` | 填写适配器名、厂商、后端，以及 3.4 需要的四个限制值 |
| `rn_create_swapchain` | 输入 HWND、宽、高、呈现模式，在 runtime 上创建交换链 |
| `rn_resize_swapchain` | 等待在途工作完成后按新的宽、高、呈现模式重建 |
| `rn_present_swapchain` | 获取后备缓冲、清成给定颜色、提交、呈现；通过输出参数返回「已呈现」「已跳过」「需要重建」之一 |
| `rn_destroy_swapchain` | 等待在途工作完成后销毁 |

2. 约定：返回值只用 `RN_STATUS_OK` 与 `RN_STATUS_FAILED`，原因写入诊断缓冲；交换链是 runtime 的子对象，存活时 runtime 拒绝销毁；所有调用在环境初始化线程；C++ 侧用 `static Create`，不做二段式构造；格式 RGBA8_UNORM，后备缓冲 3 张；窗口最小化或尺寸为零时返回「已跳过」，不算失败。
3. 这是阶段 1 专用的形态，阶段 2 把呈现并入 `rn_render_frame` 时会再次变更 ABI。现有的共享 RT、共享纹理导出及其测试保持不动。
4. Java 侧：`NativeBindings`、`NativeManifest.ABI_VERSION`、新增 `NativeSwapchain`、`NativeRuntime` 增加设备信息读取和子对象检查。
5. ABI 同步点【已读】：`native/include/raynium/bridge.h`、`native/src/bridge.cpp`（含布局静态断言）、`NativeManifest.ABI_VERSION`、`NativeBindings`、`tools/native.py`（`ABI_VERSION` 和期望导出集合，13 个变 18 个）、`NativeAbiTest`。
6. 新增 nativeTest `NativeSwapchainTest`：用 SDL 创建一个可见的小窗口，按 1.3 的方式取 HWND。
7. 同步当前事实文档里的 ABI 版本、导出数量、结构布局和测试表：`doc/architecture/interop.md`、`doc/architecture/native-runtime.md`、`doc/architecture/overview.md`、`doc/guide/build-test.md`。

验收：

- `.\gradlew.bat test`、`.\gradlew.bat nativeTest`、`.\gradlew.bat nativeTest -PrhiValidation=true` 通过，D3D12 与 Vulkan 两个后端都覆盖。
- `NativeSwapchainTest` 覆盖：连续呈现不少于 120 帧；三种尺寸的重建，其中一种是奇数尺寸；两种呈现模式互相切换；最小化期间返回「已跳过」、恢复后继续呈现；交换链存活时销毁 runtime 被拒绝；错误线程的调用被拒绝；全程没有校验层错误。
- Immediate 模式下测到的呈现帧率高于显示器刷新率的 2 倍，数值写进测试输出。达不到说明关不掉垂直同步，按 2.3 上报，因为这直接决定接管后 benchmark 有没有意义。
- `python tools/native.py stage --config Debug` 的导出检查通过，`python tools/check_docs.py` 通过。
- 开关关闭的 `runClientNative` 仍然出现预览标记。

【未验证】RadRay 交换链后备缓冲的初始状态、提交时两个同步对象的用法、D3D12 的 Immediate 是否依赖 tearing 支持。动手前先读 `external/RadRay/docs/architecture/render-rhi.md` 和 `rhi.h` 里 `SwapChain`、`SwapChainFrame`、`CommandQueueSubmitDescriptor` 的契约。

### S2 启动到 CP1（窗口与交换链）

| # | 位置 | 原行为 | 改写 |
| --- | --- | --- | --- |
| 1 | `ClientBootstrap.bootstrap` 里的 `VulkanFeatureSets.bootstrap()`（31） | 加载 Vulkan 特性集类 | K6 |
| 2 | `Minecraft.<init>` 的 `getBackendsToTry()`（488） | 构造 `GlBackend` 与 `VulkanBackend` | 返回空的 `GpuBackend[]`，不调用原方法 |
| 3 | `VulkanBackend.checkBackendAvailable()`（490，仅选项为 DEFAULT 时执行） | 创建探测用的 Vulkan 实例 | 返回 null |
| 4 | `if (device == null)`（521） | 弹出对话框并抛 `WindowInitFailed` | 条件改为 false，技术见 2.2 第 6 条 |
| 5 | `RenderSystem.initRenderer(device)`（528） | 写入 `DEVICE`、创建 `DynamicGpuData`、初始化采样器缓存 | 不调用；改为调用 `RenderSystem.getSamplerCache().initialize()` |
| 6 | `device.getDeviceInfo()`（530） | 读设备信息用于日志和 `maxSize` | K4。第一次调用时创建 `NativeRuntime`（host 传 null） |
| 7 | `Window.createWindow` 里的 `backend.createWindow(title, w, h, 8224L)`（150） | 由后端创建 SDL 窗口并附加图形 API 标志 | 直接调用 `SDLVideo.SDL_CreateWindow(title, w, h, flags)`，不加图形 API 标志 |
| 8 | `SamplerCache.initialize()` | 32 次 `createSampler` | K2 加 K1，填入 32 个 `TakeoverSampler` |
| 9 | `device.createSurface(handle, isIconified)`（552） | 创建 `GpuSurface` | 取 HWND，创建交换链，呈现一帧纯色，打印 CP1，返回 null |

`RenderSystem.trackBackendLibraryForShutdown(backend)`（529）保留，参数是 null；对应的卸载方法有空判断【已读】。原版两个后端给窗口加的标志分别是 `SDL_WINDOW_VULKAN` 和 `SDL_WINDOW_OPENGL`（后者还设置 GL 属性）；基础标志 8224 是高像素密度加可调整大小【已读】。

验收：

- 日志出现 `Raynium takeover CP1`，带适配器名、后端、交换链尺寸和「已呈现」。
- 类加载日志里没有 `com.mojang.renderpearl.backend.` 和 `com.mojang.renderpearl.frontend.` 开头的类。
- 预期失败点：`GraphicsPreset.apply`（由 `Minecraft.<init>` 570 行调用）触发 `getDevice()` 绊线。

### S3 构造期到 CP3

按 `Minecraft.<init>` 552–665 行之间的执行顺序排列。

| # | 位置 | 设备使用【已读】 | 改写 |
| --- | --- | --- | --- |
| 1 | `GraphicsPreset.apply`:36、:98 | 取设备；读 `hintsAndWorkarounds` | K2、K4 |
| 2 | `ProjectionMatrixBuffer.<init>`:23–25 | 取设备、`createBuffer`，随后对缓冲调用 `slice` | K2、K1。一处改写覆盖全部实例（post、level、3d hud、gui、cubemap、各 PiP、物品图集） |
| 3 | `TextureAtlas.<init>`:60 | 读 `maxTextureSizeForFormat` | K2、K4 |
| 4 | `MappableRingBuffer.<init>`:18 起 | 取设备、3 次 `createBuffer` | K2、K1。覆盖雾、光照图、云等全部使用者 |
| 5 | `Lightmap.<init>`:42–46 | 取设备、`createTexture`、`createTextureView`、`clearColorTexture` | K2、K1、K3 |
| 6 | `DynamicTexture.createTexture`（两个重载，36–46）与 `upload`:51 | 创建纹理与视图；写入像素 | K2、K1、K3。覆盖 `UiLightmap`、`OverlayTexture` 等 |
| 7 | `FogRenderer.<init>`:49–55 | 取设备、`createBuffer`，随后对缓冲调用 `slice` | K2、K1 |
| 8 | `Lighting.<init>`:28–30 与 `updateBuffer`:64 | 取设备、读对齐值、`createBuffer`；`writeToBuffer` | K2、K4、K1、K3 |
| 9 | `GlobalSettingsUniform` 字段初始化:14 | `createBuffer` | K2、K1 |
| 10 | `CubeMap.initializeVertices`:106 | `createBuffer` | K2、K1 |
| 11 | `DebugCrosshairRenderer.<init>`:43 | `createBuffer` | K2、K1 |
| 12 | `MainTarget.createFrameBuffer` 与 `RenderTarget.createBuffers` | 创建颜色与深度纹理，读尺寸上限 | K5：只写入 `width` 与 `height` |
| 13 | `WorldBorderRenderer` 字段初始化:45 | `createBuffer` | K2、K1 |
| 14 | `LevelRenderer.<init>`:175 | 读设备信息，判定间接多重绘制 | K2、K4 |

说明：

- `Lighting` 必须保持可用：`updateLevel` 由 `GameRenderer.setLevel` 调用，`setupFor` 被界面代码 `LoomScreen`:163 直接调用，两者都在保留路径上【已读】。用了句柄之后 `setupFor` 里的 `slice` 正常工作。
- `Lightmap` 必须保持可用：`GameRenderer.lightmap()` 被 `TextureSetup` 在 GUI 抽取时调用【已读】。
- `SkyRenderer` 只在被切断的 `addSkyPass` 里惰性创建，`LevelExtractor` 对它有空判断，不需要处理【已读】。
- `RenderBuffers`、`StagedVertexBuffer`、`ScreenEffectRenderer`、各 feature renderer 的构造不触设备【已读】。
- 第 12 项必须让主目标的 `width`、`height` 等于传入的窗口尺寸。`Minecraft.<init>` 676–696 行在两者不等时会弹出「不支持的分辨率」对话框并把窗口改回窗口化；那段代码里的 `device.getLastDebugMessages()` 包在捕获全部异常的 try 里，不会触发绊线【已读】。`RenderTarget.resize` 先调 `destroyBuffers`（有空判断）再调 `createBuffers`，所以 `TextureTarget` 的构造和之后的尺寸变化都走这一项。
- 现有的 `TerrainLightmapMixin` 对 `Lightmap.<init>` 里的 `createTexture` 有参数修改，它属于预览分组，接管时不应用，不会和第 5 项冲突。
- `TextureManager`、`PalettedTextureManager`、`MapTextureManager` 的构造函数没有读【未验证】；命中绊线时按 K1、K2 处理，`ReloadableTexture.doLoad` 提前命中就先做 S4 的第 2 项。

验收：日志出现 CP2 与 CP3。预期失败点：`GameRenderer.preloadUiShader`（经 `Minecraft.loadCriticalShaders`，706 行）。

### S4 第一帧到 CP4

| # | 位置 | 设备使用【已读】 | 改写 |
| --- | --- | --- | --- |
| 1 | `Minecraft.loadCriticalShaders` 里对 `GameRenderer.preloadUiShader` 的调用（844） | 建立后备管线缓存并编译 GUI 管线 | K6 |
| 2 | `ReloadableTexture.doLoad`:39–44 | 取设备、创建纹理与视图、写入像素 | K2、K1、K3。`NativeImage` 的关闭仍由原有的 try 语句完成 |
| 3 | `TimerQuery.<init>`:17；`beginProfile`、`endProfile`、`close` | 创建时间戳查询池；写时间戳；关闭池 | 构造函数里 K2 并让创建查询池返回 null；三个方法 K5 置空 |
| 4 | `Minecraft.<init>` 736 行守卫 `TracyFrameCapture` 的 `TracyClient.isAvailable()` | 创建帧截取资源 | 该处返回 false |
| 5 | `renderFrame` 里对 `windowSurface` 的调用 | 见 3.5 | 按 3.5 的表 |
| 6 | `renderFrame` 1376 行 `gameRenderer.render()` | 全部原版绘制 | 换成 3.6 的帧入口 |
| 7 | `renderFrame` 1415 行的提交 | `getDevice().createCommandEncoder().submit()` | K2、K3，在 `submit()` 的位置执行 Raynium 呈现 |
| 8 | `renderFrame` 1426 行 | `getDynamicUniforms().reset()` | 取对象返回 null，`reset()` 不执行 |
| 9 | `renderFrame` 1427 行 `levelRenderer.endFrame()` | 经云渲染器轮转缓冲并创建 fence | K6 |
| 10 | `FontTexture.<init>`:30–33 | 取设备、创建纹理与视图 | K2、K1 |
| 11 | `FontTexture.add` 里的 `glyph.upload(x, y, texture)`（43） | 四个字形提供器各自写入像素 | K6。阶段 3 在这里取字形像素 |
| 12 | `Minecraft.<init>` 末尾遥测 lambda（747） | 读后端名 | K2、K4。先查出合成方法名 |

说明：

- `TimerQuery` 保留对象的原因：`renderFrame` 每帧在 1445 行无条件调用 `timerQuery.get()`，置空会空指针【已读】。
- `RenderSystem.executePendingTasks()`（1374）保留不动。往它的队列里放任务的只有 `GlCommandEncoder`，接管后队列恒为空，循环不触设备【已读】。
- 第 10、11 项在第一帧就会命中：GUI 抽取在保留路径上，加载界面的文字会触发字形烘焙。

验收：日志出现 CP4，带「已呈现」。之后游戏进入主循环并开始初次资源重载；如果在重载中失败，失败点应在 S5 的表里。

### S5 资源重载与关闭到 CP5、CP7

| # | 位置 | 设备使用【已读】 | 改写 |
| --- | --- | --- | --- |
| 1 | `TextureAtlas.createTexture`:65–77 | 取设备、创建图集纹理、整体视图和逐级视图 | K2、K1 |
| 2 | `TextureAtlas.upload`:103、:115 | 读对齐值；为动画精灵创建 UBO | K2、K4、K1 |
| 3 | `TextureAtlas.upload` 里的 `sprite.createAnimationState(...)`（120） | 为每个动画帧创建纹理并上传 | 返回 null。原代码对 null 有判断，动画状态列表保持为空 |
| 4 | `TextureAtlas.upload` 里的 `uploadInitialContents()`（133） | 用渲染 pass 把精灵画进图集 | K6 |
| 5 | `MipmappedTexture.doLoad`:38–48、`CubeMapTexture.doLoad`:59–70 | 创建纹理与视图、上传各级或各面 | K2、K1、K3；确认无其它 Mixin 目标后也可以用 K5 重写为句柄赋值 |
| 6 | `ShaderManager.reload`:69 | 取设备 | K2 |
| 7 | `ShaderManager.reload`:73–74 的 `requiredPipelines()` 与 `optionalPipelines()` | 提供要编译的管线列表 | 两处都返回空列表 |
| 8 | `GpuWarnlistManager` 内部类的 `apply`:174–175 | 读设备信息 | K2、K4 |
| 9 | `PalettedTextureManager`:125 与其内部图集纹理的构造函数:193–197 | 写入像素；创建纹理与视图 | K2、K3、K1 |
| 10 | `TextureUtil.writeAsPNG` | 读回纹理 | K5，记录一次日志 |
| 11 | `Minecraft.close` 的 `windowSurface.close()`（1154） | 关闭表面 | 按 3.8 关闭交换链、runtime、环境，打印 CP7 |
| 12 | `Minecraft.onGameLoadFinished` 返回处 | — | 打印 CP5 |

说明：

- 第 3、4 项用调用点改写而不是整方法替换，原因是 Fabric API 和 Raynium 自己的 Mixin 都注入在 `TextureAtlas.upload` 的返回处；在方法头取消会让它们不再执行。
- 第 3 项使动画状态列表为空，`TextureManager.tick` 每个 tick 调用的 `cycleAnimationFrames` 因此不会走到上传【已读】。精灵表和 UV 不受影响，区块编译依赖它们。
- 第 7 项之后 `ShaderManager.apply` 的两个循环都是空的，不会抛「必需着色器加载失败」，后处理配置照常更新；`PipelineCache` 的构造函数只保存参数【已读】。`getPostChain` 与 `isPostEffectValid` 的调用方都在被切断的路径上【已读】。
- `RayniumClient` 在开关开启时不在 `CLIENT_STOPPING` 里释放 native 对象。

验收：

- 不带 `-PquickPlayWorld`、带 `-PtakeoverExitAfter=60` 运行：日志出现 CP5，统计日志的 `frames` 持续增长、`screen` 为标题界面的类名；随后出现 `Raynium runtime destroyed`、`Raynium native environment destroyed`、`Raynium native library closed` 和 CP7；退出码为 0。
- 日志不含 3.9 的禁止内容。

### S6 进入世界到 CP6

| # | 位置 | 设备使用【已读】 | 改写 |
| --- | --- | --- | --- |
| 1 | `SectionRenderDispatcher.<init>`:69 | 取设备 | K2 |
| 2 | `StagingBuffer.create`:17 的两处 `getDeviceInfo()` | 选择暂存区变体 | K4 |
| 3 | `SectionRenderDispatcher.uploadTerrainBuffersToGpu`:141–153 | 取设备、取编码器、两处 `uploadStagedAllocations` | 按 3.7 |
| 4 | `UberGpuBuffer` | — | 新增 3.7 的排空方法 |
| 5 | `LevelRenderer` | — | 新增 3.6 的关卡维护入口，由帧入口调用 |
| 6 | `Camera`:189 | 读 `isZZeroToOne` 构造剔除用投影 | K2、K4 |
| 7 | `Projection`:69 | 读 `isZZeroToOne` | K2、K4 |
| 8 | `Options`:1186 | 读 `maxAnisotropy` | K2、K4 |

第 6–8 项如果在更早的步骤命中，就在那一步处理。世界卸载走 `resetLevelRenderData` → `SectionRenderDispatcher.dispose()`，只关闭 CPU 侧结构【已读】。

验收（带 `-PquickPlayWorld` 与 `-PtakeoverExitAfter=180`，渲染距离 12；更大的视距相应放宽并记录）：

- 日志出现 CP6，统计日志 `level=true`、`screen=none`。
- 进入世界后 60 秒内 `visibleRenderable` 大于 0。
- 视角不动时 `compileQueue` 在 120 秒内回到 0 并保持。一直不回到 0 而 CPU 占用高，优先怀疑 3.7 描述的自旋或空指针重试。
- 正常退出，世界保存完成，退出码为 0，日志不含禁止内容。

### S7 事件、调试键与脚本化自检

守卫：

| 位置 | 到达方式【已读】 | 改写 |
| --- | --- | --- |
| `Screenshot.grab(Minecraft, boolean)` | 截图键（`Minecraft`:2395）；其全景分支会直接调用 `gameRenderer.renderLevel()`（`Minecraft`:2935） | K5，记录一次「阶段 1 不支持截图」 |
| `RenderSystem.isWireframeAvailable` | 线框调试键（`KeyboardHandler`:136） | K5，返回 false |
| `DebugEntrySystemSpecs`:42 | 调试覆盖层 | K2、K4 |
| `DebugEntryFps`:22 | 调试覆盖层读 `windowSurface().currentConfiguration()` | 按 3.5 |

脚本化自检（`raynium.takeover.selftest`）在进入世界 20 秒后于客户端线程依次执行，每项之后至少继续运行 2 秒：

1. 把窗口改为 640×360、1281×721、1920×1080。
2. 最小化 3 秒后恢复。
3. 切换到全屏再切回窗口。
4. 触发一次资源重载，等待完成。
5. 调用一次截图入口。
6. 打开调试覆盖层 10 秒后关闭。
7. 关闭垂直同步选项，等待 5 秒，再打开。
8. 退出世界回到标题界面，再进入同一个世界。
9. 调用 `Minecraft.stop()`。

这些操作对应的方法名没有逐个核对【未验证】，以源码为准；垂直同步选项的回调会调用 `invalidateSurfaceConfiguration`【已读】。

验收：自检全部执行完，退出码为 0；日志不含禁止内容；第 7 项关闭垂直同步期间统计日志的 `fps` 高于显示器刷新率的 2 倍（帧率上限需设为不限）；第 2 项期间 `skipped` 增长、恢复后 `presented` 继续增长。

### S8 浸泡、审计与文档

1. 浸泡：带 `-PquickPlayWorld` 与 `-PtakeoverExitAfter=600` 在世界里静置 10 分钟。
2. 类加载检查：带 `-PclassLoadLog` 完整运行一次。
3. 审计表 `sites.csv` 补全，没有未分类项。
4. 开关关闭的回归：`.\gradlew.bat build`、`.\gradlew.bat nativeTest`、`runClientNative` 的预览标记、`python tools/benchmark.py --config tools/benchmark.example.json --check`。
5. 同步当前事实文档，只写已经落地的行为：`doc/architecture/minecraft-integration.md`、`doc/architecture/overview.md`、`doc/guide/build-test.md`（新的 Gradle 属性、运行目录和日志标记）。运行 `python tools/check_docs.py` 与 `git diff --check`。
6. 向用户汇报，并询问是否把验证记录存入 `doc/history/`。

## 5. 阶段 1 总验收

全部满足才算阶段 1 完成，之后才开始阶段 2：

1. 开关开启、带 `-PquickPlayWorld` 和 `-PtakeoverSelftest=true` 的一次完整运行里，日志依次出现 CP1 到 CP7，退出码为 0。
2. 该次日志不含 3.9 列出的禁止内容。
3. 类加载日志里没有 `com.mojang.renderpearl.backend.` 和 `com.mojang.renderpearl.frontend.` 开头的类。
4. 进入世界后 60 秒内 `visibleRenderable` 大于 0；视角不动时 `compileQueue` 在 120 秒内回到 0 并保持。
5. 垂直同步关闭、帧率上限不限时，`fps` 高于显示器刷新率的 2 倍。
6. 10 分钟浸泡：取每分钟内 `heapUsedMiB` 的最小值，第 10 分钟相对第 2 分钟的增长不超过 64 MiB。
7. 审计表没有未分类的调用点。
8. 开关关闭时 build、test、nativeTest 通过，预览标记出现，与 S0 的基线一致。
9. `python tools/check_docs.py` 与 `git diff --check` 通过。

## 6. 风险与规避

| # | 风险 | 规避 | 检出 |
| --- | --- | --- | --- |
| 1 | 跳过地形上传后，编译线程在暂存区写满时永久自旋，网格不被接纳 | 3.7 的无设备排空，保留原版线程模型 | 验收 4；CPU 占用 |
| 2 | 编译线程上的空指针被 `runTask` 吞掉并无限重试 | 工作线程路径上不留任何可能为 null 的解引用；排空只在渲染线程执行 | 验收 4 |
| 3 | 每帧状态因绘制路径被切而不再清理，内存增长 | gizmo 每帧排空；阶段 1 不调用 `submitFeatures`；GUI 与关卡状态的复位在保留路径上【已读】 | 验收 6 |
| 4 | 关闭路径空指针，`Minecraft.close` 中断，native 对象没有释放 | 用身份句柄保持字段非空；采样器缓存必须填满；`TimerQuery` 的三个方法置空 | `Shutdown failure!`；CP7 |
| 5 | 资源重载失败被资源包回滚逻辑接住，而界面不可见 | 重载链上的调用点逐项改写，不抛异常 | 禁止内容 `Caught error loading resourcepacks` |
| 6 | 整方法替换让 Fabric API 的注入失效，或与其冲突 | 2.2 第 2、5、8 条；`TextureAtlas.upload` 只做调用点改写 | 启动时的 Mixin 报错；S5 验收 |
| 7 | `LevelRenderer.render` 不再执行，Fabric 的世界渲染事件不再触发，依赖它的模组不出图 | 属于接管的固有后果，阶段 1 不处理；阶段 5 决定替代方式 | 写入 S8 的文档同步 |
| 8 | 投影约定与 Raynium 现有管线不一致 | `isZZeroToOne` 固定为 true | 阶段 2 的地形画面与 nativeTest 像素基线 |
| 9 | 交换链关不掉垂直同步，benchmark 被刷新率封顶 | S1 独立测量 | S1 验收；验收 5 |
| 10 | 没有图形 API 标志的 SDL 窗口、HWND 获取、全屏、最小化、高 DPI 下交换链异常【未验证】 | 尺寸取 `queryFramebufferSize`；最小化返回「已跳过」 | S1 测试；S7 自检 |
| 11 | runtime 提前到窗口创建前创建，线程归属或 D3D12 调试层顺序出错 | 仍在渲染线程创建；环境仍在 Fabric 入口初始化，早于任何图形对象 | S1 与 S2 带 `-PrhiValidation=true` 各跑一次 |
| 12 | 不再按 Minecraft 的 Vulkan 设备匹配适配器，多显卡机器选到另一块 | CP1 打印适配器名，人工核对 | CP1 日志 |
| 13 | 界面不可见时被模态界面挡住（引导、警告、确认） | S0 在可见画面下先验证；独立运行目录和预置选项；统计日志打印当前界面类名 | 统计日志的 `screen` |
| 14 | K2 把抛异常的绊线变成 null，漏改的调用点表现不同 | K2 的使用条件；审计表；漏改处仍以空指针暴露 | 禁止内容；验收 7 |
| 15 | lambda、匿名类名称或行号漂移，注入打到错误位置或静默不生效 | `require = 1`；用 `javap` 确认；不用行号定位 | 启动时的 Mixin 报错 |
| 16 | 接管改动影响开关关闭的模式 | 插件整体分组；每步重跑关闭模式的 build | 验收 8 |
| 17 | ABI 变更在多处不同步 | S1 第 5 项的同步点清单 | `tools/native.py` 导出检查；`NativeAbiTest` |
| 18 | 丢弃像素时破坏原有的 `NativeImage` 关闭时机，堆外内存泄漏 | 只改写设备调用，不替换包含资源关闭的方法 | 验收 6；进程内存 |
| 19 | 启动崩溃后 Minecraft 改写选项文件，破坏预览模式的 Vulkan 设置 | 接管使用 `runs/takeover/` | S8 第 4 项 |
| 20 | 调试键触发被切断的绘制函数（截图、全景、纹理转储、线框） | S5 与 S7 的守卫 | S7 自检第 5 项 |
| 21 | 暂存区复制仍在发生，阶段 1 的 CPU 数据偏高 | 记录为已知代价，阶段 2 处理 | — |
| 22 | 执行者为了通过验收引入空设备、吞异常或删测试 | 2.1 的硬约束与 2.3 的停止条件 | 评审改动 |

## 7. 后续阶段

阶段 2 及以后只调查到方向一级，没有到步骤一级。每个阶段的第一步都是补做该阶段的切点调查，写出与第 4 节同样粒度的步骤和验收，经用户确认后再动手。每个阶段结束都要重新满足第 5 节中仍然适用的条目，并用 `tools/benchmark.py` 采样。

| 阶段 | 内容 | 验收要点 |
| --- | --- | --- |
| 2 | 纹理进驻，SOLID 地形画到后备缓冲 | 地形 nativeTest 的像素基线不变；游戏内全分辨率地形；接管模式跑通 benchmark 并与 S0 基线对比 |
| 3 | GUI：2D 合批、裁剪、文字 | 标题界面、选项、暂停菜单、HUD 可用；与原版截图对比 |
| 4 | 其余地形层：cutout、半透明排序 | 固定路线截图对比 |
| 5 | 实体、方块实体、物品、粒子、世界内文字；GUI 的物品图标与画中画 | 同上 |
| 6 | 天空、云、天气、世界边界、雾 | 同上 |
| 7 | 手持物、屏幕效果、3D HUD | 同上 |
| 8 | 半透明合成、实体描边、后处理 | 同上 |
| 9 | 截图、世界图标、调试覆盖层的 GPU 信息与计时 | 阶段 1 禁用的功能恢复 |

阶段 2 已知要做的事（来自本次调查）：

- ABI：通用纹理创建与区域上传；`rn_render_frame` 直接输出到交换链并呈现；移除阶段 1 的纯色呈现导出，以及共享 RT、共享纹理导出，依赖它们取像的 nativeTest 随之调整。
- 像素来源：把 K3 置空的写入点改为转交像素。原版用渲染 pass 把精灵拼进图集（含边距和各级 mip），动画帧也是；这部分要在 CPU 侧或 Raynium 侧重做。光照图由 `core/lightmap` 着色器根据 `LightmapRenderState` 计算，需要移植。
- 地形采集：`TerrainCapture.capture` 原来挂在 `prepareChunkRenders` 的开头，改由帧入口调用。投影矩阵原来取自 `GameRenderer.renderLevel` 里叠加了受伤、摇晃、反胃变换之后的值（635–656 行），需要在帧入口重算。
- 删除预览分组的 Mixin、`preview/` 包和 `TerrainPreview`；`ClientFrameEvent` 迁到两种模式共用的位置。
- `tools/benchmark.py` 与 `BenchmarkSession` 支持接管模式：日志标记、帧计数来源、存活判断不再依赖 `TerrainCapture.active()`。
- 决定是否跳过 3.7 里无人读取的暂存区复制。

其它阶段已知的要点：

- 阶段 3：`GlyphBitmap` 没有读取像素的接口，字形像素要从四个提供器各自的上传点取。GUI 顶点来自 `buildVertices(VertexConsumer)`。
- 阶段 5：恢复 `submitFeatures` 的调用和 `RenderSystem.isRenderingLevel`；几何在 `VertexConsumer` 写入时才产生；RadRay 没有四边形和三角扇拓扑，要转成索引三角形；需要 `RenderPipeline` 到 Raynium 材质的映射表（调查时统计约 133 个管线注册）。

## 8. 被否决或被替换的方案

| 方案 | 提出方 | 结果 |
| --- | --- | --- |
| 继续完善右上角预览，全部 feature 接入后一次切换 | 用户列出的路线 1 | 用户未采用，选择先切断 |
| 逐 feature 渐进替换原版 | 助手 | 用户否决：原版路径干扰 benchmark |
| 把 Raynium 实现为 RenderPearl 后端 | 助手 | 用户否决：只等于实现了一个后端 |
| 临时的空 `GpuDevice` 脚手架 | 助手 | 用户否决：「先做完CDE再继续下一步」 |
| 阶段 1 就删除预览路径和共享资源 ABI | 助手（11-10 初稿） | 助手在 11-11 修订中改为推迟到阶段 2，待用户确认。理由：阶段 1 保持纯增量，现有测试和 benchmark 可以继续当回归基线 |
| 阶段 1 按 A–F 分组依次实施 | 助手（11-10 初稿） | 助手在 11-11 修订中改为按启动检查点实施，待用户确认。理由：设备为空时各组缺一不可启动，按分组做的中间步骤无法运行验证 |

## 9. 已核实与未核实

已核实（读源码或 jar，未经运行）：

- 向 `RenderSystem` 的 fence 任务队列投递任务的只有 `GlCommandEncoder`。
- Fabric Loader 0.20.1 内嵌 MixinExtras 0.5.5；2.2 第 8 条列出的 Fabric API 注入目标。
- `lwjgl-sdl` 3.4.3 有取 HWND 所需的属性接口；原版两个后端创建窗口时附加的标志。
- `ShaderManager.apply` 在管线列表为空时的行为；`PipelineCache` 构造函数不触设备。
- `GuiRenderState.reset()` 在 `Gui.extractRenderState` 里每帧调用；`LevelRenderState.reset()` 在 `LevelExtractor` 里调用。
- `GameRenderer.close`、`LevelRenderer.close`、`RenderSystem.shutdownRenderer`、`RenderTarget.destroyBuffers` 的方法体与各自的空判断情况。
- `TextureAtlas.upload` 对 `createAnimationState` 返回 null 有判断；动画状态为空时 `uploadAnimationFrames` 不触设备。
- `StagingBuffer` 的 CPU 变体、`Uploader` 的构造与关闭、`UberGpuBuffer` 各方法在没有堆时的行为。

未核实，执行到相应步骤时先确认：

1. 接收者为 null 时 `@WrapOperation` 能否正常替换调用；MixinExtras 表达式能否匹配 `device == null`。（S2）
2. RadRay 交换链的后备缓冲初始状态、同步对象用法、D3D12 Immediate 的实际效果。（S1）
3. 没有图形 API 标志的 SDL 窗口配合 RadRay 的 Vulkan 交换链是否可用。（S1）
4. `CLIENT_STOPPING` 之后是否还会调用 `renderFrame`。（S5）
5. `finalizeGizmoCollection` 里 `gizmo().emit(...)` 是否只做 CPU 工作。（S6）
6. `TextureManager`、`PalettedTextureManager`、`MapTextureManager` 的构造函数是否触设备。（S3）
7. `FeatureRenderDispatcher.close` 及其 feature renderer 的 `close` 是否触设备。（S5）
8. `GlyphBitmap.upload` 是否只有 `FontTexture.add` 一个调用方。（S4）
9. `fabric-client-gametest-api-v1` 在 `Minecraft` 与 `Window` 上的具体注入目标。（S2）
10. `DeviceType` 的枚举成员；S7 自检用到的窗口、全屏、重载、调试覆盖层入口的方法名。（S2、S7）
11. 调试覆盖层里除已列出的两项之外，是否还有读设备的条目。（S7）
12. 独占全屏下 DXGI 交换链的行为。（S7）
13. 「类加载日志里没有 backend 与 frontend 包的类」这一判据是否会被 JVM 字节码校验引起的加载误伤。出现这类条目时，先确认该类是否被初始化、由谁引起加载，再判断是漏切还是校验加载；属于后者时向用户说明并改用初始化作为判据。（S2、S8）
