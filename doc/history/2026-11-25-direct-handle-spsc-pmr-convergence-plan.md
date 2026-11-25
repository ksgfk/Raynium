> - 适用: 重构 Raynium Native 桥接句柄、SPSC 帧通道、FrameSlot PMR 单遍编译、Java 录制热路径与地形常驻精简时的设计与实施参考
> - 权威: 历史架构设计与分步实施计划，按 2026-11-25 记录时状态理解，不构成当前技术契约；当前实现见 `doc/architecture/native-runtime.md`、`doc/architecture/interop.md` 与 `doc/architecture/terrain-rendering.md`
> - 锚点: `native/include/raynium/bridge.h`, `native/src/bridge.cpp`, `native/src/renderer.cpp`, `native/src/modular_frame_pipeline.cpp`, `native/src/terrain_residency.cpp`, `src/client/java/dev/raynium/bridge/NativeRenderer.java`, `src/client/java/dev/raynium/client/renderpearl/CommandTape.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumRenderPassBackend.java`

# 直连句柄、双槽 SPSC 帧通道、FrameSlot PMR 单遍编译与全链路精简收敛计划

- **记录日期**：2026-11-25（同日开工前追加 `D6`～`D12` 全链路精简项）
- **状态**：经用户确认由助手主导定稿的实施计划，归档时尚未实施
- **范围**：Raynium C ABI 25、跨语言不透明资源句柄、SPSC 渲染线程通道、基于 `RadRay::FrameSlot` PMR 的命令流单遍编译、单次拷贝回读路径、地形常驻单轨化与版本代数比对、Java RenderPearl 录制/瞬态内存/回调链去分配瘦身、Native 残留死代码清理及全量测试同步；不改动上游 `RadRay`
- **来源**：2026-11-25 会话两轮全仓架构复盘、当前工作树（已完成 ABI 24 的前端零屏障创建、`GpuUploadBuffer`、前端直连 `Fence` 采样与 `OnFrameSlotRetired` 遥测），以及 `native/radray.lock.json` 同日锁定的 `RadRay` 源码核查；相关前序计划见[渲染线程与新版 RadRay RHI 协同收敛重构计划](2026-11-25-render-thread-rhi-convergence-plan.md)与[C++ SRP 渲染管线基座、纯 HLSL/DXC 着色器系统与原版管线实施计划](2026-11-24-cpp-srp-and-hlsl-shader-system-plan.md)

---

## 一、背景与现状归因

### 1.1 用户目标与设计原则

在完成 ABI 24 的 RHI 多线程原语接入后，用户希望在不把 Minecraft/Raynium 业务抽象上推至 `RadRay` 的前提下，一方面彻底消除 `FramePacket`、`FrameQueue`、双端 ID 注册表与中间命令 AST 等“绕开 `RadRay` 自建一层”的历史包袱，另一方面抛开远期路线对**当前全仓（含 Java 录制端、地形常驻层与 Native 运行时）**做一次彻底的精简与性能优化，全部汇总为同一份可执行计划。

以下现状问题均经当前源码与测试集逐项核实，`D1`～`D12` 为据此制定的完整技术方案。

### 1.2 Native 桥接与帧调度层的四层结构性冗余（对应 `D1`～`D5`）

1. **双端整数 ID 镜像哈希表与异步注册闭包噪音**
   - 源码事实：`FrameQueue::CreateTexture`、`CreateBuffer`、`RegisterPipeline` 已在前端线程同步创建 `GpuTexture`、`GpuBuffer`/`GpuUploadBuffer` 与 `ImmediatePipeline`，但仍分配 `uint32_t` ID 存入 `FrameQueue` 的 `_textures`、`_usedTextureIds`、`_pipelines`、`_buffers`，再经 `RenderThreadSession::Post` 投递闭包写入 worker 侧第二套哈希表（`TextureRegistry::_textures`、`BufferRegistry::_buffers`、`ImmediatePipelineRegistry::_items`）。导致 worker 每条绘制解码均需执行多次 `unordered_map::find` 与智能指针原子引用计数增减。
2. **每帧堆分配 `FramePacket` + 闲置 `FrameSlot::GetMemoryResource()` + 多遍遍历**
   - 源码事实：`FramePacket::Create` 每次提交均新建 `Storage`（`HeapMemoryResource` + `monotonic_buffer_resource`）并全流扫描一遍 `InspectCommandStream` 以堆分配 `captures` 与 `queries`；`HOST_SHADOW` 脏区间也在 `FrameQueue::Submit` 中逐段堆分配 `radray::vector<radray::byte>`。Worker 虽通过 `_scheduler->BeginFrame()` 拿到了自带 `128 KiB` 预热线性池（`slot.GetMemoryResource()`）的 `FrameSlot`，但 `CommandTape` 仍在普通堆上分配 `vector<variant<...>>`、`make_shared<ImmediatePass>` 以及每条 `ImmediateDraw` 内的多个 `radray::vector`，经 `Decode`、`PrepareReadbacks`/`ReserveReadbacks`、`Replay` 多遍遍历后才进入 RDG。
