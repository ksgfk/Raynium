> - 适用: 查阅 2026-11-09 基于 JFR + Tracy 联合实机采样的 Java 分配/可见列表与 Native 可见批次开销重构计划
> - 权威: 本文为历史分析与待实施重构方案记录，不代表当前实现契约；当前契约以 [地形预览](../architecture/terrain-preview.md)、[互操作与加载](../architecture/interop.md) 和 [Native runtime](../architecture/native-runtime.md) 为准
> - 锚点: `src/client/java/dev/raynium/client/terrain/TerrainCapture.java`, `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`, `src/client/java/dev/raynium/client/terrain/MeshPayload.java`, `src/client/java/dev/raynium/bridge/NativeRenderer.java`, `native/include/raynium/bridge.h`, `native/src/terrain_scene.cpp`

# Java 渲染准备与 Native 可见批次开销重构计划

- 日期：2026-11-09
- 状态：已确认核心设计取舍，待实施（用户选择先归档计划至 `doc/history/`，暂不修改代码）
- 范围：`src/client/java/dev/raynium/client/terrain/`、`src/client/java/dev/raynium/bridge/`、`native/include/raynium/bridge.h`、`native/include/raynium/terrain_scene.h`、`native/src/terrain_scene.cpp`
- 来源：本机高压存档实机联合采样产物 `build/benchmarks/runtime-jfr-20261109/20261109-114422-669000/joint-analysis/analysis.json` 与 `frame-gc-overlap.csv`（忽略目录，本机可用）、Minecraft 26.3 客户端源码及当前工作树源码

---

## 1. 实机 JFR + Tracy 联合采样基线与瓶颈归因

### 1.1 帧耗时与子区间分布（26,831 个对齐世界帧，约 60.0 秒）

排除退出世界时的限帧事件后，26,831 个世界帧的对齐统计如下：

| 指标 | 平均 | P50 | P95 | P99 | 最大 |
| --- | --- | --- | --- | --- | --- |
| 帧开始间隔 `FrameInterval` | 2.234 ms | 1.896 ms | 4.368 ms | 6.963 ms | 40.608 ms |
| 客户端 `ClientFrame` (`renderFrame`) | 2.126 ms | 1.852 ms | 4.042 ms | 5.742 ms | 38.106 ms |
| Native `Raynium.Render` | 0.344 ms | 0.320 ms | 0.595 ms | 0.813 ms | 3.197 ms |
| Native GPU (`Raynium.GpuMilliseconds`) | 0.159 ms | 0.147 ms | 0.238 ms | 0.428 ms | 0.568 ms |
| Native 子区间：`Raynium.Scene.VisibleBatches` | 0.079 ms | 0.070 ms | 0.163 ms | 0.255 ms | 0.491 ms |
| Native 子区间：`RadRay.RDG.Execute` | 0.102 ms | 0.093 ms | 0.178 ms | 0.252 ms | 0.576 ms |
| Native 子区间：`Raynium.Frame.SubmitHandoff` | 0.073 ms | 0.068 ms | 0.122 ms | 0.162 ms | 0.635 ms |

场景规模与变更频率统计：

- 展开前每帧可见 section 实例数 `Raynium.Instances`：均值 `638.7`，P50 `611`，P95 `788`，P99 `1,002`，最大 `1,091`（合并后 `RadRay.Scene.VisibleBatches` 均值 `638.5`）。
- 展开后每帧跨 FFM 可见项数 `Raynium.Visible`：均值 `1,878.0`，P50 `1,805`，P95 `2,379`，P99 `3,152`，最大 `3,456`（平均每个可见 section 拆分为约 `2.94` 条子网格记录）。
- 常驻网格/图元数 `Raynium.Resident.Meshes` / `Primitives`：均值 `1,875.2`，最大 `2,179`；几何 Arena 块数 `Raynium.Arena.Blocks` 均值 `5.93`（P50 `6`，最大 `6`），每帧几何缓冲访问项 `Raynium.GeometryAccesses` 均值 `6.93`（P50 `7`，即 6 个 Arena 顶点缓冲 + 1 个共享索引缓冲）。
- 每帧新增/更新网格数 `Raynium.MeshUpdates`：均值 `0.145`（**P50 = 0，P95 = 0**，P99 = 5，最大 23，全程共 3,901 次上传）。
- 每帧移除图元数 `Raynium.PrimitiveRemovals`：均值 `0.113`（**P50 = 0，P95 = 0**，P99 = 0，最大 **117**，全程共 3,040 次移除）。

`Raynium.Render` 仅占客户端 `renderFrame` 平均耗时的 **16.2%**，Native GPU 仅 **0.159 ms**；超过 95% 的帧处于 **0 网格上传、0 网格移除** 的纯稳态。

### 1.2 渲染主线程 Java CPU 采样分布

渲染主线程 `Render thread` 共采集 3,288 个 Java 执行样本，其中宿主/JDK 占 **76.37%**，Raynium Java 路径占 **23.63%**（777 个样本）：

| 调用链（含子调用） | 样本数 | 主线程占比 | 关键热点说明 |
| --- | --- | --- | --- |
| 原版 `LevelRenderer.extractSectionDrawGroups` | 574 | 17.46% | 原版区块组提取 |
| `TerrainCapture.capture` | 425 | **12.93%** | 其中 `ArrayList.sort` / `TimSort.sort` 占 **8.06%**（265 样本） |
| 原版 `WeatherEffectRenderer.extractRenderState` | 412 | 12.53% | 原版天气与降水状态提取 |
| `TerrainPreview.draw` | 342 | **10.40%** | 含 `HashMap.getNode`/`get`/`putVal`/`containsKey`、`NativeRenderer.render`（1.82%）、`HashMap.resize`（0.70%） |

此外，渲染主线程的 Native 方法采样（`analysis.json` 中 `native_method_context`，共 945 个主线程 Native 样本）显示：除 Vulkan 提交/呈现与 `rn_render_frame`（143 样本，15.13%）外，**`jdk.internal.misc.Unsafe.allocateMemory0`（51 样本，5.40%）与 `Unsafe.freeMemory0`（23 样本，2.43%）合计占主线程全部 Native 样本的 7.83%**，直接源于 `NativeRenderer.marshal` 与 `NativeBindings.call` 每帧通过 `Arena.ofConfined()` 反复调用操作系统 `malloc`/`free`。

### 1.3 堆分配压力与 GC 尖峰关联

