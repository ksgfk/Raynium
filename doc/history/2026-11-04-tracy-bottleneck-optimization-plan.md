> - 适用: 执行基于 2026-11-04 Minecraft 实机 Tracy 采样的 Raynium Native 场景更新与 CPU 瓶颈优化
> - 权威: 用户确认的实施计划与 AI 交接文档；落地前不构成当前实现事实，完成后需同步架构与指南文档
> - 锚点: `native/include/raynium/mesh_pass.h`, `native/include/raynium/terrain_scene.h`, `native/src/renderer.cpp`, `native/src/terrain_scene.cpp`, `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`, `src/test/java/dev/raynium/bridge/NativeTerrainTest.java`

# Tracy 实机性能瓶颈分析与就地增量更新优化计划

- **日期**：2026-11-04
- **状态**：待实施（本计划第 1～2 项核心优化已由用户确认，交由执行 AI 落地；第 5 节所列后续方向暂不纳入本次修改范围）
- **范围**：移除 `Renderer::Render` 与 `TerrainScene` 的每帧事务回滚深拷贝（`Scene.Backup`、`CopyState`）、移除全表红黑树扫描校验（`ValidateResources`）与逐索引 CPU 校验（`CheckMesh`）、将 `TerrainState` 查找容器替换为 `std::unordered_map`、改为按增量数组直接就地更新 `_state` 与 `radray::Scene`，并在 `TerrainPreview.java` 预设共享 quad 索引缓冲初始容量
- **来源**：未提交工作树中的 `RADRAY_PROFILE_*` 探针埋点、本机忽略目录 `build/verification/tracy-session-20261104/session.tracy`（63.3 秒、5,655 次 `Raynium.Render` 调用、207,284 个 CPU Zone）实测剖析，以及当前会话中用户确认的设计取舍。所属当前权威文档为 [Native runtime](../architecture/native-runtime.md)、[互操作与加载](../architecture/interop.md) 与 [地形预览](../architecture/terrain-preview.md)。

---

## 1. Tracy 实机采样背景与瓶颈归因

测试环境为 Windows 10、RTX 3060、Minecraft 26.3 Vulkan 宿主 + Raynium D3D12 Native 后端（Release `/MT`，预览目标 `213x120`）。

在大场景稳态工况（驻留网格 `R >= 3,000`，平均 `3,464` 个网格、`3,687` 个可见子网格、`19,098` 条缓存命令，且当帧网格/材质/图元更新数均为 `0`）下：
- **Native GPU 实际光栅耗时**：均值仅 **0.62 ms**（p95 **0.85 ms**）；双槽 `WaitReusable` 均值 **0.001 ms**，GPU 从未阻塞 CPU。
- **Native CPU `Raynium.Render` 墙钟耗时**：均值高达 **7.947 ms**（p95 **11.353 ms**，max **23.756 ms**）；在大场景区块加载期（平均每帧更新 7.91 个网格）均值升至 **9.770 ms**（p95 **16.641 ms**，全段峰值 **32.038 ms**）。

其中占比最高的两个首要 CPU 瓶颈（合计占大场景稳态 CPU 耗时的 **66.5% / 5.285 ms**）为：

| 序号 | 瓶颈项 | 大场景稳态均值 (占比) | 实测子项拆解与根因 | 用户决策 |
| --- | --- | --- | --- | --- |
| **1** | **每帧无条件 `Scene.Backup` + `CopyState` 深拷贝与析构** | **2.744 ms** (34.5%) | `Scene.Backup` 按值深拷贝整棵 `radray::Scene` 含 3,464 个 Batch 与 19,098 条 `MeshDrawCommand`（1.583 ms）+ 函数返回时析构 `backupScene`（0.544 ms）+ `Scene.CopyState` 拷贝 4 个 `std::map` 共 6,928 个节点（0.410 ms）+ `Scene.Commit` 析构旧树（0.207 ms） | **确认全部删除** |
| **2** | **`TerrainScene` 红黑树全表遍历校验、比对与索引扩容重建** | **2.541 ms** (32.0%) | 0 更新帧仍对 3,464 个驻留对象执行全表 `ValidateResources`（0.783 ms）、`RefreshBatches`（0.796 ms，每对象 5 次 `map` 查找）、`RemovePrimitives`（0.294 ms）、`RetireResources`（0.259 ms）、`AddPrimitives`（0.221 ms）及 `VisibleBatches` 红黑树查找（0.188 ms）；且共享 quad 索引翻倍扩容时触发全场景 `CheckMesh` 与 `UpdateMeshBatch`（Frame 3441 尖峰 21.55 ms） | **确认删除全表扫描与 `CheckMesh`，信任 Java 侧数据，换用哈希表并预设索引容量** |

