> - 适用: 实施基于 2026-11-05 实机 Tracy/JFR 采样的帧内 GPU 资源持久化：每槽帧资源、每槽 staging 与 GeometryArena
> - 权威: 用户要求保存的实施计划与交接记录；落地前不构成当前实现事实，完成后同步架构与指南文档
> - 锚点: `native/include/raynium/terrain_scene.h`, `native/src/terrain_scene.cpp`, `native/include/raynium/solid_pipeline.h`, `native/src/solid_pipeline.cpp`, `native/include/raynium/render_resources.h`, `native/src/render_resources.cpp`, `native/src/renderer.cpp`, `native/CMakeLists.txt`, `src/test/java/dev/raynium/bridge/NativeTerrainTest.java`

# 帧内 GPU 资源持久化与几何分配计划

- **日期**：2026-11-05
- **状态**：已确认，未实施。整体方案由助手提出；用户回答了其中两个待定问题（第 2 节），并要求保存计划。第 3 节起的接口、数据结构、数值、估算和验证安排都是助手方案，落地时以源码和验证结果为准。
- **范围**：消除 `Renderer::Render` 中每帧、每个网格的 D3D12 资源创建与销毁。实例缓冲、timestamp query pool 与 readback、深度纹理及其 view 改为每槽持久资源，颜色 attachment view 移入 `RenderTarget`；几何 staging 改为每槽持久映射的 Upload 缓冲；网格顶点改由每个 `TerrainScene` 独有的 `GeometryArena` 分配。不改 C ABI、Java 侧和 `external/RadRay`。
- **来源**：本机忽略目录 `build/verification/tracy-session-20261105/` 中的整段客户端采样（`session.tracy`、`session.jfr`、`zones.csv`、`game.log`、`capture-settings.json`，以及采样时保存的 `raynium-working-tree.patch` 与 `radray-tracy.patch`）。采样构建基于未提交工作树，含 native `RADRAY_PROFILE_*` 探针和 RadRay 深度插桩补丁。另有当前会话中的分析、方案讨论与用户决定；分析脚本留在会话临时目录，未入库。所属当前权威文档为 [Native runtime](../architecture/native-runtime.md) 与 [构建与测试](../guide/build-test.md)。

---

## 1. 采样结果与归因

### 1.1 环境与总体耗时

Windows、RTX 3060，Minecraft 26.3 Vulkan 宿主，Raynium D3D12 native 后端，Release `/MT`，Tracy 0.14.1，预览目标 213x120。
`rhiValidation` 与 `vulkanValidation` 关闭，`terrainProfile` 开启。

- 世界内采样 62.3 秒、11,825 帧。驻留网格约 3,100～3,465 个；每帧平均 888 个实例、2,658 个可见项。
- GPU 均值 0.23 ms，来自 `Raynium.GpuMilliseconds` plot；Tracy 中没有 GPU zone（`tracy-csvexport -g` 输出为空）。
- `Raynium.Render` 均值 1.91 ms，p99 7.6 ms，最大 40.6 ms。JFR `dev.raynium.ClientFrame` 均值 5.01 ms，Raynium 占 38%。
- 超过 16.7 ms 的客户端帧有 124 个；减去同帧 `Raynium.Render` 后只剩 43 个。
- 与 [2026-11-04 采样](2026-11-04-tracy-bottleneck-optimization-plan.md)的可比窗口相比（50～55 秒，驻留 3,409、可见 3,776），`Raynium.Render` 均值从 7.95 ms 降到 1.85 ms，`Scene.Backup`、`CopyState` zone 已不存在。
- draw 状态去重已生效：每帧 1 次 pipeline 绑定、1 次 index 绑定、2 次参数组绑定，vertex 绑定约 868 次。

### 1.2 资源创建与销毁的占比

D3D12 资源创建与销毁占 `Raynium.Render` 的 55%（每帧 1.06 ms）；在 Render 超过 4 ms 的帧中占 77%，超过 8 ms 的帧中占 84%。

稳态帧（8,840 帧，当帧没有几何、材质或图元更新）：

