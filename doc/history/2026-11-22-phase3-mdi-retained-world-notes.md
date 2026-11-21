> - 适用: 追溯阶段三统一 MDI 场景管线、常驻世界快轨实施过程中遇到的问题、解决方案与验证数据
> - 权威: 阶段三实施与验证历史记录，不构成当前技术契约；当前实现见 [架构地图](../architecture/overview.md)、[地形渲染](../architecture/terrain-rendering.md) 与 [Native runtime](../architecture/native-runtime.md)
> - 锚点: `doc/history/2026-11-21-phase3-mdi-retained-world-plan.md`, `native/include/raynium/bridge.h`, `native/src/terrain_residency.cpp`, `native/src/model_mesh_registry.cpp`, `native/src/world_features.cpp`, `src/client/java/dev/raynium/client/world/*.java`

# 阶段三实施、问题排查与验证记录

- 原记录日期：2026-11-22
- 状态：已完成 R3.1–R3.5 源码实施、ABI 20 冻结、双后端核心/视觉/打包校验与 Vulkan Release 性能复测；来源为当日工作树（含未提交改动），D3D12 交换链呈现在本机锁屏/息屏状态下受 `DXGI_STATUS_OCCLUDED` 阻塞（详见 §2.12）
- 范围：承接 [阶段三实施计划](2026-11-21-phase3-mdi-retained-world-plan.md)，基于 `native/radray.lock.json` 锁定的上游 `RadRay` 与 Minecraft 26.3 完成地形 `RadRay::Scene` + MDI 收编、原版地形 98 MiB `StagingBuffer` 与 `prepareChunkRenders[Indirect]` 拦截、主世界刚体 `ModelPart` 24 B 常驻池 + 80 B 实例流快轨、56 B 四元数粒子实例流快轨及 31 个内置伴生 `*_inst` 双后端着色器程序
- 本机产物目录（仓库忽略目录，仅本机可用）：`build/validation/phase3-final/`、`build/validation/phase3-performance/`、`build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`

## 1. 实施概要（R3.1–R3.5）

1. **R3.1 Native 地形 `RadRay::Scene` + MDI 收编与旧栈清理**：
   - 删除 `native/include/raynium/frame_record.h`、`native/src/frame_record.cpp`、`native/tests/test_frame_record.cpp`。
   - `TerrainVertices` 与 `TerrainIndices` 池开启 `BufferUse::Resource`，`TerrainFeature::Declare` 改用池级 `AddPassReadAccess`；`TerrainResidency` 将常驻区块网格托管至 `RadRay::Scene` 并通过 `MeshDrawCommand` MDI 提交，OIT 三阶段共享单份 `PreparedMeshPassIndirectDraws` 与单块 RDG 间接参数缓冲，多视图批次通过 `TerrainViewDrawData::SnapshotFromScene` 隔离缓存命令指针。
2. **R3.2 Java 地形双拷与二次扫描切断**：
   - 新增 `NoopTerrainStagingBuffer`，在 `SectionUploadMixin` 与 `UberBufferDrainMixin` 中阻断原版 98 MiB `StagingBuffer` 写入并保留状态机回调；`LevelTerrainHookMixin` 在 `prepareChunkRenders` / `prepareChunkRendersIndirect` 入口完成 Raynium 视图捕获后直接返回轻量占位 `ChunkSectionsToRender`。
3. **R3.3 与 R3.4 常驻刚体模型与紧凑粒子快轨**：
   - 离线转换与打包工具新增 31 个内置伴生实例化程序（27 个 `core/entity` + 4 个 `core/particle`，`default.rnsp` 共 170 个 DXIL + SPIR-V 程序，3,746,318 字节），资源表与 Push Constant 布局与基础变体零差异；资源包覆盖着色器时自动回退原版即时路径。
   - Native 新增 `ModelMeshRegistry`（24 B/顶点 `ModelVertices` 池 + 64 B 常驻单位粒子四边形 + `FrameSlot` 延迟退役）与 `world_features`（`InstanceUploadArena` OIT 切片去重、Opcode 16 `RetainedModels`、Opcode 17 `CompactParticles`）。
   - Java 新增 `WorldModelCaptureScope`（严格限定在 `LevelRenderer` 主世界 `featureFrame`）、`RetainedModelManager`、`ModelFastTrack`、`ParticleFastTrack` 与 8 个 Mixin；不满足准入条件时无缝保序回退 `StagedVertexBuffer`。
