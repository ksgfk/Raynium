> - 适用: 执行或评审 Raynium 第三阶段（统一 MDI 场景管线、OIT 三阶段共享间接绘制流、切断原版地形 StagingBuffer 双拷与重复扫描、主世界刚体模型常驻池与紧凑粒子实例流快轨）的细化实施计划
> - 权威: 经用户确认保存的阶段三实施计划，不作为当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)、[地形渲染](../architecture/terrain-rendering.md)、[Minecraft 渲染管线](../architecture/minecraft-rendering.md) 与 [Native runtime](../architecture/native-runtime.md)
> - 锚点: `native/radray.lock.json`, `native/CMakeLists.txt`, `native/include/raynium/bridge.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_feature.cpp`, `native/src/terrain_draw.cpp`, `native/src/mesh_pass.cpp`, `native/src/immediate_pass.cpp`, `src/client/java/dev/raynium/client/terrain/TerrainCapture.java`, `src/client/java/dev/raynium/client/mixin/LevelTerrainHookMixin.java`, `src/client/java/dev/raynium/client/mixin/SectionUploadMixin.java`, `src/client/java/dev/raynium/client/mixin/UberBufferDrainMixin.java`, `tools/convert_vanilla_shaders.py`, `tools/validation/pipeline_coverage.py`

# Raynium 第三阶段：统一 MDI 场景管线与常驻世界（地形、刚体实例与紧凑粒子流）实施计划