- Render 均值 1.60 ms。线性回归为固定 0.82 ms，另加每 1000 个可见项 0.30 ms。
- 均值：`Frame.CreateInstances` 0.263 ms，`Frame.CreateTimingResources` 0.168 ms，`Frame.CreateDepth` 0.063 ms，`RadRay.FrameSlot.ResetFrameData` 0.293 ms。
- 回归截距：`Frame.PrepareResources` 0.446 ms，`ResetFrameData` 0.228 ms。两者合计构成固定部分的大头。
- `CreateBuffer` p50 按调用方：实例缓冲 233 µs（约 14 KB），timing readback 134 µs（16 B），网格 staging 8.8 µs，网格 Device 顶点缓冲 30.9 µs（均值 98 µs）。

更新帧（2,985 帧）：

- Render 均值 2.84 ms，p99 12.6 ms。
- 每多一个网格增加 0.16 ms，其中 `CreateBuffer` 占 0.154 ms。memcpy 为 0.076 ms/MiB。

尖峰（帧号为 Tracy frame 序号）：

- 帧 4210：单次 `CreateBuffer` 24.4 ms，该帧 Render 40.6 ms。
- 实例缓冲创建 21.6 ms（帧 3228）、16.8 ms（帧 4139）、14.5 ms（帧 10）；timing 缓冲创建 12.2 ms（帧 17）。
- `ResetRetiredResources` 18.3 ms（帧 4208）；`ResetFrameData` 12.4 ms（帧 40）。
- 帧 5573：86 个网格更新产生 174 次 `CreateBuffer`，`Scene.StageGeometry` 17.2 ms。

### 1.3 源码中的根因（2026-11-05 工作树）

稳态固定开销与更新帧按网格增长的开销来自同一机制：帧循环内创建 D3D12 资源，两帧后释放。

- `TerrainScene::PrepareFrame` 的 `Raynium.Frame.PrepareResources` 段每帧创建实例 Upload 缓冲、D32 深度纹理、颜色与深度 view；profile 帧另建 query pool 与 16 B readback（`native/src/terrain_scene.cpp`）。
- `Scene.StageGeometry` 为每个 index/mesh 各建一个 Device 缓冲，`Upload` 再为每项建一个 Upload staging 缓冲。
- `SolidFramePipeline::Extract` 把整个 `TerrainFrame` 移入槽 arena 的 `FrameData`（`native/src/solid_pipeline.cpp`）。
  `FrameScheduler::RunFrameImpl` 在两帧后复用该槽时调用 `ResetRetiredResources` 与 `ResetFrameData`，销毁这些对象以及经 `RetireResource` 退役的旧网格缓冲（`external/RadRay/modules/runtime/src/frame_scheduler.cpp`）。
- 小缓冲创建慢：D3D12MA 对不超过 32 KB 的缓冲优先使用 committed 分配，除非分配策略为 `ALLOCATION_FLAG_STRATEGY_MIN_TIME`（`AllocatorPimpl::PrefersCommittedAllocation`，`external/RadRay/third_party/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp`）。
  RadRay 创建 allocator 时只设置 `ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED`（`external/RadRay/modules/render/src/d3d12/d3d12_impl.cpp`）。每帧约 14 KB 的实例缓冲和 16 B 的 readback 都走 committed 路径。

---

## 2. 用户决定（2026-11-05 会话）

助手在方案中提出两个待定问题，用户的答复如下：

1. **GeometryArena 块先不回收。** 首版块在 `TerrainScene` 寿命内只增不减；网格删除、epoch 切换与 `Reset` 只把区间归还空闲表，不释放块。
2. **三项改动一次做完。** 每槽帧资源、每槽 staging 与 GeometryArena 在同一次实施中完成并统一验证，不拆成多次落地。

用户随后要求把计划写入 `doc/history/`。第 3 节起的内容是助手方案，用户未逐项审阅。

---

## 3. 资源所有权（方案）

