> - 适用: Minecraft 26.3 客户端渲染接管的实施与验收
> - 权威: 本文是待实施计划，不代表当前实现；依据为仓库源码、锁定依赖源码、本会话两份计划附件及后续审查修订
> - 锚点: `native/include/raynium/bridge.h`, `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/resources/raynium.client.mixins.json`, `tools/native.py`, `build.gradle`

# Raynium 渲染接管、GUI 与纹理接入最终计划

- 原记录日期、归档日期：2026-11-12。
- 状态：最终整合稿，待实施；归档前只有静态核对，没有接管运行验证。
- 范围：Minecraft 启动、资源与帧循环，Raynium native 交换链、纹理、GUI、地形及测试工具。
- 来源：本会话的初版与修订版附件、逐轮审查及用户替换文本、[11-10 计划](2026-11-10-vanilla-render-cutover-plan.md)，以及未提交工作树和锁定依赖的静态阅读。Minecraft 来源为 Loom 生成的 26.3 源码，定位以类与方法为准，生成入口见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)。
- 授权：用户先要求整合并存档，随后明确要求先写入 history，再实施计划。原稿中计划模式导致的“尚未落盘、仅授权存档”说明由本条归档元数据取代；技术正文保留为实施前快照。
- 证据边界：旧附件提到的 build 通过不视为本次基线。本文不宣称任何拟议接口、测试或接管能力已经实现。

## 1. 目标、范围与既定决定

目标是让 Minecraft 在启动、资源重载、进入世界和退出的全过程中不创建原版图形设备；窗口由 Raynium 交换链呈现，纹理像素从 CPU 写入点交给 Raynium，GUI 和现有三层地形直接画入后备缓冲。

本轮包含：

- 字形、独立贴图、动态贴图、调色板贴图、全部图集及其动画、16×16 光照图。
- GUI 矩形、贴图、文字、裁剪及 12 种原版管线变体。
- SOLID、CUTOUT、TRANSLUCENT 地形，包括现有透明排序和 OIT 路径。
- 窗口尺寸、呈现模式、隐藏与最小化、资源重载、世界切换和正常关闭。

本轮不包含：

- 物品图标及画中画，包括背包玩家、书、旗帜和皮肤。
- 菜单背景模糊、`END_PORTAL` 填充、标题全景图。
- 实体、粒子、天空、云、手持物、后处理和截图。

已确定的取舍：

1. 接管开关保留到 M5，M6 删除开关和预览路径。
2. 物品图标本轮留空。
3. 图集拼接及动画插值放在 CPU。
4. 交换链固定 BGRA8；离屏目标保留 RGBA8 默认行为，增加 BGRA8 选择。
5. 帧率作为观测项，低于刷新率两倍时调查并上报，不作为通过门槛。
6. 不再使用临时纯色呈现 ABI，第一帧即走正式 `rn_present_frame`。

本计划取代 11-10 计划中的阶段安排、旧 ABI 设计、像素丢弃方案、旧 Mixin 分组和帧率门槛。旧历史文件保持原样；有效的切点、维护职责、关闭顺序和执行约束已整合如下。

## 2. 实施约束

### 2.1 原版设备与资源

- `RenderSystem.DEVICE`、`Minecraft.windowSurface` 保持 null。
- 不实现 RenderPearl 的 `GpuDevice`、`CommandEncoder`、`RenderPass`、`GpuSurface` 或 `GpuBackend`。
- 只实现 `GpuTexture`、`GpuTextureView`、`GpuSampler`、`GpuBuffer` 四种身份句柄。
- 不捕获 `IllegalStateException`、`NullPointerException` 来绕过漏切调用。
- 不修改 `external/RadRay/`。
- C++ 可失败创建使用 `static Create` 和局部 RAII，不采用默认构造后 `Init` 的方式。
- C ABI 只返回 `RN_STATUS_OK`、`RN_STATUS_FAILED`；错误原因写入调用方缓冲区。
- 不为通过验收删除断言或弱化测试。M6 的测试迁移保留原测试意图和覆盖。

### 2.2 Mixin

- 接管专用行为放在 `mixin.takeover`，由配置插件整体启用；插件只读取系统属性，不提前加载 Minecraft 类。
- 优先使用可叠加的 `WrapOperation`、`WrapWithCondition`、`ModifyExpressionValue`、`ModifyReturnValue`、`Inject`。
- 禁止 `Overwrite`；`Redirect` 仅用于确认无其他注入目标的指令。
- 保持 `require = 1`，不使用静默失配。
- lambda、匿名类用实际 jar 的 `javap` 结果定位，不依赖反编译行号。
- 在方法头取消并重写前，必须确认保留全部 CPU 副作用，且不会吞掉其他模组的注入。
- `TextureAtlas.upload` 只改写内部调用，不整方法取消。
- 接收者为 null 的包装处理函数不得解引用接收者或调用 `original`；同一保留路径上的全部使用必须一起处理。
- `device == null` 分支先用 MixinExtras 表达式改写；不适用时用精确匹配的 `postApply`，失配即启动失败。

