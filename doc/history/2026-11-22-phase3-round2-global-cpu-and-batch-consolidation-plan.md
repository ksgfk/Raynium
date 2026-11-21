> - 适用: 执行或评审阶段三第二轮全局 CPU 与跨页合批优化计划（半透明顶点/索引池物理隔离与空页回收消除航线中后段 1,400~2,269 次跨页断批、地形网格 6 向分析前移至编译后台线程、地形帧间可见掩码缓存与稳态 MDI 参数复用、粒子入库 56 B 原生布局直写与实体 ModelPart 叶节点剪枝/O(1) 材质查桶/Pass 级 RHI 状态去重）
> - 权威: 经用户要求保存的阶段三第二轮全局 CPU 优化设计计划，不构成当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)、[地形渲染](../architecture/terrain-rendering.md)、[Native runtime](../architecture/native-runtime.md) 与 [互操作与加载](../architecture/interop.md)
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/include/raynium/terrain_residency.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/terrain_feature.cpp`, `native/src/world_features.cpp`, `native/src/immediate_pass.cpp`, `native/src/immediate_draw.cpp`, `src/client/java/dev/raynium/client/terrain/*.java`, `src/client/java/dev/raynium/client/world/*.java`, `src/client/java/dev/raynium/client/mixin/*.java`, `src/client/java/dev/raynium/client/renderpearl/*.java`

# 阶段三第二轮全局 CPU 与跨页合批深度优化计划

- 原记录日期：2026-11-22
- 状态：基于第一轮 CPU 优化（`phase3-cpu-opt`）四场景实测采样（`build/validation/phase3-cpu-opt/measurements/`）与全链路源码剖析制定的第二轮全局优化计划；归档时尚未实施本计划对应的源码改动
- 范围：协同上游 `RadRay` Runtime `R1`～`R3` 计划，覆盖 Raynium 地形几何池隔离与空页回收、网格 6 向法线分析后台化、帧间朝向掩码与 MDI 参数稳态缓存、粒子捕获期 56 B 原生布局直写、实体 `ModelPart` 遍历剪枝与 $O(1)$ 引用查桶，以及 `ImmediatePass` / `WorldFeatures` 光栅 Pass 级状态绑定去重（`S1`～`S4`）
- 来源：承接 [阶段三后全局 CPU 性能优化计划：从即时态多跳翻译到常驻控制面直通](2026-11-22-phase3-cpu-performance-optimization-plan.md)，基于 2026-11-22 当日工作树源码（含第一轮 CPU 优化未提交改动）与 `build/validation/phase3-cpu-opt/measurements/` 实测数据整理

---

## 0. 全局瓶颈复盘：第一轮优化后的水位与四大剩余矛盾

### 0.1 `p3-v2` $\rightarrow$ `phase3-cpu-opt` 与原版 Vulkan 的全景数据对照

在同机同配置（1920×1080，视距 32，Release `/MT`，`rhiValidation=false`）下，第一轮优化前后的 P50 中位数（括号内为 P95）对比如下：

| 场景 | 指标 | `p3-v2` (第一轮前) | `phase3-cpu-opt` (当前) | 原版 Vulkan (`reference`) | 剩余差距与核心瓶颈 |
| --- | --- | ---: | ---: | ---: | --- |
| **Metcalf**<br>*(240s 地形航线)* | 平均 FPS<br>`renderWallNs`<br>Java 侧耗时*<br>`nativePrepareNs`<br>`nativeRdgRunNs`<br>`terrainDrawCalls` | 318.8<br>2.1856 ms (11.12 ms)<br>0.9149 ms<br>0.7712 ms (4.54 ms)<br>0.3683 ms (1.16 ms)<br>121 (P95: 1,458) | **440.6** *(+38.2%)*<br>**1.4876 ms** (6.23 ms)<br>**0.6978 ms**<br>**0.4655 ms** (1.92 ms)<br>**0.2139 ms** (0.58 ms)<br>**120** (P95: **1,474**, P99: **1,979**) | **~600+**<br>**1.1536 ms** (2.23 ms)<br>—<br>—<br>—<br>— | 前 20,000 帧仅 `1.18 ms`（Draw=9~13），但**航线中段半透明顶点/索引跨入第 2 页后 DrawCall 暴增至 1,033~2,269**，帧耗时恶化到 `5.7~13.5 ms` |
| **Tideworn**<br>*(240s 高密地形)* | 平均 FPS<br>`renderWallNs`<br>Java 侧耗时*<br>`nativePrepareNs`<br>`nativeRdgRunNs`<br>`terrainDrawCalls` | 214.6<br>4.4115 ms (7.86 ms)<br>2.0297 ms<br>1.7421 ms (3.44 ms)<br>0.4567 ms (1.41 ms)<br>35 (P95: 557) | **276.8** *(+29.0%)*<br>**3.1771 ms** (5.43 ms)<br>**1.6029 ms**<br>**1.1194 ms** (2.86 ms)<br>**0.2872 ms** (0.71 ms)<br>**37** (P95: **792**) | **~410+**<br>**2.1729 ms** (4.84 ms)<br>—<br>—<br>—<br>— | `35,257` 条可见地形记录**每帧在 Java 与 Native 无状态全量重扫、重算 6 向剔除并重建 `705 KiB` MDI 参数**，仅此一项每帧固定消耗 `~2.1 ms` |
| **Entities**<br>*(高密实体)* | 平均 FPS<br>`renderWallNs`<br>Java 侧耗时*<br>`nativePrepareNs`<br>`nativeRdgRunNs`<br>`gpuFrameNs`<br>`sceneDrawCalls`<br>`geometryAndPostBindCalls` | 236.0<br>4.0263 ms (5.58 ms)<br>2.3230 ms<br>0.9760 ms<br>0.5560 ms<br>0.6420 ms<br>87 (P95: 263)<br>664 (P95: 1,312) | **287.4** *(+21.8%)*<br>**3.2756 ms** (4.09 ms)<br>**2.1443 ms** *(65.5%)*<br>**0.6123 ms**<br>**0.3577 ms**<br>**0.6258 ms**<br>**87** (P95: **262**)<br>**664** (P95: **1,310**) | **354.0**<br>**2.6521 ms** (4.73 ms)<br>—<br>—<br>—<br>**1.0086 ms**<br>—<br>— | **GPU 比原版快 38.0%**，但主线程 CPU 慢 `0.62 ms`：Java 侧逐 `ModelPart` 克隆 `PoseStack` 与 `PreparedRenderType.equals` 查桶 + `664~1,310` 次即时绑定 + Native 串行回放 |
| **Particles**<br>*(高密粒子)* | 平均 FPS<br>`renderWallNs`<br>Java 侧耗时*<br>`nativePrepareNs`<br>`nativeRdgRunNs`<br>`gpuFrameNs` | 160.0<br>5.1598 ms (6.24 ms)<br>3.5880 ms<br>0.8380 ms<br>0.5330 ms<br>1.5190 ms | **186.9** *(+16.8%)*<br>**4.3752 ms** (5.16 ms)<br>**3.0907 ms** *(70.6%)*<br>**0.6528 ms**<br>**0.4533 ms**<br>**1.5204 ms** | **219.3**<br>**3.7310 ms** (6.09 ms)<br>—<br>—<br>—<br>**1.8463 ms** | **GPU 比原版快 17.6%**，但主线程 CPU 慢 `0.64 ms`：原版 `Storage.add` 写完数组后，`executeGroup` 又在主线程逐字段循环重打包 `827 KiB/帧`（20.5 万次 `MemorySegment.set`） |

> *注：`Java 侧耗时 = renderWallNs - nativeCallNs`。

### 0.2 四大深层根因剖析

1. **根因一：经典半透明 `DepthFirst` 排序在多页几何池下的 MDI 批次雪崩，叠加流式上传期主线程同步 `AnalyzeTerrainMesh`**
   - **时序证据（`metcalf/frames.csv`）**：
     - 第 1～5,000 帧（`terrainDrawRecords = 12,539`）：`terrainAllocatedBytes = 611 MiB`，`terrainIndexCapacityBytes = 64 MiB`（1 页），`terrainDrawCalls = 9`，`nativePrepareNs = 0.31 ms`，`renderWallNs = 1.18 ms`（**~750 FPS**）。
     - 第 38,125～50,000 帧（航线中段高密区，`terrainDrawRecords = 21,861`）：随着 32 区块圆柱视距内所有方向的区块陆续进入视锥并常驻，`terrainAllocatedBytes` 涨至 `2.58 GiB`，`_translucentVertices`（128 MiB/页）跨入第 2 页，同时混放 `_quadIndices` 与所有半透明动态索引 `mesh.indices` 的 `_indices`（64 MiB/页）在第 38,135 帧也跨入第 2 页（`134,217,728 B`）。
     - 由于经典半透明层（`!oit`）必须按 `MeshPassSortMode::DepthFirst` 严格按相机距离由远及近排序，当 ~2,500 个可见半透明 section 的顶点散落在 2 个顶点页、索引散落在 2 个索引页（4 种 `(vPage, iPage)` 组合）时，深度相邻的两个半透明 section 落在同一 `(vPage, iPage)` 对的概率仅 $25\%$。`radray::BuildMeshPassIndirectDraws` 中 `sameVertices && sameIndex` 判定在 $75\%$ 的相邻元素处失败，直接把原本 1 次半透明 MDI 调用撕裂成 **1,033～2,269 次独立 `DrawIndexedIndirect` 调用**！
     - 第 100,000 帧（航线末段）：即便旧区块卸载使 `terrainAllocatedBytes` 回落至 `1.52 GiB`、`terrainDrawRecords` 回落至 `10,275`，因 `GeometryBufferPool` 页只增不减，半透明几何体仍散落在多页中，`terrainDrawCalls` 依然滞留在 `120`（初始帧的 13 倍）。
     - 此外，在航线中段每帧吃满 `8 MiB` `UPLOAD_BUDGET` 的流式上传帧中，渲染主线程在 `TerrainResidency::PrepareFrame` 内同步执行 `AnalyzeTerrainMesh`（对数十万个 28 B 顶点逐 quad 求叉积法线、分类 6 向面组并重排顶点），单帧将 `nativePrepareNs` 推高至 `2.8～4.5 ms`。

2. **根因二：地形控制面每帧“无状态三趟 Java 扫描 + 跨语言全量重写 + CPU 侧全量 6 向剔除与 MDI 参数重建”**
   - 在 `Tideworn` 稳态帧（`terrainUploadBytes = 0`，`35,257` 条记录）中，即使相机仅微动、可见 section 集合完全未变：
     - **Java 侧（`~1.0 ms/帧`）**：`TerrainCapture.capture` 持锁遍历 `visibleSections()`（~9,000 个 section），对每个 section 的 3 个 layer 调用 `compiled.getSectionDraw(layer)` + `isVertexBufferUploaded(layer)` + `isIndexBufferUploaded(layer)`（每帧约 `54,000` 次 `EnumMap` 与 `AtomicBoolean` 访问）；随后 `TerrainResidency.prepare` 再跑两趟循环，每帧从零向 `NativeFrameBuffer` 写入 `16 B * 9,000` 实例与 `24 B * 15,000` 可见记录。
     - **Native 侧（`nativePrepareNs = 1.12 ms/帧`）**：`TerrainResidency::PrepareFrame` 每帧对 `15,000+` 个可见 submesh 重新执行 `TerrainVisibleMask`（64 位整数与 `double` 精度 6 向 AABB 半空间测试）、`GatherVisibleMeshDrawCommands`、`SortVisibleCommands` 与 `BuildMeshPassIndirectDraws`，每帧在 CPU 上重新展开生成 `35,257` 条 `DrawIndexedIndirectArguments`（`705 KiB`）并每帧重新拷贝上传至瞬态 Indirect Buffer。

3. **根因三：`ParticleFastTrack` 与 `ModelFastTrack` 截获点偏晚，Java 主线程存在二次打包与冗余树遍历**
   - **粒子（`particles` Java 侧 `3.09 ms`，占 70.6%）**：原版 `ParticleEngine` 提取粒子时调用 `QuadParticleRenderState.Storage.add` 将 14 个字段写入 `floatValues`（12 floats）和 `intValues`（2 ints）；到 `executeGroup` 阶段，`ParticleFastTrack.packParticlesToSegment` 又在渲染主线程遍历 14,700 个粒子，做字段顺序重排（将 `scale` 从下标 `+7` 移到 `+3`）并执行 **$14,700 \times 14 = 205,800$ 次 `MemorySegment.set`** 写入 `CommandTape`（`827 KiB/帧`），等于在主线程付出了“原版提取 + Raynium 二次逐字段重打包”的双份成本。
   - **实体（`entities` Java 侧 `2.14 ms`，占 65.5%）**：
     - `ModelPart.render` 对每个部件（含 `skipDraw == true`、`cubes.isEmpty()` 的叶节点或无旋转/无缩放的纯平移叶节点）无条件执行 `poseStack.pushPose()`（克隆 `Matrix4f` + `Matrix3f`）、`translateAndRotate` 与 `popPose()`；
     - `GroupExtension.acquireFastBucket` 在合并同材质实例时，对 `entries` 列表做线性扫描并调用 `PreparedRenderType.equals`（深度比较纹理列表与状态字段）；
     - 实体脚下的**圆形阴影（`ShadowFeatureRenderer`）**仍走原版逐 quad CPU 顶点展开与 `StagedVertexBuffer` 即时路径，推高了 `geometryAndPostBindCalls`（P50 = 664，P95 = 1,310）。

4. **根因四：双层串行录制与 `ImmediatePass` / `WorldFeatures` 光栅 Pass 内重复 RHI 状态绑定**
   - Java 走完渲染逻辑写入 `CommandTape` 后，同一主线程在 `rn_submit_frame` 同步执行 Decode、RDG Build/Compile、Prepare（每帧数百至上千次 `AcquireParameterSet`）与 RHI Record。
   - 在 `ImmediatePass::Record` 中，连续录制的 87～262 个 `RetainedModelsFeature` 与 664～1,310 个 `ImmediateDraw` 之间**没有任何跨命令的 RHI 状态缓存**（每次 `Record` 都无条件重调 `BindGraphicsPipelineState`、`SetScissor`、`SetStencilReference(0)`、`BindShaderParameterSet(0)`、`BindIndexBuffer` 与 `BindVertexBuffers`），在驱动层造成大量冗余状态设置。

---

## 1. 四大支柱优化方案（`S1`～`S4`）

### 1.1 支柱一（`S1`）：根治地形半透明跨页断批雪崩与主线程网格分析峰值

#### S1.1 四边面共享索引与半透明动态索引物理隔离 + 半透明大页与空页及时回收

1. **拆分 `_quadIndices` 与 `_translucentIndices`（`native/include/raynium/terrain_residency.h`、`native/src/renderer.cpp`、`native/src/terrain_residency.cpp`）**：
   - 将原混用池拆分为三个各司其职的几何池：
     - `_vertices`（Solid + Cutout 顶点池）：保持 `VertexStride = 28`，`PageVertexCapacity = (128u << 20) / 28`（128 MiB/页）；
     - `_translucentVertices`（Translucent 专用顶点池）：将单页容量提升至 **`PageVertexCapacity = (256u << 20) / 28`（256 MiB/页，可容纳约 958 万个半透明顶点 / 239 万个半透明四边面）**；
     - `_translucentIndices`（Translucent 专用动态排序索引池）：将单页容量提升至 **`PageIndexCapacity = (128u << 20) / sizeof(uint32_t)`（128 MiB/页，可容纳 3,355 万个索引 / 559 万个半透明四边面）**，专供 `RN_MATERIAL_TRANSLUCENT` 的 `mesh.indices`（及按需线框索引）分配；
     - `_quadIndexPool`（静态四边面共享索引池）：独立 `PageIndexCapacity = (16u << 20) / sizeof(uint32_t)`（16 MiB 单页，专供 `_quadIndices` 给 Solid、Cutout、RetainedModels、CompactParticles 共用，彻底杜绝 `_quadIndices` 与高频重排的半透明索引交叉分配产生碎片）。
2. **接入上游 `RadRay` `R1` 的 `GetPageIndex()` 与 `TrimEmptyPages()`**：
   - 删除 `TerrainResidency` 中手写的 `unordered_map<gpu::Buffer*, uint32_t> _vertexPages`、`_indexPages` 及 `_vertexReservedBytes`、`_indexReservedBytes` 累加字段；
   - 在 `TerrainMeshPassProcessor::AddMeshBatch` 与 `TerrainResidency::PrepareFrame` 中，直接通过 `mesh.geometry.GetPageIndex()` 与 `mesh.indices.GetPageIndex()`（$O(1)$ 标量直读）获取页序号并构造 `CustomStateSortKey = (vPage << 16) | (iPage & 0xFFFFu)`；
   - 在 `TerrainResidency::PrepareFrame` 处理完 `f.removed_meshes`（且当前帧槽 `slot.frame` 已在 `BeginFrame` 完成上一轮退役对象的析构归还）后，对 `_vertices`、`_translucentVertices`、`_translucentIndices` 调用 `TrimEmptyPages()`：
     - 一旦流式高峰期的非首分页变空，立即释放其底层 `GpuBuffer` 显存，并使后续新分配重新收敛回 `Page 0`；
     - `MeasureGeometry` 直接通过各池的 `GetCapacityBytes()` 与 `GetAllocatedBytes()` $O(1)$ 汇总上报。
   - **收益**：在 `Metcalf` 与 `Tideworn` 全程 240 秒航线中，所有半透明几何体 100% 落在 `_translucentVertices[Page 0]` 与 `_translucentIndices[Page 0]` 单页内，经典半透明 `DepthFirst` 排序下全程满足 `sameVertices && sameIndex`，**半透明 MDI DrawCall 从 1,400～2,269 次永久收敛为 1 次**！

#### S1.2 地形网格 6 向法线分析（`AnalyzeTerrainMesh`）前移至 Java 区块编译后台线程池

1. **在 `SectionCompiler.compile` 后台线程预计算面组重排与包围盒（`src/client/java/dev/raynium/client/terrain/MeshPayload.java`、`src/client/java/dev/raynium/client/mixin/SectionCompilerMixin.java`）**：
   - `SectionCompilerMixin.raynium$capture` 运行在 Minecraft 的后台编译工作线程池（`SectionRenderDispatcher.executor`）上，完全不占用渲染主线程时间。
   - 在构造 `MeshPayload(long epoch, ByteBuffer source, int mode)` 时，直接在后台工作线程完成原 `native/src/terrain_draw.cpp` 中 `AnalyzeTerrainMesh` 的全部 CPU 密集工作：
     - **对半透明层（`mode == 2`）**：无需重排顶点，仅在拷贝 `vertices` 的单趟循环中按 4 顶点 quad 检验有限浮点数并同步算出局部 AABB `double minX, minY, minZ, maxX, maxY, maxZ`，生成单元素 `TerrainDrawRange{first=0, count=vertexCount/4*6, vertex=0}`；
     - **对 Solid / Cutout 层（`mode == 0 || mode == 1`）**：在后台工作线程单趟扫描每个 quad（112 B），计算法线叉积确定所属面组 `g`（`0..6`）与局部 AABB，统计 `counts[7]` 与前缀偏移 `offsets[7]`，并在写入 `this.vertices` 数组时**直接按面组 `0..6` 顺序落盘重排后的顶点流**，同时打包好 7 个面组的 `(first, count)` 布局头部（及供 Wireframe 恢复原序的紧凑 `originalQuadGroups`）。
2. **Native 主线程 `PrepareFrame` 零重排直传（`native/src/terrain_residency.cpp`、`native/src/terrain_draw.cpp`）**：
   - 当 `MeshPayload` 已携带预分析好的布局与按组重排后的 `vertices` 时，Native `TerrainResidency::PrepareFrame` 无需在渲染主线程分配 `TerrainMeshUpload` 临时缓冲或遍历数十万顶点求叉积重排，仅对预计算的 7 组 `ranges` 和 `AABB` 做 $O(1)$ 合法性校验，随后直接以 **`RDGUploadSource::BorrowedBytes`** 零拷贝引用 `x.vertices` 上传至 `VertexBufferPool`！
   - 保留 `AnalyzeTerrainMesh` 供 C++ 独立单测与未预排输入回退使用。
   - **收益**：彻底消除航线中段每帧 `8 MiB` 上传期的渲染主线程顶点扫描与重排开销，将上传峰值帧的 `nativePrepareNs` 从 `2.8～4.5 ms` 压回 `< 0.6 ms`。

---

### 1.2 支柱二（`S2`）：地形帧间可见性缓存与稳态 MDI 参数直通复用

#### S2.1 Java `CompiledSectionMesh` 就绪层掩码缓存与 `TerrainCapture` 单次位读

1. **在 `CompiledSectionMeshMixin` 中缓存 `raynium$readyLayerMask`（`src/client/java/dev/raynium/client/mixin/CompiledSectionMeshMixin.java`、`src/client/java/dev/raynium/client/terrain/TerrainCapture.java`）**：
   - 在 `CompiledSectionMeshMixin` 中新增 `@Unique private int raynium$readyLayerMask` 与 `@Unique private int raynium$vertexUploadedBits, raynium$indexUploadedBits`：
     - 注入 `setVertexBufferUploaded(ChunkSectionLayer)` 与 `setIndexBufferUploaded(ChunkSectionLayer)`：当对应层 `draws.containsKey(layer)` 且顶点与索引均已标记上传完成时，原子或就地置位 `raynium$readyLayerMask |= (1 << (layer.ordinal() * 8))`；
     - 在 `close()` 时清零 `raynium$readyLayerMask = 0`。
   - 在 `TerrainCapture.capture` 的内层循环（第 91～102 行）中，直接读取 `int mask = owner.raynium$readyLayerMask() & payload.nonNullLayerMask`，**将每帧 `9,000 × 3 = 27,000` 次 `EnumMap.get` + `54,000` 次 `AtomicBoolean.get` 压缩为每 section 1 次整型字段读取**。

#### S2.2 Native 远景 Section 6 向朝向掩码缓存与稳态 `PreparedMeshPassIndirectDraws` 跨帧复用

1. **每个 resident `TerrainMesh` 缓存相机块级 6 向可见掩码（`native/include/raynium/terrain_residency.h`、`native/src/terrain_residency.cpp`）**：
   - 数学不变量：对于轴对齐的 $16 \times 16 \times 16$ 区块网格，`TerrainVisibleMask(mesh.layout, eye, f, instances)` 对 6 个轴向面组（`±X, ±Y, ±Z`）的半空间剔除结果，仅取决于相机眼睛位置 `eye` 相对于该区块局部包围盒 `[minimum, maximum]` 在三个坐标轴上的区间位置（在 `minimum` 左侧、在 `[minimum, maximum]` 内部、或在 `maximum` 右侧）。
   - 当一个区块是单实例（`item.instance_count == 1`）且相机当前所在方块 `f.camera_block` 未跨越该区块包围盒的轴向边界平面（或相机仍处于同一方块 `f.camera_block == _cachedEyeBlock` 且相机所在本区块外的全部区块半空间符号完全不变！）时：
     - 凡是不包含当前相机所在方块的区块（占全场景 99.9% 以上的区块！），其 `eye` 与区块包围盒的相对半空间符号**仅取决于整数 `f.camera_block`（甚至仅取决于相机所在的 $16 \times 16 \times 16$ Section 坐标 `camera_block >> 4`）**，与 `camera_fraction` 亚方块小数偏移完全无关！
   - 实现方案：
     - 在 `TerrainMesh` 中增加缓存字段：
       ```cpp
       int32_t cachedOrigin[3]{};
       uint8_t cachedVisibleMask{0};
       uint32_t cachedEyeStamp{0};
       ```
     - 在 `TerrainResidency` 中维护当前帧的整数方块坐标 `_lastCameraBlock[3]` 与递增戳记 `_cameraBlockStamp`（仅当 `f.camera_block[0..2]` 发生变化时 `++_cameraBlockStamp`）；
     - 在 `PrepareFrame` 遍历 `f.visible` 时，若 `item.instance_count == 1`、`mesh.cachedEyeStamp == _cameraBlockStamp` 且实例坐标 `inst.origin == mesh.cachedOrigin`（且相机不在该区块 `16×16×16` 范围内），**直接以 $O(1)$ 读取 `mesh.cachedVisibleMask`**，完全跳过 `TerrainVisibleMask` 的 64 位坐标换算与 `double` 6 平面浮点包围盒测试！
     - 更进一步：若相机连所属方块 `f.camera_block` 都没变（常见于慢速移动、站立观察、实体/粒子基准场景），全场 99.9% 的区块 100% 命中 `cachedVisibleMask`！
2. **稳态帧 `TerrainLayerDrawData`（`VisibleCommands` + `IndirectDraws` + `indirectReps`）跨帧复用**：
   - 在 `TerrainViewDrawData` 中记录上次成功构建时的视图拓扑指纹：
     - `uint64_t builtEpoch`、`uint32_t builtFlags`、`uint64_t builtSyncStamp`（代表无任何网格新增、删除、透明索引重排或 `_quadIndices` 扩容）；
     - `uint32_t builtVisibleCount` 及在遍历 `f.visible` 时顺带累加的 64 位轻量级序列校验和 ` visibleSignature = Hash(item.mesh, selectedMask, item.first_instance, item.instance_count, item.sort_key)`。
   - 当本帧与该 `TerrainViewDrawData` 上次构建时的 `(builtEpoch, builtFlags, builtSyncStamp, builtVisibleCount, visibleSignature)` 完全一致时：
     - **直接复用 `layerData.drawList.VisibleCommands`、`layerData.drawList.IndirectDraws`（含全部 `IndexedArguments` 与 `Batches`）以及 `layerData.indirectReps`**；
     - 零开销跳过 `GatherVisibleMeshDrawCommands`、`SortVisibleCommands` 与 `BuildMeshPassIndirectDraws` 三大阶段！

---

### 1.3 支柱三（`S3`）：粒子捕获期 56 B 原生直写与实体 `ModelFastTrack` 深度瘦身

#### S3.1 `QuadParticleRenderState.Storage` 入库期直接生成 56 B 实例流（消除 `executeGroup` 二次遍历转换）

1. **在 `QuadParticleRenderState.Storage` 中维护与 Raynium 56 B 实例布局 100% 字节同构的紧凑数组（`src/client/java/dev/raynium/client/mixin/QuadParticleRenderStateMixin.java`、`src/client/java/dev/raynium/client/world/ParticleFastTrack.java`）**：
   - 在 `QuadParticleRenderState.Storage` 上通过 Mixin（或接口扩展 `ParticleFastTrack.RawStorageAccess`）维护一个 `int[] raynium$packedInts`（初始容量 `1024 * 14`，随 `grow()` 同步扩容）：
   - 在 `Storage.add(x, y, z, xRot, yRot, zRot, wRot, scale, u0, u1, v0, v1, color, lightCoords)` 时，直接按 Raynium 56 B 顶点输入布局（`[x, y, z, scale, xRot, yRot, zRot, wRot, u0, u1, v0, v1, color, lightCoords]`）顺序写入 `raynium$packedInts[base .. base + 13]`；
   - 在 `CompactLayerBatch.execute` 中，将原先逐粒子、逐字段调用 14 次 `dst.set(ValueLayout.JAVA_FLOAT/INT, ...)` 的 `packParticlesToSegment` 循环，替换为**每个 slice 仅 1 次批量内存拷贝**：
     ```java
     MemorySegment.copy(slicePackedInts[s], 0, dst, ValueLayout.JAVA_INT, dstByteOffset, count * INSTANCE_INTS);
     ```
   - **收益**：在 `particles` 场景（每帧 ~14,700 个粒子，`827 KiB`）中，将 `executeGroup` 阶段的 205,800 次独立 `MemorySegment.set` 与字段重排缩减为 1～3 次底层 `memcpy`，预计直接削减 Java 主线程耗时 `0.5～0.8 ms`。

#### S3.2 实体 `ModelFastTrack` 叶节点免矩阵克隆、$O(1)$ 引用查桶与阴影快轨化

1. **`ModelPart` 叶节点剪枝与纯平移叶节点免 `PoseStack.pushPose()`（`src/client/java/dev/raynium/client/mixin/ModelPartCompileMixin.java`、`src/client/java/dev/raynium/client/world/ModelFastTrack.java`）**：
   - 在 `ModelPartCompileMixin` 中拦截 `ModelPart.render(PoseStack, VertexConsumer, int, int, int)`：
     - 当 `ModelFastTrack.isCapturing(buffer)` 为真时：
       1. 若 `!this.visible` 或 `(this.skipDraw || this.cubes.isEmpty()) && this.children.isEmpty()`，**直接跳过返回**，避免原版无条件执行的 `poseStack.pushPose()` + `translateAndRotate` + `popPose()`；
       2. 若当前节点为**叶节点**（`this.children.isEmpty()`）且 `!this.skipDraw && !this.cubes.isEmpty()`：
          - 若该叶节点无旋转且无缩放（`(this.xRot | this.yRot | this.zRot) == 0.0f && this.xScale == 1.0f && this.yScale == 1.0f && this.zScale == 1.0f`），**无需调用 `poseStack.pushPose()` 克隆 `Matrix4f` 与 `Matrix3f` 对象**！直接读取父节点 `poseStack.last()` 的矩阵列，在写出 84 B 实例的平移分量 `M0..M2` 时用 `m.m30() + m.m00()*tx + m.m10()*ty + m.m20()*tz`（其中 `tx = x/16f, ty = y/16f, tz = z/16f`）一步算出世界空间平移，直接写入 `FastBucket`！
          - 即便叶节点带旋转/缩放，也可在单个预分配的可复用 `Matrix4f/Matrix3f` 暂存器上计算局部变换，省去 `PoseStack` 链表节点的进出栈开销。
2. **`GroupExtension` 按 `RenderType` 引用身份 $O(1)$ 查桶（`src/client/java/dev/raynium/client/world/ModelFastTrack.java`）**：
   - 在 `GroupExtension` 中维护小型开放寻址引用哈希表（`RenderType[] bucketKeys = new RenderType[32]`、`FastBucket[] bucketValues = new FastBucket[32]`，按 `System.identityHashCode(renderType)` 寻址，随 `init/recycle` $O(1)$ 世代戳重置）：
   - 当 `consolidate`（`canReorder && renderType.canConsolidateConsecutiveGeometry()`）为真时，直接通过 `renderType` 引用身份 $O(1)$ 查出对应 `FastBucket`，若未命中且 `entries` 非空再回退 `PreparedRenderType.equals`，彻底消除 `entities` 场景中每帧数千次遍历 `entries` 并深比较 `PreparedRenderType` 的 CPU 开销。
3. **实体圆形阴影（`ShadowFeatureRenderer`）纳入紧凑实例快轨（可选增强）**：
   - `entities` 场景中每个实体脚下的阴影块由 `ShadowFeatureRenderer` 生成轴对齐水平 Quad（`RenderTypes.entityShadow`），全部走 `StagedVertexBuffer` 即时路径。通过将 `ShadowFeatureRenderer` 的阴影 Quad 转换为共享单 Quad 网格的紧凑实例绘制（或合并为单次动态实例流绘制），可进一步削减数百次即时缓冲写入与绑定调用。

---

### 1.4 支柱四（`S4`）：`ImmediatePass` 光栅级 RHI 状态去重与 `TransientParameterSetPool` 快轨协同

1. **`ImmediatePass::Record` 跨绘制命令的 RHI 绑定状态缓存（`native/include/raynium/immediate_pass.h`、`native/src/immediate_pass.cpp`、`native/src/world_features.cpp`、`native/src/immediate_draw.cpp`）**：
   - 当前 `ImmediatePass::Record` 在遍历 `_commands`（混合包含 `ImmediateDraw`、`RetainedModelsFeature`、`CompactParticlesFeature`、`TerrainFeature`）时，每条命令的 `Record(encoder)` 都各自独立无条件调用：
     - `enc.BindGraphicsPipelineState(pso)`
     - `enc.SetScissor(scissor)`
     - `enc.SetStencilReference(0)`
     - `enc.BindShaderParameterSet(0, sets[0])`
     - `enc.BindShaderParameterSet(1, sets[1])`
     - `enc.BindIndexBuffer(...)`
   - 在 `ImmediatePass::Record` 栈上引入 `ImmediatePassStateCache`：
     ```cpp
     struct ImmediatePassStateCache {
         radray::gpu::GraphicsPipelineState* pso{};
         radray::Rect scissor{-1, -1, 0, 0};
         uint32_t stencil{UINT32_MAX};
         radray::array<radray::gpu::ShaderParameterSet*, 2> sets{};
         radray::gpu::IndexBufferView indexView{};
         bool hasIndexView{false};
         void Invalidate() noexcept { *this = ImmediatePassStateCache{}; }
     };
     ```
   - `ShaderResourceBindings::Bind(GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache)`、`ImmediateDraw::Record(GraphicsCommandEncoder& enc, ImmediatePassStateCache& cache)`、`RetainedModelsFeature::Record` 与 `CompactParticlesFeature::Record` 共用该 `cache`（`TerrainFeature::Record` 执行后调用 `cache.Invalidate()`）：
     - 仅当 `pso != cache.pso` 时调用 `enc.BindGraphicsPipelineState(pso)`；
     - 仅当 `scissor != cache.scissor` 时调用 `enc.SetScissor(scissor)`；
     - 仅当 `stencil != cache.stencil` 时调用 `enc.SetStencilReference(stencil)`；
     - 仅当 `sets[i] && (psoChanged || sets[i] != cache.sets[i])` 时调用 `enc.BindShaderParameterSet(i, sets[i])`（因为上游 `TransientParameterSetPool` 对相同绑定内容返回**相同的 `ShaderParameterSet*` 指针**，所以在 `entities` 的 87～262 次 `RetainedModelsFeature` 绘制和数百次同管线 `ImmediateDraw` 中，`sets[0]` 指针 100% 相等，直接省掉 95% 的 `BindShaderParameterSet(0)` 调用！）；
     - 仅当 `!cache.hasIndexView || !SameIndexBufferView(indexView, cache.indexView)` 时调用 `enc.BindIndexBuffer(indexView)`。
2. **协同上游 `RadRay` `R3` 确保 `ShaderResourceBindings` 100% 命中 `AcquireParameterSet` 免排序与 MRU 快轨**：
   - 在 `ShaderResourceBindings::Prepare` 与 `ImmediateDraw::Prepare` 中，填充 `groups[0]` 与 `groups[1]` 时，若槽位 `b.slot` 非严格递增，仅在 `groups[i].size() > 1` 且乱序时做极小栈上插入调整，使其传入 `slot.AcquireParameterSet` 时 100% 按 `BindingId` 升序排列，零拷贝直通 `R3` 的 `activeBindings` 与 1-entry MRU 快轨。

---

## 2. 实施分期与准出验收门禁

### 2.1 推进顺序

1. **阶段一（上游 `RadRay` `R1`～`R3` 落地与锁文件推进）**：
   - 在 `F:\cpp\RadRay` 落地 `R1`（`GetPageIndex`、`TrimEmptyPages`、活跃页 RDG 导入）、`R2`（`BuildMeshPassIndirectDraws` 活跃批次标量缓存与 `SubmitMeshPassIndirectDraws` 状态槽短路）、`R3`（`TransientParameterSetPool` 有序零拷贝与 1-entry MRU 快轨），通过全部 GTest 与 Google Benchmark 后更新 `native/radray.lock.json`。
2. **阶段二（Raynium `S1` + `S2`：地形池隔离/空页回收、网格分析后台化、6 向掩码缓存与稳态 MDI 参数复用）**：
   - 落地 `_quadIndexPool` / `_translucentIndices`（128 MiB） / `_translucentVertices`（256 MiB）隔离与 `TrimEmptyPages()`；
   - 将 Solid/Cutout/Translucent 的 `AnalyzeTerrainMesh` 前移至 `MeshPayload` 后台编译线程；
   - 落地 `CompiledSectionMeshMixin` 就绪层掩码缓存、`TerrainMesh` 相机块级 6 向可见掩码缓存与稳态 `TerrainViewDrawData` 跨帧复用。
3. **阶段三（Raynium `S3` + `S4`：粒子入库 56 B 原生直写、实体叶节点剪枝与 $O(1)$ 查桶、`ImmediatePassStateCache` 去重）**：
   - 落地 `QuadParticleRenderState.Storage` 56 B 原生数组直写与 `MemorySegment.copy` 块拷贝；
   - 落地 `ModelPart` 叶节点免 `PoseStack` 克隆与 `GroupExtension` 按 `RenderType` 引用身份 $O(1)$ 查桶；
   - 落地 `ImmediatePassStateCache` 跨命令 PSO/Scissor/Stencil/ParameterSet/IndexBuffer 状态绑定去重。
4. **阶段四（全量单测、86 场景双后端视觉回归与 4 场景 Release 基准复测）**。

### 2.2 准出标准

| 检查项 | 准出标准 |
| --- | --- |
| **1. 构建与单元测试** | Debug & Release 原生构建、`ctest`（`test_raynium_terrain`）、`.\gradlew.bat test`、`python -m unittest discover -s tools/tests` 及双后端 `nativeTest -PrhiValidation=true` 全量通过。 |
| **2. 86 场景视觉回归** | `python tools/validate_visual.py` 双后端 86 场景（246 张图）零新增超差退化，`pipeline_coverage.py` 195 条管线全绿。 |
| **3. 航线跨页断批根治与 CPU 性能目标** | Release Vulkan 四场景基准复测确认：<br>（1）`Metcalf` 与 `Tideworn` 全航线 `terrainDrawCalls` **P95 `< 35` 次**（彻底消除航线中后段 1,400～2,269 次的半透明跨页断批），稳态帧 `nativePrepareNs` 较 `phase3-cpu-opt` 再降 **40%+**；<br>（2）`Entities` 与 `Particles` 的 `renderWallNs` P50 较 `phase3-cpu-opt`（`3.28 ms` / `4.38 ms`）再降 **0.6～1.0 ms**，整帧 FPS 追平或反超同配置原版 Vulkan。 |
| **4. 文档同步** | 按 [文档维护](../guide/documentation.md) 同步所属架构文档及 [历史索引](README.md)，`python tools/check_docs.py` 与 `git diff --check` 零错误。 |