4. **R3.5 ABI 20 与测试/覆盖工具**：
   - 冻结 `RN_ABI_VERSION 20u`，新增 `rn_upsert_model_mesh` / `rn_remove_model_mesh` 导出并同步清单、加载器与 `tools/native.py`。
   - 新增 `WorldFastTrackTest`、`NativeWorldFeaturesTest` 及 `test_raynium_terrain` 的 `RadRay::Scene` MDI / OIT 共享 / `InstanceUploadArena` 用例，更新 `tools/validation/pipeline_coverage.py`。

## 2. 执行过程中遇到的问题与解决方案

### 2.1 `terrain_feature.cpp`：`RDGUploadSource::Generated` 模板参数推导歧义

- **现象**：编译 `native/src/terrain_feature.cpp` 报 MSVC `C2672`/`C2665`，无法从 `&draws` 推导 `RDGUploadSource::Generated` 的回调参数类型（`TerrainViewDrawData*` 与 `const TerrainViewDrawData*` 冲突）。
- **根因**：生成回调签名接收 `const TerrainViewDrawData* const&`，而实参为非常量指针。
- **解决**：显式传入 `static_cast<const TerrainViewDrawData*>(draws)`，匹配回调的只读指针类型。

### 2.2 `test_terrain_draw.cpp`：`TerrainLayerDrawData` 无默认构造（MSVC C2512）

- **现象**：单元测试值初始化 `TerrainViewDrawData` 时报 MSVC `C2512`，提示 `TerrainLayerDrawData` 无可用默认构造函数。
- **根因**：上游 `radray::MeshPassFrameDrawList` 的默认构造声明为 `explicit`，聚合花括号初始化不会调用 `explicit` 构造。
- **解决**：在 `TerrainLayerDrawData` 为 `drawList` 声明默认成员初始化器 `{radray::MeshPassSortMode::StateFirst}`，测试中直接默认构造 `TerrainViewDrawData`。

### 2.3 `LevelTerrainHookMixin.java`：`final` 类直接强转 Mixin 接口编译失败

- **现象**：`javac` 拒绝将 `new ChunkSectionsToRender.DrawSeparate(...)` 与 `DrawIndirect(...)` 直接强转为 `RetainedTerrain.ViewOwner`（incompatible types）。
- **根因**：`DrawSeparate` 与 `DrawIndirect` 为 `final` 类，编译期 Java 编译器未知 Mixin 运行时向父类 `ChunkSectionsToRender` 织入的接口。
- **解决**：先以父类类型 `ChunkSectionsToRender placeholder` 持有实例，再强转为 `(RetainedTerrain.ViewOwner)`。

### 2.4 着色器离线转换源目录与 JSON 行尾格式

- **现象**：首次执行 `tools/convert_vanilla_shaders.py` 时传入了预转换目录 `build/vanilla/shaders` 导致找不到原版 GLSL 源码与 `post_effect`；此外 `tools/compile_shader_pack.py` 在 Windows 下写出 CRLF，使 `terrain-programs.json` 出现纯行尾变更及 `git diff --check` 告警。
- **根因**：转换器输入必须是 MC 26.3 原始资源目录；`Path.write_text` 未传 `newline='\n'` 时在 Windows 默认使用 `\r\n`。
- **解决**：从 Loom 缓存的 MC 26.3 客户端 JAR 提取原始 `assets/minecraft/shaders`（93 文件）与 `assets/minecraft/post_effect`（6 文件）至临时目录完成 195 变体 / 170 程序的转换与编译，并在 `tools/compile_shader_pack.py` 的 JSON 写入处固定 `newline='\n'` 并归一化两份已提交 JSON 为 LF。

### 2.5 `CommandTape::Replay` 未指定实参求值顺序导致 RDG 上传 abort