---

## 2. 核心设计取舍（用户已确认）

1. **移除“失败回滚（Transactional Rollback）”机制**：
   - 在 Minecraft 接入层（`PreviewOverlay.java`）中，一旦 `terrain.draw()` 抛出异常，`PreviewOverlay.disable()` 会立即排空并销毁 `RenderTarget`、`TerrainPreview` 和 `NativeRenderer`，回退至纯原版渲染，从不在同一 `Renderer` 上重试下一帧。
   - 能导致 `Apply` 或 `RunFrame` 失败的情形（VRAM/CPU OOM、Device Lost、内部句柄损坏）均为不可恢复错误。因此彻底删除 `backupScene`（`radray::Scene` 深拷贝）与 `TerrainUpdate::candidate`（`TerrainState` 四个容器拷贝）。
2. **完全信任 Java 侧数据传递，删除防御性全表扫描与逐索引 CPU 校验**：
   - `TerrainPreview.java` 与 `rayniumruntime.dll` 属于同一模组内部组件。
   - 删除 `CheckMesh`（不再在 CPU 上循环校验每个 `indices[first + j] + base_vertex` 是否落在 `[0, vertex_count)` 内）以及 `TerrainIndex::values`（不再在 Native CPU 内存中复制和驻留整份索引数组）。
   - 删除 `ValidateResources` 中对全部已驻留 `meshes`、`materials`、`primitives` 的全表引用扫描，仅保留入口处对 `rn_render_frame_desc` 顶层标量/数组指针的基本非空检查。
3. **改为纯增量数组直接驱动（$O(\Delta)$）与 `std::unordered_map` 查找**：
   - 将 `TerrainState` 中的 4 个 `std::map` 和 `MaterialBindingMap` 替换为 `std::unordered_map`。由于 `TerrainMesh` 通过 `std::shared_ptr<TerrainMesh>` 存储在堆上，外层容器改为 `std::unordered_map` 后，元素的 rehash 不会改变 `mesh->input`（`radray::VertexInput`）的内存地址。
   - 资源退役（`RetireResource`）、删除、新增、材质更新全部直接遍历当帧 `rn_render_frame_desc` 的增量数组，不再通过对比新旧两份 `TerrainState`（`Changed(previous, id, owner)`）来反推变化。
   - 仅当某个**在上一帧已存在**的 `mesh` 或 `index` 在本帧被同 ID 覆盖更新时，才遍历存活 `primitive` 调用 `_scene.UpdateMeshBatch`；在稳态帧及纯新增区块网格的加载帧中，完全不遍历驻留 `primitives` 表。
4. **预设共享 quad 索引缓冲初始容量**：
   - 在 `TerrainPreview.java` 中，首次生成共享 quad 索引缓冲（`Index(1)`）时，初始容量至少按 `65,536` 顶点（即 `16,384` 个 quad = `98,304` 个 `uint32` 索引，占 384 KiB 显存）分配，避免运行过程中从 `1024 -> 2048 -> ... -> 32768` 反复翻倍重传 `Index(1)` 触发全场景几千个 `MeshBatch` 重建。

---

## 3. 分阶段实施步骤

### 阶段一：替换容器类型并精简 `TerrainScene` 数据结构