3. **SPSC 固定双缓冲场景套用通用任务队列与大锁上帝类**
   - 源码事实：Raynium 严格只有 1 个前端所属线程 + 1 个渲染线程，且在途至多 1 帧执行中 + 1 帧待消费，却用无限长 `deque<MoveOnlyTask>` 与一把保护全部字段的 `FrameQueue::_mutex` 混管所有职责，稳态提交与进度轮询均需争抢互斥锁。
4. **截图回读的双倍 `8 MiB` 内存分配与深拷贝**
   - 源码事实：`radray::AsyncReadbackQueue` 本身已持有持久映射的 `ReadBack` 缓冲并支持 `Poll` 直接按行解包，但 `FrameQueue::Collect` 先在 worker 端分配整块 `Capture::pixels` 拷第一遍，`FrameQueue::PollCapture` 再拷第二遍到 Java 目标内存。

### 1.3 Java 热路径、地形常驻与 Native 辅助层的残留包袱（对应 `D6`～`D12`）

5. **地形网格未预分析旧回退双轨并存**：生产路径早已 100% 由 Java 后台线程通过 `MeshPayload` 预计算 7 组法线分类、包围盒与堆外重排（`rn_mesh_update::layout != nullptr`），但仓库仍同时维护 C++ `TerrainQuadGroup` + `AnalyzeTerrainMesh` + `TerrainMeshUpload` 实时重排器，以及 Java `NativeRenderer.Frame` 上的装箱 `List<Index>`/`List<Mesh>`/`List<Visible>`/`List<Instance>` 与 `IndexSnapshot.heapValues`。
6. **地形跨槽可见表与实例表的全量 `std::memcmp`**：双 `FrameSlot` 交替缓存 2 帧前的可见表，当存在区块淡入动画或新区块上传而未挂 `RN_FRAME_REUSE_VISIBLE | RN_FRAME_REUSE_INSTANCES` 时，`TerrainResidency::PrepareFrame` 对整张 `visible`（`N * 24 B`）与 `instances`（`N * 16 B`）执行 `std::memcmp`，不等时还要逐项循环比对 `origin[0..2]`。
7. **`RayniumTransientMemory.upload` 无谓开闭独立 `Arena.ofConfined()`**：`upload(List<ByteBuffer>, ...)` 并不对外返回 `MappedView`，却绕道调用 `allocateStaging`/`allocateGpuMapped` 新建 `Arena.ofConfined()`、包装 `ByteBuffer` 与 `MappedView`、塞入 `scopedViews` 再立即关闭；`multiUpload*` 还额外走 `.stream().map(...).toList()`。
8. **`RayniumRenderPassBackend` 包装与 11 个平行数组中转**：每次开 Pass 分配 `Lease` 转发壳，每次 `enableScissor` 分配 `RenderArea`，且维护 11 个平行数组先暂存绑定再二次遍历写入 `CommandTape`（重复两套相同序列化循环）。
9. **批次完成回调四队列碎片与 `TransientMemory` 双层闭包**：`NativeRenderer` 维护 4 条 `ArrayDeque<Completion>` 队列逐条包装 `Completion` record，`RayniumTransientMemory.seal` 又用 `HashSet<Runnable> pending` + 嵌套 lambda 追踪回收。
10. **`MeshPayload` 上传固定租约的重量级锁与分配**：每次 `pinVertices()` / `pinIndices()` 都在 `synchronized` 下分配 `UploadLease`、方法引用和只读 `MemorySegment` 切片，释放时二次加 `synchronized` 锁。
11. **Native 早期非 RDG 死代码残留**：`render_resources.*` 中的 `using RenderBuffer`（违反根 `AGENTS.md` 不新增导出智能指针本地别名的约束）、`CreateBuffer`、`TextureBarrier`、`BufferBarrier`、`Swapchain::_timeouts`，以及 `MeshPassResources::PrepareOutput` 全仓均已无调用。

---

## 二、总体目标架构与核心不变量

十二项收敛决策（`D1`～`D12`）协同落地，一次性在 **ABI 25** 内完成跨语言契约升级与内部瘦身：

| 维度 | ABI 24 现状 | ABI 25 收敛后架构 |
| --- | --- | --- |
| 跨语言资源标识（`D1`） | `uint32_t` ID + 前后端 6 张哈希表 + FIFO 闭包 | 不透明指针 `rn_texture*`/`rn_buffer*`/`rn_pipeline*`/`rn_query_pool*`，0 张 ID 哈希表、0 个队列闭包 |
| 批次交接与预检（`D2`） | 每帧 `new FramePacket` + 全流扫描分配 `captures`/`queries` | 常驻 2 个复用 `BatchEnvelope`（`64 KiB` 线性池）+ `rn_batch_desc` 直传切片 + 零分配头预检 |
| Worker 命令编译（`D3`） | 堆 `CommandTape` `variant` AST + `make_shared<ImmediatePass>` + 每 Draw 多 `vector` | `TapeCompiler` 单遍直译进 `slot.GetMemoryResource()` 扁平 `FrameTapePlan`，内联读回预留与池去重 |
| 线程通道与水位线（`D4`） | `deque<MoveOnlyTask>` + 大锁 `FrameQueue::_mutex` | 单槽批次邮箱 + 冷路径 `BarrierSlot` + Cache-line 对齐原子无锁进度快照 |
| 截图与缓冲回读（`D5`） | Worker 分配 `8 MiB` `Capture::pixels` 拷一次 + 前端再拷一次 | `_captureMutex` 下由 `rn_poll_capture` 从 `AsyncReadbackQueue` 映射内存单次解包至 Java |
| 地形布局与缓存比对（`D6`、`D7`） | 双端维护法线分析器 + 堆回退列表 + 整表 `std::memcmp` | 必填预分析 `layout`（删 C++ 分析器与 Java 堆列表）+ 64 位 `visible_version`/`origins_version` `O(1)` 判等 |
| Java 录制与瞬态上传（`D8`～`D11`） | `upload` 开闭 `Arena`、`Lease` + 11 数组、4 回调队列、`synchronized UploadLease` | `upload` 直写持久段、单实例 `RenderPassBackend` 单遍直写、单轨池化 `InFlightBatch`、`AtomicInteger` 无锁固定计数 |
| Native 辅助层卫生（`D12`） | 残留 `RenderBuffer` 别名、手写 Barrier 辅助与废弃 `PrepareOutput` | 清空全部零调用死代码与冗余计数 |