开始前核对 Minecraft 26.3、Fabric Loader 0.20.1、Fabric API 0.165.0+26.3 及 RadRay 锁文件状态。依赖变化后重新核对切点。

需要引入上述禁止的接口实现、修改 RadRay、无法解决的 Mixin 冲突、版本不符、未分类调用点，或同一步经过三种不同修正仍失败时，停止并报告。未经授权不提交或创建分支。

## 3. Native 接口与渲染设计

### 3.1 ABI 与同步点

N1–M5 使用 ABI 14：保留现有 13 个导出，新增 8 个，共 21 个。

| 导出 | 语义 |
| --- | --- |
| `rn_query_device_info` | 查询适配器信息及 Java `DeviceInfo` 所需限制 |
| `rn_create_swapchain` | 创建 HWND 交换链，BGRA8，申请 3 张后备缓冲 |
| `rn_resize_swapchain` | 排空在途工作后按尺寸、呈现模式重建 |
| `rn_destroy_swapchain` | 释放交换链 |
| `rn_create_texture` | 创建带调用方编号的 RGBA8 或 R8 纹理 |
| `rn_upload_texture` | 支持 mip 和区域上传，调用内复制源字节 |
| `rn_destroy_texture` | 移除编号并按所有权和帧完成状态退役资源 |
| `rn_present_frame` | 使用正式帧描述绘制到交换链，另写呈现结果 |

`rn_render_frame` 与 `rn_present_frame` 共用扩展后的帧描述，增加 GUI 顶点、绘制记录、逻辑尺寸和清屏色。`rn_material_update` 布局不变。

同步修改：

- C 头文件、实现及布局静态断言。
- `NativeManifest`、`NativeBindings`、`NativeFrameBuffer`。
- `tools/native.py` 的版本、导出检查和布局检查。
- `NativeAbiTest` 及受影响的帧描述测试。
- 行为落地后同步所属当前事实文档。

### 3.2 `rn_present_frame` 的顺序与失败语义

固定顺序：

1. 校验参数和帧序号，取得帧槽。
2. **准备**：应用驻留增量、排序、构建参数、写上传缓冲、登记纹理上传。所有可恢复失败都在获取后备缓冲之前处理。
3. **检查窗口可呈现性**：Raynium 根据有效 HWND 的可见状态、最小化状态及非零窗口尺寸决定是否尝试获取。
4. 可呈现时获取后备缓冲，超时初值 100 ms。
5. 录制并提交：获取成功时包含上传、地形和 GUI；未获取时只提交上传。
6. 获取成功时调用 Present，终结该帧。

后备缓冲视图和输出格式相关 PSO 在交换链创建、重建时准备；其他可能返回可恢复错误的资源准备放在第 2 步。

**窗口判断由 Raynium 完成。** 隐藏、最小化或零尺寸时跳过获取，仍消费上传和驻留增量。不能依赖 Vulkan 的获取结果判断窗口是否隐藏，也不把普通遮挡等同于隐藏。

获取成功之后只剩录制、队列提交和 Present：

- 录制、提交沿用 RadRay 现有 `RunFrame` 的失败即终止契约。
- Present 的任何返回状态都会消费已获取帧。
- 不存在持有已获取帧却返回可恢复 FAILED 的路径。
- 不调用 `DiscardAcquiredFrame`。

| 情况 | C 返回值 | 结果结构 | Java 行为 |
| --- | --- | --- | --- |
| 准备失败 | FAILED | 不改写 | 不 `commit()`，关闭 renderer |
| 正常呈现 | OK | 已呈现 | `commit()` |
| 获取超时、窗口隐藏、最小化或零尺寸 | OK | 已跳过 | `commit()` |
| 获取或呈现发现需要重建 | OK | 需要重建 | `commit()`，驱动重新配置 |
| 获取或呈现返回设备级错误 | FAILED | 不改写 | 不重试该 renderer，执行关闭 |

OK 表示帧增量已消费，与是否呈现无关。FAILED 不承诺此前的准备操作已经回滚；Java 关闭 renderer，不复用该帧尝试恢复。