#### 1.1 修改 `native/include/raynium/mesh_pass.h`
- 将 `<map>` 替换为 `<unordered_map>`。
- 将 `MaterialBindingMap` 定义更新为：
  ```cpp
  using MaterialBindingMap = std::unordered_map<uint32_t, std::shared_ptr<MeshMaterialBinding>>;
  ```

#### 1.2 修改 `native/include/raynium/terrain_scene.h`
1. 将 `<map>` 替换为 `<unordered_map>`。
2. **精简 `TerrainIndex`**：移除 `std::vector<uint32_t> values;`，仅保留 GPU 缓冲：
   ```cpp
   struct TerrainIndex {
       RenderBuffer buffer;
   };
   ```
3. **更新 `TerrainState`**：将四个驻留字典改为 `std::unordered_map`：
   ```cpp
   struct TerrainState {
       uint64_t epoch{};
       const SharedTextures* boundTextures{};
       std::unordered_map<uint64_t, std::shared_ptr<TerrainIndex>> indices;
       std::unordered_map<uint64_t, std::shared_ptr<TerrainMesh>> meshes;
       std::unordered_map<uint64_t, std::shared_ptr<TerrainMaterial>> materials;
       std::unordered_map<uint64_t, std::shared_ptr<TerrainPrimitive>> primitives;
       MaterialBindingMap materialBindingsByHandle;
   };
   ```
4. **移除 `TerrainUpdate` 结构体，简化 `TerrainScene` 接口**：
   - 不再需要承载候选状态副本的 `TerrainUpdate`，也不再需要独立的 `RetireResources`、`Apply`、`Commit` 三段接口。
   - 将 `TerrainScene` 的帧更新入口收敛为单个就地更新与当帧资源准备方法（在 `SolidFramePipeline::ReadyForFrame` 确认复用槽就绪后调用）：
     ```cpp
     class TerrainScene final {
     public:
         explicit TerrainScene(RayniumRuntime& device) : _device(device) {}
         bool PrepareFrame(RenderTarget& target, SharedTextures* textures, const rn_render_frame_desc& frameDesc,
                           const MeshPassResources& resources, radray::FrameScheduler& scheduler,
                           TerrainFrame& outFrame, radray::string& error);
         // Requires WaitIdle before releasing bindings borrowed by cached commands or recordings.
         void DetachTextures() noexcept;
         void Reset() noexcept;
         radray::Scene& Scene() noexcept { return _scene; }
         const MaterialBindingMap& MaterialBindings() const noexcept { return _state.materialBindingsByHandle; }
         uint64_t ResidentBytes() const noexcept;
     private:
         RayniumRuntime& _device;
         TerrainState _state;
         radray::Scene _scene;
         bool _rebuildScene{true};
     };
     ```

---

### 阶段二：重写 `native/src/terrain_scene.cpp` 与 `native/src/renderer.cpp`

#### 2.1 重写 `native/src/renderer.cpp` 中的 `Renderer::Render`
1. 顺序调整为：先校验 `output_sequence`、`_textures->Validate(...)` 并调用 `_pipeline->ReadyForFrame(error)` 等待待复用槽完成。
2. 删除 `backupScene` 拷贝构造、`Raynium.Scene.Backup` 作用域、`rollback` scope guard 以及 `Raynium.Scene.Commit` 作用域。
3. 直接调用 `_scene.PrepareFrame(target, _textures, frame, _pipeline->Resources(), *_scheduler, preparedFrame, error)`。
4. 将 `&_scene.MaterialBindings()` 传给 `_pipeline->BeginFrame(preparedFrame, &_scene.MaterialBindings())`，执行 `_scheduler->RunFrame(_scene.Scene())`。
5. `RunFrame` 成功后调用 `target.Commit(frame.output_sequence)` 与 `_textures->Commit(frame.changed_textures)`。