- **现象**：`NativeWorldFeaturesTest` 首次在双后端运行时于上游 `rdg.cpp` 触发 `RDG: invalid upload range, memory or usage` 并 abort。
- **根因**：`graph.CreateBuffer({.MaxSize = bytes.size(), ...}, RDGUploadSource::OwnedBytes(std::move(bytes)))` 在 MSVC 下按从右向左顺序求值，先执行了 `std::move(bytes)` 再读取 `bytes.size()`，导致 `MaxSize == 0` 而 `source.GetMaxSize() > 0`。
- **解决**：在 `graph.CreateBuffer` 调用前单独捕获 `const uint64_t byteSize = bytes.size();` 并传入 `MaxSize`。

### 2.6 `NativeBindingsTest` 升级 ABI 20 后负例断言失败

- **现象**：`RN_ABI_VERSION` 升级到 `20` 后，`NativeBindingsTest` 失败。
- **根因**：旧测试的 `@ValueSource(ints = {17, 18, 20})` 仍把 `20` 当作非法 ABI 版本拒绝。
- **解决**：更新拒绝列表为 `@ValueSource(ints = {18, 19, 21})`。

### 2.7 `NativeWorldFeaturesTest` 首次像素读回全黑

- **现象**：双后端运行零验证错误，但读回像素全为 `0xff000000`。
- **根因**：三处测试夹具偏差：（1）MC 26.3 `dynamictransforms.glsl` UBO 字段顺序为 `(ModelViewMat, TextureMat, ColorModulator, ModelOffset)`，测试将 `ColorModulator` 放错槽位导致其被读为 `(0,0,0,1)`；（2）裁剪空间 Y 轴方向写反（上半区为 `-0.5`，下半区为 `+0.5`）；（3）1×1 覆盖纹理需要 `overlay = 0`。
- **解决**：修正测试夹具 UBO 字段顺序、`overlay = 0` 与上下半区 Y 坐标后，D3D12 与 Vulkan Debug/Release 均实现逐像素精确通过。

### 2.8 视觉差分脚本缺少 `numpy` 与 CMake 测试脱离 `VsDevCmd`

- **现象**：系统 Python 3.14 运行 `tools/validate_visual.py` 差分步骤报 `ModuleNotFoundError: No module named 'numpy'`；直接执行 `cmake`/`ctest` 缺少 x64 MSVC 环境变量与 `-S native` 源路径参数。
- **根因**：仓库图像对比环境位于独立虚拟环境 `build/validation/image-tools/`；x64 工具链由 `tools/native.py` 动态探测。
- **解决**：使用 `build/validation/image-tools/Scripts/python.exe` 运行视觉比对，并通过 `tools/native.py` 的 `msvc_environment()` 封装 `cmake -S native -B <dir>` 与 `ctest` 调用。

### 2.9 多页世界 `geometryBucket` 8 位哈希冲突与初轮基准 `nativePrepareNs` 退化排查与优化

- **现象**：
  1. 初次 `entities-vulkan` 冒烟显示间接模式下 `terrainDrawCalls p50` 仍偏高；
  2. 首轮全量 Release Vulkan 基准（下称 `p3-v1`）中，GPU 耗时已优于原版 Vulkan（`entities` GPU P50 `0.633 ms` vs 原版 `1.009 ms`），但 CPU 侧 `nativePrepareNs` 较阶段二退化明显：Metcalf 从 `0.461 ms` 升至 `0.965 ms`（帧数 `89,218 -> 67,354`），Tideworn 从 `1.253 ms` 升至 `2.150 ms`（帧数 `59,429 -> 45,547`）。