| 资源 | 当前（2026-11-05 工作树） | 方案 |
| --- | --- | --- |
| 实例缓冲 | `PrepareFrame` 每帧创建 Upload 缓冲，`FrameData` 持有 | `SlotResources::instances`，持久映射 Upload；`SolidFramePipeline::Create` 预建 64 KiB，不足时按 2 的幂增长 |
| timestamp query pool 与 16 B readback | profile 帧创建，`FrameData` 持有 | `SlotResources::timestamps` / `timing`；设备支持 timestamp query 时在 `Create` 中预建 |
| D32 深度纹理与深度 view | 每帧创建，`FrameData` 持有 | `SlotResources::depth` / `depthView`，记录尺寸和是否已初始化；首次使用或尺寸变化时在 `ReadyForFrame` 中重建 |
| 颜色 attachment view | 每帧创建，`FrameData` 持有 | `RenderTarget::Create` 创建并持有，随输出重建 |
| 几何 staging | 每个 index/mesh 一个 Upload 缓冲，`FrameData` 持有 | `SlotResources::staging`，持久映射 Upload，每帧从偏移 0 线性分配；预建 8 MiB，不足时按 2 的幂增长 |
| 网格顶点 | 每个 mesh 一个 Device 缓冲，`TerrainMesh` 持有 | `GeometryArena` 32 MiB 块内的顶点区间，`TerrainMesh` 持有 RAII `GeometryAllocation` |
| 索引缓冲（含共享 quad 索引） | 每个 index 一个 Device 缓冲 | 仍为独立缓冲；只把 staging 改为槽 staging |
| Group 0 常量与参数集 | 已是每槽持久资源 | 不变 |

每槽资源只在该槽上一帧的 GPU 工作完成后写入或替换。替换时直接释放旧对象，不经 `RetireResource`。`FrameData` 不再拥有 GPU 对象。

staging 与 arena 分工不同：staging 内容只活一帧，按槽隔离即可；arena 中的顶点跨帧驻留并被两个槽的帧同时读取，所以整个 `TerrainScene` 共用一个 arena，区间寿命仍走现有退役路径。

---

## 4. 槽复用为何安全（源码证据，2026-11-05 工作树）

1. `Renderer::Render` 先调用 `SolidFramePipeline::ReadyForFrame`，再调用 `TerrainScene::PrepareFrame`（`native/src/renderer.cpp`）。
   `ReadyForFrame` 以 5 秒超时等待 `_slotCompletion[_submitted%2]`，超时返回失败，不进入 PrepareFrame（`native/src/solid_pipeline.cpp`）。
2. `FrameScheduler::RunFrameImpl` 用 `_currentSlotIndex` 选槽，结束时加一后对槽数取模；`Record` 每帧把 `_submitted` 加一。
   两者都从 0 开始。RunFrame 只在 PrepareFrame 成功后调用，Extract/Record 失败时 `RADRAY_ABORT`，因此每次 RunFrame 同时推进两者，`_submitted % 2 == slot.GetIndex()` 成立。
   方案在 Extract 中检查该等式，不成立时 `RADRAY_ABORT`。
3. 所以 PrepareFrame 写入的槽，其上一帧 GPU 工作已经完成；另一槽的在途帧只读自己的 staging、实例和深度。
4. 退役时序：`RunFrameImpl` 依次执行 `WaitSlot`、`ResetCommandStorage`、`ResetRetiredResources`、`ResetFrameData`；`WaitIdle` 对所有槽按同样顺序处理。
   帧 N 在 PrepareFrame 中删除或替换的 mesh 由 Extract 交给槽 s 的 `RetireResource`，该槽在帧 N+2 的 RunFrame 中确认帧 N 完成后才释放 owner。
   同一队列上 completion 按提交顺序推进，帧 N 完成意味着最后一个可能引用旧 mesh 的帧 N−1 也已完成；帧 N 及以后不再绘制该 mesh。
   因此区间归还 arena 时没有在途读取者，后续帧可以直接覆写。

---

## 5. 每槽帧资源

接口草案：

```cpp
// solid_pipeline.h
struct SlotResources {
    RenderBuffer passConstants;
    radray::unique_ptr<radray::render::ShaderParameterSet> passParameterSet;
    radray::unique_ptr<MappedUploadBuffer> staging, instances;
    radray::unique_ptr<radray::render::QueryPool> timestamps; // 设备不支持 timestamp query 时为空
    RenderBuffer timing;
    radray::unique_ptr<radray::render::Texture> depth;
    radray::unique_ptr<radray::render::TextureView> depthView; // 声明在 depth 之后，先于纹理析构
    uint32_t depthWidth{}, depthHeight{};
    bool depthInitialized{};
};
bool ReadyForFrame(const RenderTarget&, radray::string&); // 等待 _submitted%2 槽，按目标尺寸准备深度
SlotUploads NextUploads() noexcept;                       // 同一槽的 staging 与实例缓冲
```