参考实现结构：
```cpp
bool Renderer::Render(RenderTarget& target, const rn_render_frame_desc& frame, radray::string& error) {
    auto markFrame = radray::MakeScopeGuard([]() noexcept { RADRAY_PROFILE_FRAME(); });
    RADRAY_PROFILE_SCOPE_N("Raynium.Render");
    RADRAY_PROFILE_PLOT("Raynium.Visible", double(frame.visible_count));
    RADRAY_PROFILE_PLOT("Raynium.MeshUpdates", double(frame.mesh_count));
    if (!frame.output_sequence || frame.output_sequence == UINT64_MAX || frame.output_sequence != target.Submitted() + 1) {
        error = "Output sequence must start at 1 and increase by one";
        return false;
    }
    if (_textures ? !_textures->Validate(frame.input_sequence, frame.changed_textures, error) : (frame.input_sequence || frame.changed_textures)) {
        if (error.empty()) error = "Frame has inputs without a shared texture set";
        return false;
    }
    if (!_pipeline->ReadyForFrame(error)) return false;
    TerrainFrame prepared;
    if (!_scene.PrepareFrame(target, _textures, frame, _pipeline->Resources(), *_scheduler, prepared, error)) return false;
    RADRAY_PROFILE_PLOT("Raynium.Cache.DirtyBatches", double(_scene.Scene().GetDirtyBatches().size()));
    _pipeline->BeginFrame(prepared, &_scene.MaterialBindings());
    auto endFrame = radray::MakeScopeGuard([&]() noexcept { _pipeline->EndFrame(); });
    const bool accepted = [&]() {
        RADRAY_PROFILE_SCOPE_N("Raynium.RunFrame");
        return _scheduler->RunFrame(_scene.Scene());
    }();
    if (!accepted) {
        error = _pipeline->Error().empty() ? "SOLID frame rejected" : _pipeline->Error();
        return false;
    }
    target.Commit(frame.output_sequence);
    if (_textures) _textures->Commit(frame.changed_textures);
    return true;
}
```

#### 2.2 重写 `native/src/terrain_scene.cpp` 中的 `TerrainScene::PrepareFrame`
1. **删除 `Changed(...)` 与 `CheckMesh(...)` 辅助函数**。
2. **保留顶层描述符基本检查**：
   - 检查 `f.struct_size == sizeof(f)`、`f.reserved == 0`、`(f.flags & ~3u) == 0`、`f.epoch != 0 && f.epoch >= _state.epoch`，以及 10 个数组指针与 count 的 `Array(...)` 边界检查。
   - 初始化 `outFrame.target = &target; outFrame.textures = textures; outFrame.parameters = f;`，并将 `outFrame.parameters` 中的所有裸指针字段置 `nullptr`。
3. **处理 `epoch` 切换与 `rebuildScene` 判定**：
   - 记录 `const bool epochChanged = (f.epoch != _state.epoch);`
   - 记录 `const bool texturesChanged = (textures != _state.boundTextures);`
   - 记录 `const bool rebuildScene = _rebuildScene || epochChanged;`
   - 若 `epochChanged` 为 `true`（且 `_state.epoch != 0`）：
     - 将旧 `_state.indices`、`_state.meshes`、`_state.materials` 中的全部 `shared_ptr` 登记到 `scheduler.RetireResource(owner)`；
     - 清空 `_state.indices`、`_state.meshes`、`_state.materials`、`_state.primitives`、`_state.materialBindingsByHandle`；
     - 调用 `_scene.Clear()`。
   - 若 `rebuildScene` 为 `true` 且 `!epochChanged`（例如 `DetachTextures()` 后首次渲染）：
     - 调用 `_scene.Clear()` 并清空 `_state.materialBindingsByHandle`。
   - 更新 `_state.epoch = f.epoch; _state.boundTextures = textures; _rebuildScene = false;`。
4. **稳态快路径判断**：
   - 定义 `const bool hasSceneUpdates = rebuildScene || texturesChanged || f.removed_index_count || f.removed_mesh_count || f.removed_material_count || f.removed_primitive_count || f.index_count || f.mesh_count || f.material_count || f.primitive_count;`
   - **当 `!hasSceneUpdates` 时（大场景稳态帧）**：直接跳过所有删除、上传、材质绑定和 Batch 同步逻辑，直接进入第 6 步生成 `visibleBatches` 和第 7 步创建当帧资源！
