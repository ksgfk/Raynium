> - 适用: 修改三层地形采集、native 光栅、上传纹理及其生命周期
> - 权威: 本文记录地形采集、绘制与上传协议；设备与交换链见 native-runtime，通用加载见 interop
> - 锚点: `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/terrain_feature.cpp`, `native/src/mesh_pass.cpp`, `native/shaders/terrain.hlsl`, `src/client/java/dev/raynium/client/terrain/*.java`, `native/include/raynium/bridge.h`

# 区块地形渲染

原版渲染图决定区块地形的附件、阶段和绘制顺序。Raynium 保留 SOLID、CUTOUT、TRANSLUCENT 的区块几何，复用同一套模型、排序和重载输入。

## 采集与采用

`SectionCompilerMixin` 在 MC `MeshData` 释放前构造 `MeshPayload`：在工作线程上对 SOLID/CUTOUT 按精确轴向平面与双三角形叉积完成六轴向加 general 分组重排、AABB 计算及多面组 3 位原始 quad 组号打包，并将顶点直接写入由 `DirectUploadPool`（4 KiB–8 MiB 无锁 2 的幂次桶，上限 64 MiB）复用的原生字节序 `DirectByteBuffer`；TRANSLUCENT 保持原始 quad 顺序，仅计算包围盒并块拷贝进池化 `DirectByteBuffer`。
`SectionPayload` 表示同一 compiled section 的三层可选 `MeshPayload`；只采集 MC 已采用且顶点/索引就绪的层。
`RenderSectionIndicesMixin` 将透明 uint16/uint32 索引直接归一为 uint32 并写入 `DirectUploadPool` 提供的 `DirectByteBuffer`，在对应原版上传回调确认采用后发布快照，同时把受影响的 `SectionPayload` 登记到脏索引队列。
透明索引校验使用固定 `vertexCount`，上传后释放并归还顶点缓冲不影响后续排序更新。

在非参考模式下，`SectionUploadMixin` 使用 `NoopTerrainStagingBuffer` 拦截 `SectionRenderDispatcher` 往原版 98 MiB `StagingBuffer` 的顶点与透明索引写入，`UberBufferDrainMixin` 跳过冗余的原版 UberBuffer GPU 排出，同时保留原版已编译状态机与透明排序流转。
`LevelTerrainHookMixin` 在 `LevelRenderer.prepareChunkRenders` 与 `prepareChunkRendersIndirect` 入口完成 Raynium 视图准备后直接返回仅携带视图句柄的占位 `ChunkSectionsToRender`，避免原版二次加锁遍历可见区块并构造未消费的绘制列表。
`CompiledSectionMeshMixin` 维护层就绪位掩码，`RenderSectionStateMixin` 在 `setSectionMesh` 与 `reset` 时将 `SectionPayload` 与就绪位掩码直接缓存到 `RenderSection` 并将变更区块加入并发脏队列；`LevelTerrainHookMixin`（`clearVisibleSections`）与 `SectionOcclusionGraphMixin`（`addSectionsInFrustum`）推进 `VISIBLE_LIST_VERSION`。

`TerrainCapture` 复用扁平数组并跨连续活跃帧保留热快照；当视图、可见列表和淡入时长匹配时，采集器按世代和槽号定位变更区块，并刷新活动淡入。区块替换和透明索引更新同时到达时，按当前槽位归并上传候选，候选用槽号世代戳去重，淡入成员用布尔数组跟踪。每次最多就地处理 `max(256, 可见槽数 / 4)` 个通知。可见区块从空变为非空或从非空变为空、脏项超限或无法沿用快照时，在 dispatcher 锁内采集区块状态、原点和上传时间并分配槽号，解锁后计算淡入和筛选上传候选。投影包含受伤、摇晃与反胃变换；每帧由 Native 进行朝向剔除。普通透明保持原版反向 section 顺序；其他层按原版可见遍历顺序送入，由 Native 按页分桶组织。连续两帧未捕获、失效或停用时释放快照引用；关闭的 section 进入延迟回收队列。

同批的每个视图独占一份 `NativeFrameBuffer`。Java `TerrainResidency` 在 MC 主线程通过空闲栈为暂存/驻留的每个 `MeshPayload` 分配 32 位稠密槽位号 `slotId`（`1..maxSlot`，`0` 保留为空），提交淘汰时回收，未提交的分配在下次准备前自动回滚，epoch 切换时重置。受控地形视图固定池化顶点和索引，直至 worker 消费完整个帧包。公共 `meshDirect`、`indexDirect` 入口复制外部段，公共 `addView` 批次在入队时复制视图负载。可见项与实例通过复用 `int[]` 暂存批量 `MemorySegment.copy` 写入。