按 JFR `jdk.ObjectAllocationSample` 样本权重估算（按 $1\text{ GiB} = 2^{30}\text{ B}$、$1\text{ MiB} = 2^{20}\text{ B}$ 换算），路线内全部线程累计分配 **32.71 GiB**（`35,118,056,984 B`，其中 Raynium 占 **8.72 GiB / 26.65%**，`9,360,310,360 B`）；渲染主线程累计分配 **19.39 GiB**（`20,823,294,496 B`），其中 **Raynium 主线程路径占 8.05 GiB（41.49%，`8,638,763,560 B`）**：

| 分配归属与源码位置 | 估算分配量 (GiB / MiB) | 原始字节数 | 主线程占比 | 主要分配类型 |
| --- | --- | --- | --- | --- |
| `TerrainPreview.draw` 合计 | **6.43 GiB** | `6,907,766,952 B` | **33.17%** | 每帧重建集合、包装对象与装箱 |
| ↳ `TerrainPreview.java:129` (`visibleItems.add(new Visible(...))`) | 2.48 GiB | `2,659,982,808 B` | 12.77% | `NativeRenderer$Visible` (1.78 GiB) + `Object[]` 扩容 |
| ↳ `TerrainPreview.java:132` (`next.put(key, draw); usedIds.add(...)`) | 2.24 GiB | `2,400,587,496 B` | 11.53% | `HashMap$Node` (1.32 GiB)、`Node[]` (789 MiB)、`Long.valueOf` (831 MiB) |
| ↳ `TerrainPreview.java:126` (`instances.add(new Instance(...))`) | 754.2 MiB | `790,806,288 B` | 3.80% | `NativeRenderer$Instance` (582.1 MiB) + `Object[]` 扩容 |
| ↳ `TerrainPreview.java:119` (`new Position(draw.x(), draw.y(), draw.z())`) | 549.1 MiB | `575,802,536 B` | 2.77% | `TerrainPreview$Position` 记录类 |
| ↳ `TerrainPreview.java:113` (`uploaded.containsKey` / `staged.containsKey`) | 416.1 MiB | `436,290,264 B` | 2.10% | `java.lang.Long` 自动装箱 |
| `TerrainCapture.capture` 合计 | **1.46 GiB** | `1,565,528,472 B` | **7.52%** | 采集快照与排序临时数组 |
| ↳ `TerrainCapture.java:54` (`new Visible(payload, new Draw(...))`) | 1.20 GiB | `1,286,743,832 B` | 6.18% | `TerrainCapture$Visible`、`Draw`、`MeshPayload.visibleGroups` 内 `double[3]` |
| ↳ `TerrainCapture.java:62` (`List.copyOf(visible)`) | 212.1 MiB | `222,421,496 B` | 1.07% | 不可变列表 `Object[]` 拷贝 |
| ↳ `TerrainCapture.java:57` (`visible.sort(...)`) | 31.3 MiB | `32,818,016 B` | 0.16% | `TimSort` 临时对象数组 |
| 区块编译工作线程 `MeshPayload.<init>` | **683.0 MiB** | `716,177,288 B` | 工作线程 | `MeshPayload.java:18` `byte[] original` (286.8 MiB) + `:31` `vertices` (396.2 MiB) |
| `PreviewOverlay.render` / `NativeRenderer.marshal` / `NativeBindings.call` | **159.2 MiB** | `166,897,048 B` | 0.80% | `Arena.ofConfined()` 段拷贝、`invokeWithArguments` 装箱与 `ArrayList` |

GC 与卡顿的对应关系：

- 路线内发生 **74 次 GC 停顿**，累计 **539.36 ms**，单次最长 **31.28 ms**，波及 72 个客户端帧，并记录到 **31 次 `jdk.EvacuationFailed` 事件**。
- **15 个超过 16.67 ms 的帧间隔中有 10 个与 GC 重叠**；10 个超过 16.67 ms 的 `ClientFrame` 中有 8 个与 GC 重叠（最慢帧 `38.11 ms` 含 `23.17 ms` GC 重叠，而 Native 仅 `0.822 ms`）。
- 注：JFR `ObjectAllocationSample` 权重仅反映累计分配量，不表示常驻堆占用，也不能单独证明某类对象对 GC 停顿或疏散失败的直接因果份额；但将 Raynium 主线程约 41.5% 的高频短命分配降至接近零，能直接降低渲染主线程触发 TLAB 分配与 GC 周期的频率。
- 另有极少数无 GC 慢帧（如第 1,180 帧 `31.21 ms`）落在 `PistonHeadRenderer.createMovingBlock` 首次类加载与 Mixin 变换上，属于沿途冷路径。

---

## 2. 现有架构的五项核心缺陷

1. **两阶段逐帧对象图构建与全量 `TimSort`（`TerrainCapture` → `TerrainPreview`）**：
   - `TerrainCapture.capture` 每帧遍历约 639 个可见 section，为每个 section 分配 `Draw`、`Visible`、`double[3] eye`，再对全部可见 section 执行 `TimSort`（占主线程 8.06% CPU），最后 `List.copyOf` 复制一份不可变列表。
   - 然而，可见列表按距离排序在现有代码中只有两个消费者：
     1. `TerrainPreview.draw` 中超出每帧 8 MiB 上传预算时的近处优先裁剪——但实测 **95% 以上的帧根本没有新网格需要上传**，即使在 P99 帧也仅有 5 个新网格（几十 KiB），远低于 8 MiB 预算；
     2. `TerrainPreview.draw` 用递增列表下标 `order / 64` 充当 `NativeRenderer.Visible.sortKey`——但距离顺序完全可以通过对相机距离平方直接量化计算 `sortKey` 获得，无需对 `Object[]` 做 $O(N \log N)$ 比较排序。
2. **`TerrainPreview.draw` 将增量状态同步退化为每帧 $O(N)$ 哈希表查找与重建**：
   - 95% 以上的帧没有任何网格增删，但 `TerrainPreview.draw` 每帧都新建 `HashMap<Position, Draw> next` 和 `HashSet<Long> usedIds`，对每个可见 section 分配 `new Position(x, y, z)`、触发多次 `Long.valueOf(id)` 自动装箱，并执行数千次哈希查找与插入。
   - 即便仅将 `HashMap`/`HashSet` 替换为 `fastutil` 原始类型哈希表，若仍在每帧热循环内调用 `uploaded.containsKey`、`nextBySection.put`、`activeIds.add` 及每帧 `clear()`，依然会在稳态帧产生约 2,500+ 次哈希探测与底层数组清零。实际上，网格是否已在当前 epoch 上传、当帧是否正在使用，都是 `MeshPayload` 自身的逐对象状态，完全可以内联为 `MeshPayload` 上的原始类型字段（寄存器级 $O(1)$ 比较），而按 section 坐标索引的驻留表只需在**真正发生网格上传或卸载（均值 0.145 / 0.113 次/帧）**时按事件增量更新。