### 核心安全不变量证明

1. **为什么不透明裸指针在命令流编译期绝不悬垂**：
   - Worker 仅在 `Renderer::SubmitFrame` 完整执行完 `_scheduler->RunFrame()`（含 `Extract`、`SetupGraph`、`Prepare`、`Record` 与 `Queue::Submit`）并清空输入指针后，才推进 `consumed_sequence` 与 `submitted_sequence`。
   - Java `CommandTape` 通过 `lastRetainedTapeId` 保活本批引用的全部资源包装，仅当观测到 `consumed_sequence >= batchSequence` 后才执行 `release()` 允许触发 `rn_destroy_*`。因此在 worker 编译与建图全过程中，裸指针指向的句柄对象 **100% 存活且不可变元数据稳定可读**。
2. **为什么 `consumed_sequence` 后前端立即销毁句柄不破坏 GPU 在途执行**：
   - `radray::GpuTexture`、`radray::GpuBuffer`、`radray::GpuUploadBuffer` 均继承 `radray::ThreadSafeRefCountedBase`（原子引用计数，`Release` 为 `memory_order_acq_rel`），且 `RadRay` RHI 允许独立资源在 GPU 使用完成且子视图先释放后于任意线程销毁（`GpuTexture` 成员顺序保证 `_viewCache` 与 `_defaultView` 先于 `_texture` 析构）。
   - `SetupGraph` 对被访问纹理与缓冲统一调用 `RDGBuilder::ImportPersistentOwned`，由 RDG 帧槽持有 `IntrusivePtr` 直至 Fence 完成后槽退役。
   - 原生 `GraphicsPipelineState` 不经 RDG 引用计数持有，故 `rn_destroy_pipeline` 将 `unique_ptr<ImmediatePipeline>` 连同当前 `enqueued` 序号推入 `_retiredPipelines`，仅在 `completed_sequence >= retireSequence` 后释放。
3. **为什么 `TapeCompiler` 放在 `BeginFrame()` 之后、`AcquireNext` 之前而非 `SetupGraph` 内部**：
   - 上游 `RenderGraphFramePipeline::SetupGraph(RDGBuilder&, FrameSlot&) noexcept` 为 `void` + `noexcept`，且交换链 `AcquireNext` 必须在命令流与读回预留全部校验通过后才执行。`BeginFrame()` 返回后 `slot.GetMemoryResource()` 已经就绪并持续有效至下次槽回收，在此处单遍编译进 `slot.GetMemoryResource()` 既消除堆 AST，又完整保留“先校验、再取图、后建图”的错误语义。

---

## 三、Native 侧详细设计（`D1`～`D7`、`D12`）

### 3.1 `D1`：ABI 25 与直连不透明句柄（清退 6 张注册表）

`RN_ABI_VERSION` 升至 `25`（29 个导出入口名称集合不变），引入四个不透明句柄：

```c
struct rn_texture;
struct rn_buffer;
struct rn_pipeline;
struct rn_query_pool;
```

#### 1. 句柄结构与 `O(1)` 归属核验
每个句柄首部固定包含 `rn_renderer* owner` 与 `bool live{true}`，前端入口统一经 `ValidHandle(renderer, handle)` 检查非空、`owner == renderer` 与 `live`（违例返回 `RN_STATUS_FAILED`）：

- **`rn_texture`**：
  - 字段：`rn_renderer* owner`、`bool live{true}`、`rn_texture_desc desc`、`radray::IntrusivePtr<radray::GpuTexture> gpu`。
  - `rn_texture_desc`（`32 B`）：`uint32_t struct_size, width, height, mip_levels; rn_pixel_format format; uint32_t array_layers, flags, reserved;`。
  - `rn_create_texture(rn_renderer*, const rn_texture_desc*, rn_texture** out_texture)`：前端直接调用 `CreateSceneTexture` 并返回指针。
  - `rn_upload_texture(rn_renderer*, const rn_texture_upload_desc*)`：`rn_texture_upload_desc`（`56 B`）偏移 8 存放 `rn_texture* texture`；前端紧凑拷贝像素后向 `PendingResourceOps` 推入持有 `IntrusivePtr<GpuTexture>` 的 `PendingTextureUpload`，即便随后立即 `rn_destroy_texture` 仍保活至下一帧 RDG 导入。
  - `rn_destroy_texture(rn_renderer*, rn_texture*)`：置 `live = false` 后前端直接 `delete texture`。