## 驻留与顶点释放

`TerrainCapture` 在全量采集可见区块时记下尚未驻留或透明索引已变更的条目。`TerrainResidency` 首次准备同一驻留 epoch 的快照时只检查这些上传候选；epoch 不匹配或再次准备同一快照时重新扫描。当单视图热快照可复用、无遗留超预算候选、无待移除项且上帧已提交时，`TerrainResidency` 仅排空脏透明索引队列，并在无淡入变化时同时设置 `RN_FRAME_REUSE_VISIBLE | RN_FRAME_REUSE_INSTANCES`（仅淡入变化时设置 `RN_FRAME_REUSE_VISIBLE` 并只更新变化槽位的实例可见度）。
Java `TerrainResidency` 保留 3 MiB 软预算，按整个 section 的三层及初始透明索引原子采用。
超预算时仅对候选集合现算平方距离并优先近处，同坐标新版本未采用时回退到旧驻留 section 的完整层集合；首个超大 section 可独自推进。
已驻留透明排序索引更新优先，可突破软预算；只更新索引，不重新上传顶点。
当前可见或回退 section 保持活跃，Native 算出的零面组掩码不触发 Java 回收；待移除列表使用 $O(1)$ swap-pop 清理。

每个视图在录制期创建驻留变更记录，后续同批视图可以沿用槽号。未入队关闭时按逆序恢复驻留项、索引与空闲槽。提交回调请求释放已上传的顶点和索引。对外读取使用独立游标的只读视图，原生帧包持有固定长度的只读段。全部借用结束后底层内存才返回 DirectUploadPool。
尚未选入上传的顶点继续保留。FAILED 不承诺回滚，关闭 renderer 并向上传播，不在原 renderer 重试。
仅交换链 resize 保留 renderer、驻留几何和纹理。
关卡或图集几何失效沿用 MC 26.3 `LevelRenderer.invalidateCompiledGeometry`，由注入在新 `SectionCompiler` 创建前推进 epoch。
显式 renderer 替换也触发同一重新编译流程；旧 epoch payload 被忽略，已释放的顶点不重传。
下次地形准备遇到新 epoch 时重建驻留状态。

## Native 分组与保守剔除

`rn_mesh_update` 携带可选的 `rn_terrain_mesh_layout* layout`：提供时 Native 通过 `BuildPreanalyzedTerrainMeshLayout` 以 $O(1)$ 校验每组 quad 计数、包围盒及可选 3 位线框组号，并以 `RDGUploadSource::BorrowedBytes` 连续上传顶点；`layout == nullptr` 时回退到 `AnalyzeTerrainMesh` 在准备期完成分组与生成写入。
分组规则为：SOLID/CUTOUT 检查精确轴向平面及两个三角形绕序，记录六轴向加 general 的分组与偏移，按实际顶点计算 AABB（允许模型超出 section），非平面、退化、绕序不一致进入 general，非有限数据保守可见；非空组映射到紧凑 submesh 位；TRANSLUCENT 保持原 quad 顺序与单 submesh。
共享 Quad 索引由 Native 管理；透明索引归对应 mesh，mesh 删除时同时退役。

多面组的 SOLID/CUTOUT 另外保留每个原始 quad 的 3 位面组号；单组及透明网格不保留此映射。
线框首次绘制时按显式面掩码生成独立索引，在已分组顶点上恢复原始 quad 顺序，保持共享边的覆盖顺序；索引按网格和掩码缓存，不随相机朝向重复上传。
线框使用完整请求面组交给 GPU 背面剔除；CPU 仍可跳过全部面组均不可见的网格。
关闭线框、替换或删除网格及 epoch 切换都通过 `FrameSlot` 退役索引子分配，材质变化时重绑对应状态。

Native 按视图求逆 projection × view，变换 (0,0,1,0) 得到有效视点偏移。
不可逆、非有限或 abs(w)<=1e-8 时关闭面组剔除。相对相机坐标先做整数减法，保留大世界精度。
按实际 AABB 及 0.001 容差计算轴向组；general 始终保留。多实例取可见组并集，再与显式请求掩码和有效元素位求交。
计算得到零掩码正常跳过绘制；显式非法掩码失败。Native 寿命与批次规则见[Native runtime](native-runtime.md)。