3. **子网格掩码“Java 逐位展开 → FFM 跨界拷贝 → C++ 循环重合并”反模式**：
   - `MeshPayload.visibleGroups` 已经算出了 7 位可见面组位掩码 `groupMask`。
   - `TerrainPreview.draw`（127-131 行）却将每个 section 的可见非空面组拆成最多 7 个独立的 `NativeRenderer.Visible` 对象（使每帧可见记录数从均值 638.7 膨胀到 1,878.0，产生 2.48 GiB 分配），经 FFM 拷贝到 C++。
   - C++ 侧 `TerrainScene::PrepareFrame`（`terrain_scene.cpp:253-262`）又用 `do { ... } while` 循环检查相邻 `rn_visible_item` 是否具有相同的 `(primitive, sort_key, first_instance, instance_count)`，把拆分出来的 `submesh` 重新合并回一个 32 位位掩码 `radray::VisibleMeshBatch::ElementMask`。
4. **区块编译临时数组双倍拷贝与 FFM 逐帧堆外 `malloc`/`free`（`MeshPayload` 与 `NativeRenderer`）**：
   - `SectionCompilerMixin` 捕获 SOLID 顶点 `ByteBuffer` 后，`MeshPayload.<init>` 先分配 `byte[] original = new byte[source.remaining()]`（286.8 MiB），紧接着又分配等大的 `byte[] vertices = new byte[original.length]`（396.2 MiB），且为每个 `MeshPayload` 额外分配 `float[3] minimum` 与 `float[3] maximum` 数组。
   - `NativeRenderer.marshal` 与 `NativeBindings.call` 每帧通过 `Arena.ofConfined()` 调用底层 `Unsafe.allocateMemory0` / `freeMemory0`（合计占主线程全部 Native 方法样本的 **7.83%**），并使用 `invokeWithArguments` 每次创建 `ArrayList` 并装箱参数。
5. **Native `Raynium.Scene.VisibleBatches` 冗余遍历与指针追逐（`terrain_scene.cpp`）**：
   - `VisibleBatches` 平均耗时 `0.079 ms`（占 `Raynium.Render` 23%，是 Native 第一大 CPU 子区间）。
   - `TerrainPrimitive` 本身不持有任何需要延迟退役的 GPU 句柄（也不传入 `RetireResource`），但 `state.primitives` 却存为未预留容量的 `std::unordered_map<uint64_t, std::shared_ptr<TerrainPrimitive>>`。
   - 每帧需遍历均值 1,878 条未合并记录，每组都要经历：`unordered_map` 桶查找 → `shared_ptr` 解引用 → `_scene.GetMeshBatch(batchHandle)` → 访问 `batch->VertexInput->Streams` 与 `batch->Elements.front()`，并对所有可见项重复对唯一共享的 quad 索引缓冲执行 `lists.geometry` 查重。

---

## 3. 用户已确认的核心设计决策

在制定本计划前，已与用户确认以下三项关键架构取舍：

1. **C ABI `rn_visible_item` 表示方式**：
   - **确认选择**：**直接传递 32 位 `element_mask`**（每个可见 section 仅生成 1 条 `rn_visible_item`，将原 `uint32_t submesh` 字段替换为 `uint32_t element_mask`，保留 `primitive / element_mask / sort_key / first_instance / instance_count` 完整字段与偏移）。
   - **效果**：将每帧跨界可见项均值从 `1,878.0` 降至 `638.5`（减少约 **66.0%**），消除 Java 侧拆分分配与 C++ `VisibleBatches` 中的循环重合并逻辑。
2. **可见列表排序与 `sort_key` / 上传预算策略**：
   - **确认选择**：**平时零排序（距离平方直接算 `sort_key`），仅在当帧未上传网格总字节超过 8 MiB 预算时才对未上传候选做距离排序**。
   - **效果**：在 >99% 的正常帧中彻底消除 `TerrainCapture.capture` 占主线程 8.06% CPU 的 `TimSort` 调用，同时严格保留超预算帧近处网格优先上传的视觉语义。
3. **推进方式**：
   - **确认选择**：**先将重构计划保存到 `doc/history/`，暂不修改代码**。

---

## 4. 分阶段重构方案

### 阶段一：网格编译瘦身与可见性扁平采集（`MeshPayload`、`TerrainCapture`）

#### 4.1 `MeshPayload` 零临时数组构造与预计算子网格元素掩码

- **直接从 `ByteBuffer` 转换顶点（消除 `byte[] original`）**：
  - 在 `MeshPayload.<init>(long epoch, ByteBuffer source)` 中，对 `source.slice().order(ByteOrder.nativeOrder())` 直接按偏移读取位置与面组判定数据，然后通过 `source.slice()`（或 `ByteBuffer.wrap(vertices)`）按面组偏移直接拷贝 112 字节 quad 到 `public final byte[] vertices`。
  - 删除 `byte[] original = new byte[source.remaining()]` 及其整块预拷贝，在区块编译工作线程直接省去约 286.8 MiB 临时数组分配。
  - **保留 `public final byte[] vertices` 不可变契约**：由于 `SectionCompilerMixin` 在 `SectionCompiler.<init>` 构造时固定捕获 `raynium$epoch`，若在上传后清空 `vertices` 或仅靠 `TerrainPreview.close()` 递增 `epoch`，同世界内重建 `TerrainPreview` 时现有编译器与已编译 `SectionMesh` 将无法恢复顶点数据。因此保持 `MeshPayload.vertices` 不可变，不引入 `releaseVertices()`。