5. **增量更新路径（仅当 `hasSceneUpdates == true` 时执行）**：
   - 使用局部 `std::unordered_set<uint64_t>` 记录本帧**覆盖了已有同 ID 记录**的 `replacedIndices`、`replacedMeshes`、`updatedMaterials` 以及本帧新增/覆盖的 `touchedPrimitives`：
   - **(a) 按依赖顺序执行显式删除（`f.removed_*`）**：
     - **先删 `f.removed_primitives`**：在 `_state.primitives` 中查找，若存在且 `primitiveHandle.IsValid()`，调用 `_scene.RemovePrimitive(primitiveHandle)`，然后从 `_state.primitives` 擦除；
     - **再删 `f.removed_meshes`**：在 `_state.meshes` 中查找，若存在则 `scheduler.RetireResource(it->second)` 并擦除；
     - **再删 `f.removed_indices`**：在 `_state.indices` 中查找，若存在则 `scheduler.RetireResource(it->second)` 并擦除；
     - **最后删 `f.removed_materials`**：在 `_state.materials` 中查找，若存在则先从 `_state.materialBindingsByHandle` 擦除 `handle.Index`，若 `handle.IsValid()` 调用 `_scene.RemoveMaterial(handle)`，再将 `it->second` 移交 `scheduler.RetireResource(it->second)` 并从 `_state.materials` 擦除。（先删 primitive 可保证其关联 `MeshBatch` 已被移除，从而使 `_scene.RemoveMaterial` 不会因仍被 batch 引用而失败。）
   - **(b) 就地新增/替换 `f.indices` 与 `f.meshes` 并暂存上传（`Raynium.Scene.StageGeometry`）**：
     - `outFrame.uploads.reserve(size_t(f.index_count) + f.mesh_count);`
     - 遍历 `f.indices[0 .. f.index_count)`：
       - 基本检查 `!x.id || x.reserved || !x.count || !Array(x.indices, x.count, 16u << 20)`；
       - 创建 `auto data = std::make_shared<TerrainIndex>();`，分配 `uint64_t(x.count) * 4` 字节的 Device Index Buffer 并调用 `Upload(device, outFrame, data->buffer.get(), x.indices, bytes, BufferState::Index)`；
       - 若 `_state.indices` 中已存在 `x.id`：将旧 `it->second` 移交 `scheduler.RetireResource(it->second)`，原地替换为 `std::move(data)`，并将 `x.id` 插入 `replacedIndices`；否则直接 `emplace(x.id, std::move(data))`。
     - 遍历 `f.meshes[0 .. f.mesh_count)`：
       - 基本检查 `!x.id || !x.vertices || !x.index_buffer || !x.vertex_count || x.vertex_count > (1u << 20) || !x.submesh_count || !Array(x.submeshes, x.submesh_count, 65536)`；
       - 创建 `auto mesh = std::make_shared<TerrainMesh>();`，填入 `indexId`、`vertexCount`、`ranges`，分配 `uint64_t(x.vertex_count) * 28` 字节的 Device Vertex Buffer 并调用 `Upload(...)`，初始化 `mesh->input`（槽 0 绑定 `mesh->buffer.get()`，槽 1 声明为 Instance）；
       - 若 `_state.meshes` 中已存在 `x.id`：将旧 `it->second` 移交 `scheduler.RetireResource(it->second)`，原地替换为 `std::move(mesh)`，并将 `x.id` 插入 `replacedMeshes`；否则直接 `emplace(x.id, std::move(mesh))`。
   - **(c) 就地新增/更新 `f.materials` 与纹理绑定（`Raynium.Scene.MaterialBindings`）**：
     - 遍历 `f.materials[0 .. f.material_count)`：
       - 基本检查 `!x.id || x.reserved || x.anisotropy > 16`；
       - 若 `_state.materials` 中已存在 `x.id`：将旧 `it->second` 移交 `scheduler.RetireResource(it->second)`，新建 `TerrainMaterial` 继承旧 `handle` 并写入新 `desc`，覆盖 `it->second`；否则新建 `TerrainMaterial` 插入 `_state.materials`。将 `x.id` 记入 `updatedMaterials`。
     - 若 `rebuildScene || texturesChanged`：需为 `_state.materials` 中的**全部**存活材质重建 `MeshMaterialBinding` 和 `MaterialRenderProxy`（若 `!rebuildScene && texturesChanged` 且旧 `material->binding` 非空，先将旧 `material->binding` 移交 `scheduler.RetireResource(material->binding)`）；
       否则仅遍历 `updatedMaterials` 中的材质创建 `MeshMaterialBinding` 和 `MaterialRenderProxy`。
     - 对每个生成了新 `proxy` 的材质：若 `rebuildScene || !material->handle.IsValid()`，调用 `material->handle = _scene.AddMaterial(std::move(proxy))`；否则调用 `_scene.UpdateMaterial(material->handle, std::move(proxy))`。同时更新 `_state.materialBindingsByHandle[material->handle.Index] = material->binding;`。
   - **(d) 就地新增/替换 `f.primitives` 并按需刷新存活 Batch（`Raynium.Scene.Apply`）**：
     - 遍历 `f.primitives[0 .. f.primitive_count)`：
       - 基本检查 `!x.id || !x.mesh || !x.material`；
       - 若 `_state.primitives` 中已存在 `x.id` 且其 `primitiveHandle.IsValid()`，先调用 `_scene.RemovePrimitive(it->second->primitiveHandle)`；
       - 创建或更新 `TerrainPrimitive`，调用 `primitive->primitiveHandle = _scene.AddPrimitive()` 与 `primitive->batchHandle = _scene.AddMeshBatch(primitive->primitiveHandle, Batch(*primitive, _state))`；
       - 将 `x.id` 记入 `touchedPrimitives`。
     - 若 `rebuildScene` 为 `true`：
       - 遍历 `_state.primitives` 中不在 `touchedPrimitives` 内的剩余存活图元，为它们调用 `AddPrimitive` 与 `AddMeshBatch` 重建句柄。
     - 否则，**仅当 `!replacedMeshes.empty() || !replacedIndices.empty()` 时**（即本帧发生了对**已存在** `mesh` 或 `index` 的同 ID 覆盖更新）：
       - 遍历 `_state.primitives` 中不在 `touchedPrimitives` 内的存活图元，若其引用的 `mesh` 在 `replacedMeshes` 中、或该 `mesh` 引用的 `indexId` 在 `replacedIndices` 中，则调用 `_scene.UpdateMeshBatch(primitive->batchHandle, Batch(*primitive, _state))`。
       - （注：在常规区块加载帧中，新区块都是新 ID，且共享索引缓冲已预设容量不扩容，`replacedMeshes` 与 `replacedIndices` 均为空，此步耗时为 0。）
