> - 适用: 浏览已归档的设计计划、源码调查、开发验证、运行与采样记录
> - 权威: 本文只维护历史导航，可随新增记录更新；记录按原日期与来源理解，不构成当前技术契约
> - 锚点: `doc/history/*.md`, `doc/guide/documentation.md`

# Raynium 历史索引

当前实现与文档入口见[架构地图](../architecture/overview.md)，当前操作见[构建与测试](../guide/build-test.md)。
已有历史记录和附件保持原样；本索引可更新，归档规则见[文档维护](../guide/documentation.md#开发历史)。

## 计划与设计快照

| 日期 | 记录内容 |
| --- | --- |
| 2026-10-26 | [渲染方向与实现方案思考记录](2026-10-26-rendering-design-notes.md)：窗口接管到原版保留的方向变化，runtime/互操作/MC 接入草案及 RT 显示候选 |
| 2026-10-30 | [可编程渲染 Runtime](2026-10-30-programmable-runtime.md)：确认的独立静态库、最小 RDG、SOLID 管线与程序化验收范围 |
| 2026-11-04 | [RadRay 新版 Runtime 接入与重写计划](2026-11-04-radray-runtime-rewrite-plan.md)：升级 ABI 9、持久 Scene 增量命令缓存、Group 0/1 绑定拆分、延迟实例流与 RetireResource 资源退役 |
| 2026-11-04 | [Tracy 实机性能瓶颈分析与分项优化计划](2026-11-04-tracy-bottleneck-optimization-plan.md)：实机 CPU/GPU 耗时归因、移除每帧事务回滚深拷贝的确认决策及后续分项优化候选 |
| 2026-11-05 | [帧内 GPU 资源持久化与几何分配计划](2026-11-05-persistent-gpu-resources-plan.md)：帧内 D3D12 资源创建/销毁的实测归因，每槽持久帧资源与 staging、GeometryArena 方案，以及块暂不回收、一次实施的用户决定 |
| 2026-11-09 | [Java 渲染准备与 Native 可见批次开销重构计划](2026-11-09-java-native-overhead-refactoring-plan.md)：基于 26,831 帧 JFR + Tracy 联合采样的分配/GC 与 TimSort 归因，确认 C ABI `element_mask` 直传、稳态零排序与堆外直写重构方案 |
| 2026-11-10 | [原版渲染切断与 Raynium 完全接管：执行计划](2026-11-10-vanilla-render-cutover-plan.md)：放弃渐进替换与 RenderPearl 后端方案，`RenderSystem` 设备保持为空；面向执行者的硬约束与停止条件、身份句柄与改写配方、阶段 1 的 S0–S8 步骤与逐步验收、总验收、风险规避表，以及后续逐项接入顺序（2026-11-11 提交前修订） |
| 2026-11-12 | [渲染接管、GUI 与纹理接入最终计划](2026-11-12-render-takeover-gui-texture-final-plan.md)：整合逐轮审查；正式帧入口获取前准备与窗口判断、BGRA8 输出及读回、CPU 图集与动画、GUI 和三层地形、N1–M6 阶段与逐项验收；归档时尚未实施 |
| 2026-11-19 | [RadRay 与 Raynium 五阶段协同演进总体计划](2026-11-19-radray-raynium-coevolution-master-plan.md)：面向可维护性与高性能通用架构的探索性总蓝图；确立双轨渲染（RenderPearl 堆外 CommandTape 兼容轨 + 常驻世界快轨）、三通道 C ABI、插件化 IRenderFeature/FrameBlackboard，以及 RadRay 从资源/PSO/参数池底座到 MDI、Bindless、异步流式与硬件光追的五阶段协同路线及中途转向条款 |
| 2026-11-19 | [Raynium 第一阶段：接入上游 RadRay 底座与清理 Native 基础设施样板计划](2026-11-19-phase1-native-runtime-convergence-plan.md)：冻结 Java 生产代码与 ABI 18 外部契约（沿用 `AcquireNext()` 前输入校验与 `RunFrame()` fail-fast 边界，保留 `RN_FRAME_FLIP_Y` 与旧 `.rnsp`/转换场景布局不返工），同步 Debug/Release CMake 包发现、`raynium_shader_pack` 与 Schema 10 解码/负例夹具，并用 `GpuTexture`、`GetOrCreateSampler`、内聚于 `FrameSlot` 的 `TransientParameterSetPool`、四态 `AsyncReadbackQueue` 及按 `MEASURE`/`PROFILE` 分档的内建 GPU 时间戳替换私有样板的实施计划 |
| 2026-11-20 | [Raynium 第二阶段：RenderPearl 接入、时序归一命令流与模块化帧图实施计划](2026-11-20-phase2-command-tape-renderpearl-plan.md)：拆分上游引擎任务后的 Raynium 侧阶段二定稿计划；确立双后端屏幕坐标恒等且无条件统一 `FrontFace::CW` 仅上屏像素级翻行、`compilePipeline` 在所属线程入口拦截不支持的线框填充并经 `supplyAsync(..., taskExecutor)` 异步完成查包/跨组稠密 `uniformIndices` 分配（解决 `CLOUDS` 跨组同号碰号并透传 `CloudFaces` 的 `R8_SINT` `TEXEL_BUFFER` 格式）与反射校验后再回所属线程 `finishCompile` 注册管线（同步迁移 `shader_pack.py` 编译/验包清单与单测，常驻地形覆盖就地告警降级，明确 `LINES`/`QUADS` 归入 `TriangleList` 且非索引 `draw` 原样直调 `Draw` 等全量 8 种拓扑契约）、`USAGE_MAP_READ` 主机内存承载配合 `FrameSlot` 自持 `writeToBuffer` 源字节副本、按槽隔离 `ReadBack` 暂存与按 `CommandTape` 顺序主机发布（严守 FFM 同步返回借用期，兼顾同批保序与跨批零 `nonCoherentAtomSize` 竞争）、`MappedView` 跨轮转自持局部暂存切片、未关闭长驻可写 `PersistentMap` 提交期自动刷新、同资源非重叠拷贝瞬态两阶段拆分、每个逻辑 `RenderPass` 入口显式建立完整附件 `Viewport` 与 `RenderArea` 初始 `Scissor`、按纹理 `initialized` 与用途分流按需零初始化（非附件 `FontTexture` 走 `CopyDestination` 零上传，首次以 `Load` 作附件完整保留既有像素与其他 mip）、设备唯一编码器与打开 Pass 的提交守卫、`createBuffer(ByteBuffer)` 初始上传、原生字节序映射、图集普通采样保留完整 mip 范围、显式单级视图 `lod = 0` 上屏及含 Vulkan 4 字节下限的单平面深度拷贝/读回边界、移除交换链全局提交序号连续递增限制、将同一 `RenderPass` 内联连续录制且自动重绑原管线全量参数集/常量的双准备分支 `view_id` 地形锚点（两张 OIT 系数绑定）/按录制批次独立绑定目标表面的 `CMD_BLIT_TO_SURFACE`/`CMD_WRITE_TIMESTAMP` 统一纳入 `CommandTape`、收敛三层正交故障边界（L1 在 `submit` 与 `wait` 共用 `enterTerminalFailure` 复位表面获取态并在 `Renderer::Finish` / `rn_destroy_*` 丢失态免超时 abort 安全析构）及三态分段等待 `GpuFence`，跑通真实 SDL 窗口（迁入 `createWindow` 与 `SDL_Window* -> HWND` 提取）端到端验证后再冻结 ABI 19，并迁出 `RenderMode.REFERENCE` 后端选择、基准计数、`VanillaPipelineCatalog`/`pipeline_coverage.py` 全路径与地形采集/epoch 生命周期后清退 40 余个接管 Mixin（附阶段三至五修正约束） |

## 源码调查与早期上下文

以下材料于 2026-11-09 从当前文档迁出，沿用原调查日期；未单列日期的段落以来源页首次保存日期标注。
保留原版本范围、源码定位、结论及限制，当前实现以 architecture/guide 为准。

| 原记录日期 | 内容 |
| --- | --- |
| 2026-10-26 | [已有模组 native 接入与加载源码调查](2026-10-26-native-integration-source-survey.md) |
| 2026-10-28 | [Remix 与 Radiance 场景输入源码调查](2026-10-28-scene-input-source-survey.md) |
| 2026-10-26 | [Minecraft 26.3 渲染源码调查与原行号](2026-10-26-minecraft-rendering-source-survey.md) |
| 2026-10-26（来源页首次保存日期；原段未单列日期） | [开发环境中的调查缓存与工程来源说明](2026-10-26-development-environment-context.md) |

## 开发验证与采样记录

以下记录于 2026-11-09 从工作树 `doc/guide/build-test.md` 按原分节归档，文件日期沿用原记录日期。
保留当时的命令、环境、结果、限制及本机证据路径；归档未重跑检查，旧结果不代表当前工作树已经通过。

| 记录日期 | 内容 |
| --- | --- |
| 2026-11-21 | [阶段二控制面、命令流与双后端验收记录](2026-11-21-phase2-command-tape-implementation-notes.md) |
| 2026-11-09 | [Java 帧准备、持久 FFM 缓冲与掩码直传](2026-11-09-java-native-overhead-validation.md) |
| 2026-11-08 | [runtime 状态排序与 RDG 交接接入](2026-11-08-runtime-state-sorting-validation.md) |
| 2026-11-06 | [可见面组批量化](2026-11-06-visible-batches-validation.md) |
| 2026-11-06 | [批量绘制命令升级与采样准备](2026-11-06-mesh-draw-command-validation.md) |
| 2026-11-06 | [命令绑定池升级](2026-11-06-command-binding-pools-validation.md) |
| 2026-11-06 | [缓存命令借用升级与 Tracy 细分](2026-11-06-borrowed-command-cache-tracy-validation.md) |
| 2026-11-06 | [RenderGraphFramePipeline 接入](2026-11-06-render-graph-pipeline-validation.md) |
| 2026-11-05 | [帧内 GPU 资源持久化](2026-11-05-persistent-gpu-resources-validation.md) |
| 2026-11-05 | [依赖升级与深度 Tracy 插桩](2026-11-05-runtime-profiler-upgrade-validation.md) |
| 2026-11-04 | [FrameSlot 资源退役接口升级](2026-11-04-frame-slot-retirement-validation.md) |
| 2026-11-04 | [Tracy 瓶颈增量更新优化](2026-11-04-tracy-overhead-optimization-validation.md) |
| 2026-11-04 | [ABI 9 与持久 Scene 命令缓存](2026-11-04-scene-command-cache-abi9-validation.md) |
| 2026-11-04 | [Release Tracy 缓存路径采集](2026-11-04-release-tracy-cache-capture.md) |
| 2026-11-03 | [Release Tracy 客户端采集](2026-11-03-release-tracy-client-capture.md) |
| 2026-11-02 | [固定 MT 构建](2026-11-02-static-crt-build-validation.md) |
| 2026-11-02 | [动态 CRT 与 mimalloc DLL](2026-11-02-dynamic-crt-mimalloc-validation.md) |
| 2026-11-02 | [FrameScheduler 与 Raynium 专用 FramePipeline](2026-11-02-frame-scheduler-validation.md) |
| 2026-10-30 | [runtime 迁入 RadRay](2026-10-30-radray-runtime-migration-validation.md) |
| 2026-10-30 | [迁移前的可编程 runtime 与 RDG](2026-10-30-programmable-runtime-rdg-validation.md) |
| 2026-10-30 | [统一 mesh draw pipeline 与 ABI 8](2026-10-30-mesh-draw-pipeline-abi8-validation.md) |
| 2026-10-29 | [SOLID 地形与 ABI 7](2026-10-29-solid-terrain-abi7-validation.md) |
| 2026-10-28 | [C++ bridge 与固定底层枚举](2026-10-28-cpp-bridge-enum-validation.md) |
| 2026-10-28 | [单例环境与 ABI 6](2026-10-28-singleton-environment-abi6-validation.md) |
| 2026-10-28 | [D3D12 校验提前启用与 ABI 5](2026-10-28-early-d3d12-validation-abi5.md) |
| 2026-10-27 | [共享预览验证](2026-10-27-shared-preview-validation.md) |