- `SolidFramePipeline::Create` 为每槽预建 staging（8 MiB）、实例缓冲（64 KiB）和 timestamp 资源，失败时返回错误，局部 RAII 清理。
- 深度在 `Create` 时尺寸未知。`ReadyForFrame` 等到槽完成后比较目标尺寸，不同则先创建新纹理与 view，成功后替换并清除 `depthInitialized`；创建失败时保留旧对象并返回失败。
- `RenderTarget::Resources` 增加颜色 view，声明在 `color` 之后；`Record` 改用 `target.ColorView()`。
- 实例：PrepareFrame 写入 `NextUploads().instances`；`Record` 的 InstanceStream view 大小用本帧 `instance_count × 16`，不用缓冲容量。
- timestamp：当帧 `parameters.flags` 含 `RN_FRAME_PROFILE` 且槽有 query pool 时录制；`Wait` 按同一条件读取槽 readback。`TerrainFrame` 不需要新字段。

---

## 6. 每槽 staging

接口草案：

```cpp
// render_resources.h
RenderBuffer CreateBuffer(radray::render::Device&, uint64_t, radray::render::MemoryType, radray::render::BufferUses,
                          radray::render::ResourceHints = {});
// 持久映射的 Upload 缓冲；调用方只在其上一次 GPU 使用完成后覆写或增长
class MappedUploadBuffer final {
public:
    static bool Create(radray::render::Device&, uint64_t capacity, radray::render::BufferUses,
                       radray::unique_ptr<MappedUploadBuffer>&, radray::string&);
    bool Reserve(radray::render::Device&, uint64_t bytes, radray::string&); // 不足时换成 2 的幂容量，丢弃旧内容
    void Flush(uint64_t offset, uint64_t bytes) noexcept;
    radray::render::Buffer* Get() const noexcept;
    std::byte* Data() const noexcept;
    uint64_t Capacity() const noexcept;
};
```

- `Create` 创建 `MemoryType::Upload`、`MapWrite`、`ResourceHint::PersistentMap` 缓冲，立即 `Map(0, capacity)` 并保存指针；寿命内不再 Map/Unmap，析构前 Unmap 一次。
- 依据（锁定 RadRay 源码）：校验层拒绝对已映射缓冲再次 Map，并允许 PersistentMap 缓冲在 Map 之外 Flush（`modules/render/src/rhi_validation.cpp`）。
  D3D12 的 PersistentMap 在首次 Map 时映射并缓存指针，`Unmap` 会真正解除映射（`BufferD3D12::Map/Unmap`）；Vulkan 由 VMA 在创建时映射，`Unmap` 是空操作（`BufferVulkan::Map/Unmap`）。
  因此每帧 Unmap 会在 D3D12 上抵消持久映射。
- `Reserve` 先创建并映射新缓冲，成功后替换旧缓冲；失败保留旧缓冲。调用方已确认槽完成，旧缓冲可直接释放。
- `Flush` 调用 `FlushMappedRange`。D3D12 实现为空操作；Vulkan 在非 coherent 内存上执行 flush。
- PrepareFrame 在 StageGeometry 前预扫描 `f.indices` 与 `f.meshes`，沿用现有逐项检查和错误信息，累加每项按 16 字节对齐后的字节数，调用一次 `staging.Reserve`。
  随后逐项 memcpy 到 `Data() + offset`，记录 `GeometryUpload`，最后对已用范围 Flush 一次。实例缓冲同样先 `Reserve(instance_count × 16)`，再 memcpy 与 Flush。
- 初始 8 MiB 对应 Java 每帧 8 MiB 顶点预算（`TerrainPreview.java` 中 `budget=8L*1024*1024`）。
  该预算不限制本帧第一个网格（`used==0` 条件），同一帧还可能上传索引，所以 staging 可能长到 16 MiB 或 32 MiB；增长后不收缩。

```cpp
// terrain_scene.h
struct SlotUploads { MappedUploadBuffer& staging; MappedUploadBuffer& instances; };
struct GeometryUpload {
    radray::render::Buffer* destination;
    uint64_t destinationOffset, stagingOffset, bytes;
    radray::render::BufferStates after;
};
struct TerrainFrame { // 不再拥有 GPU 对象
    std::vector<GeometryUpload> uploads;
    std::vector<radray::VisibleMeshBatch> visibleBatches;
    std::vector<std::shared_ptr<void>> retiredResources;
    rn_render_frame_desc parameters{};
    RenderTarget* target{};
    SharedTextures* textures{};
};
bool PrepareFrame(RenderTarget&, SharedTextures*, const rn_render_frame_desc&, const MeshPassResources&,
                  SlotUploads, TerrainFrame&, radray::string&);
```

