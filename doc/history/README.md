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
| 2026-11-21 | [Raynium 第三阶段：统一 MDI 场景管线与常驻世界（地形、刚体实例与紧凑粒子流）实施计划](2026-11-21-phase3-mdi-retained-world-plan.md)：基于上游 `RadRay` 已落地的 `O1`～`O5` 与 `M1`～`M3` 基线、阶段二基准采样归因及 MC 26.3 渲染源码审计定稿的阶段三实施计划；确立世界几何池开启 `BufferUse::Resource` 并走池级 RDG 读依赖、将 `TerrainResidency` 与 `TerrainFeature` 全面收编进 `RadRay::Scene` + `MeshDrawCommand` MDI（OIT 三阶段共享单份 `PreparedMeshPassIndirectDraws` 与单块 RDG 间接参数缓冲，清退 `frame_record.*` 与私有 `terrain_draw` 状态排序表）、切断 Java 侧 `SectionRenderDispatcher` 往原版 `98 MiB` 持久映射 `StagingBuffer` 的无用写入与 `LevelRenderer.prepareChunkRenders[Indirect]` 二次加锁扫描、落地严格限定于 `LevelRenderer` 主世界 `featureFrame` 作用域的 `ModelPart` 24 B 局部网格常驻池 + 80 B 位级精确轴向法线基向量实例流快轨（覆盖实体与方块实体，不满足准入条件自动保序回退 `StagedVertexBuffer`）、落地不入静态图元树的 `QuadParticleFeatureRenderer` 56 B/粒子紧凑四元数实例流快轨，以及在 Headless 双后端单测通过后冻结 ABI 20 |
| 2026-11-22 | [阶段三后全局 CPU 性能优化计划：从即时态多跳翻译到常驻控制面直通](2026-11-22-phase3-cpu-performance-optimization-plan.md)：基于阶段三（`p3-v2`，ABI 20）全量基准测试归因与源码审计制定的四大支柱（`P1`～`P4`）全局 CPU 优化计划；涵盖地形跨语言 32 位稠密槽位直寻址、确定性 `CustomStateSortKey` + 按页分桶收集 100% 命中 `0-sort` 短路、128 MiB 大页与独立 `TranslucentVertices` 几何池消除经典透明层 1,400+ 次跨页断批、动态实例流 `BorrowedBytes` 零拷贝直通与粒子直写堆外、刚体模型法线基向量矩阵列直读与 `FastBucket` 池化/同页 MDI、`ImmediatePass` 级几何池依赖单次去重声明、`ShaderResourceBindings` 栈上 `InlineVector` + 64 位位掩码过滤、`CommandTape` 去清零与 $O(1)$ 资源保活戳记，以及同步上游 `RadRay` 热路径瘦身（`C1`～`C5`） |
| 2026-11-22 | [阶段三第二轮全局 CPU 与跨页合批深度优化计划](2026-11-22-phase3-round2-global-cpu-and-batch-consolidation-plan.md)：基于第一轮 CPU 优化（`phase3-cpu-opt`）四场景实测采样与全链路源码剖析制定的第二轮四大支柱（`S1`～`S4`）优化计划；协同上游 `RadRay` `R1`～`R3`，涵盖 `_quadIndexPool` / `_translucentIndices`（128 MiB） / `_translucentVertices`（256 MiB）物理池隔离与 `TrimEmptyPages()` 空页显存回收（根治航线中后段 1,400～2,269 次半透明 `DepthFirst` 跨页断批）、地形网格 6 向法线分析前移至 Java 后台编译线程池、`CompiledSectionMesh` 就绪层掩码缓存 + Native 相机块级 6 向可见掩码缓存与稳态 `TerrainViewDrawData` 跨帧复用、`QuadParticleRenderState.Storage` 入库期 56 B 原生布局直写与 `MemorySegment.copy` 块拷贝、`ModelPart` 叶节点剪枝/纯平移免 `PoseStack` 克隆与 `GroupExtension` 按 `RenderType` 引用身份 $O(1)$ 查桶，以及 `ImmediatePassStateCache` 光栅 Pass 级跨命令 RHI 状态绑定去重 |
| 2026-11-24 | [系统热点采样复盘与非 MC 拓展项优化计划](2026-11-24-system-hotspots-optimization-plan.md)：基于六场景 30 轮（原版 Vulkan、无 profiler 双后端、Tracy + JFR 双后端）全矩阵系统热点采样的归因复盘与非 MC 拓展边界优化计划；明确排除继续向 MC 原版其他渲染特性扩张接管的项，围绕 `RayniumRenderPassBackend` 单实例复位复用、`NativeBindings` 全量 `invokeExact` 化与 `NativeRenderer` 固定描述符槽、`NativeBuffer` 持久写映射瘦身、`ModelPartCompileMixin` `@WrapMethod` 消除 `CallbackInfo` 逃逸、Native `BufferRegistry` 围栏安全延迟缓冲回收池（消除 `StagedVertexBuffer` 锯齿创建/销毁震荡）、`TerrainCapture` $O(1)$ 世代戳去重与局部脏刷新扩容、`512 MiB` 半透明顶点池单页收敛与跨页非交叠归并，以及上游 `RadRay` Vulkan `UploadArena` 主存策略、D3D12 Tracy GPU 光栅区间和基准 `nativeSyncWaitNs` 独立计时制定分批实施与验收方案；归档时尚未实施 |
| 2026-11-24 | [C++ SRP 渲染管线基座、纯 HLSL/DXC 着色器系统与原版管线实施计划](2026-11-24-cpp-srp-and-hlsl-shader-system-plan.md)：确立基于 `RadRay::RenderGraphFramePipeline` 的 Native C++ 类 Unity SRP 基座（不可变交错保序 `WorldDrawStream` + 每 Pass 独立 `PassDrawState` 的 `RendererList` 收编既有地形、实例化与动态绘制）、`.shader.json` + 纯标准 HLSL 2021 与 80B 包头的专用 DXC `.rnsp` v2 封包（按 `BindingGroupLayout*` 匹配 `PerView` 参数集、家族预反射与字节码池体积去重，前移材质原型 ABI 至 M2 并按着色器对覆盖静态与 `PostChain` 动态管线，清退 GLSL 工具链）、剥离主相机旋转的三层场景输入与 `MaterialTagTable` 开放标签过滤协议，以及经 `RayniumDrawSink`/`WorldCollectorRenderPass` 落地的 `VanillaRenderer`（含单次采集三阶段重放及船/云深度对齐的 `VanillaOitFeature`、严守 `blitEntityOutline` 合成时机的轮廓特性与两档视觉验收准则）；归档时尚未实施（同日开工前审校修订） |
| 2026-11-24 | [专用 Native 渲染线程架构审计与实施计划](2026-11-24-dedicated-render-thread-plan.md) 记录 ABI 23、工作队列、内存所有权、完成水位及双后端验收的三阶段实施计划。 |
| 2026-11-25 | [渲染线程与新版 RadRay RHI 协同收敛重构计划](2026-11-25-render-thread-rhi-convergence-plan.md)：针对专用渲染线程落地暴露的显式等待尾延迟、空闲 1 ms 轮询、冷路径创建屏障与旧版读回/交换链绕行，基于 2026-11-25 锁定版 `RadRay` 设计前端直连 `Fence` 与 `ceiling` 安全上界协议、类型化只移动队列、缓冲/纹理/管线零屏障创建、ABI 24 `host_coherent_atom_size` 与 `GpuUploadBuffer` 接入、单轨 `OnFrameSlotRetired` 零分配遥测、交换链原地自愈及 `RadRay` 后续基础设施评估；归档时尚未实施 |
| 2026-11-25 | [直连句柄、双槽 SPSC 帧通道、FrameSlot PMR 单遍编译与全链路精简收敛计划](2026-11-25-direct-handle-spsc-pmr-convergence-plan.md)：基于 ABI 24 工作树与同日锁定版 `RadRay` 定稿的全仓瘦身与提速计划（`D1`～`D12`）；涵盖 ABI 25 不透明资源指针句柄（清退 6 张双端哈希表与队列闭包，合并 `BufferPool` 与类型化 `PendingResourceOps`）、常驻双槽 `BatchEnvelope` + 单槽 SPSC `RenderChannel` + Cache-line 对齐原子水位线（清退 `FramePacket`/`FrameQueue`/`RenderThreadSession`）、`FrameSlot::GetMemoryResource()` 单遍扁平 `FrameTapePlan`（清退堆 `variant` AST 与 `ImmediatePass` 并内联读回预留）、`AsyncReadbackQueue` 单次拷贝截图回读、必填地形预分析 `layout`（清退 C++ `AnalyzeTerrainMesh` 与 Java 堆列表）及 `visible_version`/`origins_version` `O(1)` 判等、`RayniumTransientMemory.upload` 免 `Arena` 直写、`RayniumRenderPassBackend` 去 `Lease`/去 11 平行数组单遍直录、单轨 `InFlightBatch` 队列、`MeshPayload` 无锁固定计数及 Native 残留死代码清理；归档时尚未实施（同日开工前追加修订） |

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
| 2026-11-25 | [专用渲染线程实施与验证](2026-11-25-dedicated-render-thread-implementation.md) 记录实现中发现的问题、修正和实测结果。 |
| 2026-11-22 | [阶段三实施、问题排查与验证记录](2026-11-22-phase3-mdi-retained-world-notes.md)：R3.1–R3.5 落地摘要、ABI 20 冻结、12 项执行问题与解决方案（含 `TerrainResidency` 4 项性能优化与息屏 D3D12 `DXGI_STATUS_OCCLUDED` 边界说明）、86 场景视觉回归及 Release Vulkan 性能对照 |
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