- **根因**：
  1. 上游 `MeshDrawCommandBindingStorage::GetStateSortKey` 将几何哈希压缩为 8 位 `geometryBucket`（`mod 255`），大世界（Metcalf ~88 顶点页、Tideworn ~111 顶点页）发生哈希碰撞，若低位 `VisibilitySortKey` 未按页分组则会交替拆分 MDI 批次。
  2. `TerrainResidency::PrepareFrame` 存在四处可避免的 CPU 开销：
     - 间接模式先调用 `CountMeshPassIndirectDraws` 再调用 `BuildMeshPassIndirectDraws`，对 1–2 万个面组重复执行两次 `ForEachExpandedMeshDrawElement` 与元素校验；
     - `TerrainViewDrawData` 随每帧新建的 `CommandTape` 零容量构造并销毁，`VisibleCommands`、`_scratchCommands`、`IndexedArguments`、`Batches` 等每帧分配并释放约 1.1 MiB；
     - `StateFirst` 的 `VisibilitySortKey` 把 16 位距离键与 12 位页序横跨 4 个字节，叠加 `geometryBucket` 跨 2 个字节，导致 32 字节 `VisibleMeshDrawCommand` 每帧执行 6 趟基数排序；同时经典透明层由 Java 严格降序输出（`count - i`），直接送入排序白白触发 4 趟基数排序；
     - 恒定向 `Scene` 注册全部 7 个 Pass（含未启用的 OIT 三阶段与全层 `Wireframe`），导致区块更新与透明索引重排的缓存同步工作量放大，且稳态帧仍重复构造 `changedMeshes` 哈希表。
- **解决（`p3-v2`）**：
  1. 间接模式跳过冗余的 `CountMeshPassIndirectDraws`，按 `selectedGroups` 预留 `IndexedArguments`/`Batches` 容量并由单次 `BuildMeshPassIndirectDraws` 回填 `recordCount`；
  2. 在 `TerrainResidency` 中维护按 `FrameSlot` 索引持久复用的 `_slotViewDraws`（`unique_ptr<TerrainViewDrawData>` 稳定地址）与 `_visibleScratch`，稳态零堆分配；
  3. 压缩 `StateFirst` `VisibilitySortKey`：`OitBounds` 仅编码 1 字节 `vertexPageId`；`Solid`/`Cutout` 编码 `(vertexPageId << 8) | distBucket`（`distBucket` 为由 16 位浮点距离键单调映射的 8 位深度桶），使排序字节数降至 1–2 B；经典透明层检测严格降序后就地 `std::reverse`，使 `SortVisibleCommands()` 直接命中零趟排序短路；
  4. 按当前 `(oit, wireframe)` 模式注册活动 Pass（经典 3 个、OIT 5 个、仅开启线框时追加 `Wireframe`），无模式变化且 `!scene.HasDirtyBatches()` 时跳过 `SyncCachedMeshDrawCommands`，无上传时跳过 `changedMeshes` 分配。
- **结果**：`p3-v2` 相对 `p3-v1` 在 Metcalf 提升 +13.6% 帧数（`natPrep` `0.965 -> 0.771 ms`，`-20.1%`）、Tideworn 提升 +13.1%（`natPrep` `2.150 -> 1.742 ms`，`-19.0%`，且 GPU P50 `1.0368 ms` 低于阶段二 `1.0498 ms`）、entities 提升 +6.8%、particles 提升 +9.0%。优化后剩余的 ~0.31 ms（Metcalf）/ ~0.49 ms（Tideworn）CPU 差值源自锁定上游 `RadRay` 通用 `Scene::GatherVisibleMeshDrawCommands`、32 字节命令排序与 Release 态 `ValidateCommandElementStructure`（属阶段四 GPU Culling 消除范围）。

### 2.10 重跑大地图基准触发 `F:` 盘空间不足（`WinError 112`）

- **现象**：`p3-v2` 首次启动 Metcalf 基准复制存档时失败，报 `[WinError 112] 磁盘空间不足`。
- **根因**：此前 `p3-v1` 的 Metcalf 与 Tideworn 独立运行目录各自保留了完整存档副本（合计 ~6.2 GB），占满剩余磁盘空间。
- **解决**：删除 `build/validation/phase3-performance/` 中旧运行目录的冗余 `game/saves` 副本（完整保留 `summary.json`、`client-result.json`、`frames.csv`、`gpu.csv` 与日志），并在后续脚本中每跑完一张地图即清理该次临时存档副本。

### 2.11 `pipeline_coverage.py` 缺少粒子源码目录