`Renderer::Render` 改为先 `ReadyForFrame(target, error)`，再把 `_pipeline->NextUploads()` 传给 PrepareFrame。

---

## 7. GeometryArena

接口草案：

```cpp
// geometry_arena.h
class GeometryAllocation final {
public:
    GeometryAllocation(GeometryAllocation&&) noexcept;
    GeometryAllocation& operator=(GeometryAllocation&&) noexcept;
    ~GeometryAllocation() noexcept;                    // 把区间还给 arena；arena 必须更晚析构
    const radray::VertexInput& Input() const noexcept; // 所在块共享
    uint32_t FirstVertex() const noexcept;
    uint32_t VertexCount() const noexcept;
private:
    friend class GeometryArena;
    GeometryArena* _arena; uint32_t _block, _first, _count;
};
class GeometryArena final {
public:
    static constexpr uint32_t BlockVertices=(32u<<20)/28; // 1,198,372 个顶点，33,554,416 字节
    static bool Create(radray::render::Device&, radray::unique_ptr<GeometryArena>&, radray::string&);
    std::optional<GeometryAllocation> Allocate(uint32_t vertexCount, radray::string&);
private:
    struct Block { RenderBuffer buffer; radray::VertexInput input; std::map<uint32_t,uint32_t> free; };
    void Release(uint32_t block, uint32_t first, uint32_t count) noexcept;
    radray::render::Device& _device;
    std::vector<std::unique_ptr<Block>> _blocks;
    std::set<std::tuple<uint32_t,uint32_t,uint32_t>> _bySize; // (count, block, first)
};
```

- 每个 `TerrainScene` 一个 arena。渲染调用在环境初始化线程串行执行，arena 不加锁。
- 块是 `MemoryType::Device`、用途 `CopyDestination | Vertex` 的缓冲，以 28 字节顶点为单位分配，不需要额外对齐。
- 每块持有一个覆盖整块的 `radray::VertexInput`。布局与属性沿用现有 per-mesh 定义，stream 0 是整块。
  `MeshBatch::VertexInput` 要求地址稳定（`external/RadRay/modules/runtime/include/radray/runtime/mesh_batch.h` 注释），块用 `unique_ptr` 存放，vector 增长不移动 `input`。
- 空闲表：每块 `free` 按起始顶点排列，用于合并相邻区间；`_bySize` 用 `lower_bound` 做 best-fit。分配时拆出剩余部分，释放时与前后相邻空闲区间合并。
- 没有足够区间时新建一块。单个 mesh 超过块容量时建 max(32 MiB, mesh 大小) 的块；当前 native 拒绝 `vertex_count > 1<<20`（28 MiB），现行 ABI 下不会触发。
- 首块在 `Renderer::Create` 中通过 `GeometryArena::Create` 预建。按用户决定，块在 arena 析构前不释放。
- `TerrainMesh` 用 `GeometryAllocation geometry` 替换 `buffer` 与 `input`。区间随旧 `TerrainMesh` owner 走现有路径释放：`_retiredResources` → `TerrainFrame::retiredResources` → `FrameSlot::RetireResource`。退役机制不变。
- `Batch()` 设 `batch.VertexInput = &mesh.geometry.Input()`，每个元素 `VertexOffset = range.base_vertex + geometry.FirstVertex()`。用 int64 计算，超出 int32 时 PrepareFrame 报错（`MeshBatchElement::VertexOffset` 为 int32）。
- 同块网格共享同一个 vertex buffer view（Target、Offset、Size 相同）。`SameVertexBufferView`（`external/RadRay/modules/runtime/src/mesh_draw_command.cpp`）会跳过排序后相邻同块 draw 的重复绑定。每帧约 868 次 vertex 绑定预计降到块切换次数的量级，未测量。
- `ResidentBytes()` 当前没有调用方；改为累加 allocation 字节与索引缓冲字节。
- arena 析构时用 Debug 断言检查每块只剩一个完整空闲区间，用于发现寿命错误，不增加计数字段。
- `native/CMakeLists.txt` 的源文件列表加入 `src/geometry_arena.cpp include/raynium/geometry_arena.h`。

---

## 8. 命令录制与 barrier

几何上传改为分组录制：

