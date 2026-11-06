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