- **预计算 7 位面组到紧凑非空子网格位的映射与标量包围盒**：
  - 将 `private final float[] minimum` 与 `maximum` 两个数组替换为 6 个标量字段 `minX, minY, minZ, maxX, maxY, maxZ` 及构造期预判的 `private final boolean finiteBounds`（为每个 `MeshPayload` 省去 2 个 `float[3]` 堆对象与热循环边界检查）。
  - `MeshPayload` 构造时统计 `groupVertices[0..6]`，非空面组按顺序对应紧凑子网格序号 `submeshIndex`（`0..6`，对应位值 `1 << submeshIndex` 为 `1..64`）。
  - 在 `MeshPayload` 中用单个基本类型字段 `private final long groupElementBits` 打包保存 7 个面组的子网格位：第 `g` 个面组（`0..6`）占第 `g` 个字节（`bits [8g .. 8g+7]`），若 `groupVertices[g] > 0` 则存入 `1L << submeshIndex`（取值 `1..64`，完全容纳于 8 bit 内，无需额外分配数组对象），否则为 `0L`；同时记录全可见掩码 `public final int allElementMask`（即所有非空子网格位的按位或，`(1 << submeshCount) - 1`）。
  - 新增 `public int visibleElementMask(double x, double y, double z)`：
    - 直接使用标量参数 `x, y, z` 与标量 `minX..maxZ` 比较（**删除 `double[] eye = {x, y, z}` 数组分配与每帧 6 次 `Float.isFinite` 检查**）；
    - 对未被背面剔除的面组 `g`，将其对应的 `(int) ((groupElementBits >>> (g * 8)) & 0x7FL)` 按位或入结果，一步返回紧凑 `elementMask`（取值范围 **`0..0x7F`**；当某 section 仅含单向面组且背向相机时，合法返回 `0`）。
- **内联渲染主线程驻留与帧活跃标记（消除每帧哈希表查询）**：
  - 在 `MeshPayload` 上新增仅由渲染主线程读写的原始类型状态字段：
    - `public long uploadedEpoch;`（该网格最近一次在 Native 完成驻留的全局唯一 `renderer` epoch，未驻留或已卸载时为 `0L`）
    - `public long stagedFrame;`（该网格在当前帧已被选入待上传队列的 `frameId`）
    - `public long lastActiveFrame;`（该网格最近一次作为可见项或回退项被使用的 `frameId`）
    - `public long sectionKey;`（该网格所属区块 section 的打包坐标 `SectionPos.asLong(x >> 4, y >> 4, z >> 4)`）
  - 配合 `NativeRenderer.nextEpoch()` 采用全局单调递增 `AtomicLong`，确保即使同世界内关闭并重建 `TerrainPreview` 或 `NativeRenderer`，新 `epoch` 与 `frameId` 也严格大于任何历史值，无需清空已有 `MeshPayload` 字段即可自动识别为未上传。

#### 4.2 `TerrainCapture` 扁平缓冲复用、引用清理与零排序距离量化

- **消除每帧 `Draw`、`Visible`、`Snapshot` 对象与 `List.copyOf` 分配**：
  - 在 `TerrainCapture` 内部维护仅在渲染主线程访问的可复用扁平采集缓冲（初始容量 1,024，按需 2 倍扩容）：
    - `MeshPayload[] payloads`
    - `int[] originX`, `int[] originY`, `int[] originZ`
    - `float[] visibility`
    - `int[] elementMask`（开启面组剔除时存 `payload.visibleElementMask(...)`，关闭剔除时存 `payload.allElementMask`；允许为 `0`）
    - `int[] distKey`（相机到 section 中心距离平方对应的单调非负整数键）
    - `int count`
  - **严格的 `MeshPayload[]` 引用清理规则（防止静态数组滞留已卸载网格及其 `vertices`）**：
    - `capture()` 结束时，若本次有效采集数 `newCount < count`（上一帧有效数），执行 `Arrays.fill(payloads, newCount, count, null)` 清空失效尾部槽位，再更新 `count = newCount`；
    - 在 `beginFrame()`、`invalidate()` 和 `activate(false)` 中，均执行 `Arrays.fill(payloads, 0, count, null); count = 0; hasSnapshot = false;`（其中 `activate(false)` 与 `invalidate()` 同步清空 `REMOVED`），确保可见数量减少、帧重置或退出世界时不会在静态 `payloads` 数组中残留任何过期 `MeshPayload` 强引用。
  - 复用相机与环境参数数组（`float[16] projection`, `float[16] view`, `int[3] block`, `float[3] fraction`, `float[4] fog`, `float[4] distances`）以及 `Matrix4f capturedProjection`（使用 `capturedProjection.set(value)` 替代 `new Matrix4f(value)`）。
  - 将 `REMOVED` 队列改为 `ConcurrentLinkedQueue<MeshPayload>`（消除 `CompiledSectionMesh.close()` 时的 `Long.valueOf(payload.id)` 装箱），`TerrainCapture.drainRemoved(Collection<MeshPayload> target)` 直接把 `REMOVED.poll()` 排空写入调用方提供的列表/集合。
  - 在遍历 `renderer.visibleSections()` 时，由于视锥内约 2,500～3,000 个 `RenderSection` 中约 75% 为 `CompiledSectionMesh.EMPTY` 或 `UNCOMPILED`，先通过引用判等快速跳过 `EMPTY` 与 `UNCOMPILED`，再提取 `PayloadOwner`。
- **无排序距离量化 `distKey`（供 `rn_visible_item.sort_key` 与超预算候选比较）**：
  - 在 `TerrainCapture.capture` 遍历 `visibleSections()` 时，计算相机到 section 中心 `(ox + 8.0 - camX, oy + 8.0 - camY, oz + 8.0 - camZ)` 的距离平方 `float distSq = (float) (dx * dx + dy * dy + dz * dz)`。
  - 由于 `distSq >= 0.0f`（IEEE 754 非负浮点数），`int distKey = Float.floatToRawIntBits(distSq)` 在非负区间内关于距离严格单调递增；将其右移量化（例如 `distKey >>> 16` 或直接使用非负位模式）即可作为 `rn_visible_item.sort_key` 传入 Native。
  - 这样在 `TerrainCapture.capture` 中**完全移除 `visible.sort(...)`（省去 8.06% 主线程 CPU）**，仅做一次线性遍历写入扁平数组。

---

### 阶段二：`TerrainPreview` 稳态零分配与稳态零哈希（$O(1)$ 内联状态 + 事件驱动驻留表）

#### 4.3 用 `MeshPayload` 内联状态与事件驱动 `residentBySection` 彻底消除稳态哈希操作

若仅把 `HashMap`/`HashSet` 替换为 `fastutil` 哈希表但仍每帧重建 `nextBySection` 与 `activeIds`，每帧依然要执行约 2,500+ 次哈希探测与数组清零。从根源上消除稳态 CPU 开销的设计如下：