1. 按 destination 去重，得到本帧写入的 arena 块和索引缓冲。
2. 一次 `ResourceBarrier`：每个 destination `Undefined → CopyDestination`；staging `HostWrite → CopySource`。
3. 逐条 `CopyBufferToBuffer(destination, destinationOffset, staging, stagingOffset, bytes)`。
4. 一次 `ResourceBarrier`：arena 块 `CopyDestination → Vertex`，索引缓冲 `CopyDestination → Index`；staging `CopySource → HostWrite`。

依据与推断：

- 去重是正确性要求。RadRay D3D12 后端使用 legacy transition barrier，并把缓冲 barrier 扩大到整个资源（`rhi.h` 中 `BarrierBufferDescriptor::Range` 注释，`CmdListD3D12::ResourceBarrier` 使用 `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`）。
  同一块在一帧内出现两次 `Undefined → CopyDestination` 时，第二次声明的 before 状态 COMMON 与实际状态不符。
- D3D12：`BufferState::Undefined` 映射为 `D3D12_RESOURCE_STATE_COMMON`（`d3d12_helper.cpp` 的 `MapType(BufferStates)`）。
  缓冲在每次 `ExecuteCommandLists` 完成后衰减为 COMMON（Microsoft 文档 "Using resource barriers to synchronize resource states in Direct3D 12" 的 State decay to common 一节）。Raynium 每帧只提交一次，所以每帧开始时块处于 COMMON，before 状态成立。
  没有上传的块仍按现状从 COMMON 隐式提升为顶点读取状态。
- D3D12 后端跳过非 Device 内存且没有 UAV 用途的缓冲 barrier（`CmdListD3D12::ResourceBarrier`），所以 staging、实例和 readback 的 barrier 只在 Vulkan 上生效。
- Vulkan：`BufferState::Undefined` 的源阶段是 `TOP_OF_PIPE`，不带访问掩码（`vulkan_helper.cpp`）。这只在被写区间没有未完成访问时成立。
  新分配区间满足这一点（第 4 节）；同一块中另一槽在途帧读取的区间与本帧 copy 写入的区间不相交。
- 以上推断须由 D3D12 调试层与 Vulkan 同步验证确认（第 14 节）。

深度：

- D3D12 中没有 simultaneous access 标志的纹理不会衰减。持久深度在帧末保持 DEPTH_WRITE，若下一帧仍录制 `Undefined → DepthWrite`，before 状态 COMMON 与实际不符。
- 方案：首帧录制 `Undefined → DepthWrite`，之后录制 `DepthWrite → DepthWrite`，录制后置 `depthInitialized`。
  D3D12 后端跳过 before/after 相同的 transition（`CmdListD3D12::ResourceBarrier`）；Vulkan 生成一条深度写到深度写的 barrier。
- 颜色输出 barrier 不变，已按 `target.Submitted()` 区分首帧。

实例与 timestamp：

- 持久实例缓冲沿用 Group 0 常量的写法：render pass 前 `HostWrite → Vertex`，帧末 `Vertex → HostWrite`。
- timestamp readback 的 barrier 不变，只把资源换成槽持有的对象。

---

## 9. 生命周期与失败路径

- `Renderer::Create`：依次 `GeometryArena::Create`（首块）、`SolidFramePipeline::Create`（每槽 staging、实例、timestamp）、`FrameScheduler::Create`，全部成功后构造 Renderer，把 arena 移入 `TerrainScene`。任一步失败直接返回，局部 RAII 清理。
- `TerrainScene` 构造函数接收 `radray::unique_ptr<GeometryArena>`；`_arena` 声明在 `_state`、`_scene`、`_retiredResources` 之前，最后析构。
- `~Renderer` 保持现有顺序：`WaitIdle` 释放所有槽的退役 owner 与 `FrameData`，`_scene.Reset()` 释放驻留状态与待退役集合。
  随后成员析构：`renderer.h` 中 `_scheduler` 声明在 `_scene` 之前，所以先析构 `TerrainScene`（含 arena），再析构调度器与管线；此时槽已清空，没有 allocation 晚于 arena。
