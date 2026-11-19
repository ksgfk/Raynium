> - 适用: 理解与推进 RadRay 引擎底座与 Raynium 客户端渲染架构的五阶段协同重构总计划
> - 权威: 经用户授权保存的探索性演进总计划，不作为当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/src/solid_pipeline.cpp`, `native/src/scene_pass.cpp`, `src/client/resources/raynium.client.mixins.json`

# RadRay 与 Raynium 五阶段协同演进总体计划

- 原记录日期、归档日期：2026-11-19
- 状态：探索性总体蓝图，用户已确认主方向并授权归档至 `doc/history/`，待按阶段细化与实施；归档时仅完成对 Raynium 与上游 RadRay 当前源码的静态调查，未执行构建或运行验证
- 范围：上游 `RadRay`（`gpu`、`gpu_impl`、`shader`、`runtime`）基础设施补齐，以及 `Raynium` 的 Java 接入层、跨语言 C ABI、Shader 资产策略与 Native 模块化渲染架构演进
- 来源：2026-11-19 会话中针对 Raynium 当前维护性瓶颈与 RadRay 基础设施缺口的静态源码审计；Minecraft 26.3 事实依据见 [Minecraft 渲染流程](../architecture/minecraft-rendering.md)，Raynium 当前已落地契约见 [架构地图](../architecture/overview.md)、[互操作与加载](../architecture/interop.md) 与 [Native runtime](../architecture/native-runtime.md)，精确上游依赖版本以 `native/radray.lock.json` 为准
- 决策与授权归属：用户提出调查“如何制定高性能通用架构及 RadRay 欠缺的基础设施”，要求由助手直接拍板完整协同推进内容，并要求显式声明实施中途允许修改方向，随后授权将总计划归档入库

## 0. 可变性与阶段门禁声明

本计划确立了从底层引擎到上层模组的唯一主航道，但其中包含多项尚未经实机验证的架构假设（如 `RenderPearl` 字节流录制语义、GLSL 桥接开销、大规模动态实体分流及 GPU-Driven / 光追性能表现）。因此明确约定：

1. **非僵死契约**：本文不代表任何拟议模块或接口已经实现，也不要求机械按原样走完所有细节。
2. **双门禁驱动**：每个阶段结束均以视觉验证（`tools/validate_visual.py`）和性能采样（JFR + Tracy + `tools/benchmark.py`）作为准出闸门。
3. **允许中途转向**：实施过程中若发现某项假设可行性受阻或 CPU/GPU 性能不达标，立即按第 4 节修正预案调整切点深度、合并/拆分数据通道或替换局部实现策略，不硬扛沉没成本。

## 1. 背景：当前架构的四大维护性病灶

截至 2026-11-19，Raynium 虽已将 Minecraft 26.3 完整渲染带到 Native，但整体处于难以继续扩展的特化状态：

1. **Java 接入切面错位（空设备 + 46 个接管 Mixin）**：`RenderSystem` 设备保持为空，靠哑身份句柄（`TakeoverHandles`）配合 `src/client/resources/raynium.client.mixins.json` 中的 46 个 `takeover.*` Mixin 逐点拦截原版调用。导致图集拼接与动画插值（`TakeoverAtlas`）、光照图计算（`CpuLightmap`）、纹理清屏全部退化为 Java CPU 像素搬运，且天空、云、天气、世界边界、画中画等均需单独维护侵入式 Mixin。
2. **脆弱的离线 Shader 转换与硬编码目录**：`tools/convert_vanilla_shaders.py` 依赖正则改写 GLSL 采样坐标、拼接 `uint4` 裸常量宏并生成 3,939 行的 `native/src/scene_catalog.inc`（195 个固定管线分支）与 `native/src/post_catalog.inc`，无法支撑资源包、模组管线或自定义光影扩展。
3. **臃肿的单帧大包 C ABI**：`native/include/raynium/bridge.h` 将地形增量、可见性、相机、GUI、写死的 18 个 `rn_scene_phase`、离屏目标与后处理全部塞入 408 字节的 `rn_render_frame_desc`，任何特性增删都迫使跨语言偏移同步修改。
4. **Native 内部四套平行迷你渲染器各自为政**：由于 `native/radray.lock.json` 锁定的早期上游版本尚未提供完整的资产、布局注册表、PSO 缓存、瞬态参数池与间接绘制命令管线，`native/src/solid_pipeline.cpp` 内部拼合了地形（`mesh_pass.cpp` / `terrain_draw.cpp`）、原版场景（`scene_pass.cpp`）、GUI（`gui_pass.cpp`）和输出读回（`output_pass.cpp`）四套独立实现，重复维护 PSO、Sampler、纹理视图和逐 Draw 的 `ResetBindingsForReuse()` 清理逻辑。

## 2. 目标架构拍板（六项核心决策）

1. **Java 接入切点**：直接实现 `com.mojang.renderpearl.api.device.GpuDevice`、`CommandEncoder`、`RenderPass`、`GpuSurface` 顶层公共接口（不钻入原版 `FrontendGpuDevice` 内部后端包装），自主掌控命令编码与映射缓冲，自然承接原版及第三方模组的 `RenderSystem.getDevice()` 调用。
2. **双轨正交渲染分工**：
   - **即时命令轨（`CommandTape`）**：Java 侧将 `RenderPearl` 操作线性编码写入堆外 `MemorySegment` 字节流（`GpuBuffer.map` 写操作直接切分堆外暂存切片），每帧仅通过 **1 次 FFM 调用**交给 Native 解码进 RDG。负责 **GUI、光照图、图集 Sprite 动画插值、纹理 Clear、离屏物品/画中画、原版内置后处理、截图读回及特殊自定义渲染兜底**。
   - **常驻世界快轨（Retained World）**：仅在区块编译与刚体模型提交处保留约 5 个干净切点，负责 **区块地形、常规刚体实体/方块实体部件、批量粒子**，直入 Native 常驻 GPU 场景与加速结构。
3. **正交三通道 C ABI**：废除单体 `rn_render_frame_desc` 与硬编码的 18 个场景阶段，拆分为 **① 资源与资产事务流**、**② 即时 `CommandTape` 字节流**、**③ 常驻世界增量与视图包**。
4. **跨后端坐标与深度统一**：利用 `RadRay` RHI 在 Vulkan 后端已支持的负高度 Viewport 与 D3D12 原生正向视口，将内部所有渲染目标统一为正向坐标（左上原点）与 `[0, 1]` Reverse-Z，在视口层吸收朝向差异，根除 Shader UV 正则翻转与正反面绕序分叉。
5. **双轨 Shader 资产体系**：
   - Raynium 自有世界管线、后处理与光追特性统一采用 **原生 HLSL + `RadRay::shader`（支持 Keyword 变体与 `AssetManager` 原子热替换）**；
   - `RenderPearl` 兼容轨接收的 GLSL 采用 **“构建期字节码/反射缓存包 + 可选编译插件（Sidecar DLL）”**：正式发布单 DLL 零编译器依赖即可运行全部原版管线；开发模式或存在自定义管线模组时按需加载编译组件并落盘缓存。
6. **Native 插件化帧图（`IRenderFeature` + `FrameBlackboard`）**：拆解 `SolidFramePipeline` 巨石类，各渲染特性独立实现 `IRenderFeature`（同步资源、提取帧数据、向 `RDGBuilder` 挂载 Pass），仅通过类型化 `FrameBlackboard` 读写命名资源句柄。

## 3. 五阶段协同推进路线

每阶段均遵循 **“先在 `RadRay` 落地通用底座并完成单测 → 推进 `native/radray.lock.json` → 在 `Raynium` 消费改造并执行回归验收”** 的顺序。

### 阶段一：夯实 `RadRay` 帧与资源底座，清理 `Raynium` Native 基础设施样板

- **阶段目标**：冻结 Java 侧与 C ABI 18 外部契约，不在阶段二即将拆除的旧转换管线上做过渡翻修，先把上游通用底座做实并清理 Native 内部的私有资源、参数集清理器、读回与计时样板。
- **`RadRay` 侧建设**：
  1. 纵向贯通 RHI、双后端与 RDG 的全链路 BCn 块布局及 `Cube` / `Dim3D`（含非紧密 `RowsPerImage`）纹理上传，扩展全能力 `GpuTexture`（含显式 `Usage` 与子资源 `TextureView` 缓存）并在 `AssetManager` 提供 `TextureAsset`（复用 RHI 已内建的 `gpu::Device::GetOrCreateSampler`）；
  2. D3D12 后端扩容 Tier 1 百万级资源描述符堆并引入 `SamplerTableCache` 引用计数去重；在 `FrameSlot` 内聚严格在命令分配器重置后先于帧内对象析构解绑的 `TransientParameterSetPool` 及动态常量偏移辅助；
  3. RHI 增加原生 `PipelineCache` 序列化接口（D3D12 自持输入字节副本），`runtime` 新增绑定唯一 `AssetManager` 并联合保活布局退役的 `PsoCache`；
  4. RDG 补齐三路所有权分流的子资源 `ResolveView`、单 mip 整资源 `AddClearColor` / `AddClearDepthStencil`、类型化 `RDGBlackboard`、四态生命周期 `AsyncReadbackQueue` 以及支持 `Off` / `FrameOnly` / `FrameAndPasses` 三档模式与 `UserSequence` 的内建 GPU 时间戳采样。
- **`Raynium` 侧改造**：
  1. 升级 `native/radray.lock.json`、Debug/Release CMake 探针（含 `RADRAY_DXC_ROOT` 默认定位）、`raynium_shader_pack` 工具源码与 `.rnsp` Schema 10 校验、`DecodeBindingKind` 解码及测试夹具偏移，完成 `radray::gpu` 迁移；
  2. 删除 `native/src/texture_resources.cpp` 改用上游 `GpuTexture` 与 `GetOrCreateSampler`（保留新建单层 2D 颜色附件首帧清屏语义），通过 `FrameSlot` 瞬态参数池清除逐 Draw 的 `Emplace<Cleanup>` 绑定清理链，并用四态 `AsyncReadbackQueue`（配合入图前薄保活表）及按 `MEASURE` / `PROFILE` 分档的内建 GPU 时间戳替换手写截图与前/后命令缓冲包装。

### 阶段二：落地 `RenderPearl` 命令流（`CommandTape`）与模块化帧图，大举清退接管 Mixin

- **阶段目标**：打通 Java 堆外命令流录制与 Native 模块化 RDG 回放，切换正交新 ABI 与统一正向视口，将非世界核心路径交还原版 GPU 逻辑，移除绝大多数接管补丁与旧场景转换代码。
- **`RadRay` 侧建设**：
  1. 在 `runtime` 提供面向即时命令回放的流式顶点/索引/常量切片快速分配接口及相邻同目标 Pass 合并支持。
- **`Raynium` 侧改造**：
  1. 切换至正交三通道 C ABI 并全面启用双后端正向视口约定（Vulkan 负高度 Viewport，消除旧 `RN_FRAME_FLIP_Y` 双绕序分支）；Java 侧实现 `RenderPearl` 顶层 `GpuDevice` 与堆外 `CommandTape` 编码器，配套落地 GLSL 预编译缓存包与可选编译插件；
  2. Native 侧正式建立模块化 `IRenderFeature` + `RDGBlackboard` 帧图骨架，落地 `ImmediateReplayFeature` 解码 `CommandTape`；
  3. 恢复原版光照图 GPU Pass、图集 Sprite 动画插值、纹理 Clear、离屏物品/画中画、GUI、内置后处理与截图读回；
  4. 删除 40 余个 `takeover.*` Mixin、`TakeoverAtlas`、`CpuLightmap`、`gui_pass.cpp`、`scene_pass.cpp` 以及 `scene_catalog.inc` / `post_catalog.inc`。

### 阶段三：统一 MDI 场景管线，收编常驻世界（地形 + 刚体实例）

- **阶段目标**：让区块地形与高频动态几何统一运行在 `RadRay::Scene` 上，形成完整的常驻世界几何底座。
- **`RadRay` 侧建设**：
  1. RHI 补齐带 GPU 计数缓冲的 `DrawIndirectCount` / `DrawIndexedIndirectCount`；
  2. `MeshDrawCommand` 扩展无索引 `Draw` 支持与内建 **MDI（Multi-Draw Indirect）自动合批提交器**；
  3. 扩展 `MaterialParameterValue`（覆盖整型、矩阵、Buffer 视图、`AssetHandle<TextureAsset>` 与静态 Keyword 变体）及常驻 `MaterialBufferPool`。
- **`Raynium` 侧改造**：
  1. 移除私有的 `terrain_draw.cpp` 状态表与录制器，将区块地形收编进 `RadRay::Scene` + `MeshDrawCommand` MDI 管线；
  2. 将常规刚体实体部件（Cuboid / 常驻方块实体）与粒子批次接入常驻网格 + 实例变换流快轨；特殊或模组自定义实体自动回退至阶段二的 `CommandTape` 渲染。

### 阶段四：Bindless 材质化与 GPU-Driven 遮挡剔除

- **阶段目标**：突破大视距与 PBR 多贴图资源包下的 CPU 状态切换与可见性计算上限。
- **`RadRay` 侧建设**：
  1. RHI、`shader` 与 `runtime` 贯通 `BindlessResourceRegistry`（无界 Texture / Sampler / Buffer 描述符表与按帧槽延迟回收）；
  2. `runtime` 提供通用 GPU-Driven 剔除骨架（Compute 视锥剔除 + Hi-Z 深度金字塔构建与遮挡剔除 → 输出紧凑 MDI 流与 GPU Count Buffer）。
- **`Raynium` 侧改造**：
  1. 地形（含资源包法线/高光/发光扩展贴图）与常驻实体全部切换为 Bindless 索引寻址，消除按贴图切分批次；
  2. 启用 GPU Hi-Z 遮挡剔除接管细粒度面组与实例可见性裁剪。

### 阶段五：异步流式传输、并行命令录制与硬件混合光追

- **阶段目标**：补齐流式并发与硬件光追能力，构建可扩展的现代光追/光影架构。
- **`RadRay` 侧建设**：
  1. RHI 与 RDG 支持独立 **Async Copy 队列** 及 Timeline Fence 跨队列所有权移交；
  2. `FrameSlot` 支持线程局部 `CommandAllocator` 池，结合 `coroutine.h` 实现多 Pass 并行录制；
  3. RHI、`shader` 与 `runtime` 落地硬件光线追踪基建（`AccelerationStructure` BLAS/TLAS 池化与增量更新、Inline `RayQuery` 及 `RayTracingPipelineState`）。
- **`Raynium` 侧改造**：
  1. 区块几何与大纹理上传迁入后台 Copy 队列，平抑高速移动时的帧时间尖刺；
  2. 基于常驻世界几何池与 Bindless 材质表构建并维护世界 BLAS/TLAS，以独立 `IRenderFeature` 挂载混合光追 Pass（光追阴影、环境光遮蔽、水面反射/折射、全局光照）与原生 HLSL 光影包接口。

## 4. 中途修正方向预案

1. **若阶段二 `CommandTape` 在密集动态场景下 CPU 编码/解码开销偏高**：提前在阶段二末尾将高频普通实体与粒子迁入常驻快轨，将 `CommandTape` 严格收缩为仅承担 UI、光照图、图集动画、离屏预览与后处理等低频 Pass。
2. **若阶段二 GLSL 自动转译 DXIL 在复杂边缘语法上维护代价过高**：D3D12 与 Vulkan 的原版兼容管线固定使用按源码哈希索引的构建期预编译包，仅在 Vulkan 后端优先开放任意模组 SPIR-V 管线直通，不让边缘 GLSL 阻塞主线。
3. **若阶段三刚体实体提取在部分原版模型上出现姿态或特殊叠加偏差**：收紧常驻实例拦截白名单（仅拦截标准高频生物与方块实体），其余一律无感回退 `CommandTape`，坚持“宁走通用命令流，不新增特例 Mixin”。
4. **若阶段四/五遇到不支持 Bindless、`IndirectCount` 或硬件光追的设备**：依托阶段三已统一的 `MeshDrawCommand` 提交模式与 `IRenderFeature` 开关，自动回退至 Direct 或基础 MDI 光栅路径。