- **`rn_buffer` 与 `BufferPool`（合并 `FrontBuffer` 与 `BufferEntry`）**：
  - 字段：`rn_renderer* owner`、`bool live{true}`、`uint32_t usage`、`uint64_t size`、`radray::IntrusivePtr<radray::GpuBuffer> device`、`radray::IntrusivePtr<radray::GpuUploadBuffer> upload`、`radray::IntrusivePtr<HostReadStorage> readbackHost`（仅 `MAP_READ` 分配，继承 `ThreadSafeRefCountedBase`，含 `std::atomic<bool> alive{true}` 与 `radray::vector<radray::byte> bytes`）、`radray::vector<radray::byte> shadowHost`（仅非轮换、非 `COPY_DST` 的 `MAP_WRITE` 分配）及 `radray::byte* mapped`。
  - `rn_buffer_result`（`16 B`）：`rn_buffer* buffer; void* mapped;`；`rn_mapped_range`（`24 B`）：`rn_buffer* buffer; uint64_t offset, length;`。
  - `BufferRegistry` 瘦身为无 `_buffers` 哈希表的 **`BufferPool`**：按 `(usageKey, size)` 池化 `GpuBuffer` 与 `GpuUploadBuffer`（`16 MiB` 单块上限、`64 MiB` 总预算、`120` 次提交闲置淘汰）；`Take` 同时要求 `retireSequence <= completedSequence` 且 `UseCount() == 1`（自动涵盖 RDG 槽持有期与未消费初始上传期）。
  - `rn_create_buffer` 带 `initial` 时：`FENCE_ROTATED` 前端拷入 `mapped` 并当场 `FlushMappedRange`；`Device` 缓冲向 `PendingResourceOps` 推入 `PendingBufferUpload{buffer->device, 0, initialBytes}`。
  - `rn_destroy_buffer`：置 `live = false`，`MAP_READ` 置 `readbackHost->alive = false`，其余将底层 GPU 对象连同当前 `enqueued` 序号归还 `BufferPool`，随即 `delete buffer`。
- **`rn_pipeline`**：
  - `rn_register_pipeline(rn_renderer*, const rn_pipeline_desc*, rn_pipeline** out_pipeline)` 前端同步构建 `ImmediatePipeline`；`rn_destroy_pipeline` 将 `{std::move(impl), enqueuedSequence}` 移入 `PendingResourceOps::retiredPipelines` 后 `delete pipeline`。
- **`rn_query_pool`**：
  - 持有 `radray::IntrusivePtr<QueryPoolStorage>`（`ThreadSafeRefCountedBase`，内含 `std::mutex` 与 `radray::vector<QuerySlot>`），`rn_create_query_pool` / `rn_destroy_query_pool` / `rn_sync_query_pool` 直接读写句柄，清退 `_queries` 哈希表。

#### 2. 类型化冷路径收件箱 `PendingResourceOps`
以一把互斥锁保护四个紧凑 `radray::vector`（`textureUploads`、`bufferUploads`、`removedModelMeshes`、`retiredPipelines`），取代 `RenderThreadSession` 的通用闭包队列；worker 在每帧 `BeginFrame()` 后通过一次 `std::swap` 整批取走处理。

#### 3. 命令流二进制记录的 8 字节句柄对齐布局
全部命令保持 8 字节对齐，将原 32 位 ID 替换为 64 位句柄指针：
- `WireRegion`（`32 B`）：`rn_texture* texture; uint32_t mip, layer, x, y, width, height;`
  - `WireClear`（`56 B`）：`Header(8) + WireRegion(32) + float value[4](16)`
  - `WireCapture`（`48 B`）：`Header(8) + WireRegion(32) + uint64_t id(8)`
  - `WireUpload`（`56 B`）：`Header(8) + WireRegion(32) + uint32_t pitch, reserved(8) + uint64_t offset(8)`
  - `WireCopy`（`72 B`）：`Header(8) + WireRegion source(32) + WireRegion destination(32)`
  - `WireTextureBuffer`（`64 B`）：`Header(8) + WireRegion image(32) + rn_buffer* buffer(8) + uint64_t offset(8) + uint64_t pitch(8)`