- **现象**：只提取 `net/minecraft/client/renderer/` 源码运行 `pipeline_coverage.py` 时，报 `OIT_PARTICLE` 无显式路由。
- **根因**：`SingleQuadParticle.java` 位于 `net/minecraft/client/particle/`。
- **解决**：提取完整的 `net/minecraft/client/**/*.java` 后运行 `pipeline_coverage.py` 通过（`195 pipelines, 5 post effects, 154 selected in supplied captures`）。

### 2.12 环境限制：锁屏/息屏状态下 D3D12 `DXGI_STATUS_OCCLUDED`

- **现象**：运行环境处于 Windows 锁屏/显示器关闭状态，D3D12 `IDXGISwapChain::Present` 返回 `DXGI_STATUS_OCCLUDED`，被映射为 `RN_PRESENT_SKIPPED (1)`。
- **影响范围**：
  - Debug `nativeTest -PrhiValidation=true` 共 84 项用例中，80 项通过；4 项要求 D3D12 交换链返回 `RN_PRESENTED` 的用例失败（`NativeSwapchainTest` 3 项、`NativeCommandTapeTest [1] D3D12` 1 项）。
  - D3D12 `BenchmarkSession` 依赖 `RN_PRESENTED` 计数推进预热与路线，因此在息屏状态下超时；Vulkan 交换链不受息屏影响，全部基准正常完成。
- **处理原则**：不弱化任何测试或基准断言，如实记录受限边界。D3D12 的全部离屏渲染测试（含 `NativeWorldFeaturesTest`、`NativeWireframeTest`、`NativeRenderPearlTest`、`NativeCaptureTest`、`NativeTextureTest` 等）以及全部 86 场景 D3D12 视觉回归采集（Debug + RHI validation）均 100% 正常执行并通过。

## 3. 验证结果汇总

### 3.1 构建、单测与打包

| 验证项 | 命令 / 配置 | 结果 |
| --- | --- | --- |
| Debug & Release 原生构建 + stage | `cmake --build` + `tools/native.py stage`（ABI 20，静态 CRT `/MTd` & `/MT`） | 通过（Release 零 Raynium 编译告警，`dumpbin /exports` 确认 `rn_upsert_model_mesh`/`rn_remove_model_mesh`） |
| C++ 单元测试 (`test_raynium_terrain`) | `ctest` Debug & Release | **16/16 通过**（Debug 0.19 s，Release 0.13 s） |
| 纯 Java 单元测试 | `.\gradlew.bat test` | **51/51 通过**（含 `WorldFastTrackTest` 500 组位级法线基向量与四元数粒子校验、`TerrainCaptureTest`、`NativeBindingsTest`） |
| Python 工具单测 | `python -m unittest discover -s tools/tests` | **15/15 通过（1 skipped）** |
| Shader pack 验包 | `python tools/shader_pack.py ...default.rnsp` | 通过（170 程序，DXIL + SPIR-V 完整） |
| `NativeWorldFeaturesTest` | Debug & Release，`-PrhiValidation=true`，Vulkan + D3D12 | **双后端逐像素通过**，零 validation 报错 |
| 优化后定向双后端 GPU 回归 | `nativeTest -PrhiValidation=true`（`WorldFeatures`/`Wireframe`/`RenderPearl`/`CommandTape`） | 31/32 通过（仅 `CommandTapeTest [1] D3D12` 受 §2.12 `DXGI_STATUS_OCCLUDED` 阻塞；全部 Vulkan 与 D3D12 离屏/地形/线框/多视图/模型/粒子通过） |
| 全量 Debug `nativeTest` | `.\gradlew.bat nativeTest -PrhiValidation=true` | 80/84 通过（全部 Vulkan 用例 + D3D12 非交换链呈现用例通过；4 项 D3D12 交换链用例受 §2.12 阻塞） |
| Release 分发打包与验包 | `.\gradlew.bat packageNativeMod verifyNativePackage` | 通过（`raynium-0.1.0-SNAPSHOT-windows-x86_64.jar` 结构与哈希校验通过） |
| 文档规范检查 | `python tools/check_docs.py` & `git diff --check` | 通过（0 错误，无 CRLF 警告） |

### 3.2 86 场景双后端视觉回归与管线覆盖

