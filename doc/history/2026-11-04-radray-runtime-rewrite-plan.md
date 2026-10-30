> - 适用: 执行 RadRay 新版 runtime 接入与 SOLID 地形绘制的 Java/native 重写
> - 权威: 用户确认的实施计划与 AI 交接文档；落地前不构成当前实现事实，完成后需同步架构与指南文档
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/src/renderer.cpp`, `native/src/terrain_scene.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `src/client/java/dev/raynium/bridge/NativeRenderer.java`, `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`

# RadRay 新版 Runtime 接入与 SOLID 地形绘制重写计划

日期：2026-11-04。
来源：用户在会话中明确要求升级至最新 `RadRay::runtime` 帧概念提交，并在不考虑旧兼容性的前提下重写 Java 与 native 适配层，直接消费新版 runtime 的增量命令缓存、按 Pass 可见命令提交、延迟实例流和跨槽资源退役能力，仅保留 SOLID 地形绘制。

## 1. 范围与核心取舍

本计划由用户明确确认以下三项核心决策：

1. **仅保留 SOLID 地形 + 新框架直通录制**：
   - 删除 `UvScene`、`uv.hlsl`、`uv_bytecode.h`、`RN_PROGRAM_UV`、`RN_LAYOUT_PROCEDURAL`。
   - 删除绕过 `RadRay::runtime` 的手写 Indirect 参数构建与手写 Draw 循环，统一使用 `MeshPassFrameDrawList::Submit`（内部调用 `SubmitMeshDrawCommands` 执行预检、PSO/参数集去重、延迟实例流绑定与 `DrawIndexed`）。
2. **每 Section Mesh 独立 GPU Buffer + `FrameScheduler::RetireResource` 退役**：
   - 删除 `GeometryPage`（4 MiB 顶点页池）与 `GeometryAllocation` 空闲区间合并逻辑。
   - 每个驻留 `Index`、`Mesh`、`Material` 各自持有独立 GPU 资源；当资源被同 ID 替换、显式删除、纹理重绑或 `epoch` 切换时，将旧 GPU 资源 owner 移交 `FrameScheduler::RetireResource`，待所有在途 `FrameSlot` 重置命令分配器后自动释放。
3. **升级并精简为 C ABI 9**：
   - 不保留 ABI 8 兼容层，同步精简 `bridge.h`、Java FFM 绑定、构建清单与测试。

---

## 2. 新版 `RadRay::runtime` 机制映射

| 子系统 | 旧版 Raynium 方式 | 重写后目标方式（基于新版 `RadRay::runtime`） |
| --- | --- | --- |
| 场景容器 | 每帧深拷贝 `TerrainState`，每帧新建临时 `radray::Scene` 仅填入可见项 | `TerrainScene` 直接持有持久 `radray::Scene`，通过 `Add/Update/Remove` 维护 `Primitives`、`Meshes`、`Materials` 与脏标记 |
| 命令缓存 | 无跨帧缓存，每帧在 `Record` 中对可见项跑一遍 `Processor` | `FrameScheduler::RunFrame(Scene&)` 在 `Extract` 前自动调用 `Scene::SyncCachedMeshDrawCommands`，仅对脏 `MeshBatch` 调用 `SolidPassProcessor::AddMeshBatch` |
| 参数绑定 | 帧常量与纹理混在 Group 0，每帧重建材质 `ShaderParameterSet` 并存在旁路数组 | 拆分为 **Group 0（Pass 级每槽帧常量 CBuffer）** 与 **Group 1（Command 级持久材质纹理与采样器）**；Group 1 存入缓存的 `MeshDrawCommand::ParameterSets` |
| 实例绘制 | CPU 循环展开每个实例，生成多条带 VB 字节偏移的单实例命令 | `VertexInput` 声明槽位 1 为 `VertexStepMode::Instance` 且不绑默认流；`Processor` 开启 `.AllowDeferredInstanceStream = true`；`Extract` 通过 `VisibleMeshBatch` 覆盖 `FirstInstance`/`InstanceCount`；`Record` 通过 `MeshPassSubmitArgs::InstanceStream` 传入当帧实例缓冲 |
| 排序与录制 | CPU 对 `rn_visible_item` 排序后逐条手写 RHI 绑定 | `Extract` 调用 `Scene::GatherVisibleMeshDrawCommands` 与 `MeshPassFrameDrawList::SortVisibleCommands`；`Record` 调用 `MeshPassFrameDrawList::Submit` |

---

## 3. 分阶段执行步骤

### 阶段一：更新 RadRay 锁文件与第三方依赖

1. **更新锁文件**：
   - 将 `native/radray.lock.json` 中的 `commit` 更新为用户指定的 2026-11-04 `帧概念` 提交。
   - 在仓库根运行：
     ```powershell
     python tools/fetch_radray.py restore
     ```