6. **生成当帧可见列表（`Raynium.Scene.VisibleBatches`）**：
   - 在 `_state` 上输出 `RADRAY_PROFILE_PLOT("Raynium.Resident.Meshes", ...)` 等三个驻留计数 Plot。
   - 若 `f.visible_count > 0`：
     - `outFrame.visibleBatches.reserve(f.visible_count);`
     - 遍历 `f.visible[0 .. f.visible_count)`：通过 `_state.primitives.find(item.primitive)` 取得 `batchHandle`（若未找到或 `!item.instance_count || uint64_t(item.first_instance) + item.instance_count > f.instance_count` 则报错返回 `false`），直接 `push_back` 到 `outFrame.visibleBatches`（无需再先拷入中间 `std::vector<rn_visible_item>`）。
7. **创建当帧瞬态 GPU 资源（`Raynium.Frame.PrepareResources`）**：
   - 保持现有逻辑创建 `outFrame.instances`、`outFrame.depth`、`outFrame.colorView`、`outFrame.depthView` 及可选 `outFrame.timestamps` / `outFrame.timing`。

---

### 阶段三：预设 Java 侧共享 quad 索引缓冲容量并更新测试

#### 3.1 修改 `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`
- 在 `draw(NativeRenderTarget output, long outputSequence)`（约第 135～139 行）中，将共享 quad 索引缓冲（`Index(1)`）的最小初始容量从 `4` 提升为 `65536` 顶点（即 `16,384` 个 quad，`98,304` 个 `int` 索引，384 KiB）：
  ```java
  int capacity = indexCapacity;
  if (needed > indexCapacity) {
      capacity = Math.max(65536, Integer.highestOneBit(needed - 1) << 1);
      frame.indices = List.of(new NativeRenderer.Index(1, TerrainScene.quadIndices(capacity)));
  }
  ```