- **稳态零哈希判别**：
  - 判断 `payload` 是否已驻留：`payload.uploadedEpoch == this.epoch`（单次寄存器级 `long` 比较，与 `payload.id`、`allElementMask` 位于同一 L1 缓存行，**0 次哈希查找**）。
  - 标记当帧正在使用：`activePayload.lastActiveFrame = this.frameId`（单次字段写入，**彻底省去 `activeIds` 哈希集合及其每帧 `clear()`**）。
- **事件驱动的坐标驻留表 `Long2ObjectOpenHashMap<MeshPayload> residentBySection`**：
  - 以打包后的 section 坐标 `long sectionKey = SectionPos.asLong(x >> 4, y >> 4, z >> 4)` 为键，持久记录该坐标当前在 Native 驻留的 `MeshPayload`。
  - **仅在发生网格上传（均值 0.145 次/帧）或网格卸载（均值 0.113 次/帧）时更新，仅在超 8 MiB 预算回退（<0.01% 的帧）时查询**；在 >95% 的稳态帧中对 `residentBySection` **0 次读取、0 次写入、0 次清空**。
- **待回收列表 `ObjectArrayList<MeshPayload> pendingRemovals`（或 `ObjectOpenHashSet<MeshPayload>`）**：
  - 收集 `TerrainCapture.drainRemoved` 排空的已关闭 `MeshPayload`；在 >95% 无卸载事件的帧中列表为空，直接跳过回收阶段。
- **彻底删除 `record Position(int x, int y, int z)`、`uploaded` 哈希表、每帧新建的 `HashMap<Position, Draw> next`、`HashSet<Long> usedIds` 和 `HashMap<Long, MeshPayload> staged`**（驻留网格总数由 `int residentCount` 计数器精确维护）。

#### 4.4 按需超预算排序、零掩码剔除与事务性延迟回收

在 `TerrainPreview.draw` 中处理 `TerrainCapture` 的扁平缓冲（设当帧共 `N` 个可见 section，进入本帧时递增全局单调 `frameId`）：

1. **检查未上传网格总字节数（快速路径 vs 超预算慢速路径）**：
   - 扫描 `0 .. N-1`：若 `payload.uploadedEpoch != this.epoch && payload.stagedFrame != this.frameId && payload.vertices.length > 0`，记录 `payload.stagedFrame = this.frameId`、`payload.sectionKey = SectionPos.asLong(x >> 4, y >> 4, z >> 4)`，将该下标 `i` 记入可复用的 `IntArrayList unuploadedIndices`，并累加 `unuploadedBytes += payload.vertices.length`。
   - **快速路径（>99.9% 的帧，`unuploadedBytes <= 8 MiB`）**：
     - 若 `unuploadedIndices.isEmpty()`（>95% 的稳态帧），本帧无任何新网格上传；
     - 否则所有未上传网格全部准许当帧上传（加入可复用的 `ObjectArrayList<MeshPayload> stagedMeshes`），**无需任何排序**。
   - **超预算慢速路径（仅在首帧或剧烈传送且当帧新网格 > 8 MiB 时触发）**：
     - 先将 `unuploadedIndices` 中各 `payload.stagedFrame` 重置为 `0L`，再按 `capture.distKey[idx]` 对 `unuploadedIndices` 执行 `IntArrays.unstableSort` 排序；
     - 从近到远累加字节数直至达到 8 MiB（至少放行最近 1 个），仅对获准上传的 `payload` 设置 `payload.stagedFrame = this.frameId` 并加入 `stagedMeshes`。
2. **单遍线性遍历可见项，处理回退、活跃标记与零掩码过滤**：
   - 对每个可见项 `i`（`0 .. N-1`）：
     - 若 `payload.uploadedEpoch == this.epoch || payload.stagedFrame == this.frameId`（>99.9% 帧中 100% 命中此分支，纯寄存器比较）：
       - `activePayload = payload`，`mask = capture.elementMask[i]`，`vis = capture.visibility[i]`。
     - 若未就绪（超出 8 MiB 预算未能在当帧上传）：
       - 计算 `long sectionKey = SectionPos.asLong(x >> 4, y >> 4, z >> 4)`，查询 `MeshPayload oldPayload = residentBySection.get(sectionKey)`；
       - 若 `oldPayload == null || oldPayload.uploadedEpoch != this.epoch`，跳过该项；
       - 否则选用回退网格 `activePayload = oldPayload`，`mask = oldPayload.allElementMask`，`vis = capture.visibility[i]`。
     - **标记当帧活跃状态**：执行 `activePayload.lastActiveFrame = this.frameId`（**即使当帧 `mask == 0` 因背面剔除不产生绘制，也必须标记 `lastActiveFrame`，以保护 `pendingRemovals` 中尚未执行的关闭标记**）。
     - **零掩码跳过提交**：若 `mask == 0`，直接 `continue`，不写入 `rn_instance` 与 `rn_visible_item`，避免向 Native 提交零掩码记录。
     - 若 `mask != 0`，直接将 `(x, y, z, vis)` 写入堆外 `instanceSegment` 第 `instanceIndex` 槽，并将 `(activePayload.id, mask, sortKey, instanceIndex, 1)` 写入堆外 `visibleSegment` 第 `visibleIndex` 槽。
3. **保留未就绪回退项的待回收状态与事务性提交（修复单次关闭通知丢失风险）**：
   - 若 `!pendingRemovals.isEmpty()`，遍历 `pendingRemovals`：
     - 若 `p.uploadedEpoch != this.epoch`，说明该网格从未在当前 epoch 驻留（或已卸载），直接从 `pendingRemovals` 移除；
     - 若 `p.lastActiveFrame == this.frameId`，说明该旧网格在当帧正因新网格超预算而被回退使用，**保留在 `pendingRemovals` 中暂缓删除**；
     - 否则（`p.uploadedEpoch == this.epoch && p.lastActiveFrame != this.frameId`），将 `p` 加入 `evictingPayloads` 并将 `p.id` 写入堆外 `removedSegment`。
   - 调用 `renderer` 执行 `rn_render_frame`；**仅在 `rn_render_frame` 成功后**提交状态变更（保证异常时状态不损坏）：
     - 对 `stagedMeshes` 中的每个 `p`：设置 `p.uploadedEpoch = this.epoch`，执行 `residentBySection.put(p.sectionKey, p)`，递增 `residentCount`；
     - 对 `evictingPayloads` 中的每个 `p`：设置 `p.uploadedEpoch = 0L`，若 `residentBySection.get(p.sectionKey) == p` 则 `residentBySection.remove(p.sectionKey)`，递减 `residentCount`，并从 `pendingRemovals` 中移除。