重建、销毁交换链之前，排空帧槽，释放后备缓冲视图和命令引用。不得带着已获取帧或未释放引用销毁交换链。

### 3.3 输出格式及 BGRA8 像素验证

- 交换链使用 `BGRA8_UNORM`；设备不支持时创建失败并说明原因，不自动回退。
- `rn_render_target_desc.reserved` 改作格式选择：0 为 RGBA8，1 为 BGRA8，其他值失败；结构尺寸不变。
- 写最终颜色的地形、OIT 合成和 GUI PSO 按输出格式创建。
- OIT 浮点中间附件保持原格式。
- M6 后仍保留离屏 RGBA8 默认值和 BGRA8 选项。

BGRA8 测试同时改通导入与读回链路：

1. 扩展 `VulkanPreviewImage` 的格式处理，保留现有 RGBA8 默认入口。
2. 根据共享句柄类型解释 `nativeFormat`：Vulkan 格式和 DXGI 格式分别映射，不能把 DXGI 枚举直接当 Vulkan 枚举使用。
3. 格式校验、外部内存能力查询、导入图像创建全部使用一致的 RGBA8/BGRA8 格式。
4. 保留设备身份、用途、平铺方式等现有验证。
5. `VulkanConsumer` 的目标图像与读回路径同步支持两种格式。
6. BGRA8 原始读回数据只在比较或生成图片前交换一次红蓝通道，再与同一份 RGBA 预期比较。

N1 地形场景和 N3 GUI 场景在双后端、两种输出格式上分别运行。原有 RGBA8 测试入口和断言保持不变。

### 3.4 纹理编号与所有权

- 编号 0–15 保留给现有共享集合的逻辑纹理；现有 0、1 含义不变。
- 上传纹理由调用方分配 16 及以上编号，在 renderer 生命周期内不复用。
- 没有共享集合时引用 0–15 返回 FAILED。
- 纹理属于 renderer，不随地形 epoch 销毁。
- 上传调用内复制 Java 提供的字节；下一帧录制拷贝，不保留调用方指针。
- 销毁先从编号表移除。已有材质绑定、排队上传和在途帧持有必要所有权，直到安全退役；新查找不能重新获得已销毁编号。
- 图集重载递增地形 epoch，清空驻留状态，下一次材质更新发送新纹理编号；native 在 epoch 切换时退役旧材质。

### 3.5 renderer、shader pack 与 GUI

renderer 在初始交换链建立时创建，存活到会话关闭；世界切换只重置地形 epoch。shader pack 从 mod 容器路径读取，不依赖尚未建立的 `ResourceManager`。

程序分为：

- `TerrainPrograms`：现有 8 个程序及 `ValidateTerrainShader` 保持独立。
- `GuiPrograms`：新增 `gui_color`、`gui_textured`、`gui_text`，独立反射校验和资源类。

Python 与 native 包校验要求两个集合的并集在 DXIL、SPIR-V 中齐全。

GUI 顶点固定 24 字节：float3 位置、float2 UV、RGBA8 颜色。绘制记录包含管线编号、纹理编号、采样器位、裁剪矩形和顶点范围；四边形索引由 native 生成。GUI 无深度，保持输入顺序，在地形之后绘制。

| 程序 | 管线变体 |
| --- | --- |
| `gui_color` | `GUI`、`GUI_INVERT`、`GUI_TEXT_HIGHLIGHT` |
| `gui_textured` | `GUI_TEXTURED`、`GUI_TEXTURED_PREMULTIPLIED_ALPHA`、`GUI_OPAQUE_TEXTURED_BACKGROUND`、`GUI_NAUSEA_OVERLAY`、`VIGNETTE`、`CROSSHAIR`、`MOJANG_LOGO` |
| `gui_text` | `GUI_TEXT`、`GUI_TEXT_GRAYSCALE` |

各变体复刻对应混合、采样和颜色行为；灰度字形读取 R 通道，文字 alpha 小于 0.1 时丢弃。未知管线跳过，每种只记录一次。

## 4. Minecraft 接入

### 4.1 身份句柄与设备信息

身份句柄保留 CPU 查询和关闭所需元数据：

- 纹理记录格式、尺寸、层数、mip 和 native 编号；关闭时释放对应 native 所有权。
- 视图记录所属纹理及 mip 范围，不重复拥有纹理。
- 采样器记录寻址、过滤、各向异性及 LOD。
- Buffer 记录用途与尺寸，不保留创建时的临时 `ByteBuffer`；`map()` 抛出以 `takeover:` 开头的异常，暴露漏切调用。
- 本轮排除的 GPU 功能只保留必要身份与 CPU 状态，不因此扩展渲染范围。