- 这样在进入世界首次上传地形网格时，`Index(1)` 直接一步到位分配 65,536 顶点容量，消除运行期因索引缓冲翻倍引发的全场景 `UpdateMeshBatch` 重建尖峰；若极端模组区块超出 65,536 顶点，仍保留向上取 2 的幂次的扩容能力。

#### 3.2 修改 `src/test/java/dev/raynium/bridge/NativeTerrainTest.java`
由于移除了 CPU 逐索引越界检查（`CheckMesh`）以及单帧失败后的事务回滚（宿主契约为失败后直接关闭渲染器，不在同一 `Renderer` 上继续渲染下一帧），清理 `NativeTerrainTest.java` 中专门测试“中途注入非法帧抛异常后继续在原 `Renderer` 上渲染下一帧”的 3 处断言：
1. **`twoPendingSlotsKeepParametersAndReplacedGeometry`**：
   - 删除第 77～79 行注入越界索引 `999` 的测试代码：
     ```java
     // 删除以下三行：
     var invalid=frame(1,pair*2+3,pair*2+3);
     invalid.indices=List.of(new NativeRenderer.Index(1,new int[]{999,1,2,2,3,0}));
     assertThrows(IllegalStateException.class,()->renderer.render(target,invalid));
     ```
   - 删除第 92～95 行在删除 `primitive 1` 后故意引用不存在的 `Visible(1, ...)` 并期望同一 `renderer` 在第 97 行继续渲染 epoch 2 的测试代码：
     ```java
     // 删除以下四行：
     var missing=frame(1,18,18);
     missing.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
     missing.visible=List.of(new NativeRenderer.Visible(1,0,0,0,1));
     assertThrows(IllegalStateException.class,()->renderer.render(target,missing));
     ```
2. **`sceneSurvivesOutputAndInputRecreation`**：
   - 删除第 262～265 行故意在 `generation == 0` 注入 `Visible(99, 0, 0, 0, 1)` 触发回滚后再用同一 `packet` 重试的测试代码，直接设置 `packet.visible = List.of(new NativeRenderer.Visible(1, 0, 0, 0, 1));` 并执行渲染验证。

---

### 阶段四：同步更新事实文档

代码修改与测试通过后，按 [文档维护](../guide/documentation.md) 规范同步更新以下当前文档（不修改 `doc/history/` 中的已有历史文件）：

1. **`doc/architecture/native-runtime.md`**：
   - 更新“适配层职责”：说明 `TerrainScene` 持有持久 `radray::Scene` 和以 `std::unordered_map` 索引的 `TerrainState`；`Renderer::Render` 先等待待复用槽完成，再由 `TerrainScene` 按当帧 ABI 增量数组就地更新 `_state` 与 `_scene`、将被替换/删除的旧资源登记到 `RetireResource`、直接将顶点/索引写入 GPU/staging（不在 CPU 保留索引值副本或做逐索引扫描），并生成当帧可见批次与瞬态资源；`RunFrame` 成功后推进输入/输出 timeline 序号。移除关于“Prepare 浅拷贝候选状态”、“备份 Scene”、“失败通过 noexcept move 恢复备份 Scene / 候选状态”的描述。
   - 更新“上传、帧槽与资源退役”表格与正文：移除“索引内容”CPU 驻留和“回滚失败”措辞。