---

### 阶段三：C ABI 升级与 FFM 全帧堆外持久缓冲直写（`bridge.h`、`NativeRenderer`、`NativeBindings`）

#### 4.5 C ABI `rn_visible_item` 契约更新（ABI 版本升级至 10）

在 `native/include/raynium/bridge.h` 中：

- 将 `RN_ABI_VERSION` 从 `9u` 升级为 `10u`（同步更新 `NativeManifest.ABI_VERSION` 与 `tools/native.py` 中的 `ABI_VERSION` 常量）。
- 在 `rn_visible_item` 中**仅将 `uint32_t submesh` 替换为 `uint32_t element_mask`**，完整保留 `sort_key`、`first_instance` 与 `instance_count` 字段名、顺序及偏移：

```c
struct rn_visible_item {
    uint64_t primitive;
    uint32_t element_mask, sort_key, first_instance, instance_count;
};
```

- 结构体大小（24 字节）与所有字段偏移（`0, 8, 12, 16, 20`）完全不变，现有多实例范围语义（`first_instance`、`instance_count`）原样保留。
- **契约语义**：
  - `element_mask` 为非零子网格位掩码（bit `k` 为 1 表示该图元的第 `k` 个子网格可见；`UINT32_MAX` 表示该图元的全部子网格可见）。
  - Native 校验：`item.element_mask == 0` 返回 `RN_STATUS_FAILED`；若 `item.element_mask != UINT32_MAX` 且存在超出该图元子网格数量范围的置位，返回 `RN_STATUS_FAILED`。

#### 4.6 `NativeRenderer` 持久堆外帧缓冲（消除 7.83% Native `malloc`/`free`）与 5 参数 `invokeExact`

- **覆盖稳态帧与上传帧的持久可扩容堆外缓冲（修正 `rn_render_frame_desc` 大小为 344 字节）**：
  - 在 `NativeRenderer` 中维护与 renderer 生命周期绑定的持久 `Arena` 及可复用堆外缓冲（按 2 的幂次自动扩容，关闭 renderer 时统一释放）：
    - `frameDescSegment`：**固定 `344` 字节**（8 字节对齐，偏移 `0` 处 `struct_size = 344`）
    - `errorSegment`：固定 `1024` 字节诊断缓冲
    - `visibleSegment`：按 `24L * capacity` 自动扩容（初始容量 1,024 项 = 24 KiB）
    - `instanceSegment`：按 `16L * capacity` 自动扩容（初始容量 1,024 项 = 16 KiB）
    - `removedSegment`：按 `8L * capacity` 自动扩容（初始容量 256 项 = 2 KiB）
    - 上传描述与暂存段：`meshDescSegment`（`40L * cap`）、`submeshDescSegment`（`16L * cap`）、`primitiveDescSegment`（`24L * cap`）、`materialDescSegment`（`24L * cap`）、`indexDescSegment`（`24L * cap`）以及顶点/索引字节暂存段 `uploadDataSegment`：
      - **不设 8 MiB 硬上限**：8 MiB 预算仅用于 `TerrainPreview` 选择当帧待上传候选网格；若单个最近网格本身超过 8 MiB（例如 `stagingGrowsBeyondInitialCapacity` 测试的 `11,200,000` 字节顶点），或当帧同时包含未计入顶点预算的共享 quad 索引缓冲上传，`uploadDataSegment` 均按当帧实际获准上传的顶点字节、索引字节及 8 字节对齐填充总和动态计算所需容量并按需 2 倍扩容。
      - **帧间描述符状态复位**：每次写入 `frameDescSegment` 时，所有当帧数量为 `0` 的数组槽位（`indices`、`meshes`、`materials`、`primitives`、`removed_*`、`visible`、`instances`）必须显式将指针写为 `MemorySegment.NULL`、计数写为 `0`，确保从“有上传/移除帧”切换到“无更新稳态帧”时不会残留上一帧的指针与计数。
  - 无论稳态帧还是含区块上传/卸载的帧，均直接向上述持久堆外段写入数据并调用 `rn_render_frame`，**在 100% 的渲染帧中彻底消除 `Arena.ofConfined()` 触发的 `Unsafe.allocateMemory0` / `freeMemory0`（消除主线程 7.83% 的 Native 方法采样开销）**；同时保留 `NativeRenderer.render(NativeRenderTarget, Frame)` 接口兼容单元测试（`Visible` 记录类签名为 `record Visible(long primitive, int elementMask, int sortKey, int firstInstance, int instanceCount)`）。
- **消除 `NativeBindings.call` 在 `renderFrame` 上的 `invokeWithArguments` 装箱**：
  - `rn_render_frame` 的 C 签名为 `(rn_renderer*, rn_render_target*, const rn_render_frame_desc*, char*, uint32_t) -> rn_status`（共 5 个参数）。
  - 在 `NativeBindings` 中提供专用方法：

```java
static void callRenderFrame(MethodHandle renderFrame, MemorySegment renderer,
                            MemorySegment target, MemorySegment frameDesc,
                            MemorySegment error, int errorCapacity)
```

  - 内部直接调用 `(int) renderFrame.invokeExact(renderer, target, frameDesc, error, errorCapacity)`，消除每帧 `ArrayList` 分配、可变参数数组拷贝与返回值装箱。

---

### 阶段四：Native C++ `TerrainScene` 与 `VisibleBatches` 加速（`terrain_scene.h`、`terrain_scene.cpp`）

#### 4.7 `TerrainPrimitive` 按值存储、表容量预留与几何缓冲指针内联缓存

- **移除 `std::shared_ptr<TerrainPrimitive>` 间接层并预留哈希表容量**：
  - `TerrainPrimitive` 仅包含标量句柄与描述，不持有需要跨帧延迟释放的 RHI 对象。
  - 将 `TerrainState::primitives` 从 `std::unordered_map<uint64_t, std::shared_ptr<TerrainPrimitive>>` 改为 `std::unordered_map<uint64_t, TerrainPrimitive>`（按值存储），并在初始化及 epoch 重置后对 `state.meshes` 与 `state.primitives` 调用 `reserve(4096)`（实测常驻峰值 `2,179`），避免运行期渐进 rehash 并缩短桶链长度。