2. **重建 Debug `/MTd` 依赖（如锁文件提交变化触发身份校验）**：
   - 若 CMake configure 提示 `Debug dependency identity differs`，运行：
     ```powershell
     python tools/build_debug_dependencies.py --clean
     ```

---

### 阶段二：Shader 绑定拆分与字节码重生成

1. **删除 UV Shader 相关文件**：
   - 删除 `native/shaders/uv.hlsl` 与 `native/include/raynium/uv_bytecode.h`。
   - 修改 `tools/compile_preview_shaders.py`：移除 `uv` 选项，默认且仅编译 `terrain.hlsl` 生成 `native/include/raynium/terrain_bytecode.h`。
2. **拆分 `native/shaders/terrain.hlsl` 的绑定集合**：
   - **Group 0 (`space0` / Vulkan Descriptor Set `0`)**：Pass 级每帧常量
     ```hlsl
     [[vk::binding(0, 0)]] cbuffer Frame : register(b0, space0) {
         float4x4 Projection;
         float4x4 View;
         int4 CameraBlock;
         float4 CameraFraction;
         float4 FogColor;
         float4 FogDistances;
     };
     ```
   - **Group 1 (`space1` / Vulkan Descriptor Set `1`)**：材质级纹理与采样器
     ```hlsl
     [[vk::binding(1, 1)]] Texture2D<float4> Atlas : register(t0, space1);
     [[vk::binding(2, 1)]] Texture2D<float4> Lightmap : register(t1, space1);
     [[vk::binding(3, 1)]] SamplerState AtlasSampler : register(s0, space1);
     [[vk::binding(4, 1)]] SamplerState LightSampler : register(s1, space1);
     ```
3. **重新编译 Shader 字节码**：
   - 运行 `python tools/compile_preview_shaders.py` 更新 `native/include/raynium/terrain_bytecode.h`。

---

### 阶段三：升级 C ABI 9 与构建清单

1. **修改 `native/include/raynium/bridge.h`**：
   - 将 `#define RN_ABI_VERSION` 更新为 `9u`。
   - 删除 `enum rn_shader_program` 与 `enum rn_vertex_layout`。
   - 调整 `enum rn_frame_flags : uint32_t`：
     ```cpp
     enum rn_frame_flags : uint32_t {
         RN_FRAME_RGSS = 1,
         RN_FRAME_PROFILE = 2
     };
     ```
   - 调整场景更新结构体布局：
     ```cpp
     struct rn_index_update { uint64_t id; const uint32_t* indices; uint32_t count, reserved; }; // 24B
     struct rn_submesh { uint32_t first, count; int32_t base_vertex; uint32_t reserved; }; // 16B
     struct rn_mesh_update {
         uint64_t id;
         const void* vertices;
         uint64_t index_buffer;
         const rn_submesh* submeshes;
         uint32_t vertex_count, submesh_count;
     }; // 40B (固定 28B 顶点步长)
     struct rn_material_update {
         uint64_t id;
         uint32_t albedo_texture, light_texture, anisotropy, reserved;
     }; // 24B
     struct rn_primitive_update { uint64_t id, mesh, material; }; // 24B
     struct rn_instance { int32_t origin[3]; float visibility; }; // 16B
     struct rn_visible_item {
         uint64_t primitive;
         uint32_t submesh, sort_key, first_instance, instance_count;
     }; // 24B
     ```
   - `rn_render_frame_desc` 保持 344 字节及各字段偏移不变。
2. **同步 ABI 9 构建与清单检查**：
   - `native/src/bridge.cpp`：更新 `static_assert(sizeof(rn_mesh_update) == 40)` 等静态断言。
   - `native/cmake/native-build.json.in`：`"abiVersion": 9`。
   - `tools/native.py`：`check_manifest` 中 `manifest.get('abiVersion') != 9`，错误提示更新为 `ABI 9`。
   - `src/client/java/dev/raynium/bridge/NativeManifest.java`：`ABI_VERSION = 9`。

---

### 阶段四：重写 Native 渲染与场景模块 (`native/`)

#### 4.1 `mesh_pass.h` / `mesh_pass.cpp`（SOLID 管线与材质绑定）