- `WireBlit`（`24 B`）：`Header(8) + rn_texture* texture(8) + uint32_t mip, reserved(8)`
- `WireTimestamp`（`24 B`）：`Header(8) + rn_query_pool* pool(8) + uint32_t slot, reserved(8)`
- `WireWriteBuffer`（`40 B`）：`Header(8) + rn_buffer* target(8) + uint64_t offset, size, uploadOffset(24)`
- `WireCopyBuffer`（`48 B`）：`Header(8) + rn_buffer* source(8) + rn_buffer* destination(8) + uint64_t srcOffset, dstOffset, size(24)`
- 光栅记录（`native/include/raynium/raster_records.h`）：
  - `wire::Pass` 头 `32 B`，每个附件项固定 `40 B`：`rn_texture* texture; uint32_t mip, layer, clear, reserved; ColorClearValue value;`。
  - `wire::Draw`（`64 B`）：`uint32_t opcode, size; rn_pipeline* pipeline; uint32_t kind, count, instances, first, firstInstance; int32_t base; radray::Rect scissor; uint32_t bindings, push;`。
  - `ShaderResourceBindings::Binding`（`48 B`）：`uint64_t resource; uint64_t offset; uint64_t size; uint8_t kind, format; uint16_t slot; uint32_t reserved; rn_texture_sampling sampling;`。
  - `wire::Geometry`（`64 B`）：`uint32_t opcode, size; rn_pipeline* pipeline; uint32_t bindings, runCount, instanceCount, instanceStride, push, reserved; uint64_t uploadOffset; radray::Rect scissor;`。
  - `wire::Terrain`（`136 B`）：`uint32_t opcode, size, view, stage; radray::Rect scissor; rn_texture* sources[3]; uint32_t mips[3], reserved; TerrainMaterial materials[2];`（`TerrainMaterial` 为 `32 B`：`rn_texture* texture; uint32_t flags, reserved; rn_texture_sampling sampling;`）。

---

### 3.2 `D2`：常驻双槽 `BatchEnvelope` 与零分配头预检（删除 `FramePacket`）

1. **`rn_batch_desc`（`128 B`）直传 `captures` 与 `queries`**：
   - 新增 `struct rn_timestamp_write { rn_query_pool* pool; uint32_t slot, reserved; };`（`16 B`），在 `rn_batch_desc` 末尾追加 `captures`（`const uint64_t*` + `capture_count`）与 `queries`（`const rn_timestamp_write*` + `query_count`）。
2. **双槽复用 `BatchEnvelope`**：
   - `RenderChannel` 持有 `radray::array<BatchEnvelope, 2> _envelopes`，每槽内置 `64 KiB` 初始块的 `HeapMemoryResource` + `std::optional<std::pmr::monotonic_buffer_resource>`，在批次消费完成后原地复位。
   - 非轮换 `HOST_SHADOW` 脏区间字节在前端 `Submit` 中直接拷入当前 `BatchEnvelope` 线性池，生成 `std::span<ShadowWrite>`，worker 在 `SetupGraph` 中经 `RDGUploadSource::BorrowedBytes` 零拷贝上传。
3. **无分配 `ValidateCommandHeaders`**：
   - 对 L1 驻留的 `batch.commands` 做一次纯指针跳跃结构预检（不分配任何容器），校验对齐、Pass 闭合、Opcode 作用域、`BlitToSurface` 配对及上传边界，完整保留 `rn_submit_frame` 入队前对损坏字节流同步返回 `RN_STATUS_FAILED` 并置 `RN_SUBMIT_FLAG_TERMINAL_ERROR` 的测试契约。

---

### 3.3 `D3`：基于 `FrameSlot::GetMemoryResource()` 单遍编译 `FrameTapePlan`（清退堆 AST）

1. **`TapeCompiler::Compile` 单遍流式构建**：
   - 位于 `BeginFrame()` 之后、`Swapchain::AcquireNext` 之前，全部输出结构从 `slot.GetMemoryResource()` 线性切分（临时去重集合使用 `slot.GetScratchMemoryResource()`）。
   - 单遍扫描二进制命令流生成平凡可析构（`is_trivially_destructible_v`）的 `FrameTapePlan`（`std::span<TerrainView>` + `std::span<TapeOp>`）与扁平 `RasterPassPlan`：
     - 解码时直接解引用句柄指针，零哈希查找、零 `shared_ptr` 原子计数；
     - 扫描遇到 `CaptureColor`、`MAP_READ` 目标传输与 `WriteTimestamp` 时**当场内联完成** `AsyncReadbackQueue::ReserveTexture`、`FrameReadbacks::Reserve` 与时间戳登记；
     - 添加光栅子命令时**顺手累加** Pass 级 5 个几何池读标志及实例/间接缓冲读标志，消除 `ImmediatePass::Replay` 的二次遍历；
     - Push Constant 若无需补零直接借用命令流切片，需补零时从帧槽线性池切出。
2. **`SetupGraph`、`Prepare` 与 `Record` 全程 PMR 化**：
   - `SetupGraph` 顺序遍历 `FrameTapePlan::ops` 注册 RDG 节点；`RDGPrepareContext` 中顶点缓冲绑定切片与 `drawBaseSets` 同样从 `slot.GetMemoryResource()` 分配；帧槽轮转时由 `FrameSlot::ResetFrameData()` 一键回收整块内存。

---

### 3.4 `D4`：SPSC `RenderChannel` 与无锁原子水位线（删除 `FrameQueue` / `RenderThreadSession`）

新增 `native/include/raynium/render_channel.h` 与 `src/render_channel.cpp`，删除 `frame_queue.*` 与 `render_thread_session.*`：
- **单槽批次邮箱 + 冷路径 `BarrierSlot`**：热路径仅交换 `BatchEnvelope* _queuedBatch`；冷路径生命周期、窗口与 `rn_upsert_model_mesh` 共用栈上借用的同步 `BarrierSlot`，彻底清退 `std::deque<MoveOnlyTask>`。
- **Cache-Line 对齐无锁进度快照**：`_consumed`、`_submitted`、`_completed`、`_presentedFrames`、`_inFlightReadbackCeiling`（无读回时为 `UINT64_MAX`）、`_pendingHostPublishes` 独立原子化。常规帧前端直接采样硬件 `Fence` 并通过 CAS 推高 `_completed`，组装 `rn_wait_result` 全程零互斥锁争用；存在在途读回时维持 `ceiling = firstPendingSequence - 1` 与按需 `2 ms` 唤醒，阻塞 `Wait` 维持 `2 ms` 时间片 + `PumpSentMessages()`。