环境：RTX 4080，Windows x64，Debug 构建，`-PrhiValidation=true`，对比阶段二原版 Vulkan 基准图。共 11 组 **86 个场景（每后端 123 张图，双后端合计 246 张图）**，全程零崩溃、零 GPU validation 报错、零相对阶段二的渲染退化。

| 场景组 | 图像数 | Vulkan 结果 | D3D12 结果 | 备注 |
| --- | ---: | --- | --- | --- |
| `vanilla-scenes`（`p3-v2` 复测） | 10 | `world-*`/`rain-*`/`resource-reload`/`inventory-preview`/`first-person` 均 `px>2=0`；`wireframe-*` 较阶段二改善（`175->117`, `124->117`） | `world-*`/`first-person`/`inventory-preview`/`resource-reload`/`pause-blur` 均 `px>2=0`；`rain-*` `px>2=2` 与阶段二一致 | Vulkan `pause-blur` 偶发 69 px Unifont 边缘平局；D3D12 线框与阶段二硬件光栅平局一致 |
| `vanilla-entity-variants` | 5 | **5/5 `px>2=0`**（`maxErr<=1`） | **5/5 `px>2=0`**（`maxErr<=1`） | 涵盖半透明隐身实体、潜影贝深度偏移、画、移动方块（Classic + OIT） |
| `vanilla-gallery` | 5 | **5/5 exact（`maxErr=0`）** | **5/5 `px>2=0`**（`maxErr<=1`） | 验证 `WorldModelCaptureScope` 严格排除 GUI/PiP |
| `vanilla-translucent-sorting` | 2 | **2/2 exact（`maxErr=0`）** | **2/2 `px>2=0`**（`maxErr<=1`） | 验证物品排序与 OIT |
| `vanilla-fonts` | 6 | **6/6 `px>2=0`**（`maxErr<=2`） | **6/6 `px>2=0`**（`maxErr<=2`） | 验证世界文字/告示牌/透明指南针 |
| `vanilla-resource-animation` | 30 | **25/30 exact，30/30 `px>2=0`**（`maxErr<=1`） | **25/30 exact，30/30 `px>2=0`**（`maxErr<=1`） | 验证动画关键帧、连续重载与 mip 导出 |
| `vanilla-water-mask` | 3 | `boat-water-mask*` `px>2=0`；`world-border-oit` `px>2=2` | `boat-water-mask*` `px>2=0`；`world-border-oit` `px>2=72` | 与阶段二一致 |
| `vanilla-effects` | 31 | 与阶段二一致 | 与阶段二一致（`water-oit` `px>2: 1->0`） | 验证特殊材质、粒子、后处理与维度切换 |
| `vanilla-interactions` | 18 | 与阶段二一致（`title-panorama` `px>2: 77->0`） | 与阶段二一致 | 验证破损贴花回退、图腾、地图、调试线 |
| `vanilla-outlines` | 4 | `px>2` 较阶段二 `794->92` | `px>2=794`（与阶段二一致） | 云层相位差异 |
| `vanilla-specials` | 9 | 与阶段二同频段（刚体模型零差异，仅动态风灵/光束相位点） | 与阶段二同频段 | 人工核查 `rare-materials.png` 确认刚体盔甲/纹饰无差异 |

`pipeline_coverage.py`（33 份 `visual-result.json`）：**`195 pipelines, 5 post effects, 154 selected in supplied captures`**，全部管线路由、伴生 `*_inst` 双后端产物与源码锚点校验通过。

### 3.3 Release Vulkan 性能基准对比

环境：RTX 4080，Release `/MT`，`rhiValidation=false`，`nativeProfiler=false`，1920×1080，视距 32，模拟距离 8。Metcalf 与 Tideworn 为完整 60 s 预热 + 240 s 路线；`entities` 与 `particles` 为 15 s 预热 + 30 s 路线对照同配置原版 Vulkan。帧间隔、渲染墙钟、Native 阶段与 GPU 单位均为毫秒（ms）。