- 原记录日期：2026-11-21
- 状态：经上游 `RadRay` 基线核查、阶段二基准采样归因与 Minecraft 26.3 渲染源码审计后定稿的阶段三实施计划；归档时未执行源码改动、构建或运行验证
- 范围：将 `TerrainResidency` 与 `TerrainFeature` 全面收编进 `RadRay::Scene` + `MeshPassProcessor` + `MeshDrawCommand` MDI（OIT 三阶段共享单份 `PreparedMeshPassIndirectDraws` 与单块 RDG 间接参数缓冲，清退 `frame_record.*` 与私有 `terrain_draw` 状态排序表）、切断 Java 侧 `SectionRenderDispatcher` 往原版 `98 MiB` 持久映射 `StagingBuffer` 的无用写入与 `LevelRenderer.prepareChunkRenders[Indirect]` 二次加锁扫描、落地严格限定于 `LevelRenderer` 主世界 `featureFrame` 的 `ModelFeatureRenderer` / `ModelPart` 刚体网格常驻池 + 位级精确法线基向量实例流快轨、落地不入静态图元树的 `QuadParticleFeatureRenderer` 56 B/粒子紧凑四元数实例流快轨，以及在 Headless 双后端单测通过后冻结 `RN_ABI_VERSION 20u`
- 来源：承接 [RadRay 与 Raynium 五阶段协同演进总体计划](2026-11-19-radray-raynium-coevolution-master-plan.md)、[Raynium 第二阶段实施计划](2026-11-20-phase2-command-tape-renderpearl-plan.md) §4 跨阶段约束与 [阶段二控制面、命令流与双后端验收记录](2026-11-21-phase2-command-tape-implementation-notes.md)，结合 2026-11-21 对锁定 `RadRay` 源码（版本见 `native/radray.lock.json`）与 Minecraft 26.3（Mojang 命名，源码生成入口见 [开发环境](../guide/dev-environment.md#阅读-minecraft-源码)）的核查结论整理

## 0. 上游基线确认、推进顺序与阶段准出门禁

### 0.1 上游 `RadRay` 基线盘点（已在当前锁定的 `native/radray.lock.json` 中就绪）

对照 `RadRay` 仓库 `docs/history/2026-11-21-runtime-phase2-optimization-and-phase3-mdi-plan.md` 与 `radray/runtime/mesh_draw_command.h`：

1. **阶段二反馈优化 `O1`～`O5` 已就位**：
   - `O1`：RDG 回调与节点名 PMR 化（64 B SBO `Generated`）；
   - `O2`：`TransientParameterSetPool` 开放寻址平坦去重表；
   - `O3`：`DefaultViewRequest` 全 mip 视图推导与 `RDGTransientResourcePool` 8 帧滑窗回收；
   - `O4`：`RDGAttachmentSlice` 显式 slice/mip 与 `BufferUse::Indirect` 4 B 对齐；
   - `O5`：`ShaderPushConstantOverride`（`PromoteDxilCBufferToRootConstants = true`、`AllowDxilOnly = true`）。
2. **阶段三 MDI 与网格管线基础设施 `M1`～`M3` 已就位**：
   - `DeviceContext::GetPipelineCache()` 已暴露；
   - `MeshPassFrameDrawList::SortVisibleCommands()` 在 `_dirtyCount == 0` 稳态帧已具备 0-sort 短路（直接按桶顺序线性分派或逆序遍历）；
   - `MeshBatchElement::IndexBuffer == nullptr` 非索引 `Draw` 与 `MaterialParameterValue` 值类型参数已打通；
   - `CountMeshPassIndirectDraws` / `BuildMeshPassIndirectDraws` / `SubmitMeshPassIndirectDraws`（自动按 `(PSO, ParameterSet, VertexBuffers, IndexBuffer, IndexFormat)` 切分 `PreparedMeshPassIndirectBatch` 并合并 `DrawIndexedIndirectCommand` / `DrawIndirectCommand`）已就绪；
   - 带 GPU Count Buffer 的 `Draw[Indexed]IndirectCount` 已明确收敛至**阶段四（GPU Compute Culling）**；阶段三由 CPU 侧 MDI Batcher 直接消费精确 `ArgumentCount` 的 `Draw[Indexed]Indirect`。

### 0.2 推进顺序与阶段准出门禁

1. **第一步（Native 地形 MDI 收编与旧私有录制栈清理）**：将 `TerrainResidency` 与 `TerrainFeature` 全面接入 `radray::Scene` + `MeshPassProcessor` + `MeshDrawCommand` MDI，实现 OIT 三阶段（`OitBounds` / `OitTransmittance` / `OitAccumulate`）共享同一份 MDI 参数流与 GPU 间接参数缓冲区，移除 `native/src/terrain_draw.h` 中的私有状态表 / 排序录制栈与 `native/src/frame_record.h`、`native/src/frame_record.cpp`。
2. **第二步（Java 地形双拷热路径切断 + 常驻刚体模型与紧凑粒子实例流快轨）**：
   - 切断 Java 侧 `SectionRenderDispatcher` 往原版 `98 MiB` 持久映射 `StagingBuffer` 的无用拷贝与 `LevelRenderer.prepareChunkRenders[Indirect]` 重复加锁扫描；
   - 落地主世界 `ModelFeatureRenderer` / `ModelPart` 刚体网格常驻池 + 实例变换流快轨；
   - 落地 `QuadParticleFeatureRenderer` / `QuadParticleRenderState` 紧凑粒子实例流快轨（不入静态图元树）。
3. **第三步（Headless 双后端单测 → ABI 20 定版 → 全量视觉与性能验收）**：
   - 先通过 C++ GoogleTest（Vulkan + D3D12 双后端）与 Java JUnit 测试后，一次性冻结 `RN_ABI_VERSION 20u`（`native/include/raynium/bridge.h` 与 `src/client/java/dev/raynium/bridge/NativeBridge.java`）；
   - 执行 `tools/validate_visual.py` 86 场景视觉回归（`mismatch == 0` 且 `pipeline_coverage == 86/86`）与 `tools/benchmark.py`（Metcalf + Tideworn + 高密实体/粒子对比），最后同步当前架构文档。

---

## 1. 核心架构定稿（五项闭合契约）

### 1.1 契约 1：地形全面收编进 `RadRay::Scene` + `MeshDrawCommand` MDI，且 OIT 三阶段共享单份 MDI 参数流

1. **世界几何池 `BufferUse::Resource` 与池级 RDG 读依赖**：
   - 在 `native/src/renderer.cpp` 创建世界 `VertexBufferPool` 与 `IndexBufferPool` 时，开启 `.AdditionalUsage = radray::gpu::BufferUse::Resource`（满足总纲阶段三对世界几何池 `ShaderRead` 的前置要求）；
   - 在 `native/src/terrain_feature.cpp` 的 `DeclarePassReads` 中直接调用 `_vertices->AddPassReadAccess(graph, pass)` 与 `_indices->AddPassReadAccess(graph, pass)`，彻底删除每 Pass 构造 `radray::unordered_set<radray::gpu::GpuBuffer*>` 遍历 Batch 声明依赖的开销。
2. **清理私有地形录制栈与历史残留**：
   - 在 `native/src/terrain_residency.cpp` 中删除阶段一遗留但从未被 `TerrainFeature` 消费的 `state.material`（`CreateMaterialBinding` / `MeshMaterialBinding`）以及私有 `TerrainBindingStates` / `TerrainDrawTemplate`；
   - 删除 `native/src/frame_record.h` 与 `native/src/frame_record.cpp`；`native/src/terrain_draw.h` / `native/src/terrain_draw.cpp` 收缩为纯粹的体素面片方向分析与 6 轴保守可见性裁剪工具（保留 `AnalyzeTerrainMesh`、`ComputeTerrainEye`、`TerrainVisibleMask`、`WriteQuadIndices`、`WriteWireframeIndices`，删除 `TerrainDrawBatch`、`TerrainFrameLists`、`BuildTerrainDraws`、`TerrainIndirectChunk`）。
3. **`RadRay::Scene` Pass 拓扑与稳态零重排缓存**：
   - 在 `TerrainResidency` 内持有一套 `radray::Scene` 与紧凑 `MeshPassId` 处理器列表：
     - `Pass 0: Solid`（`StateFirst`，接受 `RN_MATERIAL_SOLID`）
     - `Pass 1: Cutout`（`StateFirst`，接受 `RN_MATERIAL_CUTOUT`）
     - `Pass 2: Translucent`（`DepthFirst`，接受 `RN_MATERIAL_TRANSLUCENT`，用于非 OIT 回退路径按 chunk 中心深度由远及近排序）
     - `Pass 3: OitBounds`（`StateFirst`，接受 `RN_MATERIAL_TRANSLUCENT`）
     - `Pass 4: OitTransmittance`（`StateFirst`，接受 `RN_MATERIAL_TRANSLUCENT`）
     - `Pass 5: OitAccumulate`（`StateFirst`，接受 `RN_MATERIAL_TRANSLUCENT`）
     - `Pass 6: Wireframe`（`StateFirst`，当硬件支持 `PolygonMode::Line` 时注册，绑定各 section 的 `wireframeIndices` 与 wireframe PSO）
   - 每个常驻 Section 在 `UpsertSection` 时注册/更新 `Scene` 中的 `PrimitiveHandle` + `MeshBatchHandle`（携带 6 轴 + 通用 `faceCounts[7]` 子范围作为 7 个 `MeshBatchElement`，其 `VertexOffset` = 池内顶点起点，`FirstIndex` = 子面组在 `IndexBufferSlice` 内的索引起点，使 `DrawIndexedIndirectCommand` 直接烘焙 `firstIndex` 与 `vertexOffset`）；`RemoveSection` 时从 `Scene` 移除；
   - 每帧 `Prepare` 调用 `_scene.SyncCachedMeshDrawCommands(passes)`：当本帧无区块增删改（`_dirtyCount == 0`）时，命令缓存与桶边界完全不变，`SortVisibleCommands()` 直接走 $O(N)$ 稳态无 `std::sort` 短路。
4. **6 轴面组可见性过滤与 OIT 三阶段单份 MDI 流复用**：
   - 通过 `GatherVisibleMeshDrawCommands(scene, passId, filter, drawList)` 注入逐 Section 的 `ComputeTerrainEye` + `TerrainVisibleMask` 过滤回调（使用 1-pass 预计算每个可见 `PrimitiveHandle` 的 7-bit `visibleMask` 与半透明深度排序键 `viewDepthKey`）；
   - **OIT 三阶段零重复构建与单份间接缓冲区共享**：
     - 因为 `OitBounds (3)`、`OitTransmittance (4)`、`OitAccumulate (5)` 同步接纳相同的 `RN_MATERIAL_TRANSLUCENT` Section 与相同的 7 个 `ElementIndex`，它们在 `Scene::GetPassCache(pass)->GetCommands()` 中的稠密下标 $k \in [0, M)$ 一一对应，且 `geometryBucket`（`(VertexBuffer[0], IndexBuffer, IndexFormat)`）排序顺序完全一致；
     - 当启用 OIT 时，每个视图**仅对 `OitBounds (3)` 执行一次** `GatherVisibleMeshDrawCommands` → `SortVisibleCommands` → `CountMeshPassIndirectDraws` → 分配单块 RDG Indirect Buffer（`sizeof(DrawIndexedIndirectCommand) * totalCommands`）→ `BuildMeshPassIndirectDraws` → `QueueBufferUpload`；
     - `OitTransmittance (4)` 与 `OitAccumulate (5)` 直接复用 `OitBounds` 已生成的 `PreparedMeshPassIndirectDraws` 与同一块 RDG `IndirectArgs` 切片，仅将 `Batches` 中仅有的 $B$ 个批次代表命令（通常 $B \le 4$）通过指针偏移 `k = batch.Command - boundsCache.data()` 映射为 `transmittanceCache[k]` / `accumulateCache[k]` 后调用 `SubmitMeshPassIndirectDraws`。

### 1.2 契约 2：Java 侧切断原版 `SectionRenderDispatcher` 98 MiB `StagingBuffer` 双拷与 `prepareChunkRenders` 重复扫描

阶段二基准采样暴露了两处原版地形准备路径在 `TerrainCapture.active()` 下仍在空转的热点，阶段三予以根治：

1. **阻断 `SectionRenderDispatcher` → `UberGpuBuffer` 98 MiB `StagingBuffer` 无用写入与每帧脏区 flush**：
   - Minecraft 26.3 `SectionRenderDispatcher.RenderSection.addSectionBuffersToUberBuffer` 会调用 `UberGpuBuffer.addAllocation`，将每个编译完成的区块顶点/索引 `ByteBuffer` 拷贝进 `SectionRenderDispatcher` 持有的 `98 MiB` 持久映射 `StagingBuffer`（`USAGE_MAP_WRITE | USAGE_COPY_SRC`），随后 `src/client/java/dev/raynium/client/mixin/UberBufferDrainMixin.java` 在 `uploadStagingBuffers` 中仅清空队列而不真正上传，且该 `98 MiB` 持久映射 `MappedView` 未关闭导致每帧 `rn_submit_frame` 都在 `dirty_mapped_ranges[]` 中刷写它；
   - **收敛方案**：在 `src/client/java/dev/raynium/client/mixin/SectionUploadMixin.java` 与 `src/client/java/dev/raynium/client/mixin/UberBufferDrainMixin.java` 中，当 `TerrainCapture.active()` 为真时，拦截 `RenderSection.addSectionBuffersToUberBuffer`（或 `UberGpuBuffer.addAllocation`），不再向 `StagingBuffer` 拷贝字节，仅登记 `onUpload` 回调并在渲染线程 `uploadTerrainBuffersToGpu` 时直接触发 `onUpload.run()`（更新 `RenderSection` 内部状态机），同时确保 `StagingBuffer` 不产生持久映射脏区。
2. **短路 `LevelRenderer.prepareChunkRenders` / `prepareChunkRendersIndirect` 的原版分组与 UBO 写入**：
   - 在 `src/client/java/dev/raynium/client/mixin/LevelTerrainHookMixin.java` 中，将 `@Inject(at = @At("RETURN"))` 改为 `@Inject(at = @At("HEAD"), cancellable = true)`：当 `TerrainCapture.active()` 为真时，直接调用 `TerrainCapture.capture(...)` 并构造仅含空分组壳的 `ChunkSectionsToRender`（供后续 `renderChunkLayer` 钩子携带 `chunkLayerGroup` 进入 `TerrainAnchorMixin`）后 `cir.setReturnValue(...)` 提前返回，彻底省去原版 `extractSectionDrawGroups` 对 `sectionRenderDispatcher` 的二次加锁、数千个可见 Section × 3 图层的 Map/List 分配以及 `DynamicUniforms.writeChunkSections/Commands` 的无效 UBO 写入。

### 1.3 契约 3：常驻刚体模型快轨（`ModelFeatureRenderer` / `ModelPart` 常驻网格 + 实例变换流）

针对世界内的实体（生物、玩家、盔甲架、矿车、船等）与方块实体（箱子、告示牌、床、潜影盒、信标、旗帜、头颅等）通过 `SubmitNodeCollection.submitModel` / `submitModelPart` 进入 `ModelFeatureRenderer` 的刚体部件，建立常驻网格与紧凑实例流快轨：

1. **严格的作用域门禁（仅限 `LevelRenderer` 主世界 `featureFrame`）**：
   - Minecraft 26.3 的 `GuiItemAtlas`（GUI 物品图标烘焙）、`PictureInPictureRenderer`（物品栏 3D 小人/挂画预览）以及 `GameRenderer.renderItemInHand`（第一人称手持）同样会复用 `FeatureRenderDispatcher`；
   - 快轨通过显式作用域守卫 `WorldModelCaptureScope` **仅在 `LevelRenderer.render` 主世界 `featureFrame` 渲染期间开启**（并在 `renderItemInHand` 前关闭）；所有 GUI Atlas、PiP 与第一人称手部渲染保持 100% 原版 `StagedVertexBuffer` + `CommandTape` 路径。
2. **刚体 `ModelPart` 网格一次性烘焙与常驻池管理**：
   - 在 Minecraft 26.3 中，`ModelPart.cubes`（`List<ModelPart.Cube>`，每个 `Cube` 含 6 个 `Polygon`，每 `Polygon` 含 4 个不可变 `Vertex(x, y, z, u, v)` 与面法线 `Vector3fc normal`）在 `EntityModelSet` 构建后完全不可变；
   - 对非空 `ModelPart`（`!cubes.isEmpty()`）：首次命中快轨时，将其所有 `Polygon` 的 4 个局部顶点提取为 **24 B/顶点** 的紧凑局部格式：
     - `float3 localPos`（已除以 `16.0f`，与原版 `ModelPart.Cube.compile` 一致：0..12 bytes）
     - `float2 uv0`（12..20 bytes）
     - `snorm8x4 localNormal`（20..24 bytes，将 `polygon.normal` 按原版 `BufferBuilder.normalIntValue` 量化为 3 个有符号字节 + 1 字节 `faceAxisIndex`：`0 = ±X, 1 = ±Y, 2 = ±Z`）
   - 通过 C ABI `rn_upsert_model_mesh` 上传至 Native 世界 `VertexBufferPool`（共享 `TerrainResidency` 的 `quadIndices` 6-index/quad 索引切片），返回 `mesh_id`；
   - **生命周期与资源重载**：`ModelPart` 关联的 `mesh_id` 绑定当前 `rendererEpoch`；当 `EntityModelSet` 重载或 `RayniumRenderer` 重建时，旧 `mesh_id` 自动失效并集中释放。
3. **位级精确（Bit-exact）的姿态与法线变换契约**：
   - 原版 `ModelPart.Cube.compile` 在 Java CPU 上先计算 `Vector3f normal = pose.transformNormal(polygon.normal, new Vector3f())`，再对 `normal.x/y/z` 调用 `BufferBuilder.normalIntValue`（`clamp(c, -1, 1) * 127.0f` 向零取整为 `int8`）写入 `SNORM8X4` 顶点属性；
   - 由于 `ModelPart.Polygon` 的 `polygon.normal` 恒为 6 个轴向单位向量 `(±1,0,0), (0,±1,0), (0,0,±1)`，只需在每个 `ModelPart.compile(PoseStack.Pose pose, ...)` 提交实例时，由 Java 侧对 `pose.normal()` 的三列基向量分别预计算一次 `normalIntValue`（打包为 3 个 `int32` / `snorm8x4`：`normalBasisX`, `normalBasisY`, `normalBasisZ`），连同 `pose.pose()` 的 `4x3` 仿射行（`12 × float`）、`int color (ABGR)`、`int overlayCoords (UV1)`、`int lightCoords (UV2)` 组成 **68 B（对齐至 80 B / `5 × float4`）紧凑实例记录**：
     - 顶点着色器根据顶点局部 `localNormal` 的轴向与符号直接选取对应的 `±normalBasis{X,Y,Z}`，**不仅完全省去 GPU 侧 $3\times 3$ 矩阵乘法与归一化，而且与原版 Java CPU 逐顶点 `normalIntValue` 量化结果实现 100% 比特级一致**，从根本上杜绝 `minecraft_mix_light` 在临界角度下的 1 LSB 舍入抖动。
4. **准入条件与无缝回退（Fallback）**：
   - 仅当当前 `PreparedRenderType` 与 `VertexConsumer` 满足以下条件时走刚体实例快轨：
     1. 处于 `WorldModelCaptureScope` 主世界上下文；
     2. `sheetedDecalPose == null` 且 `uvMapping == null`（排除破损贴花与自定义 Sprite 包装器 `SpriteCoordinateExpander`）；
     3. `format == DefaultVertexFormat.ENTITY` 且 `topology == PrimitiveTopology.QUADS`；
     4. `!renderType.sortOnUpload() || minecraft.options.improvedTransparency().get()`（在非 OIT 经典透明模式下，需要 CPU 侧按面片质心 `QuadSorting` 的半透明图层保持走原版 CPU 排序路径，绝不破坏半透明面片顺序；而在 OIT 模式或非排序图层下全量走实例快轨）；
     5. 当前 `RenderPipeline` 使用内置 `core/entity` 着色器变体（未被外部资源包覆盖为不兼容的自定义顶点着色器）。
   - **混合路径顺序保真**：若某个自定义 `Model` 混写了直接向 `VertexConsumer.addVertex` 吐点的代码，或中途切换到不满足快轨条件的 `RenderType`，则在发射实例批次前先按序刷出 `BufferBuilder` 中已累积的即时顶点（或将实例批次作为内联 `CommandTape` 命令按确切提交顺序锚定在当前 `RenderPass` 中），保证与同阶段其它 `FeatureRenderer` 的绘制顺序与状态绑定（`DynamicTransforms`、`Lighting`、`Fog`、`Globals`、`Sampler0/1/2`、`GlintSampler`、`DissolveMaskSampler`、OIT 绑定及 `VisualValidation.pipeline(...)` 计数）完全一致。

### 1.4 契约 4：批量粒子紧凑实例流快轨（`QuadParticleFeatureRenderer`，不入静态图元树）

严格遵循 [Raynium 第二阶段实施计划](2026-11-20-phase2-command-tape-renderpearl-plan.md) §4 跨阶段约束（**“粒子保持合批流/紧凑实例流而不入静态图元树”**）：

1. **消除 Java CPU 四元数逐顶点旋转与 `StagedVertexBuffer` 双重拷贝**：
   - 在 Minecraft 26.3 中，`QuadParticleRenderState.Storage` 已经按 `SingleQuadParticle.Layer` 将每颗粒子存储在平坦数组中：
     - `floatValues`（每粒子 12 个 `float` = 48 B：`x, y, z, xRot, yRot, zRot, wRot, scale, u0, u1, v0, v1`）
     - `intValues`（每粒子 2 个 `int` = 8 B：`color (ARGB), lightCoords`）
   - 原版 `QuadParticleRenderState.renderSection` 在渲染线程遍历每颗粒子，在 CPU 上用 `Quaternionf` 旋转 4 个角点 `(1,-1,0), (1,1,0), (-1,1,0), (-1,-1,0)`，写入 `BufferBuilder`（112 B/粒子），再拷贝到 `StagedVertexBuffer`（又是 112 B/粒子，且 `quadSorting == null` 从不触发 CPU 排序）。
2. **56 B/粒子紧凑实例流 + GPU 顶点着色器角点展开**：
   - 在 `QuadParticleFeatureRenderer` 中（当内置 `core/particle` 着色器未被资源包替换时）：直接将每个 `Layer` 的 `Storage.floatValues`（`48 B × count`）与 `Storage.intValues`（`8 B × count`）按粒子写入当前帧 `NativeFrameBuffer` 的紧凑粒子实例流（**56 B/粒子，较原版 112 B × 2 次 CPU 拷贝缩减 75% 内存带宽且零 CPU 四元数运算**）；
   - Native 侧常驻一个 4 顶点单位 Quad（`cornerIndex 0..3` 对应 `(1,-1), (1,1), (-1,1), (-1,-1)`）与共享 `quadIndices`，在当前 `RenderPass` 内通过内联命令触发 `DrawIndexed(6, particleCount, ...)`，由粒子实例顶点着色器完成四元数旋转、缩放、UV 选取与 `ARGB -> RGBA` 颜色解包，同时完整保留 `Sampler0`（粒子图集）、`Sampler2`（光照贴图）、`DynamicTransforms`、`Fog` 以及 OIT 三阶段（`OIT_BOUNDS` / `OIT_TRANSMITTANCE` / `OIT_ACCUMULATE`）的全部管线变体与 `VisualValidation.pipeline(...)` 计数。

### 1.5 契约 5：C ABI（`RN_ABI_VERSION 20u`）与 `CommandTape` 内联协同契约

1. **C ABI 扩展（`native/include/raynium/bridge.h` 与 `src/client/java/dev/raynium/bridge/NativeBridge.java`）**：
   - 新增刚体模型网格常驻接口：
     - `rn_upsert_model_mesh(int32_t mesh_id, const void* vertex_data, uint32_t vertex_bytes, uint32_t vertex_count, uint32_t index_count)`
     - `rn_remove_model_mesh(int32_t mesh_id)`
   - 在 `rn_batch_desc_t`（或帧级流描述）与 `CommandTape` 内联命令中补充常驻模型实例与粒子实例流描述符，供 `CommandTape` 在 `ImmediatePass` 执行期间按确切录制顺序回放。
2. **`ImmediatePass` 状态脏化与恢复契约**：
   - 与 `CMD_DRAW_RETAINED_TERRAIN` 一致，当 `ImmediatePass` 执行内联常驻地形 MDI、常驻刚体模型实例或紧凑粒子实例流命令后，必须维持或标记 `Viewport`、`Scissor`、`PSO`、Descriptor/Root Signature、`VB`/`IB` 与 Push Constants 脏位，确保同一 `RenderPass` 内紧随其后的任何 `ImmediateDraw`（如自定义 `RenderType`、调试线框、光束等）完整重绑自身状态。

---

## 2. 分步实施清单（R3.1 ～ R3.5）

### R3.1 Native 地形 `RadRay::Scene` + `MeshDrawCommand` MDI 收编与旧栈清理

- **修改 `native/src/renderer.cpp`**：
  - 世界 `VertexBufferPool` 与 `IndexBufferPool` 开启 `.AdditionalUsage = radray::gpu::BufferUse::Resource`。
- **重构 `native/src/terrain_residency.h` / `native/src/terrain_residency.cpp`**：
  - 移除死代码 `CreateMaterialBinding` / `state.material` 与私有 `TerrainBindingStates` / `TerrainDrawTemplate`；
  - 集成 `radray::Scene` 与 7 个地形 `MeshPassId`（`Solid`, `Cutout`, `Translucent`, `OitBounds`, `OitTransmittance`, `OitAccumulate`, `Wireframe`）的自定义 `MeshPassProcessor` 实现；
  - `UpsertSection` / `RemoveSection` / `Clear` 直接维护 `Scene` 中的 `PrimitiveHandle` 与 `MeshBatchHandle`（含 7 个面组 `MeshBatchElement`），每帧 `SyncCachedMeshDrawCommands` 驱动增量缓存同步。
- **重构 `native/src/terrain_feature.cpp` 与 `native/src/mesh_pass.h` / `native/src/mesh_pass.cpp`**：
  - 用 `_vertices->AddPassReadAccess` / `_indices->AddPassReadAccess` 替换逐 Batch 集合扫描；
  - 用 `GatherVisibleMeshDrawCommands` + `SortVisibleCommands` + `CountMeshPassIndirectDraws` + `BuildMeshPassIndirectDraws` + `SubmitMeshPassIndirectDraws` 替换 `TerrainFrameLists` + `BuildTerrainDraws` + `RecordTerrainDraws`；
  - 实现 OIT 三阶段（`OitBounds` / `OitTransmittance` / `OitAccumulate`）共享单份 `PreparedMeshPassIndirectDraws` 与单块 RDG `IndirectArgs` 切片。
- **删除 `native/src/frame_record.h` / `native/src/frame_record.cpp`，精简 `native/src/terrain_draw.h` / `native/src/terrain_draw.cpp` 并更新 `native/CMakeLists.txt`**。

### R3.2 Java 侧地形 `StagingBuffer` 98 MiB 双拷切断与 `prepareChunkRenders` 短路

- **修改 `src/client/java/dev/raynium/client/mixin/SectionUploadMixin.java` 与 `src/client/java/dev/raynium/client/mixin/UberBufferDrainMixin.java`**：
  - 在 `TerrainCapture.active()` 时跳过向 `SectionRenderDispatcher.StagingBuffer`（`98 MiB`）写入顶点/索引拷贝，仅排队并在 `uploadTerrainBuffersToGpu` 中排空 `onUpload` 回调，关闭或避免打开 `StagingBuffer` 的持久映射脏视图。
- **修改 `src/client/java/dev/raynium/client/mixin/LevelTerrainHookMixin.java` 与 `src/client/java/dev/raynium/client/terrain/TerrainCapture.java`**：
  - 在 `prepareChunkRenders` 与 `prepareChunkRendersIndirect` 的 `@At("HEAD")` 短路原版 `extractSectionDrawGroups` 与 `DynamicUniforms` 写入，直接由 `TerrainCapture.capture` 产出轻量占位 `ChunkSectionsToRender`。

### R3.3 常驻刚体模型快轨（`ModelFeatureRenderer` / `ModelPart`）

- **新增局部网格提取与常驻管理（`src/client/java/dev/raynium/client/world/RetainedModelManager.java`、`WorldModelCaptureScope.java`）与 Mixin（`ModelPartCompileMixin.java`、`LevelWorldFeatureScopeMixin.java`）**：
  - 在 `LevelRenderer.render` 主世界 `featureFrame` 作用域内启用 `WorldModelCaptureScope`（排除 `GuiItemAtlas`、`PictureInPictureRenderer`、第一人称手持）；
  - 拦截满足准入条件的 `ModelPart.compile`，惰性提取 `ModelPart.cubes` 的 24 B 局部顶点通过 `rn_upsert_model_mesh` 常驻至 Native 世界 `VertexBufferPool`，并在每帧发射 80 B 紧凑实例（`4x3 pose` + 3 个预量化 `snorm8x4 normalBasis` + `color` + `overlay` + `light`）。
- **扩展着色器与 Native 管线（`tools/convert_vanilla_shaders.py`、`native/src/render_program.h` / `native/src/render_program.cpp`、`native/src/immediate_pass.cpp`）**：
  - 为内置 `core/entity` 管线族提供刚体实例顶点输入变体（Slot 0 `PerVertex` 24 B 局部网格 + Slot 1 `PerInstance` 80 B 实例变换，复用完全一致的 Fragment Shader 与 OIT/Glint/Dissolve 变体），在 `CommandTape` 中按录制顺序执行 `DrawIndexed(indexCount, instanceCount, ...)`。

### R3.4 批量粒子紧凑实例流快轨（`QuadParticleFeatureRenderer`）

- **新增 `QuadParticleStorageMixin` / `ParticleFastTrack` 与 `CommandTape` 内联粒子批次命令**：
  - 在 `QuadParticleFeatureRenderer` 渲染内置 `core/particle` 图层时，直接将 `QuadParticleRenderState.Storage` 的 `floatValues`（12 `float`）与 `intValues`（2 `int`）拷贝进帧级 56 B/粒子实例流，跳过 CPU 四元数旋转、`BufferBuilder` 与 `StagedVertexBuffer`。
- **扩展内置 `core/particle` 实例顶点着色器变体与 Native 单位 Quad 绑定**：
  - 在 Native 侧绑定常驻 4 顶点单位 Quad 与 56 B `PerInstance` 输入，在 GPU 顶点着色器中展开四元数旋转角点并透传 `uv`、`ARGB->RGBA color`、`lightCoords`。

### R3.5 C ABI 20 定版、测试覆盖、视觉/性能验收与文档同步

- **更新 `tools/validation/pipeline_coverage.py`**：
  - 将 `TERRAIN_NATIVE` 规则中的旧文件引用（`native/src/frame_record.cpp`）同步更新为新的 `Scene` / `MeshPass` MDI 执行文件列表，并覆盖刚体模型与粒子快轨路径校验。
- **补齐 C++ 与 Java 单元测试**：
  - 在 `native/tests/terrain_draw_test.cpp` 与 `native/tests/bridge_abi_test.cpp` 中覆盖：
    1. `TerrainResidency` + `RadRay::Scene` 稳态帧 `_dirtyCount == 0` 缓存复用与 6 轴可见性过滤；
    2. OIT 三阶段（`OitBounds` / `OitTransmittance` / `OitAccumulate`）单份 `PreparedMeshPassIndirectDraws` 与 `RDGBufferSlice` 共享正确性；
    3. Vulkan + D3D12 双后端下常驻地形 MDI、常驻刚体模型实例与紧凑粒子实例流在 `CommandTape` 混合回放中的状态隔离与渲染一致性；
  - 在 Java JUnit 测试中覆盖 `ModelPart` 24 B 局部网格提取、轴向法线基向量预量化位级等价性（对比原版 `pose.transformNormal` + `normalIntValue`）以及 56 B 粒子实例打包。

---

## 3. 验证清单与准出门禁

| 检查项 | 准出标准 |
| --- | --- |
| **1. 构建与静态检查** | `cmake --build native/build --config Release` 与 `./gradlew testClasses` 零错误。 |
| **2. Headless 双后端单测** | `ctest --test-dir native/build --output-on-failure -C Release`（Vulkan + D3D12）与 `./gradlew test` 100% 通过。 |
| **3. 86 场景全量视觉回归** | `python tools/validate_visual.py` 全部 86 场景 `mismatch == 0`（含实体、方块实体、半透明 OIT、盔甲纹饰、附魔光效、破损贴花、粒子、GUI 物品栏与第一人称手部），且 `pipeline_coverage` 86/86 全绿。 |
| **4. 基准采样与性能验收** | `python tools/benchmark.py`（Metcalf 9 视点 + Tideworn 4 视点）确认：Java 侧 `98 MiB` 地形 `StagingBuffer` 每帧脏区 flush 归零；稳态帧地形 `Scene` 命令缓存重排归零，OIT 三阶段 MDI 构建与间接缓冲上传收敛为每视图 1 次；主世界刚体实体/方块实体与四元数粒子不再经过 CPU 逐顶点变换与 `StagedVertexBuffer` 拷贝。 |
| **5. 事实文档与历史归档** | 按 [文档维护](../guide/documentation.md) 同步 [架构地图](../architecture/overview.md)、[地形渲染](../architecture/terrain-rendering.md)、[Minecraft 渲染管线](../architecture/minecraft-rendering.md)、[Native runtime](../architecture/native-runtime.md)、[互操作与加载](../architecture/interop.md)，并更新 [历史索引](README.md)。 |