1. **`MeshPassResources`**：
   - 只创建一套 SOLID 地形管线（移除双 program 数组）：
     - `passGroupLayout`（Group 0）：Binding 0 为 `BindingKind::CBuffer`（`ShaderStage::Vertex | ShaderStage::Pixel`，D3D12 `CBV b0 space0`，Vulkan `set 0 binding 0`）。
     - `materialGroupLayout`（Group 1）：
       - Binding 1：`Texture` (`ShaderStage::Pixel`, D3D12 `SRV t0 space1`, Vulkan `set 1 binding 1`)
       - Binding 2：`Texture` (`ShaderStage::Vertex`, D3D12 `SRV t1 space1`, Vulkan `set 1 binding 2`)
       - Binding 3：`Sampler` (`ShaderStage::Pixel`, D3D12 `Sampler s0 space1`, Vulkan `set 1 binding 3`)
       - Binding 4：`Sampler` (`ShaderStage::Vertex`, D3D12 `Sampler s1 space1`, Vulkan `set 1 binding 4`)
     - `pipelineLayout`：包含 Group 0 (`passGroupLayout`) 和 Group 1 (`materialGroupLayout`)。
     - `pipeline`（`GraphicsPipelineState`）：
       - 顶点输入：Binding 0（Stride 28，`VertexStepMode::Vertex`，4 个属性：`POSITION`, `COLOR`, `TEXCOORD0`, `TEXCOORD1`）与 Binding 1（Stride 16，`VertexStepMode::Instance`，2 个属性：`TEXCOORD2` `SINT32X3`, `TEXCOORD3` `FLOAT32`）。
       - 光栅与深度：背面剔除（`CullMode::Back`）、`D32_FLOAT`、`CompareFunction::GreaterEqual`、开启深度测试与写入、颜色附件 `RGBA8_UNORM`。
     - **成员声明与析构顺序约束**：RHI 校验层对 `ShaderParameterSet -> BindingGroupLayout`、`PipelineLayout -> BindingGroupLayout`、`GraphicsPipelineState -> PipelineLayout` 维护强引用计数（`Dependents != 0` 时析构被依赖方会触发 `Fatal`）。因此 `MeshPassResources` 内部成员必须按 `vs/ps` -> `passGroupLayout/materialGroupLayout` -> `pipelineLayout` -> `pipeline` -> `materialResource` -> `baseMaterial` 顺序声明（逆序析构）。
   - 提供 `CreateMaterialBinding(Device&, const rn_material_update&, const SharedTextures&, MeshMaterialBinding&, MaterialRenderProxy&, radray::string& error)`：
     - 根据 `albedo_texture`、`light_texture`、`anisotropy` 创建 `albedoSampler`、`lightSampler` 与 Group 1 `ShaderParameterSet`，写入绑定并 `FlushWrites()`，同时生成对应的 `radray::MaterialRenderProxy`。

#### 4.2 `terrain_scene.h` / `terrain_scene.cpp`（持久 `radray::Scene` 与资源退役）

1. **数据结构设计**：
   - `TerrainIndex`：持有 `std::vector<uint32_t> values`（用于校验 `submesh` 顶点越界）和 `RenderBuffer buffer`（Device Index Buffer）。
   - `TerrainMesh`：
     - 持有 `uint64_t indexId`、`uint32_t vertexCount`、`std::vector<rn_submesh> ranges`、`RenderBuffer buffer`（独立 Device Vertex Buffer，大小 `vertexCount * 28`）以及 `radray::VertexInput input`。
     - **地址稳定性要求**：`TerrainScene` 用 `std::map<uint64_t, std::shared_ptr<TerrainMesh>>` 存储 `TerrainMesh`，保证每个版本节点的 `mesh->input` 地址在生命周期内固定不变。
     - `mesh->input` 初始化：
       - `Layouts`：`{{0, 28, VertexStepMode::Vertex}, {1, 16, VertexStepMode::Instance}}`。
       - `Attributes`：6 个顶点/实例属性。
       - `Streams`：`{{mesh->buffer.get(), 0, uint64_t(vertexCount) * 28, 0}}`（**仅绑定槽位 0**，槽位 1 留给延迟实例流）。
   - `TerrainMaterial`：
     - 持有 `rn_material_update desc`、`radray::SparseSetHandle handle`（在 `radray::Scene::Materials` 中的句柄）、`std::shared_ptr<MeshMaterialBinding> binding`（持有 `albedoSampler`、`lightSampler` 和 Group 1 `ShaderParameterSet`）。
   - `TerrainPrimitive`：
     - 持有 `rn_primitive_update desc`、`radray::SparseSetHandle primitiveHandle`、`radray::SparseSetHandle batchHandle`。
   - `TerrainState`：
     - 聚合 `uint64_t epoch{}`、`const SharedTextures* boundTextures{}` 以及 `indices`、`meshes`、`materials`、`primitives` 四个 `std::map<uint64_t, std::shared_ptr<...>>` 字典，并维护一个按 `SparseSetHandle::Index`（即 `radray::Scene::Materials` 句柄下标）索引到 `std::shared_ptr<MeshMaterialBinding>` 的查找表 `materialBindingsByHandle`。