缓存的 Java `DeviceInfo` 使用下列规则：

| 项目 | 值 |
| --- | --- |
| 名称、厂商、驱动信息 | native 查询结果 |
| `backendName` | `Raynium` |
| `isZZeroToOne` | true |
| 时间戳周期 | 1.0 |
| 最大各向异性 | native 限制取整，至少 1 |
| uniform 对齐 | native 限制，至少 1 |
| 最大纹理尺寸、颜色附件数 | native 限制 |
| 最大内存分配尺寸 | `Long.MAX_VALUE` |
| MC 两项 multidraw 数量限制 | 0 |
| 8 项 features、4 项 hints | false |
| 扩展集合 | 空 |
| 设备类型 | 按实际枚举及 native 设备信息映射 |

这些值控制 Minecraft 保留的 CPU 路径，不改变 Raynium 自身的绘制能力选择。

### 4.2 Mixin 分组与地形职责

| 分组 | 成员 | 条件 |
| --- | --- | --- |
| 公共 | `MinecraftBenchmarkMixin`、`SectionCompilerMixin`、`SectionResultsMixin`、`CompiledSectionMeshMixin`、`RenderSectionIndicesMixin`、新拆出的关卡失效 Mixin | 总是 |
| 预览 | `MinecraftPreviewMixin`、`VulkanFeatureSetsMixin`、`VulkanCommandEncoderAccessor`、`LevelTerrainMixin` 剩余参考绘制与冻结部分、`GameTerrainMixin`、`TerrainAtlasMixin`、`TerrainLightmapMixin` | 开关关闭 |
| 接管 | `mixin.takeover` 全部 | 开关开启 |

`TerrainCapture.ENABLED` 为“预览开启或接管开启”。

| 职责 | 接管归属 |
| --- | --- |
| 透明层索引快照 | 公共 `RenderSectionIndicesMixin` |
| 世界重置时 epoch 失效 | 公共关卡失效 Mixin |
| 图集重载时 epoch 失效 | 接管 CPU 图集上传钩子 |
| 可见快照采集 | 接管帧入口 |
| 透明模式 | `gameRenderer.useImprovedTransparency()` |
| 世界投影 | 接管帧入口重算 |
| 每帧采集清理和帧事件 | 接管帧入口 |
| 激活采集 | renderer 就绪后 |
| 驻留准备与提交 | 新接管帧类，不依赖共享输入 |

接管模式下 `RayniumClient` 不创建 `PreviewOverlay`，不做 `PreviewHost` 转型，不查询原版 Vulkan 设备，不在 `CLIENT_STARTED` 创建 runtime。native 环境仍在 Fabric 入口初始化；runtime 在首次需要设备信息时创建，host 传 null。没有 native payload 时明确失败。

### 4.3 帧入口

保留 `renderFrame` 中的更新、拾取、`gameRenderer.extract(...)`、限帧和统计。替换 `gameRenderer.render()`，在原 `submit()` 位置调用 `rn_present_frame`。

接管帧按以下顺序处理：

1. 清理本帧采集状态并处理帧事件；renderer 就绪后激活采集。
2. 尺寸变化时调用 `gameRenderer.resize`。
3. **仅当 `gameRenderState.shouldRenderLevel` 为真**时，执行关卡维护、世界投影重算、地形采集和光照图更新。
4. 条件为假时，地形段没有可见项和驻留增量，清屏色取 `guiRenderState.clearColorOverride`。
5. 纹理更新、GUI 采集和呈现每帧执行，不受关卡条件影响。
6. 按 native 返回值决定 `commit()`、重新配置或关闭 renderer。

关卡维护顺序：

1. `repositionCamera`。
2. `finalizeGizmoCollection`。
3. `performTransientBlockRemovals` 及临时方块过期清理。
4. `compileSections`。
5. 持锁调用无设备版 `uploadTerrainBuffersToGpu`。
6. 更新遮挡图。
7. 执行玩家区块编译完成回调。

世界投影保留受伤、视角摇晃和反胃变换。世界帧使用对应世界清屏色；不调用原版 `submitFeatures` 或世界 GPU 绘制。

不能用 `level != null` 代替 `shouldRenderLevel`：断开连接和世界切换中的强制 GUI 帧也必须符合上述条件。

### 4.4 无设备地形排空