- epoch 切换：旧 owner 照常退役，区间在槽复用时归还，块保留。
- `DetachTextures`：不涉及几何，行为不变。
- resize：宿主排空后重建 `RenderTarget`，颜色 view 随之重建；每个槽在下一次 `ReadyForFrame` 发现尺寸变化时重建深度。
- PrepareFrame 中途失败：只写入已完成槽的 staging 与实例；为新 mesh 分配的区间由 `_state` 持有（现有契约不回滚状态）；待退役集合留在 `TerrainScene`。宿主随后关闭 renderer，另一槽在途帧的资源未被触及。
- `ReadyForFrame` 失败（completion 超时或深度创建失败）在 PrepareFrame 之前返回。`Reserve` 或新建块失败时 PrepareFrame 返回现有上传错误信息。

---

## 10. 预计效果（助手估算，未测量）

| 指标 | 2026-11-05 采样 | 估算 |
| --- | --- | --- |
| 稳态 Render 固定部分 | 0.82 ms | 约 0.2 ms（基本去掉 `PrepareResources` 截距 0.446 ms 与 `ResetFrameData` 截距 0.228 ms） |
| 稳态 Render 均值 | 1.60 ms | 约 0.95 ms |
| 每个更新网格 | 0.16 ms | 约 0.01 ms，另加 0.076 ms/MiB memcpy |
| 帧 5573 `Scene.StageGeometry` | 17.2 ms | 约 1 ms |
| 稳态每帧 `CreateBuffer` 次数 | 2 | 0 |
| 整段 arena 块创建 | 无此项 | 约 5～6 次（驻留约 160 MB，约 3,400 个网格 × 平均 47 KB） |

创建尖峰在稳态帧中应消失；新建块、staging 或实例缓冲增长以及索引缓冲重建时仍可能出现。

---

## 11. 风险

- 显存：块按峰值驻留量保留到 `TerrainScene` 销毁，epoch 切换也不释放；碎片会使块数高于驻留量所需。这是用户选择首版不回收的代价。
- 分配器错误（区间重叠、base vertex 错误、提前归还）通常表现为错误像素或闪烁，不会崩溃，也不一定触发校验，需要像素测试覆盖。
- barrier 推断（缓冲衰减、深度状态跟踪、Vulkan `Undefined` 无访问掩码）是新依赖，必须通过双后端驱动校验。
- staging 增长后不收缩，每槽可能长期占用 16～32 MiB Upload 内存。
- 单个 mesh 最大 28 MiB，低于 32 MiB 块容量；若以后放宽 ABI 顶点上限，需要重新确定块大小。

## 12. 未采用的替代

- 让 RadRay 对小缓冲使用 `ALLOCATION_FLAG_STRATEGY_MIN_TIME`，绕开 committed 分配：需要修改 `external/RadRay`，而且每帧创建与销毁仍然存在。
- 整个 renderer 共用一个 staging 环：需要按帧记录水位并在复用前检查对应帧完成；双槽各持一份更简单，寿命与现有槽完成边界一致。

---

## 13. 改动文件

| 文件 | 改动 |
| --- | --- |
| `native/include/raynium/geometry_arena.h`、`native/src/geometry_arena.cpp`（新增） | `GeometryArena` 与 `GeometryAllocation` |
| `native/CMakeLists.txt` | 源文件列表加入 geometry_arena |
| `native/include/raynium/render_resources.h`、`native/src/render_resources.cpp` | `CreateBuffer` hints 参数；`MappedUploadBuffer`；`RenderTarget` 持有颜色 view |
| `native/include/raynium/terrain_scene.h`、`native/src/terrain_scene.cpp` | `TerrainMesh`、`GeometryUpload`、`TerrainFrame`、`SlotUploads`；arena 成员；预扫描与槽 staging；删除 `PrepareResources` 中的资源创建；`Batch()` 的 VertexInput 与 VertexOffset；`ResidentBytes()` |
| `native/include/raynium/solid_pipeline.h`、`native/src/solid_pipeline.cpp` | `SlotResources` 扩展与预建；`ReadyForFrame(target)`；`NextUploads()`；Extract 槽号检查；Record 分组 barrier、深度状态、槽资源；Wait 读取槽 readback |
| `native/include/raynium/renderer.h`、`native/src/renderer.cpp` | Create 预建 arena 并移入 `TerrainScene`；Render 传入目标与槽上传缓冲 |
| `src/test/java/dev/raynium/bridge/NativeTerrainTest.java` | 新增第 14 节用例 |