2. **`doc/architecture/interop.md`**：
   - 更新“ABI 9 布局”正文（第 35 行、第 73～74 行）：移除“索引被复制到 CPU 保留”和“native 逐索引检查 `indices[first+j]+base_vertex` 落在顶点范围内、全表扫描悬空引用”的表述，改为说明索引与顶点直接写入 native GPU/staging 缓冲，资源增删改按增量数组直接应用。
3. **`doc/architecture/terrain-preview.md`**：
   - 更新“几何与绘制”：说明共享 quad 索引缓冲以 65,536 顶点为最小初始容量生成并按需增长。
4. **`doc/guide/build-test.md`**：
   - 更新 Tracy zones 说明（移除已删除的 `CopyState`、`ValidateResources`、`Scene.Backup`、`Scene.Commit` 等 zone 描述，匹配精简后的实际 zone 名称），并记录本次构建与 `nativeTest` 验证结果。

---

## 4. 验证与交付检查清单

执行 AI 完成上述阶段一至阶段四后，必须在仓库根目录依次运行并通过以下全部检查：

```powershell
# 1. 构建 Java、编译并 stage Debug native DLL，运行全部纯 Java 测试与双后端 GPU 集成测试
.\gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel

# 2. 构建并测试 Release 配置，打包正式 native Mod JAR 并校验包结构与哈希
.\gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel

# 3. 运行文档规范与空白字符检查
python tools/check_docs.py
git diff --check
```

### 通过标准
- Debug（`/MTd`）与 Release（`/MT`）配置下，`rayniumruntime.dll` 编译零错误，导出符号与 ABI 9 清单校验通过。
- 全部纯 Java 单元测试与 `@Tag("native")` 双后端（D3D12 / Vulkan）GPU 集成测试 100% 通过，无跳过、无 RHI 校验错误、无 Vulkan VUID 报错。
- 在 0 更新稳态帧下，`Renderer::Render` 不再执行 `radray::Scene` 拷贝构造、`TerrainState` 容器拷贝或驻留表遍历。
- `python tools/check_docs.py` 与 `git diff --check` 零错误通过，且文档中不包含用户未要求保存的 Git commit hash。

---

## 5. 暂不纳入本次实施的后续候选方向（仅作归档备查）

以下三项为本次 Tracy 分析识别出的其余瓶颈，**不纳入本次执行范围**（执行 AI 请勿在本次任务中修改 `external/RadRay` 或更改瞬态 RHI 资源生命周期）：

1. **上游 `RadRay::runtime` 的 `SubmitMeshDrawCommands` 状态去重与 `Scene` 反向句柄（稳态约 1.41 ms/帧）**：
   - `external/RadRay/modules/runtime/src/mesh_draw_command.cpp` 每帧对 3,687 条可见命令逐条执行 `ValidateMeshDrawCommand`，且仅对 `Pipeline` 去重，重复绑定 3,687 次相同的 `InstanceStream`（Slot 1）、共享 `IndexBuffer`、材质 `ParameterSet`（Group 1）和 `StencilRef`；
   - `external/RadRay/modules/runtime/src/scene.cpp` 的 `FindOwningPrimitive` 线性遍历全部 `Primitives`。
2. **每帧瞬态 D3D12 RHI 对象改为持久复用（稳态约 1.02 ms/帧）**：
   - `PrepareResources` 每帧新建 `instances` Upload Buffer、`depth` Texture、`colorView`、`depthView`、`timestamps` QueryPool 与 `timing` Readback Buffer，并在 2 帧后 `ResetFrameData` 中销毁。后续可将 `depth` / `View` 绑定至 `RenderTarget` 尺寸生命周期，将 `instances` / `timestamps` / `timing` 移入 `SolidFramePipeline::SlotResources` 双槽复用。
3. **区块加载期 Staging Buffer 合并与 FFM 编组优化（加载期均值 1.37 ms，尖峰 29.83 ms）**：
   - 单帧上传多个新网格时合并单次 Upload Staging Buffer 分配，并减少 Java `NativeRenderer.marshal` 每帧堆外内存分配与复制次数。
