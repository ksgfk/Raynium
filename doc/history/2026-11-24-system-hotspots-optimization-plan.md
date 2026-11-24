> - 适用: 执行或评审基于六场景全矩阵系统热点采样的 Raynium 内部与上游 RadRay 协同性能优化计划（排除继续向 Minecraft 原版其他渲染特性拓展新接管范围）
> - 权威: 经用户要求保存的设计与优化计划，不构成当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)、[互操作与加载](../architecture/interop.md)、[Native runtime](../architecture/native-runtime.md) 与 [地形渲染](../architecture/terrain-rendering.md)
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/include/raynium/buffer_registry.h`, `native/include/raynium/terrain_residency.h`, `native/src/renderer.cpp`, `native/src/buffer_registry.cpp`, `native/src/terrain_residency.cpp`, `native/src/command_tape.cpp`, `src/client/java/dev/raynium/bridge/NativeBindings.java`, `src/client/java/dev/raynium/bridge/NativeBuffer.java`, `src/client/java/dev/raynium/bridge/NativeRenderer.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumCommandEncoderBackend.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumRenderPassBackend.java`, `src/client/java/dev/raynium/client/mixin/ModelPartCompileMixin.java`, `src/client/java/dev/raynium/client/terrain/TerrainCapture.java`, `src/client/java/dev/raynium/client/benchmark/BenchmarkMeasurements.java`, `src/client/java/dev/raynium/client/benchmark/BenchmarkSession.java`

# 系统热点采样复盘与非 MC 拓展项优化计划

- 原记录日期：2026-11-24
- 状态：经用户要求归档的候选优化计划；归档时尚未实施本计划对应的源码与上游依赖改动
- 范围：严格限定在 Raynium 现有互操作桥接、RenderPearl 后端录制、已有刚体 `ModelPart` 快轨 Mixin、Native 缓冲生命周期、驻留地形捕获/半透明合批、基准诊断口径以及上游 `RadRay` Runtime 协同边界内；**按用户明确指令排除“继续向 Minecraft 侧拓展新渲染接管范围”的方向**
- 来源：2026-11-24 会话分析与当前工作树源码审计；实测证据来自 2026-11-23 于外部测试机采集并拷入本机忽略目录 `build/system-hotspots/` 的 30 轮完整采样（原版 Vulkan 对照 6 场、无 profiler 双后端 12 场、Tracy 0.15.0 + JFR 双后端联合录制 12 场）

---

## 0. 实测背景、数据口径与范围边界

### 0.1 采样环境与三项关键口径核实

测试环境为 i7-13700K、RTX 4080（NVIDIA 621.24）、OpenJDK 25（G1GC，最大堆 7.9 GiB）、Minecraft 26.3（窗口 1920×1080，渲染距离 32，模拟距离 8，Release `/MT`，ABI 22，关闭 VSync、Java Tracy、LWJGL 检查、Java 断言、RHI 与驱动校验）。对照源码与原始产物核实了以下三项影响归因判断的口径事实：

1. **OIT（`improvedTransparency`）实际生效范围**：虽然运行选项模板含 `improvedTransparency:true`，但 [BenchmarkSession.java](../../src/client/java/dev/raynium/client/benchmark/BenchmarkSession.java) 按基准配置 JSON 的 `"oit"` 字段显式覆写该选项。6 个场景中仅 `particles` 为 `oit = true`，其余 `metcalf`、`tideworn`、`newisle`、`entities`、`gui` 全部以 **`oit = false`（经典半透明）** 运行。
2. **`renderWallNs - nativeCallNs` 包含帧前段外部 GPU Fence 等待**：`BenchmarkSession.nativeBatch` 仅将 `rn_submit_frame` 调用计入 `nativeCallNs`。JFR `jdk.NativeMethodSample` 显示，在 `tideworn` 与 `metcalf` 中，MC 26.3 的 `DynamicGpuDataStorageMapped.writeData -> MappableRingBuffer.currentBuffer`（源码获取方式见 [阅读 Minecraft 源码](../guide/dev-environment.md#阅读-minecraft-源码)）在 `renderFrame` 前半段调用 `NativeRenderer.awaitSubmission -> waitSubmit` 阻塞等待前序批次完成（占 `tideworn` 渲染线程原生采样的 37.5% D3D12 / 51.0% Vulkan）。该 GPU 回压等待未计入 `nativeCallNs`，一方面抬高了表观“非 Native 渲染时间”，另一方面使 `rn_submit_frame` 内部的 `nativeWaitNs`（仅 0.014～0.050 ms）看似极低。
3. **Tracy 联合录制开销与 D3D12 GPU 区间盲区**：开启 `RAYNIUM_ENABLE_PROFILER` 后，`nativeCallNs` 单帧均值仅增加 `+10 ~ +47 µs/帧`；总帧率 `-5.5% ~ +5.8%` 的波动主要由跨进程 JVM C2 内联决策差异引起（如 `entities` D3D12 在联合采样下 `nativeCall` 慢 `+19.5 µs`、但 Java 侧快 `-108.0 µs`）。此外，D3D12 的 Tracy `gpu_inclusive` 仅覆盖了 `RDG UploadBatch` 与 `Raynium buffer copy`，未包裹光栅 Pass，整帧 GPU 对比须以 RDG 时间戳产出的 `gpuFrameNs` 为准。

### 0.2 六场景实测基线摘要（无 Profiler 轮 + 联合录制关键指标）

| 场景 | 原版 VK FPS | Raynium D3D12 / VK FPS | D3D12 / VK `gpuFrame` P50 (P95) ms | D3D12 / VK JVM 分配率 (MiB/s) | Raynium / RadRay 边界内的实测异常信号 |
| --- | ---: | ---: | ---: | ---: | --- |
| **Metcalf** | 532.03 | **997.84** / 954.91 | 0.683 (1.469) / 0.760 (1.496) | 517.9 / 518.8 | `t = 85~135s` 半透明顶点跨第 2 页使 `terrainDrawCalls` 从 P50 24 飙升至 **P95 636~646、峰值 1,096**；D3D12 `MeshDraw.SubmitIndirect` 达 `31.8 µs/帧`（VK `10.5 µs/帧`） |
| **Tideworn** | 377.30 | **713.35** / 699.87 | 1.253 (1.743) / 1.266 (1.941) | 568.6 / 579.4 | 后台编译占 59% CPU；`TerrainCapture.refreshDirty` 64 上限溢出触发全量持锁遍历，`SectionRenderDispatcher.unlock` 唤醒占 D3D12 `renderFrame` 样本 **7.7%**（慢帧 **26.8%**） |
| **Newisle** | 179.83 | **264.44** / 253.54 | 0.951 (1.145) / **0.883 (1.042)** | 1213.4 / 1172.0 | `nativeCall` 均值仅 0.65~0.68 ms；Vulkan `RDG.FillUploads` `41.7 µs/帧` 为 D3D12 `19.5 µs/帧` 的 **2.14 倍** |
| **实体 (128)** | 381.77 | **609.88** / 591.13 | **0.597 (0.646)** / 0.620 (0.647) | 628.4 / 637.4 | `ModelPartCompileMixin` 在 `ModelPart.render` 逃逸分配 **`CallbackInfo` 4.6~5.0 GiB（占全场分配 3.2%）** 并干扰多态 `VertexConsumer` 内联 |
| **粒子与透明** | 240.16 | 360.11 / **363.77** | **1.397 (1.447)** / 1.468 (1.785) | 700.8 / 703.7 | 每帧 ~810 KB `sceneUploadBytes`（`285 MiB/s`）；Vulkan `RDG.FillUploads` 高达 **`79.34 µs/帧`**（D3D12 仅 `24.07 µs/帧`，相差 **`55.27 µs/帧`**） |
| **背包 GUI** | 872.69 | 1142.51 / **1316.42** | **0.496 (0.501)** / 0.565 (0.586) | **227.2** / 257.5 | 每帧 12.14 个 Pass：`RayniumRenderPassBackend.<init>` 占堆分配 **6.4%~7.1%**，`NativeBuffer$Mapping.<init>` 占 **4.3%~4.7%**；`StagedVertexBuffer` 锯齿抖动使 `NativeBuffer.create` 占原生采样 **7.8%**、D3D12 `FrameSlot.DestroyObjects` 达 **`21.02 µs/帧`** |

### 0.3 范围切分：纳入项 vs 排除项

| 类别 | 具体内容 | 处理策略 |
| --- | --- | --- |
| **纳入本计划（`O1`～`O4`）** | `RayniumRenderPassBackend` 单实例复位复用；`NativeBindings` 运行期全量 `invokeExact` 化与 `NativeRenderer` 所属线程持久描述符槽；`NativeBuffer` 持久写映射轻量视图；`ModelPartCompileMixin` `@WrapMethod` 零分配改造；Native `BufferRegistry` 围栏安全延迟缓冲回收池；`TerrainCapture` $O(1)$ 位集去重与局部脏刷新扩容；经典半透明 `512 MiB` 顶点单页与非交叠相邻归并；上游 `RadRay` Vulkan `UploadArena` 主存策略与 D3D12 Tracy GPU 光栅区间；基准外部 `waitSubmit` 独立计时 | 全部位于 Raynium 现有模块或上游 `RadRay` 边界内，按本文第 1～4 节设计与验收标准实施 |
| **按用户要求排除（不纳入本计划）** | `ItemFeatureRenderer` 静态烘焙 Quad / 物品展示框模型快轨；Fabric Indigo `BlockModelRenderState` 网格拷贝绕过；`CampfireRenderer` 等空方块实体提取早退；`Particle.move` 体素碰撞与 `Particle.getLightCoords` 光照采样改造；实体名称标签 ICU `Bidi` / 字形排版缓存；静止 GUI 下 `LevelExtractor` 可见性重扫裁剪 | 均属继续向 Minecraft 原版或第三方模组业务层扩张新接管点，仅保留前文采样事实备查，不在本计划设计或落地 |

---

## 1. 支柱一（`O1`）：RenderPearl 编码、FFM 桥接与快轨 Mixin 零分配瘦身

> 以下 `O1`～`O4` 为助手基于实测数据与源码契约整理的技术方案，待后续执行阶段逐项落地。

### 1.1 `O1.1`：`RayniumRenderPassBackend` 单实例状态机复位复用

- **问题与证据**：[RayniumGpuDeviceBackend.java](../../src/client/java/dev/raynium/client/renderpearl/RayniumGpuDeviceBackend.java) 以 `RayniumRenderPassBackend::new` 构造编码器，每次 `createRenderPass` 均分配 1 个 `RayniumRenderPassBackend` 及 14 个内部数组（`Object[16]`、`GpuBufferSlice[16]`、7 个 `int[16]`、2 个 `long[16]`、`float[16]`、`NativeBuffer[16]`、`NativeTexture[16]`，约 1.1 KB/Pass）。在 `gui`（12.14 Pass/帧 × 1,316 FPS ≈ 1.6 万次/秒）与 `particles`（14.43 Pass/帧）中产生 16～18 MiB/s 无意义分配（GUI 场累计 >3.5 GiB，占总堆分配 6.4%～7.1%）。
- **契约核实**：MC 26.3 `FrontendCommandEncoder` 与 [RayniumCommandEncoderBackend.java](../../src/client/java/dev/raynium/client/renderpearl/RayniumCommandEncoderBackend.java) 均强制同一时刻至多打开一个 `RenderPass`（`passOpen == true` 时再次打开抛 `IllegalStateException`）。`FrontendRenderPass.close()` 置 `isClosed = true` 后立即回调 `submitRenderPass()`，关闭后所有前端绘制入口均被 `isClosed` 拦截，且 [RayniumGpuDevice.java](../../src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java) 的 `activeTape`、`recordInstancedGeometry`、`recordTerrain` 仅在活跃 Pass 内调用。
- **方案与生命周期**：
  1. `RayniumCommandEncoderBackend` 直接持有一个长驻 `RayniumRenderPassBackend` 实例（移除构造器上的 `BiFunction` 工厂分配，测试如需定制保留包级重载）。
  2. `RayniumRenderPassBackend` 新增包级 `open(CommandTape tape, RenderPassDescriptor descriptor)` 与 `closePass()`：
     - `open` 校验当前未处于活跃态，绑定新 `tape`、`area`、`scissor = area`，置 `active = true`；
     - 所有录制入口（`setPipeline`、`setUniform`、`drawCommand`、`instancedGeometry`、`terrain` 等）校验 `if (!active) throw new IllegalStateException("Render pass is closed")`；
     - `closePass()` 在 `submitRenderPass()` 与 `encoder.close()` 中调用：置 `active = false`，清空 `tape = null`、`pipeline = null`、`indexBuffer = null`、`indexType = null`、`pushLength = 0`，并对本 Pass 实际写入过的区间 `uniforms[0..uniformCount)`、非空 `vertices[0..16)`（维护 16 位 `vertexMask` 按位精准清零）、`bindingBuffer[0..bindingCount)`、`bindingImage[0..bindingCount)` 置 `null`，防止已关闭纹理/缓冲被空闲录制器意外强引用保活。

### 1.2 `O1.2`：`NativeBindings` 运行期全量 `invokeExact` 化与 `NativeRenderer` 固定描述符槽

- **问题与证据**：
  1. [NativeBindings.java](../../src/client/java/dev/raynium/bridge/NativeBindings.java) 仅对 `submitFrame`、`waitSubmit`、`syncQueryPool` 使用了 `invokeExact`，其余运行期高频/中频入口（`createBuffer`、`destroyBuffer`、`createTexture`、`uploadTexture`、`destroyTexture`、`upsertModelMesh`、`removeModelMesh`、`registerPipeline`、`destroyPipeline`、`createQueryPool`、`destroyQueryPool`）仍走可变参数 `NativeBindings.call(MethodHandle, String, Object...)` -> `MethodHandle.invokeWithArguments`，每次调用触发原始类型装箱、`Object[]` 分配与非内联参数适配（GUI 场 `NativeBuffer.create` 占渲染线程 `NativeMethodSample` 的 7.8%）。
  2. [NativeRenderer.java](../../src/client/java/dev/raynium/bridge/NativeRenderer.java) 的 `waitSubmit`（每帧 `pollCompleted()` 及 `MappableRingBuffer` 等待必调）与 `submitFrame` 每次调用都新建并关闭 `Arena.ofConfined()`，在 JFR 原生采样中可见 `Unsafe.allocateMemory0` / `Unsafe.freeMemory0` 开销。
- **方案、所有权与重入不变量**：
  1. 在 `NativeBindings` 中删除运行期热路径对 `invokeWithArguments` 的依赖，按签名提供具名或按固定形参类型重载的 `invokeExact` 辅助方法（保留统一的 `checkStatus(operation, status)` 异常语义）。
  2. 利用 `NativeRenderer` 严格绑定单所属线程的不变量（见 [Native runtime](../architecture/native-runtime.md)），在 `NativeRenderer` 内部持有一个随 `NativeRenderer.close()` 一起关闭的 `Arena scratchArena = Arena.ofConfined()` 及预分配对齐槽：
     - `waitResultSlot`（16 B，供 `waitSubmit` 复用）；
     - `batchDescSlot`（96 B）与 `submitResultSlot`（160 B，供 `submitFrame` 复用）；
     - `bufferDescSlot`（24 B）与 `bufferResultSlot`（16 B，供 `NativeBuffer.create` 在 `initial == null` 或直接 `ByteBuffer` 时零分配复用）；
     - 按高水位倍增复用的 `viewRecordsSlot` 与 `dirtyRangesSlot`（类似 `CommandTape.Stream`，低水位超 120 帧收缩回默认大小）。
  3. **回调重入安全约束**：`waitSubmit`、`syncQueryPool`、`submitFrame` 必须在 `invokeExact` 返回后的第一时间，将原生输出槽中的标量字段全部读取到 Java 栈局部变量（如 `long completed = waitResultSlot.get(JAVA_LONG, 0)`），**随后才调用 `publishCompleted`、`writableMappings.removeIf` 或完成回调**；严禁跨越任何回调读取共享槽，确保回调内部即便同步触发 `destroyBuffer` 或再次查询状态也绝无数据覆写风险。

### 1.3 `O1.3`：`NativeBuffer` 持久写映射轻量视图与脏区间紧凑追踪

- **问题与证据**：在 `gui` 场，`NativeBuffer$Mapping.<init>` 占全场堆分配的 4.3%～4.7%。当前 [NativeBuffer.java](../../src/client/java/dev/raynium/bridge/NativeBuffer.java) 每次 `map()` 即使针对持久映射上传缓冲（`address != 0`，如 `MappableRingBuffer` 与 `StagedVertexBuffer` staging 缓冲），也无条件创建 `Arena.ofConfined()`、`MemorySegment.reinterpret`、`HashSet<Mapping>` 节点与 `writableMappings` 列表项，并在每次 `submitFrame` 做 `writableMappings.removeIf(...)`。
- **方案、所有权与生命周期**：
  1. 当 `NativeBuffer` 的 `address != 0`（持久映射上传缓冲）时，在 `NativeBuffer` 创建期绑定一个归属该 `NativeBuffer` 的受限 `Arena`（在 `retire()` 真正释放或 `endRenderer()` 时关闭），预建全范围原生字节序 `MemorySegment baseSegment`；
  2. 针对最常见的单活跃写映射模式，在 `NativeBuffer` 内缓存单个可复用的写映射槽与 `ArrayList<Mapping> activeViews`（替代 `HashSet`），仅在存在并发多切片映射时才额外实例化 `Mapping`；非持久映射（`address == 0` 暂存缓冲）仍按需从临时或池化堆外段分配并在 `close()` 时写入当前 `CommandTape`。
  3. 用 `NativeBuffer` 上的布尔位 `trackedDirty` 与紧凑区间合并替代重复向 `NativeRenderer.writableMappings` 追加同一缓冲的多个短命 `Mapping` 对象；`Mapping.close()` 后仍通过显式状态位与受限作用域失效保证关闭后不可继续访问底层指针。

### 1.4 `O1.4`：`ModelPartCompileMixin` 切换 `@WrapMethod` 消除 `CallbackInfo` 堆逃逸

- **问题与证据**：[ModelPartCompileMixin.java](../../src/client/java/dev/raynium/client/mixin/ModelPartCompileMixin.java) 在 `ModelPart.render` 与 `ModelPart.compile` 两个递归/高频方法头部使用 `@Inject(at = @At("HEAD"), cancellable = true)`。在 `entities` 场中，由于 `ModelPart.render` 存在子节点递归与 `VertexConsumer` 多态调用（`ModelSink` vs `BufferBuilder`），HotSpot C2 无法对 SpongeMixin 生成的 `new CallbackInfo(...)` 完成逃逸标量替换，单轮 240 秒在堆上分配了 **4.6～5.0 GiB `CallbackInfo`（约 20 MiB/s，占全场总分配的 3.2%）**，且拉大了热方法字节码尺寸，加剧跨进程 C2 内联抖动。
- **方案**：将 `raynium$fastLeafRender` 与 `raynium$fastCompile` 两处 `@Inject(..., cancellable = true)` 直接改写为工程已内置的 MixinExtras 0.5.5 `@WrapMethod`（快轨处理完毕直接 `return`，需要回退原版路径时调用 `original.call(...)`）。外部语义、空节点剪枝、纯平移叶节点快轨及非主世界作用域回退行为 100% 保持不变，且 `CallbackInfo` 分配彻底归零。

---

## 2. 支柱二（`O2`）：Native `BufferRegistry` 围栏安全延迟回收池（消除瞬态缓冲锯齿抖动）

### 2.1 根因还原：MC 26.3 `StagedVertexBuffer$GpuBufferPool` 的隔帧创建/销毁震荡

JFR 与 Tracy 联合证据显示，静止打开生存背包的 `gui` 场中：
- Java 渲染线程 `NativeMethodSample` 有 **7.8%（656 次采样）** 落在 `StagedVertexBuffer$GpuBufferPool.acquire -> RayniumGpuDeviceBackend.createBuffer -> NativeBuffer.create`；
- Native 提交线程 `RadRay.FrameSlot.DestroyObjects` 在 D3D12 耗费 **`21.02 µs/帧`**（累计 5,474.6 ms，提交线程第 6 大 CPU 热点），是 Vulkan（`9.26 µs/帧`）的 **2.27 倍**，直接构成 `gui` 场 D3D12 慢于 Vulkan 的核心差值来源之一。

对照 MC 26.3 `net.minecraft.client.renderer.StagedVertexBuffer` 内部类 `GpuBufferPool`（按 `262,144 B = 256 KiB` 步进取整，含 `Staging`、`Vertex`、`Index` 三类 `usage`）的 `endFrame` 逻辑：
1. 将本帧使用的缓冲连同当前未提交批次的 `GpuFence` 放入 `pendingRecycle`；
2. **立即将 `available` 列表中本帧未消费完的所有空闲缓冲全部 `close()` 销毁**；
3. 再调用 `tryRecycleBuffers()` 以 `awaitCompletion(0L)` 轮询前序 Fence，将已完成批次的缓冲移入 `available` 供下一帧使用。

在 2-slot 在途流水线下，一旦相邻两帧的 GPU Fence 在同一轮轮询中同时跨过完成点，`available` 会一次性收到 2 块缓冲；下一帧 `acquire` 仅取走 1 块，剩余 1 块在 `endFrame` 步骤 2 被无条件 `close()` 销毁，而紧接着的下一帧若前序批次仍在 GPU 执行，`available` 变为空表，迫使下一帧 `acquire` 重新调用 `createBuffer` 分配 `256 KiB` 倍数的新缓冲并映射指针——由此陷入稳态下持续不断的“创建 + 映射 $\leftrightarrow$ 解映射 + 释放 RHI 资源”锯齿抖动。

### 2.2 方案、所有权与在途寿命设计（纯 Native 侧收敛，不侵入 MC 类）

在不新增 MC Mixin 的前提下，于 [buffer_registry.h](../../native/include/raynium/buffer_registry.h) / [buffer_registry.cpp](../../native/src/buffer_registry.cpp) 与 `Renderer` 之间建立围栏安全的延迟缓冲复用池：

1. **准入范围与键定义**：
   - 仅池化非主机读回缓冲（排除 `uses & RN_BUFFER_MAP_READ`，保留 `entry->upload` 持久映射上传缓冲与 `entry->device` 设备缓冲）；
   - 仅池化单块大小 `size <= 16 MiB`（覆盖 `StagedVertexBuffer` 常见的 `256 KiB`～数 MiB 块）的资源，以精确二元组 `(uint32_t uses, uint64_t size)` 作为键；
   - 设全局池字节上限（如 `64 MiB`）与最大空闲存活帧数（如 `120` 个提交序号未命中即真正释放），防止突发大缓冲长期驻留显存。
2. **退役隔离与 GPU 完成门控**：
   - 当前 `CommandTape` 解码期通过 `shared_ptr<BufferEntry>` 将缓冲保活在 `FrameSlot` 内；当 Java 调 `rn_destroy_buffer(id)` 时，`BufferRegistry::Destroy` 从活跃表 `_buffers` 移除该项，并传入当前 `Renderer` 的 `lastSubmittedSequence` 与 `completedSequence`：
     - 若该 `BufferEntry` 无挂起的 `_initial` 上传且满足池化条件，不让底层 `upload`（及其已 `Map` 的持久指针 `mapped`）或 `device` 随 `FrameSlot` 重置而释放 RHI 对象，而是将其底层句柄转移至退役池项 `RetiredBuffer{uses, size, retireSequence = lastSubmittedSequence, lastTouchedSequence = lastSubmittedSequence, device, upload, mapped}`；
     - 注意：若移除时尚有在途 `FrameSlot` 持有 `BufferEntry`（如 `CopyBuffer` 或 `ImmediateDraw` 引用 `upload.get()` / `device`），必须保证底层 RHI `Buffer` 在 `completedSequence < retireSequence` 之前绝不被复写或提前析构——因此仅当 `retireSequence <= completedSequence`（且 `BufferEntry` 外部 `shared_ptr` 引用已归零，即 `use_count() == 1` 或由 `BufferEntry` 析构钩子归还池）时，该缓冲才进入“可分配空闲态”！
   - 更简洁且零竞态的所有权流转：**由 `BufferEntry` 持有指向所属 `BufferPool` 的非拥有指针（随 `BufferRegistry` 存活）**。当 `_buffers.erase(id)` 且所有持有该 `shared_ptr<BufferEntry>` 的 `FrameSlot` 在 `BeginFrame -> DestroyObjects` 中均已完成 GPU 等待并释放最后一个 `shared_ptr` 时，`~BufferEntry()` 被调用——此时 **GPU 100% 已完成所有引用该缓冲的批次**！`~BufferEntry()` 若发现池未关闭且未超 `64 MiB` 上限，直接将已映射好的 `(upload, mapped)` 或 `device` `O(1)` 推入空闲桶（无需额外 Fence 判断，天然与 `FrameSlot` 生命周期完全对齐），否则才执行原有的 `Unmap()` 与 RHI 释放！
3. **创建命中与清理**：
   - `BufferRegistry::Create` 在分配新 RHI 缓冲前先查空闲桶 `(uses, size)`：命中则直接弹出已有 `(upload, mapped)` 或 `device` 组装新 `BufferEntry` 与新 `id`（若 `desc.initial != nullptr`，按原逻辑执行 `memcpy` + `FlushMappedRange` 或加入 `_initial`）；
   - 每次 `Renderer::SubmitFrame` 末尾按提交序号推进一次轻量淘汰，释放超过 120 帧未使用的池块；`BufferRegistry` 析构前先关闭池标记并清空空闲桶，确保设备销毁前全部 `Unmap` 与 RHI 资源干净释放。
4. **预期收益**：稳态帧下 `StagedVertexBuffer` 锯齿回收 100% 命中 `BufferRegistry` 内存池，D3D12MA / VMA 资源创建、`Map`/`Unmap` 与 `FrameSlot.DestroyObjects` 中的 RHI 销毁开销同步降至接近 0。

---

## 3. 支柱三（`O3`）：地形局部捕获锁竞争削峰与经典半透明跨页断批收敛

### 3.1 `O3.1`：`TerrainCapture` $O(1)$ 位集去重、自适应局部脏刷新与锁临界区瘦身

- **问题与证据**：
  1. 在 `tideworn` 中，后台 `Worker-Main-*` 线程占全进程 59% CPU 样本；高速航线（最高 ~77 方块/秒）下每帧变动的区段数频繁超过 [TerrainCapture.java](../../src/client/java/dev/raynium/client/terrain/TerrainCapture.java) `refreshDirty` 硬编码的 **`changed >= 64`** 上限，迫使主线程退回全量重捕路径：获取 `SectionRenderDispatcher.lock()`，持锁遍历 32 视距下 10,000～18,000 个 `RenderSection`（大量 L3 缓存未命中，占 Tideworn Vulkan 慢帧样本 28.0%），并在 `SectionRenderDispatcher.unlock()` 时频繁通过 `AbstractQueuedSynchronizer.signalNext` 唤醒阻塞的后台编译线程（占 Tideworn D3D12 `renderFrame` 总样本 **7.7%**、P95 慢帧样本 **26.8%**，另有单次 21.1 ms `lock` 阻塞）。
  2. 现有 `TerrainCapture.Buffer.refreshIndexCandidate`（第 74 行）与 `refreshDirty`（第 292、296 行）调用了 `uploadCandidates.contains(slot)` 和 `fadingSlots.contains(slot)`——对 `IntArrayList` 做 $O(k)$ 线性扫描；若直接调大 `64` 阈值而不消除线性查重，将退化为 $O(k^2)$。
- **方案、所有权与生命周期**：
  1. **$O(1)$ 世代戳去重替代 `IntArrayList.contains`**：
     - 在 `TerrainCapture.Buffer` 中新增与 `payloads` 同步 `ensureCapacity` 扩容的 `int[] candidateStamp`（配合 `int currentCandidateStamp`，整数回绕时清零数组）和 `boolean[] inFadingSlots`（或 `int[] fadingStamp`）；
     - `uploadCandidates` 查重/入队与 `fadingSlots` 在位判断/移出全部降为 $O(1)$ 数组下标读写。
  2. **放宽 `refreshDirty` 局部刷新阈值**：
     - 在完成 $O(1)$ 去重后，将 `refreshDirty` 的回退上限从固定 `64` 提升为自适应阈值 `int limit = Math.max(256, BUFFER.count >>> 2)`（即允许单帧局部刷新最多 256 个或当前可见区段数 25% 的原地脏更新；仅当视锥可见列表版本 `VISIBLE_LIST_VERSION` 变更、区段存在性翻转 `present != (slot >= 0)` 或超过 25% 可见区段同帧失效时才走全量重捕）。
  3. **缩短全量重捕持锁时间**：
     - 在全量重捕必须获取 `dispatcher.lock()` 时，临界区内仅顺序读取 `visibleSections()` 引用、`raynium$state()` 与 `getRenderOrigin()` 标量并记录 `captureSlot`；将 `getVisibility(now, fade)` 计算、`uploadCandidates` 筛选与尾部数组清理移至 `dispatcher.unlock()` 之后执行，压缩主线程持锁时长，降低与后台工作线程的锁碰撞概率。

### 3.2 `O3.2`：经典半透明 `512 MiB` 顶点单页收敛与跨页非交叠归并

- **问题与证据**：
  - 逐帧检查 `metcalf` 无 profiler 轮 `frames.csv` 可见：`_translucentIndices`（128 MiB/页）与 `_quadPool`（16 MiB/页）全程稳定在 `144 MiB`，**整场 240 秒从未跨入第 2 个索引页**；且在 `t = 155s` 飞离摩天楼密集区后，[阶段三第二轮全局 CPU 与跨页合批深度优化计划](2026-11-22-phase3-round2-global-cpu-and-batch-consolidation-plan.md) 落地的 `TrimEmptyPages()` 已正常将 `terrainDrawCalls` 压回 19～28。
  - **唯一残留的断批窗口在 `t = 85s..135s`（高密玻璃建筑群）**：常驻半透明顶点突破单页 `256 MiB`，溢出至第 2 个 `_translucentVertices` 页（总半透明顶点峰值低于 `512 MiB`）。由于经典半透明（`oit = false`）在 `TerrainResidency.java` 中为每个区段赋了严格唯一的 `sort_key = count - i`，当相邻远近区段在 `vertexPageId = 0` 与 `vertexPageId = 1` 之间交替时，[terrain_residency.cpp](../../native/src/terrain_residency.cpp) 的 `_fastClassicTranslucent` 合并条件 `item.vertexPageId == prevVPage` 连续失效，使 `terrainDrawCalls` 从 22 激增至 **P95 636～646、P99 929、峰值 1,096**，并将 D3D12 的 `RadRay.MeshDraw.SubmitIndirect` 推至 `31.8 µs/帧`。
- **方案与显存边界**：
  1. **`_translucentVertices` 单页容量提升至受设备上限约束的 `512 MiB`**：
     - 在 [renderer.cpp](../../native/src/renderer.cpp) `Renderer::Create` 中，根据 `caps.Limits.MaxBufferSize` 计算半透明顶点池单页字节上限 `min<uint64_t>(512ull << 20, caps.Limits.MaxBufferSize ? caps.Limits.MaxBufferSize : (512ull << 20))`，设 `PageVertexCapacity = uint32_t(pageBytes / 28)`；
     - **显存权衡**：在 `Metcalf` 高密区原本就会同时分配 `2 × 256 MiB = 512 MiB` 两张半透明顶点页，改为单张 `512 MiB` 页在 `Metcalf` 峰值期 **零额外显存增量**，而在普通无半透明世界若未分配半透明网格则不创建页（仅在存在半透明区块的小场景比原方案多预留 `256 MiB` 虚拟/专用池页，在现代独显预算内完全可控，且彻底消除 32 视距城市图经典半透明跨页断批）。
  2. **跨页兜底：相邻空间不交叠区段的局部同页归并（防御超 512 MiB 极端场景）**：
     - 在 `TerrainResidency::PrepareFrame` 构建 `_fastClassicTranslucent` 批次时，若检测到全局仍存在多个 `(vertexPageId, indexPageId)` 页对，可在保持画家算法等价性的前提下对小窗口（如固定长度 8～16 的滑动窗口）做屏幕投影/视线锥体不交叠交换：利用 `TerrainHotSlot` 已缓存的局部 AABB `[minBound, maxBound]` 与实例原点 `origin`，若两个相邻区段的 3D 世界包围盒相对于相机原点在任意分离轴上完全不产生前后视线遮挡，则允许将同 `(vertexPageId, indexPageId)` 的项相邻聚类，避免极端多页场景退化为上千次独立间接绘制。

---

## 4. 支柱四（`O4`）：上游 RadRay Vulkan 上传策略协同与诊断口径补齐

### 4.1 `O4.1`：Vulkan `RDG.FillUploads` 瞬态上传内存类型优化（上游 RadRay 协同）

- **问题与证据**：全部 6 个场景中，Vulkan 的提交线程 CPU 区间 `RadRay.RDG.FillUploads` 均显著慢于 D3D12（`particles`：`79.34 µs/帧` vs `24.07 µs/帧`，**慢 3.30 倍 / `+55.27 µs/帧`**；`newisle`：`41.66 µs/帧` vs `19.47 µs/帧`；`tideworn`：`28.63 µs/帧` vs `13.04 µs/帧`；`entities`：`15.39 µs/帧` vs `4.71 µs/帧`）。与此同时，GPU 侧 `RDG UploadBatch` 耗时呈现完全相反的比例（`particles`：Vulkan `1.69 µs/帧` vs D3D12 `4.97 µs/帧`；`tideworn`：Vulkan `2.09 µs/帧` vs D3D12 `7.10 µs/帧`）。
- **根因**：在启用 ReBAR 的 RTX 4080 上，Vulkan 侧 RDG `UploadArena` 选用了带 `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` 的 BAR 映射显存：CPU 在 `FillUploads` 中执行 `memcpy`（如 `particles` 每帧 ~810 KB 实例与动态数据）时受限于 PCIe BAR 写入带宽（实测等效 ~10.2 GB/s），而 GPU `UploadBatch` 变为显存内拷贝；反观 D3D12 的 `D3D12_HEAP_TYPE_UPLOAD` 位于系统内存（CPU 写入 ~33.6 GB/s，GPU 通过单次批量拷贝经 PCIe DMA 读取仅多花 `3.3 µs` GPU 时间，却省下 `55.3 µs` 提交线程关键路径 CPU 时间）。
- **协同方案**：在上游 `RadRay` Vulkan 后端针对 RDG `UploadArena`（即“CPU 密集顺序填充 $\rightarrow$ 紧跟 `RDG UploadBatch` GPU `vkCmdCopyBuffer` 复制到 `DEVICE_LOCAL` 目标缓冲”的瞬态暂存池）调整 VMA / 内存类型选择优先级：优先选择**不含 `DEVICE_LOCAL_BIT` 的主机系统内存**（`HOST_VISIBLE | HOST_COHERENT`，若平台提供 `HOST_CACHED` 则一并偏好，仅在 UMA 集显架构上自然保留统一内存），把跨 PCIe 传输交给 GPU 拷贝引擎异步流水化完成，释放主线程 `FillUploads` CPU 时间。更新后通过 [radray.lock.json](../../native/radray.lock.json) 正常升级锁文件接入。

### 4.2 `O4.2`：D3D12 Tracy GPU 光栅区间对齐与基准外部 `waitSubmit` 独立计时

- **问题与证据**：
  1. D3D12 下 Tracy `gpu_inclusive` 目前仅有 `RDG UploadBatch` 与 `Raynium buffer copy`，缺少与 Vulkan 对齐的 `RenderPearl raster pass` 及 `Raynium Output` GPU 区间；
  2. `BenchmarkSession` 未统计 `rn_submit_frame` 之外触发的 `NativeRenderer.waitSubmit` 耗时，导致 `tideworn` / `metcalf` 中 `MappableRingBuffer.currentBuffer()` 的 GPU Fence 同步等待混入“非 Native 渲染时间”。
- **方案与零侵入边界**：
  1. **上游 RadRay D3D12 Tracy GPU 光栅区间**：在 `RADRAY_ENABLE_PROFILER=ON` 编译开关下，为 D3D12 RDG 光栅 Pass 执行路径补齐与 Vulkan 一致的 Tracy GPU Zone 采集，关闭 profiler 的正式构建（`RAYNIUM_ENABLE_PROFILER=OFF`）保持零指令开销。
  2. **Java 基准 `nativeSyncWaitNs` 独立列**：在 [BenchmarkSession.java](../../src/client/java/dev/raynium/client/benchmark/BenchmarkSession.java) 与 [BenchmarkMeasurements.java](../../src/client/java/dev/raynium/client/benchmark/BenchmarkMeasurements.java) 中，仅当 `BenchmarkSession.isMeasuring()` 为真时，记录 `rn_submit_frame` 作用域之外 `NativeRenderer.waitSubmit`（含 `awaitSubmission` 与 `pollCompleted`）的累计耗时 `nativeSyncWaitNs` 写入 `frames.csv` 与 `client-result.json`。**不修改 C ABI（`bridge.h` 保持 ABI 22 不变）**，不增加非基准运行时的分支负担。

---

## 5. 实施顺序、兼容性约束与验收标准

### 5.1 工程硬约束核对

1. **C ABI 稳定性**：`O1`～`O3` 与 `O4.2` 均不新增或修改 `native/include/raynium/bridge.h` 导出函数、结构体布局或状态码（继续仅使用 `RN_STATUS_OK` / `RN_STATUS_FAILED`）。
2. **C++ 代码规范**：Native 新增的退役缓冲池与地形辅助结构禁止默认构造后 `Init` 的二段式初始化，统一保证构造即有效或通过 `static Create` 交付；容器与智能指针显式使用 `radray::vector`、`radray::unordered_map`、`radray::unique_ptr`、`radray::shared_ptr`、`radray::IntrusivePtr`，不引入 `using namespace radray`。
3. **文档同步边界**：本计划归档期间不修改 `doc/architecture/` 与 `doc/guide/`；待后续各分项实际合入源码后，再按实际落地行为同步更新 [互操作与加载](../architecture/interop.md)、[Native runtime](../architecture/native-runtime.md)、[地形渲染](../architecture/terrain-rendering.md) 或 [构建与测试](../guide/build-test.md)。

### 5.2 推荐分批实施顺序与验收指标

| 批次 | 包含项 | 必跑单元/集成测试 | 定向基准复测目标（同配置对照本次基线） |
| --- | --- | --- | --- |
| **第一批：Java 桥接与编码零分配 + Native 缓冲池化** | `O1.1`、`O1.2`、`O1.3`、`O1.4`、`O2` | `./gradlew test`（重点覆盖 `NativeBindingsTest`、`NativeCommandTapeTest`、`NativeRenderPearlTest`、`WorldFastTrackTest`；新增 `RayniumRenderPassBackend` 跨 Pass 复位/关闭守卫单测、回调重入安全单测及 `BufferRegistry` 退役复用与上限淘汰单测） | 1. `gui` 场 JFR 中 `RayniumRenderPassBackend.<init>` 堆分配归零，`NativeBuffer.create` 原生采样占比下降 **>90%**，D3D12 `FrameSlot.DestroyObjects` 降至 `< 5 µs/帧`，D3D12 与 Vulkan GUI 帧率差值明显收敛；<br>2. `entities` 场 JFR 中 `CallbackInfo` 堆分配在 `ModelPart.render`/`compile` 栈下归零 |
| **第二批：地形捕获削峰与半透明单页合批** | `O3.1`、`O3.2` | `./gradlew test`（`TerrainCaptureTest`、`TerrainResidencyTest`；补充 >64 脏区段局部刷新与世代戳去重单测）+ CTest `test_terrain_draw` + 透明/地形视觉一致性回归 | 1. `metcalf` 全程 240s `terrainDrawCalls` P95 从 `636~646` 收敛至 **`< 50`**，D3D12 `MeshDraw.SubmitIndirect` 降至 `< 12 µs/帧`；<br>2. `tideworn` JFR 中 `TerrainCapture.capture -> SectionRenderDispatcher.unlock` 样本占比显著下降 |
| **第三批：上游 RadRay 协同与诊断补齐** | `O4.1`、`O4.2` | 上游 `RadRay` 测试 + 更新 `native/radray.lock.json` 后全量 Native CTest 与 `./gradlew test`（更新 `BenchmarkMeasurementsTest` 列校验） | 1. `particles` 与 `newisle` Vulkan 场 `RadRay.RDG.FillUploads` CPU 耗时向 D3D12 水位（`~20~30 µs/帧`）收敛；<br>2. 开启 profiler 时 D3D12 `route-zones.csv` 可见 `RenderPearl raster pass` GPU 区间，且 `frames.csv` 准确分离 `nativeSyncWaitNs` |