2. **三阶段无副作用事务（Pre-allocate -> Apply & RunFrame -> Commit or Rollback）**：
   - **第一阶段：纯校验与全部当帧/增量 GPU 资源预分配（不修改持久 `_state` 与 `radray::Scene`）**：
     - 校验 `rn_render_frame_desc`：`struct_size == 344`、`reserved == 0`、`(flags & ~3u) == 0`、`epoch != 0 && epoch >= _state.epoch`、所有数组指针与数量上限、同帧新增/删除 ID 无零值且无重复、同类别新增与删除 ID 无交集。
     - 构造候选状态副本 `TerrainState candidate`（若 `f.epoch == _state.epoch` 则浅拷贝 `_state` 的四个 `shared_ptr` map，否则从空 map 开始；记录 `const bool texturesChanged = (textures != _state.boundTextures)` 并设置 `candidate.boundTextures = textures`）：
       - 在 `candidate` 上应用 `removed_*` 与新增/更新项；
       - 校验所有存活 `mesh` 的 `index_buffer` 均存在，且对新增/更新的 `mesh` 或其引用的 `index_buffer` 更新，`CheckMesh` 验证每个 `submesh` 的 `count > 0 && count % 3 == 0 && reserved == 0`，且每个 `indices[first + j] + base_vertex` 落在 `[0, vertex_count)` 内；
       - 校验所有存活 `primitive` 引用的 `mesh` 和 `material` 均存在；
       - 若候选状态中存在存活 `material` 或 `primitive`，则 `SharedTextures*` 必须非空，且 `albedo_texture < textures->Count()`、`light_texture < textures->Count()`、`anisotropy <= 16`；
       - 校验所有 `visible` 项引用的 `primitive` 存在、`submesh < mesh.ranges.size()`、`instance_count > 0`、`first_instance + instance_count <= f.instance_count`，且累加 `instance_count <= 1,048,576`。
     - 为本帧所有新增/更新的 `indices` 和 `meshes` 分配独立 Device `Buffer` 与 Upload Staging `Buffer`（写入顶点/索引字节，生成 `GeometryUpload` 记录）。
     - 为本帧所有新增/更新的 `materials`（若 `texturesChanged` 为 `true`，则包含 `candidate.materials` 中的**全部**存活材质）创建新的 `std::shared_ptr<MeshMaterialBinding>` 和 `MaterialRenderProxy`。
     - **同时在第一阶段预创建当帧全部瞬态 GPU 资源**（当帧 `instances` Upload Buffer 并完成映射写入、当帧 `depth` 纹理、`colorView`、`depthView`、可选 Timestamp `QueryPool` 与 `timing` Readback Buffer、以及 `Record` 阶段去重所需的 destination buffer 列表），避免进入 `Record` 后因资源或内存分配失败而中止。
     - 若上述任意一步失败，直接返回 `false`，所有临时分配的 GPU 资源随局部变量析构释放，持久 `_state` 与 `radray::Scene` 零改动。
   - **第二阶段：在确认复用槽就绪后预登记退役资源、同步 `radray::Scene` 并执行 `RunFrame`**：
     - 先调用 `SolidFramePipeline::ReadyForFrame` 等待待复用槽的上次提交完成。
     - **在提交前完成退役批次分配**：收集将被替换或删除的旧 `TerrainIndex`、`TerrainMesh`、`TerrainMaterial` 的 `std::shared_ptr` GPU 资源 owner，**在调用 `RunFrame` 之前**调用 `_scheduler->RetireResource(oldOwnerSharedPtr)`：
       - 由于 `_state` 此时仍持有这些 `shared_ptr` 的引用，即便此处 `RetireResource` 抛 `std::bad_alloc` 或后续 `RunFrame` 失败回滚，旧资源仍由 `_state` 保活，绝不会提前释放；
       - 而一旦 `RunFrame` 成功并在第三阶段移动覆盖 `_state`，就不再需要在 `Queue::Submit` 之后执行任何可能抛异常的内存分配，且当前复用槽在 `RunFrame` 开头 `ResetCommandStorage()` 时即可立即标记本槽已重置。
     - 备份当前 `radray::Scene backupScene = _scene;`。
     - 将增量同步到 `_scene`，并在 `candidate.materialBindingsByHandle` 中建立最新的 `SparseSetHandle::Index -> std::shared_ptr<MeshMaterialBinding>` 映射：
       - 若 `f.epoch > _state.epoch`：调用 `_scene.Clear()`，在 `_scene` 中按 `candidate` 全量添加新 `Materials`、`Primitives` 和 `MeshBatches`。
       - 若 `f.epoch == _state.epoch`，按依赖顺序更新 `_scene`：
         1. **删除/替换 Primitive**：对 `removed_primitives` 以及本帧被覆盖的同 ID `primitive`，调用 `_scene.RemovePrimitive(oldPrim.primitiveHandle)`（自动级联删除其关联的 `batchHandle`）。
         2. **更新/新增 Material**：对新增 `material` 调用 `_scene.AddMaterial(std::move(proxy))` 并写入新的 `handle`；对已存在且被更新（或因 `texturesChanged` 重绑）的 `material`，调用 `_scene.UpdateMaterial(mat.handle, std::move(proxy))` 自动标脏所有引用它的 `MeshBatch`。
         3. **删除不再被引用的 Material**：对 `removed_materials` 调用 `_scene.RemoveMaterial(oldMat.handle)`。
         4. **原地更新受 Index / Mesh 替换影响的存活 Primitive Batch**：遍历所有未在步骤 1 重建、但其引用的 `mesh` 或 `index` 在本帧发生了同 ID 替换的存活 `primitive`，**保留其原有 `prim.batchHandle` 不删除**，构造指向新 `&mesh->input` 和新 `IndexBuffer`/`Elements` 的 `MeshBatch`，调用 `_scene.UpdateMeshBatch(prim.batchHandle, std::move(batch))` 原地替换并标脏。
         5. **新增/重建 Primitive**：对本帧新增或同 ID 覆盖的 `primitive`，调用 `_scene.AddPrimitive()` 与 `_scene.AddMeshBatch(primitiveHandle, std::move(batch))`，记录新的 `primitiveHandle` 与 `batchHandle`。
     - 根据 `visible` 数组生成当帧 `std::vector<radray::VisibleMeshBatch>`（填入 `Batch = prim.batchHandle`、`ElementIndex = item.submesh`、`VisibilitySortKey = item.sort_key`、`FirstInstanceOverride = item.first_instance`、`InstanceCountOverride = item.instance_count`）。
     - **复位 Processor 错误状态并绑定候选材质表**：调用 `_pipeline->BeginFrame(preparedFrame, &candidate.materialBindingsByHandle)`（内部复位 `_processor.ClearError()` 并将候选材质绑定表提供给 `SolidPassProcessor`），确保 `RunFrame` 内部紧接着执行的 `SyncCachedMeshDrawCommands` 读取到本帧最新的候选材质 `ShaderParameterSet`。
     - 调用 `_scheduler->RunFrame(_scene)`（配合 `MakeScopeGuard` 在退出时调用 `_pipeline->EndFrame()` 清空临时指针）。
   - **第三阶段：无异常提交确认或无损回滚（Commit or Rollback）**：
     - **若 `RunFrame` 返回 `true`**（表示 `SyncCachedMeshDrawCommands`、`Extract`、`Record` 及单次 `Queue::Submit` 全部成功）：
       - 执行纯 `noexcept` 状态切换：`_state = std::move(candidate);`（旧资源在 `_state` 中的 `shared_ptr` 引用释放，由第二阶段已登记到 `_scheduler` 的退役批次继续保活至所有在途槽重置）；
       - 推进 `target.Commit(f.output_sequence)` 与 `textures->Commit(f.changed_textures)`（均为 `noexcept`）。
     - **若 `RunFrame` 返回 `false`**（录制阶段未提交而返回错误）：
       - 执行纯 `noexcept` 回滚：`_scene = std::move(backupScene);`（持久 `_state` 未被修改，旧资源的 `shared_ptr` 仍由 `_state` 持有，本帧新分配的未提交 GPU 资源与 staging 随 `candidate` 析构自动释放），保证失败调用不发布任何未上传几何、不推进序号、不丢失旧资源。