`TerrainResidency` 通过按 `slotId` 索引的平坦数组（`meshesBySlot`，上限 `kMaxMeshSlot`）和 64 字节对齐热数组（`_hotSlots`）实现 $O(1)$ 寻址。几何池均开启 `BufferUse::Resource`，分为 128 MiB/页的 `TerrainVertices`（承载 `OPAQUE` 与 `CUTOUT`）、独立的每页至多 512 MiB、受设备单缓冲上限约束的 `TranslucentVertices`（承载 `TRANSLUCENT`）、专供共享四边形的 16 MiB 索引页及半透明和线框使用的 128 MiB 索引页。帧槽回收后可释放空闲页。

每个视图按层缓存实例、可见列表、分页代表命令及索引绘制参数。Solid 和 Cutout 在有效视点各轴 ±1 方块内记录可能跨越可见平面的区块及其有效面掩码。视点在盒内移动且触及候选平面时，只复查这些候选。相同视点和未触及平面的位移直接复用绘制参数。有效掩码翻转、离开盒子或几何版本变化会触发重建。线框对多面组使用按请求掩码生成的原序索引。经典透明以排序键为初始顺序，跨页时在八项窗口内仅将投影包围盒与所有被跨越项严格分离的单实例调到同页项旁。包围盒无效或穿过视点时维持原序。发生交换的绘制参数在下个视图重建。OIT 按顶点和索引页合批。

变化的实例表和逐层间接参数使用当前帧槽的瞬态 RDG 缓冲。内容和有效字节数保持相同，且连续两次用于绘制后，视图将数据上传到专属 Device 缓冲。后续命中帧只导入缓冲。没有地形绘制时，未变的实例表保留原缓冲和命中计数。实例表有变化则重新计数，等待恢复绘制后晋升。

首次晋升或扩容时，容量从 4 KiB 起按 2 的幂向上取整，最高到设备缓冲大小上限。再次晋升时可复用已跨过两个帧槽且容量足够的旧分配。上传和绘制只使用有效字节。数据清空时释放分配。容量大于 4 KiB 且用量降至容量的四分之一以下时也释放分配。分配失败保留原缓冲和晋升状态，可在下一次准备时重试。Direct 在相同的批次上逐条调用 `DrawIndexed`。

## 绘制与 OIT

`TerrainAnchorMixin` 从原版 `renderLayers` 调用捕获当前三层或 OIT 阶段以及该次绑定的 atlas、lightmap、mip 范围、采样器和 OIT 视图。命令引用本批地形视图的索引。native 在所属 `ImmediatePass` 内按需对 `TerrainVertices`、`TranslucentVertices`、`QuadIndices` 与 `TranslucentIndices` 执行单次去重池级读依赖声明（`AddPassReadAccess`），不逐命令或逐网格重复声明屏障，同时按层声明间接参数缓冲与实例/常量/纹理访问，按原版附件与 scissor 内联录制。光栅 Pass 只按原版指定的 load/clear 操作开始一次。

SOLID 与 CUTOUT 使用反向 Z，普通透明按原版可见顺序并使用透明索引。启用改进透明度后，原版控制范围、裁剪、透射率、颜色累积和合成的顺序与附件。OIT 三个几何阶段（范围、透射率、颜色累积）复用同一份 `PreparedMeshPassIndirectDraws` 与第 2 层的间接参数缓冲，只上传一次参数；专用地形程序承担带区块几何的阶段，全屏裁剪与合成经即时绘制路径执行。绘制完地形后，`ImmediatePass` 立即使 Pass 级 RHI 状态缓存失效，确保下一条即时 draw 按自身状态重新绑定。

## 纹理、ABI 与测试

原版维护并更新图集、光照和采样器。每次准备产生独立的视图描述，输入数据的归池和驻留回调分别使用消费、提交水位。资源重载后新 epoch 重新采集编译产物。[互操作与加载](interop.md#提交布局)

CPU 用例覆盖网格、面组和索引排序；双后端原生测试检查 direct/indirect 线框、显式面掩码、缓存与跨页扩容。基准与视觉工具分别通过世界帧计数及实际截图检验游戏路径。[构建与测试](../guide/build-test.md)