---

### 3.5 `D5`：单次拷贝回读路径

- **截图单次解包**：`ModularFramePipeline` 用专用 `_captureMutex` 保护 `_readback`（`AsyncReadbackQueue`）与 `_pendingCaptures`。Worker `Collect()` 传空目标指针 `_readback.Poll(id, done, info, nullptr, 0)` 验证就绪，不分配缓冲、不拷贝字节；前端 `rn_poll_capture` 在 `pixels != nullptr` 时直接调 `_readback.Poll(id, completed, result, pixels, capacity)` 从持久映射 `ReadBack` 缓冲一次性逐行解包写入 Java 目标内存。
- **`MAP_READ` 主机发布**：`FrameReadbacks` 直接持有 `IntrusivePtr<HostReadStorage>`，仅当 `_pendingHostPublishes > 0` 时在前端同步点（`Submit`/`Wait`/`SyncQueryPool`/`Flush`）将已失效缓存的字节按序拷入仍存活缓冲的 `bytes`。

---

### 3.6 `D6` 与 `D7`：地形常驻单轨化与 `O(1)` 版本代数判等

1. **`D6`：收紧 `rn_mesh_update::layout` 为必填，删除 C++ 实时法线分析器**：
   - `BatchEnvelope` 视图冻结与 `TerrainResidency::PrepareFrame` 直接要求 `mesh.layout != nullptr`（空指针返回 `false`）。
   - 删除 `native/include/raynium/terrain_draw.h` 与 `native/src/terrain_draw.cpp` 中的 `TerrainMeshUpload`、`TerrainQuadGroup`、`AnalyzeTerrainMesh`，以及 `terrain_residency.cpp` 中 `x.layout == nullptr` 的 `RDGUploadSource::Generated` 重排分支，消除 Java `MeshPayload.classifyQuad` 与 C++ 双端镜像维护负担。
2. **`D7`：`rn_terrain_view_desc` 增加 `visible_version` 与 `origins_version`（消除整表 `std::memcmp`）**：
   - `rn_terrain_view_desc` 升级为 `304 B`（`terrain_albedo_texture` 与 `terrain_light_texture` 改为 `rn_texture*`，并新增 `uint64_t visible_version, origins_version;`）：
     - Java `TerrainResidency` 在每次构建可见集或槽位映射变化时递增单调非零 `visibleVersion`；在实例区块坐标列表变化时递增单调非零 `originsVersion`（仅 `visibility` 淡入系数更新时保持 `originsVersion` 不变）。
     - C++ `TerrainResidency::PrepareFrame` 对非零版本号直接以 `cache.visibleVersion == f.visible_version` 与 `cache.originsVersion == f.origins_version` 在 `O(1)` 时间内判定 `visibleIdentical` 和 `originsIdentical`（仅在不相等时才更新 `cache.visible` / `cache.instances`），彻底省去数千项 `rn_visible_item`（`N * 24 B`）与 `rn_instance`（`N * 16 B`）的全量 `std::memcmp` 及逐项 `origin[0..2]` 循环扫描。

---

### 3.7 `D12`：清理 Native C++ 零调用残留死代码

- 从 `native/include/raynium/render_resources.h` 与 `native/src/render_resources.cpp` 删除已无任何调用方的 `using RenderBuffer`、`CreateBuffer`、`TextureBarrier`、`BufferBarrier`，以及只增不读的 `Swapchain::_timeouts` / `Timeouts()` / `TimedOut()`；
- 从 `mesh_pass.h/.cpp`、`renderer.h`、`modular_frame_pipeline.h` 与 `bridge.cpp` 删除零调用的 `MeshPassResources::PrepareOutput` 及仅比较 `BGRA8_UNORM` 常量的 `PrepareOutput` 空壳链路（`Swapchain::Create` 自身已查询并校验 `BGRA8_UNORM` 渲染目标支持）。

---

## 四、Java 桥接、录制热路径与地形常驻精简设计（`D8`～`D11`）