- `StagingBuffer` 选择 CPU 变体，保留原线程模型。
- `UberGpuBuffer` 增加排空入口：对未被跳过的项执行上传回调，随后关闭各项，清空 pending 和 skipped 集合。
- 排空不创建 GPU heap，不调用 `uploader.copyTo`。
- 保留 `startUploading` 和原有 try-with-resources；`Uploader.close()` 继续归零暂存区写指针。
- 两处 `uploadStagedAllocations` 改为排空并返回 false。
- 保留正常帧维护和渲染线程追加失败后的直接排空两个调用方。
- 公共索引 Mixin 包装的接纳回调必须继续执行，使索引快照及 uploaded 标记符合采集条件。
- 本轮接受现有一次无人读取的暂存区复制，不夹带相关优化。

### 4.5 GUI 采集与像素来源

GUI 采集入口依次执行：

1. 原 `prepareText()`，保留字形烘焙。
2. `sortElements`。
3. 按模糊前、模糊后两段及原有节点顺序遍历。
4. 自有 `VertexConsumer` 直接写堆外帧缓冲；管线、裁剪或 `TextureSetup` 改变时开新批次。
5. 执行原 `render()` 末尾必要的 CPU 清理与状态复位。

跳过物品和画中画准备，不调用 `addElementToMesh`；对应 Fabric GUI 钩子本轮不触发。

| 像素 | 来源与处理 |
| --- | --- |
| 独立贴图 | `ReloadableTexture.doLoad`、`MipmappedTexture.doLoad` 转交 `NativeImage` |
| 动态贴图 | `DynamicTexture.upload` 转交像素 |
| 字形 | 四个提供器各自的 upload；BitmapProvider 从 CPU 图像取子矩形，并切断其 GPU 缓冲创建 |
| 调色板 | `PalettedTextureManager` 的写入点 |
| 全部图集 | `TextureAtlas.uploadInitialContents` CPU 拼接各级 mip |
| 图集动画 | `cycleAnimationFrames` CPU 推进帧状态并区域上传 |
| 光照图 | 按 `core/lightmap` 公式计算 256 个像素 |

CPU 图集使用原版布局、mip 与边距；每级 mip 的目标坐标按 `clamp(i - padding, 0, width - 1)` 取样，二维分别计算。

动画保留原帧时长、循环和插值进度规则，只处理实际变化的精灵。原 GPU 动画状态创建被切断，另存 CPU 动画状态；初次上传包含正确首帧。

保留原 `NativeImage` 的关闭位置。CPU 读取完成、native 复制字节之后才能释放源数据，不延长临时像素指针寿命。

### 4.6 表面映射与关闭

| 原版调用 | 接管行为 |
| --- | --- |
| 帧入口、blit、present 相关 `isAcquired()` | false |
| `isSuboptimal()` | 上次 native 是否要求重建 |
| `supportedPresentModes()` | `IMMEDIATE`、`FIFO` |
| `configure()` | 调用 native 重建，失败向上传递 |
| `acquireNextTexture()` | 跳过，由 `rn_present_frame` 获取 |
| `currentConfiguration()` | 返回保存的请求尺寸和模式 |

VSync 开启映射 FIFO，关闭映射 Immediate。请求模式不作为“后端实际没有回退”的证据。

接管模式不在 `CLIENT_STOPPING` 销毁 native 对象。关闭放在 `Minecraft.close()` 原 `windowSurface.close()` 的位置，在 Java 渲染对象关闭之后执行：

1. 排空帧槽，释放命令与后备缓冲引用。
2. 关闭交换链、renderer。
3. 关闭 runtime、native 环境和 DLL。
4. 成功完成对应阶段后记录日志。

等待或关闭失败时沿用现有所有权保护，不提前释放仍被使用的资源，也不伪造关闭成功。

## 5. 实施阶段与切点

每步先完成本步验收，再进入下一步。N1–M5 每步结束运行关闭模式的 `build`；native 改动运行对应测试。当前事实文档随已落地行为同步。

### 5.1 前置基线

- 检查工作树和依赖版本。
- 运行 `build`、`test`、`nativeTest`，记录本次结果。
- 关闭模式运行预览，确认现有就绪标记及 benchmark 检查。
- 准备独立 `runs/takeover/` 和已在本版本打开过的存档副本。
- 仅对新建测试目录设置引导关闭、教程关闭、初始窗口化等选项，不覆盖用户已有目录。
- 旧附件提到的构建通过不视为本次基线。

### 5.2 Native 阶段

