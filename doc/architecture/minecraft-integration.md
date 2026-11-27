> - 适用: 修改后端选择、窗口与表面生命周期、地形切点、世界快轨切点与验证入口
> - 权威: 本文记录 MC 26.3 与 Raynium 的接入点
> - 锚点: `src/client/resources/raynium.client.mixins.json`, `src/harness/resources/raynium-harness.mixins.json`, `src/client/java/dev/raynium/client/mixin/backend/RayniumBackendMixin.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuBackend.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuSurfaceBackend.java`, `src/client/java/dev/raynium/client/world/*.java`

# Minecraft 接入

## 生命周期与入口

`Minecraft` 构造时，`RayniumBackendMixin`（`mixin/backend/`）加载 `<gameDir>/config/raynium.json`（及 `-Draynium.*` 内存覆盖），并用单个 `RayniumGpuBackend` 替换待尝试的图形后端。后端加载阶段检查 JVM native access 权限、定位并验证 DLL，按配置中的 `backend` 与 `rhiValidation` 初始化环境。游戏通过 SDL 创建窗口，设备创建阶段建立 runtime、renderer 和 `RayniumGpuDevice`。窗口表面使用该 SDL 窗口的 HWND。关闭顺序是表面、设备、后端库。

核心模组 `raynium` 不注册 Fabric entrypoint，也没有运行时参考模式开关；所有 26 个核心 Mixin 按子系统分置在 `dev.raynium.client.mixin.{backend,terrain,world}`。原版参考入口 `runClientReference`（以及 `tools/benchmark.py` / `tools/validate_visual.py` 的参考模式）仅挂载独立开发/测试模组 `raynium-harness`（`src/harness`），不把核心 `raynium` 放入类路径，并以 `--graphicsBackend VULKAN` 启动原版后端。[构建与测试](../guide/build-test.md)

## 录制与呈现

MC 的实体、天空、GUI、画中画、光照图、动画图集与后处理继续使用自己的 RenderPass、管线、附件和视图。`RayniumGpuDevice` 提供 `FrontendCommandEncoder`，后端在绘制点保存当前管线、窗口区域、绑定、动态偏移与 Push Constant。时间戳、跨资源复制和最终上屏随实际调用位置进入批次。

RenderPearl 编码器复用单个 `RayniumRenderPassBackend` 光栅录制器及其绑定数组，保持 `FrontendRenderPass.backend` 为 `final` 且全 JVM 仅此一处 `RenderPassBackend` 实现。Pass 关闭时清除已使用的绑定和裁剪状态，外部入口通过只读 `FrontendRenderPass.isClosed` 拒绝已关闭的 Pass。

`acquireNextTexture` 的前端获取状态可以跨多个离屏提交。只有记录上屏 blit 的批次携带 surface。主线程轮询工作线程发布的进度，按交换链代数保存需要重建的状态。后续 configure 排空在途呈现并重建交换链。终态通过共用路径释放前端获取状态。截图和图集导出使用 MC 自身的纹理复制、只读映射与 Fence 路径。

## 地形、世界快轨与帧事件

编译、索引采用和区块移除切点位于 `mixin/terrain/`，维护驻留数据；`SectionUploadMixin` 与 `UberBufferDrainMixin` 拦截原版地形 98 MiB `StagingBuffer` 写入与冗余排出，`LevelTerrainHookMixin` 在 `LevelRenderer.prepareChunkRenders[Indirect]` 入口完成视图准备并短路原版扫描。原版 `renderLayers` 切点（`TerrainAnchorMixin`）在当前打开的 Pass 插入地形命令，记录实际图集、lightmap 和 OIT 输入。随后游戏可继续在同一组附件绘制即时几何。[区块地形渲染](terrain-rendering.md)

主世界实例化切点位于 `mixin/world/`，在 Raynium 后端按原版提交组录制：

- `LevelWorldFeatureScopeMixin` 将 `WorldModelCaptureScope` 严格限定在 `LevelRenderer` 主世界 `featureFrame` 区间内（GUI/画中画/手持物沿用即时路径）。
- `ModelPartCompileMixin` 在同一世代第二次访问 `ModelPart` 时缓存 24 B/顶点局部几何；`ModelManagerReloadMixin` 在模型重载时清理并退役旧常驻网格。叶节点在可见性检查处处理恒等或纯平移快轨，其余节点沿用原版姿态栈与子节点遍历。常驻编译未接管时，原版直接调用带原始整型参数的 `compile`。GC 队列返回空闲句柄，允许重排的组按准备后的材质值查桶，并按顶点页聚集网格 run。组执行结束后解除扩展引用。实例桶只复用实际容量不超过 256 个实例的桶，最多保留 256 个。分组工作数组由渲染线程共享，持续低使用量或模型重载时回收。
- `ModelFeatureRendererMixin`、`RenderTypeGroupMixin` 与 `RenderTypeFeatureRendererMixin` 在满足纯刚体模型提交条件且目标管线存在内置伴生实例化变体时，将 `ModelPart` 提交编码为 80 B/实例流并按原版组顺序发出 Opcode 16；遇到覆写 `renderToBuffer`/`compile`、贴花/轮廓/自定义消费者或资源包着色器覆盖时无缝保序回退原版 `StagedVertexBuffer`。
- `QuadParticleRenderStateMixin` 与 `QuadParticleFeatureMixin` 为匹配内置粒子实例化变体的四边形粒子组跳过 CPU 顶点展开，写入 56 B/粒子四元数实例流并经 Opcode 16 合批绘制；各组在准备时计算图层资格。执行时将原版 float/int 数组写入当前命令带。同一命令带的 OIT 阶段复用上传，切换命令带后重传。组执行结束时释放紧凑批次、纹理表和外层组列表。非默认状态或着色器覆盖自动回退原版路径。
- `ShadowFeatureRendererMixin` 在原版 `buildGroup` 切点采集阴影片，保留 `RenderType.prepare()` 产生的视图偏移变换、裁剪和纹理。每片使用 56 B 实例，四角共享单位几何。组执行时选择当前透明阶段，着色器覆盖或缺少所需变体时沿用原版组构建。

核心 `MinecraftFrameMixin`（`mixin/backend/`）在 `Minecraft.renderFrame` 边界记录 JFR `dev.raynium.ClientFrame` 事件。测试驱动 `raynium-harness` 中的 `BenchmarkMinecraftMixin` 与 `VisualValidationMinecraftMixin`（`src/harness`）由 `HarnessMixinPlugin` 按 `-Draynium.benchmark.config` / `-Draynium.validation.config` 条件加载，在世界帧推进基准或视觉采集并通过 `RayniumHarnessBridge` 读取可选核心遥测。测试模式可冻结时间、相机与动画并采集实际选用的管线；管线身份、视觉证据与离线产物通过覆盖率脚本关联。[Minecraft 渲染流程](minecraft-rendering.md)