#### 4.3 `solid_pipeline.h` / `solid_pipeline.cpp` 与 `renderer.h` / `renderer.cpp`

1. **`SolidPassProcessor`**：
   - 继承 `radray::MeshPassProcessor`，默认绑定到一个占位的 `radray::DynamicMeshPassDrawListContext`。
   - 持有对 `MeshPassResources` 的只读引用，以及由 `SolidFramePipeline::BeginFrame` 在每帧同步前注入的当前候选材质绑定表指针 `_activeMaterialBindings` 和错误标记 `_hasErrors`。
   - 提供 `void BeginSync(const MaterialBindingMap* bindings) noexcept { _activeMaterialBindings = bindings; _hasErrors = false; }`，在每次 `RunFrame` 前复位 `_hasErrors` 并指向候选材质表。
   - `AddMeshBatch(const radray::MeshBatchContext& context)` 实现：
     ```cpp
     const auto* materialBinding = FindMaterialBinding(_activeMaterialBindings, context.Batch.Material);
     if (!materialBinding || !materialBinding->parameters) {
         _hasErrors = true;
         return;
     }
     const radray::MeshShaderParameterBinding group1{
         .GroupIndex = 1,
         .ParameterSet = materialBinding->parameters.get(),
         .DynamicOffsets = {}
     };
     const uint64_t baseSortKey = (uint64_t(context.Batch.Material.Index) << 48) |
                                  (uint64_t(context.BatchHandle.Index) << 32);
     if (!BuildMeshDrawCommands(context, radray::MeshDrawCommandSharedState{
             .Pipeline = _resources.Pipeline(),
             .BaseSortKey = baseSortKey,
             .ParameterSets = std::span{&group1, 1},
             .AllowDeferredInstanceStream = true})) {
         _hasErrors = true;
     }
     ```