| 阶段 | 实现 | 验收 |
| --- | --- | --- |
| N1 | 设备信息、交换链、正式帧入口、输出格式 PSO、BGRA8 导入读回、renderer 无世界存活 | 双后端连续呈现至少 120 帧；640×360、1281×721、1920×1080 重建；两种呈现模式切换；错误线程和错误所有权关闭被拒绝；无校验错误；两种格式地形像素验证 |
| N2 | 纹理 ABI、编号空间、区域上传、所有权和退役 | RGBA8/R8、多 mip、区域上传逐像素验证；地形材质引用上传纹理；销毁后新查找失败，已有绑定安全退役；新资源使用新编号；旧共享集合测试保持 |
| N3 | GUI 程序、反射校验、资源类和 pass | 12 种管线固定场景；裁剪、灰度字形、GUI 覆盖地形；双后端、两种输出格式像素验证；shader pack 与容器测试通过 |

N1 另加：

- 带有效交换链提交非法帧，可见项引用不存在的 primitive：获取前返回 FAILED；renderer 和交换链可正常关闭、重建，之后正常呈现。
- 隐藏、最小化和零尺寸窗口返回 OK＋已跳过；期间上传恢复后可见，不要求重发。
- 销毁和重建前释放全部后备缓冲引用。
- 记录获取超时计数。无法稳定构造超时的情况下，仅记录静态审查证据，明确保留运行验证缺口。
- Immediate 帧率低于刷新率两倍时调查并报告，不判验收失败。

### 5.3 M0：开关与分组

增加 `raynium.takeover.enabled`、`-Ptakeover=true`、独立运行目录和配置插件，按第 4.2 节分组。

本阶段接管组尚为空，开启开关时仍由原版出图，但不建立预览。验证能快速进入世界、运行 30 秒并正常退出。关闭模式行为保持基线。

### 5.4 M1：启动与构造，达到 CP1–CP3

| 切点 | 处理 |
| --- | --- |
| `ClientBootstrap` Vulkan 特性 bootstrap | 跳过 |
| `getBackendsToTry`、默认后端探测 | 返回空后端列表，跳过 Vulkan 探测 |
| `device == null` 致命分支 | 精确改写条件 |
| `RenderSystem.initRenderer` | 跳过；单独填满 32 个采样器身份句柄 |
| 首次 `getDeviceInfo` | 提前建立 runtime，返回缓存信息 |
| `Window.createWindow` | SDL 直接创建高 DPI、可调整尺寸窗口，不加 OpenGL/Vulkan 标志 |
| `createSurface` | 取 HWND，创建 renderer 和交换链，通过正式空帧入口呈现清屏，返回 null |
| `GraphicsPreset`、`TextureAtlas`、`LevelRenderer` 构造 | 使用接管设备信息 |
| 投影、环形缓冲、雾、光照、全局 uniform、CubeMap 顶点、调试准星、世界边界缓冲 | 创建身份句柄，切断 GPU 写入 |
| `Lightmap`、`DynamicTexture` 构造和写入 | 创建必要 native 纹理并接收像素或初始清色 |
| `MainTarget`、`RenderTarget.createBuffers` | 保持尺寸字段，不创建原版 GPU 附件 |

同时核对 TextureManager、PalettedTextureManager、MapTextureManager 构造路径。实际提前命中的资源写入按对应规则接入。

验收 CP1–CP3；CP1 包含适配器、后端、尺寸和已呈现状态。该阶段之后允许的已知未完成点是 UI shader 预加载切点。

### 5.5 M2：第一帧与加载 GUI，达到 CP4

- 跳过 `GameRenderer.preloadUiShader`。
- 接入独立贴图、字体纹理及四类字形上传。
- 保留 `TimerQuery` 对象，使其不创建查询池，begin/end/close 不触 GPU。
- 仅在构造帧截取资源的目标调用处关闭 Tracy 可用判断。
- 落地表面映射、接管帧入口和正式提交。
- 跳过原版 dynamic uniform reset 及 `LevelRenderer.endFrame` 的 GPU 操作。
- 保留 `RenderSystem.executePendingTasks()`。
- 改写遥测 lambda 的设备信息读取。
- 审核 `GameRenderer.render` 末尾清理：必要 CPU 清理保留，GPU 清理随对应对象切断。

验收加载界面的 Mojang 标志、进度条、文字可见，CP4 为首次游戏帧已呈现。

### 5.6 M3：资源重载与关闭，达到 CP5、CP7

- 接入 TextureAtlas 创建、CPU 初次拼接、CPU 动画状态与更新。
- 保留 `TextureAtlas.upload` 返回处的 Fabric 与项目钩子。
- 接入 mipmap、调色板写入；切断 CubeMap 原版设备调用，标题全景图仍不绘制。
- ShaderManager 的 required/optional 管线列表为空，保留资源与后处理配置的 CPU 更新。
- GpuWarnlistManager 使用接管设备信息。
- `TextureUtil.writeAsPNG` 记录一次不支持并返回。
- 落地关闭顺序，检查 feature renderer 关闭链。