不改 `native/include/raynium/bridge.h`（ABI 9）、Java 主代码与 `external/RadRay`。
`terrain_scene.cpp`、`solid_pipeline.cpp`、`render_resources.cpp` 当前含未提交的 Tracy 探针改动；实施在其上进行，保留这些 zone，只移除随资源创建一起删除的 `Frame.Create*` zone。

---

## 14. 验证计划

仓库根目录执行：

```powershell
.\gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel
.\gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel
python tools/check_docs.py
git diff --check
```

通过标准：

- Debug 与 Release 的全部 Java 与 native/GPU 用例通过，无跳过。
- 日志中没有 RadRay RHI 校验拒绝、D3D12 调试层错误、Vulkan VUID 或同步验证报告。

现有用例须继续通过，其中与本方案直接相关的有：

- `twoPendingSlotsKeepParametersAndReplacedGeometry`：双槽在途时替换几何。
- `failedPreparationRetainsResourcesForPendingFrame`：PrepareFrame 失败时在途帧资源保活。
- `sceneSurvivesOutputAndInputRecreation`：输出按 32、36、40 像素逐代重建，每代一帧，槽 0 会遇到尺寸变化，覆盖深度重建。
- `incrementalAdditionsKeepResidentBatches`、`cachedSubmeshesFollowMaterialIndexAndMeshUpdates`、`reversedDepthEpochAndLargeCoordinates`。

新增用例（双后端 × 完整/split 两种输入路径，沿用 `paths()`）：

1. 多块：驻留顶点总数超过 `BlockVertices`（例如两个 `1<<20` 顶点的 mesh），两者都可见，按像素检查第二块中的 base vertex。
2. staging 增长：单帧上传超过 8 MiB，检查像素；之后再渲染至少两帧，让两个槽都经过增长或复用。
3. 区间复用：删除 mesh A，再渲染至少两帧让区间归还，然后新增不大于 A 的 mesh B；检查 B 正确显示且没有 A 的残留像素。同块中另一个 mesh C 全程可见，覆盖在途读取与新写入区间不相交的情形。

Tracy 复采：按[构建与测试](../guide/build-test.md#tracy-与整段客户端采样)的流程，使用与本次相同的配置（Release、D3D12、213x120、两种 validation 关闭、`terrainProfile` 开启）。记录并对比：

- 稳态帧 `Raynium.Render` 均值与回归截距，`ResetFrameData` 均值。
- 稳态帧中 `Raynium.Buffer.Create` 以及 D3D12 纹理、view、query pool 创建次数（预期为 0）。
- 更新帧每网格增量、`Scene.StageGeometry`、帧 5573 类多网格帧。
- Render 超过 8 ms 的帧数与最大值，`dev.raynium.ClientFrame` 超过 16.7 ms 的帧数。
- 每帧 vertex 绑定次数（`RadRay.Draw.*` plots）。

建议新增 plot：`Raynium.Arena.Blocks`、`Raynium.Arena.FreeBytes` 与每槽 staging 容量。空闲字节只在 `RADRAY_ENABLE_PROFILER` 下统计，写法同现有 `Raynium.Uploads.Bytes`。

---

## 15. 落地后需同步的当前文档

实现与验证通过后按[文档维护](../guide/documentation.md)更新：

- `doc/architecture/native-runtime.md`：适配层职责中 staging 与实例的描述；资源表中“每个索引/网格的独立 Device Buffer”与“staging、实例、深度、view、timestamp/readback”两行；“每个 mesh 独占 vertexCount×28 字节 Buffer”一段；FrameData 只拥有瞬态资源的说明；RenderTarget 与深度归属；resize 后深度重建。
- `doc/architecture/overview.md`：职责表“地形驻留”行的“独立几何缓冲”。
- `doc/architecture/terrain-preview.md`：“帧内 D32”与 resize 后“按新尺寸创建深度”。
- `doc/architecture/interop.md`：`NativeRenderTarget` 行中“深度归 native 帧槽 arena”。
- `doc/guide/build-test.md`：Tracy zone 与 plot 列表、`NativeTerrainTest` 覆盖说明，以及本次验证记录。

---

## 16. 不在本次范围

- Java 侧开销：JFR 中 `TerrainPreview` 每帧的 `HashMap`/`Position` 哈希与排序。
- draw 合并与 `GatherVisibleMeshDrawCommands` 的可见项扫描。
- arena 块回收与整理（用户决定首版不做）。
- 索引缓冲并入 arena；共享 quad 索引缓冲保持独立。