1. **ABI 25 签名与句柄长整化（配套 `D1`、`D2`、`D6`、`D7`）**：
   - `NativeManifest.ABI_VERSION` 与 `tools/native.py` 升至 `25`，`NativeBindingsTest` 负例列表加入 `24`；
   - `NativeBindings.java` 将纹理、缓冲、管线、查询池的创建/销毁/同步签名统一改为 64 位句柄（`ADDRESS` / `JAVA_LONG`），更新 `BATCH_BYTES = 128`、`NativeFrameBuffer.BYTE_SIZE = 304`；
   - `NativeTexture`（移除 `nextTextureId`）、`NativeBuffer`、`RayniumPipeline`、`RayniumQueryPool` 统一保存 `long handle`；`CommandTape` 更新全部 16 个 Opcode 偏移，并用紧凑基本类型数组 `long[] captureIds`（替代 `HashSet<Long>`）与 `long[] queryWrites` 直填 `rn_batch_desc`。
   - **清退 Java 地形堆回退路径（配套 `D6`）**：删除 `MeshPayload.IndexSnapshot` 的 `int[] heapValues` 构造器与 `values()` 拷贝分支；删除 `NativeRenderer` 上的 `Index`、`Mesh`、`Visible`、`Instance` 装箱 Record、`NativeRenderer.Frame` 上的 4 个 `List` 字段以及 `NativeFrameBuffer` 中的 `mesh(byte[])` / `index(int[])` / `write(Frame)` 堆拷贝方法。由于 `MeshPayload` 本身零 MC 类依赖，单测辅助统一通过 `new MeshPayload(epoch, ByteBuffer.wrap(...), layer)` 与 `copyIndices` 构造预分析输入。
2. **`D8`：`RayniumTransientMemory.upload` 直写持久段（消除无谓 `Arena.ofConfined()` 开闭）**：
   - 在 `NativeBuffer` 上提供包内直写入口 `writePersistent(long offset, List<ByteBuffer> chunks, long alignment)`，直接向持久映射 `base` `MemorySegment` 拷贝各 `ByteBuffer` 内容并调用 `addRange(offset, totalLength)` 登记脏区间（若未追踪则挂入 `owner.trackWritable(this)`）。
   - `RayniumTransientMemory.upload(...)` 改为 `allocateGpu(...)` 后直调 `writePersistent`，不再经过 `allocateStaging`/`allocateGpuMapped` 创建和关闭 `Arena.ofConfined()`、`MappedView` 与 `scopedViews`；`multiUploadStaging` 与 `multiUploadGpu` 改为预分配 `ArrayList` 普通循环，移除 `.stream().map(...).toList()`。
   - **重要边界保留**：显式调用 `GpuBuffer.map(...)`（含 `allocateStaging`/`allocateGpuMapped` 对外返回 `MappedView` 的路径）继续保留每视图独立 `Arena.ofConfined()`，确保单独 `view.close()` 后立即访问旧 `ByteBuffer` 仍按契约抛出 `IllegalStateException`（兼容 `NativeCommandTapeTest` 第 276 行断言）。
3. **`D9`：`RayniumRenderPassBackend` 单实例直录（净删 `Lease`、`RenderArea` 与 11 个平行数组）**：
   - `RayniumRenderPassBackend` 直接实现 `RenderPassBackend`，用 `boolean open` 状态位替代 `Lease` 内部类；裁剪框改为基本类型 `int areaX, areaY, areaW, areaH` 与 `int scissorX, scissorY, scissorW, scissorH`。
   - 删除 `bindingKind`～`bindingAnisotropy` 共 11 个平行数组、`growBindings()`、`usedBindings` 及两套重复的绑定写入循环：在 `drawCommand` 与 `instancedGeometry` 中先校验绑定对象并由 `Integer.bitCount(vertexMask) + (hasIndex ? 1 : 0) + (indirect != null ? 1 : 0) + uniformCount` 一步确定 `bindingCount`，在 `CommandTape` 分配记录段后单遍直接写入 48 B 绑定槽并调用 `tape.retain(...)`。
4. **`D10`：`NativeRenderer` 单轨池化 `InFlightBatch` 与 `TransientMemory` 去闭包化**：
   - 将 `NativeRenderer` 的 `consumedCallbacks`、`submittedCallbacks`、`completions`、`pendingBatches` 4 个队列合并为单条 `ArrayDeque<InFlightBatch> inFlightBatches`（`InFlightBatch` 节点池化复用，保存 `sequence`、`CommandTape` 借用释放钩子、`BatchSubmission` 引用、提交回调列表与完成回调列表），在 `syncWorkerState` 中按 `consumed`、`submitted`、`completed` 水位单调推进各阶段状态位并支持回调内重入 `pollCompleted()`。
   - `RayniumTransientMemory` 删除 `HashSet<Runnable> pending` 与双层 lambda 包装：在 `seal(tape)` 中直接将各 `TransientBlockAllocator.rotate()` 返回的回收器推入批次完成回收列表，在正常完成或终端失败/关闭时统一执行一次。
5. **`D11`：`MeshPayload` 与 `IndexSnapshot` 无锁固定计数（删除 `UploadLease` 与 `synchronized`）**：
   - 删除 `MeshPayload.UploadLease` 类及全部 `synchronized` 方法；
   - `MeshPayload` 与 `IndexSnapshot` 在构造取得 `DirectByteBuffer` 时一次性创建并缓存只读 `MemorySegment` 视图，内部用单个 `AtomicInteger pinState`（低 30 位为活跃固定计数 `pins`，最高位 `0x8000_0000` 为 `RELEASE_REQUESTED` 位）配合 CAS 实现无锁 `pin()`、`unpin()` 与 `release()`；
   - `NativeFrameBuffer` 直接用 `ObjectArrayList<MeshPayload> pinnedMeshes` 与 `ObjectArrayList<MeshPayload.IndexSnapshot> pinnedIndices` 记录本批借用对象，`clearBorrowed()` 时直接逐项调用 `unpin()`，零 `UploadLease` 分配、零方法引用分配、零监视器锁竞争。