- **在 `TerrainPrimitive` 内缓存 `vertexBuffer`、`indexBuffer` 与 `elementCount`**：
  - 在 `TerrainPrimitive` 中新增字段：
    - `radray::render::Buffer* vertexBuffer = nullptr;`
    - `radray::render::Buffer* indexBuffer = nullptr;`
    - `uint32_t elementCount = 0;`
  - 当 `Batch(TerrainPrimitive& primitive, const TerrainState& state, radray::MeshBatch& batch)` 构建或刷新批次时，直接记录 `primitive.vertexBuffer = mesh.geometry.VertexBuffer()`、`primitive.indexBuffer = index`、`primitive.elementCount = uint32_t(mesh.ranges.size())`。

#### 4.8 `Raynium.Scene.VisibleBatches` 直接消费 `element_mask` 与索引缓冲短路去重

在 `TerrainScene::PrepareFrame` 的 `Raynium.Scene.VisibleBatches` 区间（`native/src/terrain_scene.cpp:238-269`）：

- **直接消费每条 `rn_visible_item.element_mask`，移除内层 `do { ... } while` 重合并循环**：
  - 对每个 `const auto& item : std::span{f.visible, f.visible_count}`：
    - 查找 `const auto it = state.primitives.find(item.primitive)`，校验图元存在、`primitive.batchHandle.IsValid()`、`primitive.elementCount > 0` 且实例范围 `item.instance_count > 0 && uint64_t(item.first_instance) + item.instance_count <= f.instance_count`；
    - 计算该图元全集掩码：`const uint32_t fullMask = primitive.elementCount >= 32 ? UINT32_MAX : ((1u << primitive.elementCount) - 1u);`
    - 校验 `item.element_mask != 0 && (item.element_mask == UINT32_MAX || (item.element_mask & ~fullMask) == 0)`，不满足则返回 `fail("Invalid visible SOLID submesh")`；
    - 计算实际掩码 `const uint32_t mask = item.element_mask == UINT32_MAX ? fullMask : item.element_mask;`
    - **使用指定成员初始化**推入 `lists.visibleBatches`：

```cpp
lists.visibleBatches.push_back({
    .Batch = primitive.batchHandle,
    .ElementMask = mask,
    .VisibilitySortKey = item.sort_key,
    .FirstInstanceOverride = item.first_instance,
    .InstanceCountOverride = item.instance_count
});
```

- **几何缓冲访问列表 `lists.geometry` 去重与相邻指针短路**：
  - 实测 `Raynium.Arena.Blocks` P50 为 **6**，`Raynium.GeometryAccesses` P50 为 **7**（6 个 Arena 顶点缓冲 + 1 个全局共享 quad 索引缓冲）。
  - 在循环外维护 `radray::render::Buffer* lastVB = nullptr;` 与 `radray::render::Buffer* lastIB = nullptr;`：仅当 `primitive.vertexBuffer != lastVB` 时调用 `addGeometry(primitive.vertexBuffer, ...)` 并更新 `lastVB`；仅当 `primitive.indexBuffer != lastIB` 时调用 `addGeometry(primitive.indexBuffer, ...)` 并更新 `lastIB`。
  - 由于全部地形图元共享唯一的 quad 索引缓冲，`lastIB` 短路可**消除第 1 个可见项之后 99.8%（638/639）的索引缓冲 `std::find` 调用**；而 `vertexBuffer` 也在省去 `_scene.GetMeshBatch` 与 `batch->VertexInput->Streams` 解引用的同时，仅对极小（$\le 7$ 项，常驻 L1）的 `lists.geometry` 查找去重。

---

## 5. 预期收益对比

| 维度 | 重构前实测基线（26,831 帧） | 重构后预期效果 |
| --- | --- | --- |
| `TerrainCapture.capture` 主线程 CPU | 占主线程 **12.93%**（其中 `TimSort` **8.06%**） | 移除稳态 `TimSort`、对象分配与冗余 `Float.isFinite`，空 section 快速跳过，仅保留扁平数组线性写入与标量位掩码计算 |
| `TerrainPreview.draw` 主线程 CPU 与哈希 | 占主线程 **10.40%**（每帧数千次 `HashMap`/`HashSet` 操作） | 稳态帧（>95% 帧）**0 次哈希表查找/插入/清空**（`MeshPayload` 内联 `uploadedEpoch`/`lastActiveFrame` 寄存器比较 + 事件驱动 `residentBySection`） |
| `TerrainPreview.draw` 主线程堆分配 | **6.43 GiB**（`6,907,766,952 B`，占主线程 **33.17%**） | 稳态帧 **0 对象分配**（内联状态 + 直接写入持久堆外 `MemorySegment`） |
| `TerrainCapture.capture` 主线程堆分配 | **1.46 GiB**（`1,565,528,472 B`，占主线程 **7.52%**） | 稳态帧 **0 对象分配**（复用扁平数组与矩阵/相机数组，移除 `Long` 装箱队列） |
| 主线程 Native `malloc`/`free` (`Arena`) | `Unsafe.allocateMemory0` + `freeMemory0` 占主线程 Native 样本 **7.83%** | 持久可扩容堆外缓冲覆盖稳态与上传帧，预热后 **0 次每帧 `Arena.ofConfined()` 系统分配** |
| 工作线程 `MeshPayload.<init>` 分配 | **683.0 MiB**（`716,177,288 B`，双份 `byte[]` + 2 个 `float[3]`） | 消除 `byte[] original`（省 **286.8 MiB**）与 `float[3]` 数组，保留不可变 `vertices` 支持同世界安全重建 |
| 每帧跨 FFM `rn_visible_item` 数量 | 均值 **1,878.0** 项（展开后） | 均值 **638.5** 项（每可见非空掩码 section 1 项），跨界可见记录数减少约 **66.0%** |
| Native `Raynium.Scene.VisibleBatches` | 均值 `0.079 ms`，P99 `0.255 ms` | 遍历项数减少 66%，移除 `shared_ptr` 解引用、`_scene.GetMeshBatch` 与内层重合并循环，预留桶容量并短路 99.8% 索引缓冲去重 |

---

## 6. 涉及文件与实施验证计划

### 6.1 涉及源码与构建文件清单