验收标题、选项、世界列表可见可操作；F3+T 重载后正常；不进世界运行 60 秒后正常退出，出现 CP5、CP7，退出码 0。

### 5.7 M4：世界与地形，达到 CP6

- 接入 SectionRenderDispatcher、CPU staging 和无设备排空。
- 落地完整关卡维护顺序、投影、透明模式、采集、驻留与光照图。
- Camera、Projection 使用零到一深度约定；Options 各向异性读取接管限制。
- 开始前审核 `LevelRenderer.render`、`submitFeatures` 及子方法、`GameRenderer.render` 的非绘制副作用。

验收：

- 全分辨率三层地形和 HUD，暂停菜单可用。
- 进入世界 60 秒内可见区块数大于 0。
- 视角不动时编译队列在 120 秒内归零并保持。
- 移动、触发区块重建 5 分钟，临时方块清理正常，队列不持续增长。
- 世界内 F3+T 后纹理正确，退出重进世界后正常。
- 断开连接、世界切换中的强制 GUI 帧不执行关卡维护和采集，以日志计数确认。
- 无设备接纳回调后 uploaded 标记和索引快照符合采集要求。

### 5.8 M5：守卫、自检、浸泡与总验收

- 截图和全景截图入口记录一次不支持，避免进入被切断的绘制路径。
- 线框能力返回 false。
- F3 系统信息、帧率配置等条目使用接管信息。
- 完成第 6 节全部验收。

### 5.9 M6：删除过渡路径

M5 通过后：

- 删除接管开关、预览分组、游戏侧 `preview/` 路径及共享输入纹理路径。
- 使用上传纹理重写依赖共享输入的 nativeTest，保留原场景、预期和错误断言。
- 先整理具体测试迁移差异，按此前约定完成确认后修改。
- 保留离屏目标与共享输出读回能力；将仍需要的导入、读回代码迁到测试辅助代码。
- 删除共享输入专用 ABI 时同步递增 ABI 版本，更新导出清单、布局、Java 绑定和测试；不保留无实现的兼容空壳。
- `tools/benchmark.py` 和 BenchmarkSession 使用接管日志、帧计数与存活条件。
- 保留 RGBA8 默认离屏格式和 BGRA8 选项。
- 完成当前事实文档同步。

验收 `build`、`test`、`nativeTest`、文档检查、diff 检查通过，接管 benchmark 跑通。M6 不再执行已删除的关闭模式预览验收。

## 6. 自动化、日志与最终验收

### 6.1 运行入口与日志

保留到 M5 的入口：

| 属性 | 作用 |
| --- | --- |
| `-Ptakeover=true` | 启用接管，使用独立运行目录 |
| `raynium.takeover.statsSeconds` | 默认 5 秒，0 关闭统计 |
| `-PtakeoverExitAfter=<秒>` | 首次加载完成后计时，正常调用 stop |
| `-PtakeoverSelftest=true` | 执行脚本化自检 |
| `-PquickPlayWorld=<目录名>` | 快速进入测试世界 |
| `-PclassLoadLog=<绝对路径>` | 保存类加载日志 |

检查点每个只打印一次：

| 标记 | 位置 |
| --- | --- |
| CP1 | 交换链通过正式帧入口首次清屏呈现 |
| CP2 | GameRenderer 构造返回 |
| CP3 | LevelRenderer 构造返回 |
| CP4 | `renderFrame` 首次完成呈现 |
| CP5 | `onGameLoadFinished` 返回 |
| CP6 | 首次完成关卡维护 |
| CP7 | native 环境成功关闭 |

统计包括帧数、呈现数、跳过数、重建数、获取超时数、fps、世界和界面状态、可见区块、编译队列、Java 堆占用；重载和浸泡同时观察堆外/native 内存及临时清理队列。

每步证据保存在 `build/verification/takeover/<阶段>/`。调用点审计表覆盖 `getDevice()`、`tryGetDevice()`、`getDynamicUniforms()`，分类为上游已切、已改写或保留路径不可达，并写明依据。

### 6.2 脚本化自检

进入世界 20 秒后在客户端线程依次执行，各项之间至少继续运行 2 秒：