---

## 五、分步实施顺序与验收标准

按以下 5 个可独立核对的步骤推进，完成后统一执行双后端全量测试与当前事实文档同步：

1. **步骤 1：ABI 25 契约、不透明句柄、`BufferPool`/`PendingResourceOps` 与死代码清理（`D1`、`D6`、`D7`、`D12`）**
   - 更新 `native/include/raynium/bridge.h`、`raster_records.h` 与 `bridge.cpp` 静态布局断言（`rn_batch_desc = 128 B`、`rn_terrain_view_desc = 304 B`、四大不透明句柄与二进制记录表）；
   - 落地 `BufferPool`、`PendingResourceOps`；删除 `TextureRegistry` / `ImmediatePipelineRegistry` 的 ID 哈希表；
   - 收紧 `rn_mesh_update::layout` 为必填并删除 `AnalyzeTerrainMesh`/`TerrainMeshUpload`，接入 `visible_version`/`origins_version` `O(1)` 判等；清理 `render_resources.*` 与 `mesh_pass.*` 死代码。
2. **步骤 2：SPSC `RenderChannel`、双槽 `BatchEnvelope` 与单次拷贝回读落地（`D2`、`D4`、`D5`）**
   - 新增 `render_channel.h/.cpp`，删除 `frame_packet.*`、`frame_queue.*`、`render_thread_session.*` 并更新 `native/CMakeLists.txt`；
   - 实现双槽 `BatchEnvelope`、零分配 `ValidateCommandHeaders`、Cache-line 对齐原子进度水位线、两阶段 `Wait` + `PumpSentMessages()`，以及 `_captureMutex` 保护的 `AsyncReadbackQueue` 单次拷贝 `PollCapture` 和 `HostReadStorage` 同步点发布。
3. **步骤 3：`TapeCompiler` 与 `FrameSlot::GetMemoryResource()` 扁平 `FrameTapePlan`（`D3`）**
   - 重构 `command_tape.*`、`immediate_pass.*`、`immediate_draw.*`、`world_features.*`、`terrain_feature.*`，将命令解码、读回/时间戳内联预留、几何池依赖聚合与绘制参数存储全部线性构建于 `slot.GetMemoryResource()`，彻底移除堆 `std::variant` AST、`make_shared<ImmediatePass>` 与每条绘制内的堆 `radray::vector`。
4. **步骤 4：Java 桥接升级、录制热路径与地形常驻瘦身（`D8`～`D11` 及 Java 侧 `D1`/`D2`/`D6`/`D7`）**
   - 更新 `tools/native.py`、`NativeManifest`、`NativeBindings`、`NativeRenderer`（含单轨 `InFlightBatch`）、`NativeTexture`、`NativeBuffer`（含 `writePersistent`）、`NativeFrameBuffer`、`CommandTape`；
   - 重构 `RayniumTransientMemory`、`RayniumRenderPassBackend`（移除 `Lease`、`RenderArea` 与 11 个平行数组）、`MeshPayload`（无锁 `pinState` 与移除 `heapValues`）及 `TerrainResidency`（输出 `visibleVersion`/`originsVersion`）。
5. **步骤 5：C++ GTest、Java 双后端全量测试迁移验证与当前文档同步**
   - 同步 `native/tests/test_terrain_draw.cpp`（移除旧 `AnalyzeTerrainMesh` 对照用例，保留并强化预分析布局正负例与版本代数缓存命中测试）及 `src/test/java/**`（将旧 `NativeRenderer.Mesh`/`Index` 测试辅助统一迁至 `MeshPayload` 构造，更新 `NativeCommandTapeTest` 原始字节流用例至 ABI 25 偏移）；
   - 执行构建与验证：
     - `python tools/native.py build --config Release` 与 `python tools/native.py stage --config Release`；
     - 开启 `RAYNIUM_BUILD_TESTS=ON` 运行 `ctest`；
     - 运行 `.\gradlew.bat test` 通过全部单元测试与 D3D12/Vulkan 双后端 `@Tag("native")` 测试；
   - 测试全部通过后，按源码事实同步更新 `doc/architecture/interop.md`、`doc/architecture/native-runtime.md`、`doc/architecture/terrain-rendering.md` 与 `doc/architecture/overview.md`，并运行 `python tools/check_docs.py` 与 `git diff --check`。

---

## 六、与后续 C++ SRP 基座的演进衔接

本计划落地的 `D1`～`D12` 全部改动均与[C++ SRP 渲染管线基座、纯 HLSL/DXC 着色器系统与原版管线实施计划](2026-11-24-cpp-srp-and-hlsl-shader-system-plan.md)同向收敛、零返工衔接：
- 不透明指针句柄、双槽 `BatchEnvelope` + SPSC `RenderChannel` + 原子水位线，以及在 `RadRay::FrameSlot::GetMemoryResource()` 上线性构建零析构执行计划的模式，直接构成未来 C++ SRP `WorldDrawStream` 与 `RendererList` 的运行底座；
- Java 侧 `RayniumTransientMemory` 直写、`InFlightBatch` 单轨队列、`MeshPayload` 无锁固定计数与地形单轨预分析契约在切入 `VanillaRenderer` 后原样继续服役；
- 上游 `RadRay` 无需新增任何接口。
