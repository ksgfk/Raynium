> - 适用: 定位 Raynium 的游戏入口、命令录制与 native 帧调度
> - 权威: 本文维护当前实现地图与文档索引
> - 锚点: `src/client/resources/raynium.client.mixins.json`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java`, `src/client/java/dev/raynium/client/world/*.java`, `src/client/java/dev/raynium/client/config/RayniumConfig.java`, `src/client/java/dev/raynium/diagnostics/RayniumTelemetry.java`, `src/harness/resources/fabric.mod.json`, `native/src/modular_frame_pipeline.cpp`, `native/src/world_features.cpp`, `native/radray.lock.json`

# Raynium 架构地图

MC 26.3 负责窗口生命周期、世界更新、渲染顺序和资源管理。Raynium 向 RenderPearl 提供 Windows D3D12 或 Vulkan 后端，在原版地形绘制点按几何页提交驻留区块。默认资源支持主世界刚体模型和四边形粒子的实例化绘制。

| 从哪里开始 | 数据流与职责 |
| --- | --- |
| `RayniumConfig`、`RayniumBackendMixin`、`RayniumGpuBackend` | 核心 Mixin 替换候选后端。Raynium 在加载后端时读取配置并初始化环境，随后创建设备和 renderer。原版参考入口仅加载测试模组并优先选择 Vulkan。 |
| `RayniumGpuDevice`、`RayniumPrograms`、`BuiltinShaderPack` | 从实际设备取得能力，后台准备着色器及内置伴生实例化变体，在设备线程注册每条管线的固定状态。 |
| `FrontendCommandEncoder`、`FrontendRenderPass` | 原版校验和整理绑定。后端将即时绘制、纹理操作、时间戳、地形、常驻世界快轨及上屏顺序写入同一批次。 |
| `RetainedTerrain`、`TerrainFeature` | 拦截原版地形暂存写入与二次准备，每次地形准备产生独立视图，在原版当前光栅 Pass 按页直接或间接绘制驻留区块几何。 |
| `RetainedModelManager`、`ModelFastTrack`、`ParticleFastTrack`、`ShadowFastTrack`、`world_features` | 在主世界准备绘制组时筛选可实例化的模型和粒子，执行组时录入当前命令带。实体阴影也有实例编码与准入检查，默认包缺少对应变体，游戏使用原版阴影组。[Shader pack](shader-pack.md) |
| `RayniumTelemetry`、`ClientFrameEvent`、`TerrainFrameEvent` | 位于 `dev.raynium.diagnostics` 的底层观测设施，承载 JFR 帧事件和零分配遥测回调，供核心层无耦合上报给可选测试驱动。 |
| `RenderChannel`、`TapeCompiler`、`ModularFramePipeline` | 双槽批次通过单生产者单消费者通道提交，在帧槽内编译命令并按序构建 RadRay RDG，管理帧槽、资源在途寿命、Fence、读回及呈现。 |
| `HarnessClient`、`VisualValidation`、`BenchmarkSession`（`src/harness`） | 独立开发/测试模组通过 `RayniumHarnessBridge` 可选对接核心，在原版或 Raynium 后端采集截图和性能数据。Python 驱动比较各次运行的截图。 |

## 文档索引

| 主题 | 权威入口 |
| --- | --- |
| native 设备会话、所有权、关闭及能力边界 | [Native runtime](native-runtime.md) |
| shader pack、离线编译与资源来源 | [Shader pack](shader-pack.md) |
| 当前 C ABI、FFM、payload 与加载 | [互操作与加载](interop.md) |
| 完整地形数据、纹理上传与绘制 | [地形渲染](terrain-rendering.md) |
| Fabric 生命周期和当前 MC 接入范围 | [Minecraft 接入](minecraft-integration.md) |
| MC 26.3 原版渲染、纹理 API 与源码节点 | [Minecraft 渲染流程](minecraft-rendering.md) |
| 模块分层、配置命名与 Java/C++ 编码规范 | [编码与工程规范](../guide/conventions.md) |
| 依赖、构建、测试、启动、stage 和分发 | [构建与测试](../guide/build-test.md) |
| 工具链、IDEA 与 MC 源码生成 | [开发环境](../guide/dev-environment.md) |
| 文档归属、事实边界、引用和历史规则 | [文档维护](../guide/documentation.md) |
| 历史计划、开发验证与采样记录 | [历史索引](../history/README.md) |
| 项目文档维护流程 | [raynium-doc](../../.agents/skills/raynium-doc/SKILL.md) |