1. 调整为 640×360、1281×721、1920×1080。
2. 最小化 3 秒后恢复。
3. 全屏再切回窗口。
4. 资源重载并等待完成。
5. 调用截图入口。
6. 打开 F3 10 秒后关闭。
7. 关闭 VSync、帧率设为不限，记录 5 秒后恢复 VSync。
8. 退出世界并重进同一世界。
9. 正常调用 `Minecraft.stop()`。

最小化期间 skipped 增长，恢复后 presented 继续增长。帧率记录同时注明后端、刷新率和请求的呈现模式；不能由帧率单独断言实际呈现模式。

### 6.3 M5 总验收

1. 开关开启、快速进世界并执行自检的一次完整运行，CP1–CP7 按顺序出现，退出码 0。
2. 该次日志不含：
   - `Can't getDevice() before it was initialized`
   - `Can't getDynamicUniforms() before device was initialized`
   - 以 `takeover:` 开头的异常
   - `Shutdown failure!`
   - `Caught error loading resourcepacks`
   - `Failed to load required shader programs`
   - `Batching sections`
3. 类加载日志无 `com.mojang.renderpearl.backend.`、`com.mojang.renderpearl.frontend.` 类。
4. 默认以视距 12 验收：进世界 60 秒内可见区块数大于 0；视角不动时编译队列在 120 秒内归零并保持。
5. 单独执行 10 分钟浸泡：取每分钟堆占用最小值，第 10 分钟相对第 2 分钟增长不超过 64 MiB。
6. 调用点审计表没有未分类项。
7. 开关关闭时 `build`、`test`、`nativeTest` 通过，预览就绪标记出现。
8. `python tools/check_docs.py`、`git diff --check` 通过。
9. 使用外部窗口捕获逐界面及世界画面对比，按本轮明确排除的功能判读；不使用 F2。

**不引用旧第 5 节第 5 条或旧 S7 的硬性帧率门槛。** VSync 关闭、帧率不限时低于刷新率两倍，需要调查并上报，不因此判失败。

如果 backend/frontend 类仅因 JVM 校验被加载，先记录触发来源并确认是否初始化，向用户报告后再调整判据，不自行忽略。

## 7. 实施前核实项与验证边界

这些项目是阶段开始前需要消除的技术事实，不授权改变上述方案：

| 阶段 | 核实内容 |
| --- | --- |
| N1 | 后备缓冲 RDG 导入、初末状态、获取与呈现同步对象；无图形 API 标志的 SDL 窗口；双后端重建与关闭 |
| M1 | null 接收者包装、构造分支表达式；真实合成方法；DeviceType 映射；Fabric gametest 注入目标；未读构造链 |
| M2 | NativeImage 像素接口；BitmapProvider GPU 缓冲创建；四类字形上传调用方；每帧 CPU 清理 |
| M3 | feature renderer 关闭链；资源重载所有权；停止事件之后仍可能出现的帧调用 |
| M4 | gizmo CPU 行为；上传回调与 uploaded 标记；被切绘制方法的非绘制副作用 |
| M5 | 自检实际入口、其余 F3 条目、DXGI 全屏、类加载与初始化差异 |

主要风险与处理：

- 漏切设备调用：按启动顺序定位，补审计，不吞异常。
- 获取后无法安全返回：全部可恢复准备前移，成功获取后必 Present。
- 隐藏窗口仍可能被 Vulkan 获取：Raynium 在获取前显式判断。
- BGRA8 测试无法导入或发生双重交换：统一格式映射，读回只归一化一次。
- 重载内存峰值：staging 按需增长，完成后归还，观察堆外内存。
- 动画 CPU 开销：只更新变化精灵，增加 Tracy 区间。
- 原版 CPU 清理被跳过：逐方法核对，并用队列和浸泡验收。
- 呈现模式回退：记录请求模式与实测结果，不伪称实际模式已确认。

获取超时目前没有稳定运行复现方法。代码审查和超时计数不能替代运行覆盖，最终验证记录必须保留这一缺口。

本轮完成后的已知缺口：物品图标留空，世界只有地形，菜单不模糊、标题无全景图、截图不可用；依赖被跳过的 Fabric 世界渲染事件或 GUI mesh 钩子的模组不出图。

## 8. 存档执行

1. 新增本文，注明实际归档日期及来源。
2. 在 `doc/history/README.md` 的计划与设计快照中新增索引。
3. 保持已有历史和两份源附件原样，不把未来设计写入当前事实页面，不记录 Git hash。
4. 运行 `python tools/check_docs.py` 和 `git diff --check`。
5. 汇报实际检查结果，然后按用户追加授权从前置基线与 N1 开始实施。

本文保存后冻结。实现进度与实际验证不回写此快照；证据放在忽略目录，当前事实按源码同步。