2. **`SolidFramePipeline`**：
   - 遵循 `AGENTS.md` 禁止二段式构造的约束：在 `SolidFramePipeline::Create` 中先创建 `MeshPassResources`、完成 `Fence` 以及全部双槽 `SlotResources[2]`（每个槽包含 256 字节 Upload CBuffer `passConstants` 与对应的 Group 0 `ShaderParameterSet`，并在 `Create` 内完成 `Set(0, 0, ShaderBufferBinding{...})` 和 `FlushWrites()`），全部成功后再构造并交付 `SolidFramePipeline`（不设 `Initialize` 成员函数；声明顺序上 `passConstants` 在 `passParameterSet` 之前，且 `SlotResources` 在 `MeshPassResources` 之前析构）。
   - 覆写 `GetMeshPassRegistrations() noexcept`：返回单元素 `{radray::MeshPassRegistration{.Pass = {0}, .Processor = &_processor}}`。
   - **`Extract(const radray::Scene& source, radray::FrameSlot& slot) noexcept`**：
     - 检查 `_processor.HasErrors()`，若有错误则写诊断 `"Invalid SOLID mesh command or material binding"` 并返回 `false`。
     - 将第一阶段已预创建好的当帧资源（`GeometryUpload` 列表、`instances` Upload Buffer、`depth` 纹理、`colorView`/`depthView`、可选 `timestamps`/`timing`）移入 `slot.GetArena().Emplace<FrameData>(...)`。
     - 若 `!_pending->visibleBatches.empty()`：
       - 获取 `auto& drawList = slot.GetPassDrawList({0});`
       - 调用 `source.GatherVisibleMeshDrawCommands({0}, _pending->visibleBatches, drawList.VisibleCommands)`；若返回 `false` 则失败返回。
       - 调用 `drawList.SortVisibleCommands()`。
   - **`Record(radray::FrameSlot& slot) noexcept`**：
     - 将当帧常量（`Projection`、`View`、`CameraBlock`、`CameraFraction`、`FogColor`、`FogDistances`）写入当前槽的 `SlotResources[slot.GetIndex()].passConstants`。
     - 调用 `slot.AcquireCommandBuffer()`，若为空则返回 `false`。
     - 调用 `command.Begin()`，并**立即注册 `MakeScopeGuard([&]() noexcept { command.End(); })`**，确保任何后续提前返回路径都会闭合 `CommandBuffer` 录制（否则 `FrameScheduler` 在失败清理时调用 `CommandAllocator::Reset()` 会因 `allocator has an open recording` 触发 RHI 校验错误）：
       1. 录制所有 `GeometryUpload`（对目标 Device Vertex/Index Buffer 执行 `Undefined -> CopyDestination` 屏障、`CopyBufferToBuffer`、`CopyDestination -> Vertex/Index` 屏障）。
       2. 若绑定了 `SharedTextures`，调用 `textures->Acquire(command, f.changed_textures)`。
       3. 对当前槽 `passConstants`（`HostWrite -> CBuffer`）、当帧 `instances`（`HostWrite -> Vertex`）、`depth`（`Undefined -> DepthWrite`）和输出 `target.Color()`（首帧 `Undefined` / 后续帧 `Common + Acquire` -> `RenderTarget`）录制屏障。
       4. 开启 `RenderPass`（清颜色为 `fog_color`，清深度为 `0`），若 `!encoder` 则直接返回 `false`（由外层 scope guard 自动执行 `command.End()`）。
       5. 开启 `RenderPass` 后**立即注册 `MakeScopeGuard` 确保 `command.EndRenderPass(encoder.Release())` 在 `command.End()` 之前执行**；随后设置 `Viewport` 与 `Scissor`。
       6. 若 `!slot.GetPassDrawList({0}).VisibleCommands.empty()`：
          - 构造 `MeshShaderParameterBinding passBinding{.GroupIndex = 0, .ParameterSet = slotResources.passParameterSet.get()}`；
          - 构造 `MeshPassSubmitArgs submitArgs{.ParameterSets = std::span{&passBinding, 1}, .InstanceStream = VertexBufferBinding{1, {frame.instances.get(), 0, frame.instances->GetDesc().Size}}}`；
          - 调用 `slot.GetPassDrawList({0}).Submit(*encoder, source->GetCachedDrawList({0}), submitArgs)`；若返回 `false`，直接返回 `false`（由两个嵌套 scope guard 依次关闭 `RenderPass` 和 `CommandBuffer`，不提交队列）。
       7. 正常关闭 `RenderPass`（触发或提前释放内层 guard）。
       8. 若绑定了 `SharedTextures`，调用 `textures->Release(command)`；对 `target.Color()` 录制 `RenderTarget -> Common (Release)` 屏障。
       9. 对当前槽 `passConstants` 录制 `CBuffer -> HostWrite` 屏障以便下次该槽复用写入；处理可选 Timestamp 写入与 Resolve。
       10. 关闭 `CommandBuffer`（触发或 Dismiss 外层 guard），执行单次 `slot.GetQueue().Submit(...)`（等待 `Consumed` 与 `Available`，信号 `_completion`、`target.Ready()` 与 `textures->Released()`）。
   - **`Wait(radray::FrameSlot& slot) noexcept`**：
     - 等待该槽对应的 `_completion` 值完成，读取可选 Timestamp 样本。