1. **C ABI 与构建配置**：
   - `native/include/raynium/bridge.h`：`RN_ABI_VERSION` 升至 `10u`，`rn_visible_item` 将 `submesh` 替换为 `element_mask`（保持 `primitive, element_mask, sort_key, first_instance, instance_count` 24 字节布局）。
   - `src/client/java/dev/raynium/bridge/NativeManifest.java`：`ABI_VERSION = 10`。
   - `tools/native.py`：`ABI_VERSION = 10`。
2. **Native C++ 场景与校验**：
   - `native/include/raynium/terrain_scene.h`：`TerrainPrimitive` 按值存储，内联缓存 `vertexBuffer`、`indexBuffer`、`elementCount`。
   - `native/src/terrain_scene.cpp`：`state.meshes`/`state.primitives` 预留容量，`PrepareFrame` 校验并直接消费 `element_mask`，移除内层 `do { ... } while` 合并循环，使用指定成员初始化构建 `VisibleMeshBatch`，增加 `lastVB`/`lastIB` 短路。
3. **Java FFM 桥接层**：
   - `src/client/java/dev/raynium/bridge/NativeBindings.java`：新增 5 参数 `callRenderFrame` 使用 `invokeExact`。
   - `src/client/java/dev/raynium/bridge/NativeRenderer.java`：`nextEpoch()` 采用全局单调递增计数器，`Visible` 第 2 参数改为 `elementMask`，新增 344 字节 `frameDesc` 与覆盖稳态/上传帧的持久可扩容堆外帧缓冲直写通道。
4. **Java 地形采集与同步层**：
   - `src/client/java/dev/raynium/client/terrain/MeshPayload.java`：直接从 `ByteBuffer` 读取顶点（省去 `byte[] original`）、改用标量包围盒 `minX..maxZ`、用 `long groupElementBits` 预计算紧凑子网格位并提供 `visibleElementMask`，内联主线程状态字段 `uploadedEpoch`、`stagedFrame`、`lastActiveFrame`、`sectionKey`。
   - `src/client/java/dev/raynium/client/terrain/TerrainCapture.java`：改为可复用扁平数组缓冲，快速跳过 `EMPTY`/`UNCOMPILED` section，计算浮点距离位 `distKey`，移除逐帧 `TimSort` 与 `List.copyOf`，将 `REMOVED` 改为 `ConcurrentLinkedQueue<MeshPayload>`。
   - `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`：基于 `MeshPayload` 内联字段与事件驱动 `residentBySection` 实现稳态零哈希、零分配堆外直写，跳过 `elementMask == 0` 的完全背面剔除项，保留回退旧网格在 `pendingRemovals` 中的待回收标记并在 `renderFrame` 成功后事务性提交，仅在当帧未上传字节 > 8 MiB 时对未上传候选排序。
5. **自动化测试（纯 Java `test` 与 Native `nativeTest`）**：
   - `src/test/java/dev/raynium/client/terrain/MeshPayloadTest.java`：验证直接从 `ByteBuffer` 提取并按面组重排顶点、标量包围盒边界、紧凑子网格位 `groupElementBits`（覆盖第 0～6 组映射到 `1..64`）、`allElementMask` 以及 `visibleElementMask`（覆盖部分面组剔除、单向面组完全背面剔除返回 `0`、`NaN` 视点回退 `allElementMask`）。
   - 新增/扩展 Java 地形同步与持久缓冲单元测试（归入 `test` 任务执行）：
     - 覆盖 `TerrainCapture` 可见数量缩减时的尾部 `null` 清理，以及 `beginFrame()` / `invalidate()` / `activate(false)` 时的全量 `payloads` 引用释放；
     - 覆盖超 8 MiB 预算时的近处优先上传、单个超大（>8 MiB）最近网格放行、同坐标未就绪新网格回退使用 `residentBySection` 旧网格；
     - 覆盖连续多帧超预算回退期间旧网格收到 `removed(oldPayload)` 关闭通知后的延迟回收（回退期间保留在 `pendingRemovals`，待新网格上传或离开视野后才真正卸载）；
     - 覆盖同世界内关闭并重建 `TerrainPreview`（`renderer.nextEpoch()` 递增）后，已有 `MeshPayload` 无需重编译即可重新上传恢复地形；
     - 覆盖 `NativeRenderer` 持久 `frameDescSegment` 从“含上传/移除帧”切换到“无更新稳态帧”时的指针与计数清零，以及 `uploadDataSegment` 超 8 MiB（含共享索引数据）动态扩容。
   - `src/test/java/dev/raynium/bridge/NativeTerrainTest.java`：将 `NativeRenderer.Visible` 的子网格索引参数迁移为位掩码 `elementMask`（如 `0 -> 1`、`1 -> 2`、`31 -> 1 << 31`、`-1` 保持全选，并覆盖多子网格掩码 `0b11`、多实例范围 `instanceCount = 2`、零掩码报错及越界位掩码报错）。

### 6.2 实施后验证步骤

1. **Java 单元测试、Native 集成测试与打包验证（注意 Gradle 属性名为大小写敏感的 `nativeConfig`）**：
   - 执行 `./gradlew.bat test nativeTest -PnativeConfig=Debug --rerun-tasks --console=plain --no-parallel`（同时运行纯 Java `test` 与 Debug `nativeTest`）。
   - 执行 `./gradlew.bat test nativeTest -PnativeConfig=Release --rerun-tasks --console=plain --no-parallel`（同时运行纯 Java `test` 与 Release `nativeTest`）。
   - 执行 `./gradlew.bat packageNativeMod verifyNativePackage -PnativeConfig=Release --console=plain --no-parallel`。
2. **实机高压存档 JFR + Tracy 复测对比**：
   - 在相同存档与固定路线下重跑 JFR + Tracy 联合采样，对比：
     - 主线程 `TerrainPreview.draw` 与 `TerrainCapture.capture` 的堆分配速率与累计分配量是否下降 >90%；
     - `TerrainCapture.capture` 的 `TimSort` CPU 样本是否归零；
     - GC 停顿次数与 >16.67 ms 慢帧数是否显著下降；
     - Native `Raynium.Scene.VisibleBatches` 均值与 P99 耗时是否下降。
3. **当前事实文档同步（待代码落地后执行）**：
   - 同步更新 [互操作与加载](../architecture/interop.md)（ABI 10 与 `rn_visible_item.element_mask`、持久堆外帧缓冲）、[地形预览](../architecture/terrain-preview.md)（扁平采集、按需预算排序、`fastutil` 驻留表）和 [架构地图](../architecture/overview.md)。
   - 运行 `python tools/check_docs.py` 与 `git diff --check`。
