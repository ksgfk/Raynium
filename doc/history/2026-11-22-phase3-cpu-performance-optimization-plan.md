> - 适用: 执行或评审阶段三落地后的全局 CPU 性能优化计划（地形真帧同步与大页/透明独立几何池、刚体模型与紧凑粒子零拷贝及合批收敛、CommandTape 与 RDG 执行引擎 O(1) 化、上游 RadRay 热路径同步）
> - 权威: 经用户要求保存的全局 CPU 优化设计计划，不构成当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)、[地形渲染](../architecture/terrain-rendering.md)、[Native runtime](../architecture/native-runtime.md) 与 [互操作与加载](../architecture/interop.md)
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_feature.cpp`, `native/src/world_features.cpp`, `native/src/immediate_pass.cpp`, `native/src/immediate_draw.cpp`, `native/src/command_tape.cpp`, `src/client/java/dev/raynium/client/terrain/*.java`, `src/client/java/dev/raynium/client/world/*.java`, `src/client/java/dev/raynium/client/renderpearl/*.java`, `src/client/java/dev/raynium/bridge/NativeFrameBuffer.java`

# 阶段三后全局 CPU 性能优化计划：从即时态多跳翻译到常驻控制面直通

- 原记录日期：2026-11-22
- 状态：基于阶段三（`p3-v2`，ABI 20）全量基准测试归因与源码审计制定的全局 CPU 优化计划；归档时尚未实施本计划对应的源码改动
- 范围：覆盖 Java 前端（`TerrainCapture`、`TerrainResidency`、`ModelFastTrack`、`ParticleFastTrack`、`CommandTape`）、跨语言数据布局、Raynium Native 控制面（`TerrainResidency`、`RetainedModelsFeature`、`CompactParticlesFeature`、`ImmediatePass`、`ShaderResourceBindings`）及上游 `RadRay` Runtime 热路径协同升级的四大支柱方案（`P1`～`P4`）
- 来源：承接 [阶段三实施、问题排查与验证记录](2026-11-22-phase3-mdi-retained-world-notes.md)，基于 2026-11-22 当日工作树源码（含阶段三未提交改动）、`native/radray.lock.json` 锁定上游版本与 `build/validation/phase3-performance/` 实测数据整理

---

## 0. 全局瓶颈诊断：为什么 GPU 已经快 17.5%～38%，整帧 FPS 却低于原版 Vulkan？

### 0.1 阶段三（`p3-v2`）与原版 Vulkan 的 CPU / GPU 时间分解

在 RTX 4080（1920×1080，视距 32，Release `/MT`，`rhiValidation=false`）下，各基准场景的 P50 中位数耗时分解如下（单位：`ms`）：

| 阶段 / 指标 (P50) | `entities` (Phase 3) | `entities` (原版 Vulkan) | `particles` (Phase 3) | `particles` (原版 Vulkan) | `Metcalf` (Phase 3) | `Tideworn` (Phase 3) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **GPU 实际耗时 (`gpuFrameNs`)** | **0.6258 ms** | 1.0086 ms (**快 38.0%**) | **1.5239 ms** | 1.8463 ms (**快 17.5%**) | **0.6798 ms** | **1.0368 ms** |
| **渲染线程总墙钟 (`renderWallNs`)** | **4.0263 ms** | **2.6521 ms** | **5.1598 ms** | **3.7310 ms** | **2.1856 ms** *(原版帧间隔 1.1536)* | **4.4115 ms** *(原版帧间隔 2.1729)* |
| ├─ **Java 侧耗时 (`renderWall - nativeCall`)** | **2.3597 ms** | *(含驱动总计 2.6521)* | **3.2870 ms** | *(含驱动总计 3.7310)* | **0.9149 ms** | **2.0297 ms** |
| └─ **Native 提交耗时 (`nativeCallNs`)** | **1.6666 ms** | — | **1.8728 ms** | — | **1.2707 ms** | **2.3818 ms** |
| 　 ├─ `nativeWaitNs` (帧槽 Fence 等待与退役) | 0.0342 ms | — | 0.0415 ms | — | 0.0232 ms | 0.0318 ms |
| 　 ├─ **`nativePrepareNs` (地形 Scene + 解码)** | **1.0651 ms** | — | **1.1753 ms** | — | **0.7712 ms** | **1.7421 ms** |
| 　 ├─ **`nativeRdgRunNs` (RDG 建图/编译/录制)** | **0.4233 ms** | — | **0.5006 ms** | — | **0.3683 ms** | **0.4567 ms** |
| 　 └─ `nativePresentNs` (`Present`) | 0.1016 ms | — | 0.1116 ms | — | 0.0885 ms | 0.1122 ms |

### 0.2 四大架构级根源矛盾

从全局数据流（Java 前端 $\rightarrow$ C ABI $\rightarrow$ Raynium Native $\rightarrow$ RadRay Runtime）审视，当前架构的根本问题在于 **“几何数据已在 GPU 常驻，但 CPU 控制面依然是零记忆的全量即时态”**：

1. **常驻世界的“假常驻控制面”（地形每帧跨语言重走 6 趟循环 + 100% 失灵的 0-sort 短路）**：
   - Java 与 Native 之间使用 64 位业务 `mesh_id` 通信而非稠密槽位下标，导致 Java 遍历 3 趟可见区块写出 1～2 万条记录后，Native `TerrainResidency::PrepareFrame` 每帧必须执行 **1～2 万次 `unordered_map::find`** 将 ID 翻译回 `MeshBatchHandle`；
   - `TerrainViewDrawData::Clear()` 每帧清空上一帧的可见命令列表；而 Java `visibleSections()` 按空间 BFS 距离序输出，`Solid` / `Cutout` 的 `StateFirst` 排序键高位却是 8 位几何页哈希桶 `geometryBucket` 与 `vertexPageId`，空间序与显存页序正交，导致上游 `SortVisibleCommands()` 的 `if (sorted) return;` 零排序短路在 `Solid` / `Cutout` 上 **100% 不触发**，每帧必跑多趟 32 B 结构体基数排序（且上游在 1/3 字节奇数趟基数排序末尾还多做了一整趟 320～640 KiB `std::copy_n`）。
2. **几何池“32 MiB 小分页混装”击穿了经典透明层 MDI 合批率与 RDG 依赖声明复杂度**：
   - `native/src/renderer.cpp` 将 `TerrainVertices` 设为 `32 MiB/页` 且混装三层地形，大视距下膨胀至 89～111 页。非 OIT 经典 `Translucent` 层按 `DepthFirst` 由远及近排序后，相邻区块极大概率位于不同 32 MiB 顶点页（`VertexBuffersIndex` 不同），导致 MDI 无法合并，退化为 **1,200～1,400 次 `BindVertexBuffers` + `DrawIndexedIndirect(count=1)`**（`Metcalf` 1,386 次、`Tideworn` 1,607 次，对比 OIT `particles` 仅 90 次）；
   - 同时，同一 `ImmediatePass` 内的多个 `TerrainFeature`（3～5 次）与上百条 `RetainedModelsFeature` / `CompactParticlesFeature`（150～350 次）在**每条命令的 `Declare` 中重复调用 `AddPassReadAccess` 遍历全部池页**，叠加 RDG `AddAccess` 内部的线性扫描，形成 $O(P \times M)$ 二次方开销。
3. **刚体模型快轨“合批粒度过细（按 `ModelPart` 而非材质桶）”引发每帧堆分配与散列拷贝**：
   - 单个实体（僵尸、玩家、盔甲、箱子等）由 6～12 个不同 `meshId` 的 `ModelPart` 组成且共享同一 `RenderType`，导致 `FastBucket.buildGroupedAndCopy` 几乎永远无法命中 `singleMesh` 快路径，**每帧为每个实体材质桶 `new Int2IntOpenHashMap()` + `new int[runs]`** 并逐部件散列拷贝 80 B 实例；
   - 每个 `ModelPart` 独立调用 3 次 `pose.transformNormal` 计算法线基向量（未直接读取 `pose.normal()` 3×3 矩阵列）；Native 侧每个部件 run 仍单独调用一次 `DrawIndexed`。
4. **动态实例流与 `CommandTape` 的多重缓冲复制（Copy-on-Copy）及每命令堆分配**：
   - 粒子与模型实例在 CPU 上经历了 `Storage` $\rightarrow$ Java `int[]` 重排 $\rightarrow$ `CommandTape.uploads` $\rightarrow$ Native `InstanceUploadArena::Intern` `std::memcpy` $\rightarrow$ RDG `UploadArena` 共 **3～4 跳内存搬运**（`particles` 单帧搬运 ~3.3 MiB）；
   - Java `CommandTape` 每次重置对 `commands` 流整段清零，每次绑定资源执行 `HashSet.add`；Native 每条命令在 `ShaderResourceBindings::Prepare` 中堆分配 2 个 `radray::vector<TransientParameterBinding>` 并做 `std::erase_if` + `std::find`；基准 `RN_BATCH_MEASURE` 每批遍历百余个几何页调 `GetFreeBytes()`。

---

## 1. 全局优化四大支柱方案（`P1`～`P4`）

### 1.1 支柱一（`P1`）：地形控制面“真·帧同步”、稠密槽位寻址、按页分桶 0-sort 与几何池拓扑重构

1. **P1.1 跨语言 32 位稠密槽位（Dense Slot ID）直寻址（消灭每帧 1～2 万次哈希查找）**：
   - 在 Java 侧 `src/client/java/dev/raynium/client/terrain/MeshPayload.java` 与 `TerrainResidency.java` 中，为已驻留（或本帧排队上传）的每个 `MeshPayload` 分配一个由渲染线程空闲栈管理的 32 位稠密槽位序号 `slotId`（`1 .. maxSlot`，`0` 保留为空）；区块淘汰时在 `commit` 阶段回收 `slotId`；
   - 在 Native 侧 `native/src/terrain_residency.h` / `native/src/terrain_residency.cpp` 中，用平坦数组 `radray::vector<TerrainMesh> _meshesBySlot`（或定长槽位表）替代 `radray::unordered_map<uint64_t, TerrainMesh> meshes`；
   - `PrepareFrame` 遍历 `f.visible` 时直接通过 `_meshesBySlot[slotId]` 进行 **$O(1)$ 数组寻址**，将每帧 1～2 万次 `unordered_map::find` 彻底归零。

2. **P1.2 确定性 `CustomStateSortKey` + 按页分桶收集（使 `Solid`/`Cutout` 100% 命中 `0-sort` 短路）**：
   - 在 `TerrainMeshPassProcessor::AddMeshBatch`（`native/src/terrain_residency.cpp`）中，通过 `MeshBatchContext` 或网格关联页号显式设置 `shared.CustomStateSortKey = mesh.vertexPageId`（不透明与 OIT 层均共享同一个 `_quadIndices` 页且无 per-mesh payload，唯一区分 MDI 批次的状态即为 `vertexPageId`），彻底绕开上游 `GetStateSortKey` 的 8 位 `geometryBucket` 模 255 哈希碰撞；
   - 在 `TerrainResidency::PrepareFrame` 收集可见项时，对 `Solid`、`Cutout` 与 `OitBounds` 直接按 `mesh.vertexPageId` 推入持久复用的分页桶 `_pageBuckets[layer][mesh.vertexPageId]`（因为 Java `visible[]` 本身按空间距离由近及远传入，同页桶内天然保持 front-to-back 顺序，可将 `VisibilitySortKey` 直接设为 `0` 或单调递增的 `distBucket`）；
   - 按 `pageId = 0 .. pageCount - 1` 顺序将非空桶线性拼入 `GatherVisibleMeshDrawCommands` 后，`drawList.VisibleCommands` 的 64 位 `SortKey` **天然 100% 单调非递减**，调用 `layerData.drawList.SortVisibleCommands()` 时 **100% 命中首轮线性预检 `if (sorted) return;`**，实现真正的稳态与移动帧 **零基数排序**。

3. **P1.3 地形几何池拓扑重构：大页化 + 透明层独立顶点池（消灭经典透明层 1,400+ 次跨页断批）**：
   - 在 `native/src/renderer.cpp` 与 `native/src/terrain_residency.cpp` 中：
     - 将不透明顶点池 `TerrainVertices`（承载 `RN_MATERIAL_SOLID` 与 `RN_MATERIAL_CUTOUT`）的单页容量从 `32 MiB` 提升至 `128 MiB`（`PageVertexCapacity = (128u << 20) / 28`），将大视距世界总页数从 ~110 页收敛至 ~25 页以内；
     - 新增独立的半透明顶点池 `TranslucentVertices`（承载 `RN_MATERIAL_TRANSLUCENT`，单页容量 `128 MiB`）并将索引池 `TerrainIndices` 单页容量提升至 `64 MiB`；
   - **架构收益**：大视距下所有半透明区块的顶点总量远低于 `128 MiB`，100% 驻留在 `TranslucentVertices` 的第 0 页（同一个 `gpu::Buffer*`）与同一个 `TerrainIndices` 页中。在非 OIT 经典透明模式下，即便 `Translucent` 按 `DepthFirst` 严格由远及近排序，所有透明区块的 `(VertexBuffersIndex, IndexBuffer)` 也完全一致，`BuildMeshPassIndirectDraws` 可将其全部合并为 **1 个 MDI 批次（1 次 `DrawIndexedIndirect` 调用）**，将 Metcalf / Tideworn 的 `terrainDrawCalls` 从 1,386～1,607 次直接压降至 **30～50 次以内**。

4. **P1.4 Java 侧地形采集与可见列表写出稳态减趟**：
   - 在 `src/client/java/dev/raynium/client/terrain/TerrainCapture.java` 与 `TerrainResidency.java` 中，优化稳态帧（无超预算候选排序需求时）的数组流转，并使用批量偏移写入减少 `NativeFrameBuffer` 的逐字段边界检查开销。

---

### 1.2 支柱二（`P2`）：刚体模型与紧凑粒子快轨收敛（1 跳零拷贝、矩阵列直读、桶池化与同页 MDI）

1. **P2.1 Native 实例流 `BorrowedBytes` 零拷贝直通（废除 `InstanceUploadArena` 二次 `std::memcpy`）**：
   - `rn_submit_frame` 的 FFM 调用生命周期完整覆盖 `_scheduler->RunFrame()` 中的 RDG `Prepare` 阶段（`Prepare` 将上传源直接 `memcpy` 至 GPU 持久映射 `UploadArena`）；
   - 在 `native/src/world_features.h`、`native/src/world_features.cpp` 与 `native/src/command_tape.cpp` 中，**移除 `InstanceUploadArena` 的 `radray::vector<radray::byte> _bytes` 动态扩容与 `std::memcpy`**：
     - `CommandTape` 仅统计或验证 `batch.uploads` 中被引用的实例区间边界，在 `CommandTape::Replay` 中直接通过 `RDGUploadSource::BorrowedBytes` 借用 `batch.uploads`（或合并后的最小连续包围区间）创建 `instanceBuffer`；
     - OIT 三阶段在 Java 侧原本就共享同一个 `uploadedOffset`，Native 直接复用该偏移即可，在 `nativePrepareNs` 中实现 **0 字节实例拷贝与 0 堆内存分配**。

2. **P2.2 Java 紧凑粒子快轨直写堆外 `CommandTape.uploads`（消除中间 `int[]` 重排缓冲）**：
   - 在 `src/client/java/dev/raynium/client/world/ParticleFastTrack.java` 与 `src/client/java/dev/raynium/client/mixin/QuadParticleRenderStateMixin.java` 中：
     - 移除 `CompactLayerBatch` 内部的 Java 堆数组 `int[] packedInts` 及其 `Arrays.copyOf` 扩容；
     - 在 `QuadParticleFeatureMixin` 统计完某图层总粒子数 `totalParticles` 后，直接在当前 `CommandTape` 上一次性 `reserveAlignedUpload((long) totalParticles * INSTANCE_BYTES, 16)` 获取堆外 `MemorySegment dst`，将 `Storage.floatValues` 与 `Storage.intValues` 直接写入 `dst`（若同步将粒子实例顶点输入属性顺序微调为与 `Storage` 完全一致的 `12 × float` + `2 × int`，甚至可直接用两次批量 `MemorySegment.copy` 替代逐粒子 12 次 `Float.floatToRawIntBits` 循环），将粒子数据在 CPU 上的 4 次搬运彻底压缩为 **1 次堆外写入 + 1 次 RDG 映射写入**。

3. **P2.3 刚体模型法线基向量矩阵列直读（废除每部件 3 次 `pose.transformNormal`）**：
   - 在 `src/client/java/dev/raynium/client/world/RetainedModelManager.java` 的 `packInstance` 中：
     - `pose.transformNormal(1, 0, 0)`、`pose.transformNormal(0, 1, 0)`、`pose.transformNormal(0, 0, 1)` 在数学和 IEEE-754 浮点运算上严格等价于直接读取 `Matrix3f n = pose.normal()` 的三列 `(n.m00(), n.m01(), n.m02())`、`(n.m10(), n.m11(), n.m12())`、`(n.m20(), n.m21(), n.m22())`（因为乘数仅为 `1.0F` 与 `0.0F`）；
     - 直接读取 `n.m00() .. n.m22()` 传入 `packNormalByte4`，保持 100% 比特级一致的同时，彻底消除每个 `ModelPart` 的 3 次 `transformNormal` 矩阵乘法与 `scratchNormal` 读写。

4. **P2.4 `FastBucket` 跨帧对象池化与零 `Int2IntOpenHashMap` 分配 + 同页多部件间接绘制**：
   - 在 `src/client/java/dev/raynium/client/world/ModelFastTrack.java` 中：
     - 为 `GroupExtension` 与 `FastBucket` 建立渲染线程复用的对象池，保留 `meshIds`、`instanceInts`、`runMeshIds` 等数组容量，消除每帧新建 `GroupExtension`、`FastBucket` 及数组扩容的 GC 压力；
     - 在 `buildGroupedAndCopy` 中，用 `FastBucket` 内部持久持有的轻量开放寻址数组（或按小容量线性表 + 世代戳）替代每帧 `new Int2IntOpenHashMap()` 与 `new int[runs]`；
     - 在 Native `RetainedModelsFeature`（`native/src/world_features.cpp`）中，当同一命令包含多个共享同一 `ModelVertices` 页的 `WireRun` 且设备支持 `IndirectDraw`（且满足 `IndirectFirstInstance`）时，将多个 `WireRun` 打包进帧级间接参数缓冲，以**单次 `enc.DrawIndexedIndirect`** 替代逐部件循环调用 `enc.DrawIndexed`，大幅压降 `entities` 场景的 `sceneDrawCalls`。

---

### 1.3 支柱三（`P3`）：`CommandTape` 与 RDG 执行引擎 $O(1)$ 化

1. **P3.1 `ImmediatePass` 级几何池读依赖单次去重声明（消除 $O(P \times M)$ 重复遍历）**：
   - 在 `native/src/immediate_pass.h` / `native/src/immediate_pass.cpp`、`native/src/terrain_feature.cpp` 与 `native/src/world_features.cpp` 中：
     - 从 `TerrainFeature::Declare`、`RetainedModelsFeature::Declare`、`CompactParticlesFeature::Declare` 中移除对 `vertexPool->AddPassReadAccess(graph, pass)` 与 `indexPool->AddPassReadAccess(graph, pass)` 的逐命令调用；
     - 改为在 `ImmediatePass::Replay` 遍历 `_commands` 声明依赖时，在 Pass 级用布尔标志（`declaredTerrainVertices`、`declaredTranslucentVertices`、`declaredTerrainIndices`、`declaredModelVertices`）统合，**每个 `ImmediatePass` 对每个几何池至多调用 1 次 `AddPassReadAccess`**。

2. **P3.2 `ShaderResourceBindings::Prepare` 与 `ImmediateDraw::Prepare` 栈上 `InlineVector` + 64 位位掩码过滤**：
   - 在 `native/src/render_program.h` / `native/src/render_program.cpp` 中，在程序加载时为 `required[0]`、`required[1]` 与 `samplers` 预计算 `uint64_t requiredMask[2]` 与 `uint64_t samplerMask`（槽位号均 `< 64`）；
   - 在 `native/src/world_features.cpp`（`ShaderResourceBindings::Prepare`）与 `native/src/immediate_draw.cpp`（`ImmediateDraw::Prepare`）中：
     - 将 `radray::array<radray::vector<radray::TransientParameterBinding>, 2> groups` 替换为栈上 `radray::InlineVector<radray::TransientParameterBinding, 16> groups[2]`（彻底消除每条绘制命令在 `Prepare` 期的 2 次堆分配与释放）；
     - 在收集绑定时直接用 `(program.requiredMask[groupIdx] & (1ull << b.slot)) != 0` 与 `(program.samplerMask & (1ull << (b.slot + 1))) != 0` 进行 $O(1)$ 位测试过滤，删除末尾的 `std::erase_if` + `std::find` 双层线性扫描。

3. **P3.3 Java `CommandTape` 编码器去清零与 $O(1)$ 资源保活标记**：
   - 在 `src/client/java/dev/raynium/client/renderpearl/CommandTape.java` 中：
     - 将 `Storage` 构造函数中的 `this.commands = new Stream(initialCommands, true)` 改为 `zeroOnReset = false`（所有命令均按 `header.size` 精确写入定长字段，无需在每次 `reset()` 时对整段 `commands` 内存执行 `.fill((byte) 0)`）；
     - 为 `NativeTexture`、`NativeBuffer`、`RayniumPipeline`、`RayniumQueryPool` 增加单调批次戳记字段 `int lastRetainedTapeId`，`CommandTape` 每次启用分配递增 `tapeId`，`retain(...)` 时若 `resource.lastRetainedTapeId == this.tapeId` 则 $O(1)$ 直接返回，否则更新戳记并追加入 `ArrayList`，彻底消除每帧数千次 `HashSet.add` 的哈希与节点开销。

---

### 1.4 支柱四（`P4`）：同步上游 `RadRay` Runtime 热路径瘦身

待上游 `RadRay` 完成 `C1`～`C5` 热路径优化并通过全量单测后，推进 `native/radray.lock.json`：

1. **`C1`**：`MeshPassFrameDrawList::SortVisibleCommands` 在奇数个活跃字节趟数下预对齐 `_scratchCommands.capacity()` 并在末尾使用 `VisibleCommands.swap(_scratchCommands)` 替代 `std::copy_n` 整表拷贝；
2. **`C2`**：`MeshDrawCommandElements` 入库预验附加元素掩码 `ValidatedExtraMask`（与 `command.ValidElementMask & (command.ValidElementMask - 1)` 比较），将 `ForEachExpandedMeshDrawElement` 每命令 $O(E)$ 结构校验降为 $O(1)$ 位运算比较，并提供完整保留 `IndexBuffer` 状态缓存绑定的单元素命令（`!elements`）直通快轨；
3. **`C3`**：`VertexBufferPool` / `IndexBufferPool` 维护增量 `TotalFreeElements` 计数，使 `GetFreeBytes()`（被 `RN_BATCH_MEASURE` 每批调用）从 $O(P)$ 遍历全部分页降为 $O(1)$ 乘法；
4. **`C4`**：`RDGUserPass` 采用“小 Pass（$\le 16$ 项）零分配线性扫描 + 大 Pass（$> 16$ 项）在 `data.MemoryResource` 上按实际访问规模建立仅存 `uint32_t slot` 的开放寻址哈希桶（回查 `Accesses[slot]`）”，将多页几何池与高频特征下的 `AddAccess` 与 `ResolveTextureView` 访问声明核验降为平均 $O(1)$，且对稀疏 Pass 零额外桶分配；
5. **`C5`**：`Scene::GatherVisibleMeshDrawCommands` 与 `GatherVisiblePrimitives` 双入口循环外提 `GetPassCache` 与带边界检查的 `std::span` 槽表。

---

## 2. 实施分期与准出验收门禁

### 2.1 推进顺序

1. **第一步（上游 `RadRay` `C1`～`C5` 落地与锁文件更新）**：在上游完成 `C1`～`C5` 及对应 GTest 单测后，更新 `native/radray.lock.json` 并同步 Debug/Release 依赖；
2. **第二步（Raynium `P1` + `P3`：地形真帧同步、大页/透明独立池、Pass 级池依赖去重、`InlineVector`/位掩码与 `CommandTape` 瘦身）**：优先解决 `nativePrepareNs` 与 `nativeRdgRunNs` 的最大头开销及经典透明层跨页断批；
3. **第三步（Raynium `P2`：实例流 `BorrowedBytes` 零拷贝、粒子直写堆外、模型矩阵列直读、`FastBucket` 池化与同页 MDI）**：消除 `entities` 与 `particles` 的多重拷贝与每帧桶分配；
4. **第四步（全量单测、86 场景双后端视觉回归与 4 场景 Release 基准复测）**。

### 2.2 准出标准

| 检查项 | 准出标准 |
| --- | --- |
| **1. 构建与单元测试** | Debug & Release 原生构建、`ctest`（`test_raynium_terrain`）、`.\gradlew.bat test`、`python -m unittest discover -s tools/tests` 及双后端 `nativeTest -PrhiValidation=true` 全量通过。 |
| **2. 86 场景视觉回归** | `python tools/validate_visual.py` 双后端 86 场景（246 张图）零新增超差退化，`pipeline_coverage.py` 195 条管线全绿。 |
| **3. CPU 性能与 DrawCall 收敛** | Release Vulkan 基准复测确认：（1）`Metcalf` 与 `Tideworn` 的 `terrainDrawCalls` P50 从 ~1,400–1,600 次压降至 `< 100` 次，`Solid`/`Cutout` 稳态命中 0-sort 短路，`nativePrepareNs` 与 `nativeRdgRunNs` 显著低于阶段三 `p3-v2` 及阶段二基线；（2）`entities` 与 `particles` 的 `renderWallNs` 大幅下降并逼近或优于同配置原版 Vulkan。 |
| **4. 文档同步** | 按 [文档维护](../guide/documentation.md) 同步 [地形渲染](../architecture/terrain-rendering.md)、[Native runtime](../architecture/native-runtime.md)、[互操作与加载](../architecture/interop.md) 及 [历史索引](README.md)，`python tools/check_docs.py` 与 `git diff --check` 零错误。 |