3. **`Renderer` 销毁顺序与生命周期约束**：
   - `TerrainScene` 持久持有材质的 Group 1 `ShaderParameterSet`，而该参数集依赖 `MeshPassResources` 中的 `materialGroupLayout`（由 `SolidFramePipeline` / `FrameScheduler` 持有）。
   - 因此在 `Renderer` 中：
     - **成员声明顺序**：`_scheduler` 必须声明在 `_scene` **之前**（即 `_scheduler` 晚于 `_scene` 析构）；
     - **显式清理顺序**：在 `Renderer::~Renderer()`（或 `Finish` 后销毁时），先调用 `_scheduler->WaitIdle()` 重置所有槽的 `CommandAllocator` 并排空退役资源，再调用 `_scene.Reset()` 清空全部持久材质 `ShaderParameterSet` 与几何 `Buffer`，最后销毁 `_scheduler` 与 `SolidFramePipeline`。

---

### 阶段五：重写 Java Bridge 与客户端接入 (`src/`)

1. **删除旧 UV 诊断类**：
   - 删除 `src/client/java/dev/raynium/client/preview/UvScene.java`。
2. **更新 `src/client/java/dev/raynium/bridge/NativeRenderer.java`**：
   - 移除 `UV`、`TEXTURED`、`PROCEDURAL`、`POSITION_COLOR_UV_LIGHT`、`DIRECT` 常量。
   - 保留 `public static final int RGSS = 1, PROFILE = 2;`。
   - 精简 Record 定义以匹配 ABI 9：
     ```java
     public record Index(long id, int[] values) {}
     public record Submesh(int first, int count, int baseVertex) {}
     public record Mesh(long id, byte[] vertices, long index, int vertexCount, List<Submesh> submeshes) {}
     public record Material(long id, int albedo, int light, int anisotropy) {}
     public record Primitive(long id, long mesh, long material) {}
     public record Instance(int x, int y, int z, float visibility) {}
     public record Visible(long primitive, int submesh, int sortKey, int firstInstance, int instanceCount) {}
     ```
   - 更新 `marshal(Arena a, Frame f)`：
     - `meshes` 数组每项 `40` 字节：校验 `x.vertexCount() * 28L == x.vertices().length`；偏移 `0` 写 `id` (`JAVA_LONG`)，偏移 `8` 写 `vertices` 指针 (`ADDRESS`)，偏移 `16` 写 `index` (`JAVA_LONG`)，偏移 `24` 写 `ranges` 指针 (`ADDRESS`)，偏移 `32` 写 `vertexCount` (`JAVA_INT`)，偏移 `36` 写 `submeshes.size()` (`JAVA_INT`)。
     - `materials` 数组每项 `24` 字节：偏移 `0` 写 `id` (`JAVA_LONG`)，偏移 `8` 写 `albedo` (`JAVA_INT`)，偏移 `12` 写 `light` (`JAVA_INT`)，偏移 `16` 写 `anisotropy` (`JAVA_INT`)，偏移 `20` 写 `0` (`JAVA_INT`)。
     - `visible` 数组每项 `24` 字节：偏移 `0` 写 `primitive` (`JAVA_LONG`)，偏移 `8` 写 `submesh` (`JAVA_INT`)，偏移 `12` 写 `sortKey` (`JAVA_INT`)，偏移 `16` 写 `firstInstance` (`JAVA_INT`)，偏移 `20` 写 `instanceCount` (`JAVA_INT`)。
3. **更新 `src/client/java/dev/raynium/client/terrain/TerrainScene.java`**：
   - `mesh(MeshPayload payload)` 返回精简后的 `new NativeRenderer.Mesh(payload.id, payload.vertices, 1, payload.vertices.length / 28, ranges)`。
