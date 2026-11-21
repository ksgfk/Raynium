> - 适用: 定位 Raynium 的游戏入口、命令录制与 native 帧调度
> - 权威: 本文维护当前实现地图与文档索引
> - 锚点: `src/client/resources/raynium.client.mixins.json`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java`, `src/client/java/dev/raynium/client/world/*.java`, `native/src/modular_frame_pipeline.cpp`, `native/src/world_features.cpp`, `native/radray.lock.json`

# Raynium 架构地图

MC 26.3 继续负责窗口生命周期、世界更新、渲染顺序和资源管理。Raynium 向 RenderPearl 提供 Windows D3D12 或 Vulkan 后端，在原版地形绘制点按几何页提交驻留区块，并在主世界提交路径为刚体模型、四边形粒子与实体圆形阴影提供实例化绘制。

| 从哪里开始 | 数据流与职责 |
| --- | --- |
| `RayniumBackendMixin`、`RayniumGpuBackend` | 原版选择后端时创建 SDL 窗口、独立 RHI 设备和 renderer。参考模式沿用原版 Vulkan。 |
| `RayniumGpuDevice`、`RayniumPrograms` | 从实际设备取得能力，后台准备着色器及内置伴生实例化变体，在设备线程注册每条管线的固定状态。 |
| `FrontendCommandEncoder`、`FrontendRenderPass` | 原版校验和整理绑定。后端将即时绘制、纹理操作、时间戳、地形、常驻世界快轨及上屏顺序写入同一批次。 |
| `RetainedTerrain`、`TerrainFeature` | 拦截原版地形暂存写入与二次准备，每次地形准备产生独立视图，在原版当前光栅 Pass 按页直接或间接绘制驻留区块几何。 |
| `RetainedModelManager`、`ModelFastTrack`、`ParticleFastTrack`、`ShadowFastTrack`、`world_features` | 仅在主世界 `featureFrame` 作用域内将合格刚体 `ModelPart`（24 B 局部网格 + 80 B 实例）、粒子和阴影片（各 56 B/实例）合批送入 `CommandTape`；不满足条件或着色器被覆盖时保序回退原版即时路径。 |
| `rn_submit_frame`、`ModularFramePipeline` | 按命令顺序构建 RadRay RDG，管理帧槽、资源在途寿命、Fence、读回及呈现。 |
| `VisualValidation`、`BenchmarkSession` | 用相同场景驱动原版和新后端，单独保存截图差异和性能数据。 |

## 文档索引

| 主题 | 权威入口 |
| --- | --- |
| native 设备会话、所有权、关闭及能力边界 | [Native runtime](native-runtime.md) |
| shader pack、离线编译与资源来源 | [Shader pack](shader-pack.md) |
| 当前 C ABI、FFM、payload 与加载 | [互操作与加载](interop.md) |
| 完整地形数据、纹理上传与绘制 | [地形渲染](terrain-rendering.md) |
| Fabric 生命周期和当前 MC 接入范围 | [Minecraft 接入](minecraft-integration.md) |
| MC 26.3 原版渲染、纹理 API 与源码节点 | [Minecraft 渲染流程](minecraft-rendering.md) |
| 依赖、构建、测试、启动、stage 和分发 | [构建与测试](../guide/build-test.md) |
| 工具链、IDEA 与 MC 源码生成 | [开发环境](../guide/dev-environment.md) |
| 文档归属、事实边界、引用和历史规则 | [文档维护](../guide/documentation.md) |
| 历史计划、开发验证与采样记录 | [历史索引](../history/README.md) |
| 项目文档维护流程 | [raynium-doc](../../.agents/skills/raynium-doc/SKILL.md) |