| 场景 / 版本 | 世界帧数 | FPS | 帧间隔 P50 / P95 | 渲染墙钟 P50 | Native Prepare P50 | RDG P50 | GPU P50 | 地形 DrawCalls / Records P50 | 场景 DrawCalls / Upload(B) P50 | GC 次数 (ms) |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| Metcalf 阶段二 Vulkan | 89,218 | 371.7 | 1.8034 / 6.8320 | 1.7605 | 0.4610 | 0.3232 | 0.6620 | 1,448 / 10,730 | 8 / 112 | 244 (2,454) |
| Metcalf 阶段三 `p3-v1` | 67,354 | 280.6 | 2.6189 / 8.2912 | 2.5397 | 0.9651 | 0.3831 | 0.7000 | 1,421 / 10,995 | 8 / 3,304 | 212 (2,040) |
| **Metcalf 阶段三 `p3-v2`** | **76,527** | **318.8** | **2.2624 / 7.6472** | **2.1856** | **0.7712** | **0.3683** | **0.6798** | **1,386 / 10,789** | **8 / 2,976** | **204 (1,995)** |
| Tideworn 阶段二 Vulkan | 59,429 | 247.6 | 3.7638 / 8.8996 | 3.6819 | 1.2534 | 0.4159 | 1.0498 | 1,665 / 21,124 | 7 / 112 | 177 (2,866) |
| Tideworn 阶段三 `p3-v1` | 45,547 | 189.7 | 5.1110 / 10.7620 | 5.0079 | 2.1498 | 0.4779 | 1.1383 | 1,663 / 21,218 | 7 / 7,896 | 161 (2,982) |
| **Tideworn 阶段三 `p3-v2`** | **51,511** | **214.6** | **4.5106 / 10.0024** | **4.4115** | **1.7421** | **0.4567** | **1.0368** | **1,607 / 20,277** | **7 / 7,288** | **180 (2,710)** |
| `entities` 阶段三 `p3-v1` | 6,639 | 220.9 | 4.4117 / 5.4228 | 4.3180 | 1.2792 | 0.4359 | 0.6331 | 512 / 15,781 | 163 / 146,864 | 25 (113) |
| **`entities` 阶段三 `p3-v2`** | **7,093** | **236.0** | **4.1203 / 5.1382** | **4.0263** | **1.0651** | **0.4233** | **0.6258** | **490 / 15,782** | **154 / 146,576** | **34 (149)** |
| `entities` 原版 Vulkan | 10,638 | 354.0 | 2.6976 / 3.6322 | 2.6521 | — | — | 1.0086 | — | — | 54 (218) |
| `particles` 阶段三 `p3-v1` | 4,412 | 146.9 | 5.8476 / 12.7881 | 5.6772 | 1.4163 | 0.5215 | 1.5118 | 90 / 18,549 | 72 / 827,040 | 47 (181) |
| **`particles` 阶段三 `p3-v2`** | **4,808** | **160.0** | **5.2957 / 12.4417** | **5.1598** | **1.1753** | **0.5006** | **1.5239** | **90 / 18,534** | **72 / 827,208** | **50 (203)** |
| `particles` 原版 Vulkan | 6,589 | 219.3 | 3.8253 / 11.1805 | 3.7310 | — | — | 1.8463 | — | — | 44 (232) |

关键观测：
- `NoopTerrainStagingBuffer` 消除原版 98 MiB 地形暂存写入与每帧脏区刷写，Metcalf GC 总时长较阶段二下降 18.7%（`2,454 ms -> 1,995 ms`），Tideworn GC 总时长由 `2,866 ms` 降至 `2,710 ms`。
- 启用 OIT 的 `particles` 场景中，18,534 条地形记录收敛为 90 次间接绘制调用（OIT 三阶段复用单次参数构建与单块 RDG 间接缓冲）；GPU P50 在 `entities`（`0.6258 ms` vs 原版 `1.0086 ms`，快 38%）、`particles`（`1.5239 ms` vs 原版 `1.8463 ms`，快 17.5%）与 Tideworn（`1.0368 ms` vs 阶段二 `1.0498 ms`）均保持领先。
- `entities` 场景的 Java 帧均分配与 GC 均显著低于原版 Vulkan（`1,881.4 KiB/frame`、`149 ms` GC vs 原版 `2,060.2 KiB/frame`、`218 ms` GC）。