4. **更新 `src/client/java/dev/raynium/client/terrain/TerrainPreview.java`**：
   - 移除 `direct` 字段与 `raynium.terrain.directDraw` 系统属性读取。
   - `frame.flags = (snapshot.rgss() ? NativeRenderer.RGSS : 0) | (profile ? NativeRenderer.PROFILE : 0);`
   - **保留原有的按需材质更新判断**（避免每帧重发材质导致全部缓存 `MeshBatch` 标脏重建）：
     ```java
     if (anisotropy != snapshot.anisotropy()) {
         frame.materials = List.of(new NativeRenderer.Material(1, 0, 1, snapshot.anisotropy()));
     }
     ```
   - 日志中的 `draws=...` 更新为固定说明（如 `pipeline=cached-mesh-pass`）。
5. **更新 `src/client/java/dev/raynium/client/terrain/TerrainCapture.java` 与 `PreviewOverlay.java`**：
   - `TerrainCapture.ENABLED` 直接由 `Boolean.parseBoolean(System.getProperty("raynium.preview.enabled", "true"))` 决定，移除 `raynium.preview.content=uv` 分支。
   - `PreviewOverlay` 移除 `UvScene uv` 字段及其实例化分支，仅驱动 `TerrainPreview`。

---

### 阶段六：更新测试集与文档

1. **`src/test/java/dev/raynium/bridge/NativeManifestTest.java`**：
   - 将测试 manifest 中的 `"abiVersion"` 更新为 `9`。
2. **`src/test/java/dev/raynium/bridge/NativeRuntimeTest.java` 与 `NativeRendererTest.java`**：
   - 移除对 `UvScene` 的引用。
   - 新增辅助方法构造一个带 `fogColor`（如 `{0.25f, 0.5f, 0.75f, 1.0f}`，对应像素 `64, 128, 191`）的 `NativeRenderer.Frame`，并用 `consumer.copyAndCheckSolid(image, frame, width, height, new int[]{64, 128, 191})` 验证跨帧交接、尺寸重建、多线程拒绝与消费者未完成时的 owner 保活。
3. **`src/test/java/dev/raynium/bridge/NativeTerrainTest.java`**：
   - 适配 ABI 9 的 `NativeRenderer.Mesh` 与 `NativeRenderer.Material` 构造器（移除 `direct` 参数和 `mixedProgramsShareOneVisibleList` 中的 `PROCEDURAL`/`UV` 项）。
   - 将原 `mixedProgramsShareOneVisibleList` 替换为 **多材质/多 Submesh 缓存与局部更新回归测试**（验证同一 `Mesh` 的多个 `Submesh`、不同 `Material` 绑定、单材质更新只重建对应命令、索引扩容替换自动刷新关联 `MeshBatch`、以及 `RetireResource` 在双槽轮转下的安全释放）。
   - 注意：若启用了 `-PtestBaseline` 图像比对，需同步更新测试或按新测试名称生成对应验证输出。
4. **同步事实文档**：
   - 实现与测试全部通过后，同步更新：
     - `doc/architecture/overview.md`（更新 ABI 9、持久 Scene 与命令缓存描述）
     - `doc/architecture/interop.md`（更新 ABI 9 结构体尺寸与字段表）
     - `doc/architecture/native-runtime.md`（更新持久 `Scene`、Group 0/Group 1 参数绑定、延迟实例流与 `RetireResource` 退役机制，移除 `GeometryPage`、UV 与手写 indirect 说明）
     - `doc/architecture/terrain-preview.md`与 `doc/architecture/minecraft-integration.md`（移除 UV 诊断模式与 `directDraw` 开关说明）
     - `doc/guide/build-test.md`（记录本次重写后的实际构建与测试验证结果）

---

## 4. 验证与交付检查清单

执行 AI 完成代码与文档修改后，必须在仓库根目录依次执行并通过以下全部检查：

```powershell
# 1. 重新编译 Shader 字节码（如有修改 terrain.hlsl）
python tools/compile_preview_shaders.py

# 2. 构建 Java、编译并 stage Debug native DLL，运行全部纯 Java 测试与双后端 GPU 集成测试
.\gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel

# 3. 构建并测试 Release 配置，打包正式 native Mod JAR 并校验包结构与哈希
.\gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel

# 4. 运行文档规范与空白字符检查
python tools/check_docs.py
git diff --check
```

### 通过标准

- Debug（`/MTd`）与 Release（`/MT`）配置下，`rayniumruntime.dll` 均无编译错误、无动态 CRT 依赖、导出符号严格为 13 个 C ABI 入口且 `rn_get_abi_version()` 返回 `9`。
- 全部纯 Java 单元测试与 `@Tag("native")` 双后端（D3D12 / Vulkan）GPU 测试 100% 通过，无跳过、无 RHI 校验错误、无 Vulkan VUID 报错。
- `python tools/check_docs.py` 与 `git diff --check` 零错误通过，且文档中不包含用户未要求保存的 Git commit hash。
