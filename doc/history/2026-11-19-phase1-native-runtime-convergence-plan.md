> - 适用: 执行或评审 Raynium 第一阶段接入上游 RadRay 新底座并清理 Native 资源/参数集/读回/计时样板的详细计划
> - 权威: 经用户确认保存的阶段一实施计划，不作为当前已实现契约；当前实现见 [架构地图](../architecture/overview.md) 与 [Native runtime](../architecture/native-runtime.md)
> - 锚点: `native/radray.lock.json`, `native/CMakeLists.txt`, `native/CMakePresets.json`, `native/tools/shader_pack.cpp`, `tools/shader_pack.py`, `doc/guide/build-test.md`, `src/test/java/dev/raynium/bridge/NativeShaderPackTest.java`, `native/include/raynium/renderer.h`, `native/include/raynium/solid_pipeline.h`, `native/src/solid_pipeline.cpp`, `native/src/scene_pass.cpp`, `native/src/gui_pass.cpp`, `native/src/mesh_pass.cpp`, `native/src/texture_resources.cpp`

# Raynium 第一阶段：接入上游 RadRay 底座与清理 Native 基础设施样板计划

- 原记录日期、归档日期：2026-11-19
- 状态：第一阶段细化实施计划，技术边界与去过度设计收敛经用户确认并授权在原计划文件内修订，待上游 `RadRay` 阶段一底座完成后推进实施；归档时未执行源码改动、构建或运行验证
- 范围：`Raynium` 的上游 `native/radray.lock.json` 升级、`radray::gpu` 命名空间与 Debug/Release CMake 包发现及 `.rnsp` 工具链 Schema 10 同步、Native 私有纹理/采样器/每 Draw 参数集清理器/截图读回/手工 GPU 时间戳基础设施替换；**第一阶段冻结 `src/main/java` 与 `src/client/java` 的 Java 生产代码及 C ABI 18 外部契约（`src/test/java` 仅同步 `NativeShaderPackTest` 二进制记录偏移夹具及 `NativeCaptureTest` 底层 `rn_poll_capture` 协议断言），严格沿用现行 `AcquireNext()` 前输入校验与 `RunFrame()` fail-fast 边界，且不对计划于阶段二/三淘汰的 `.rnsp` 与旧转换管线做过渡翻修**
- 来源：承接 [RadRay 与 Raynium 五阶段协同演进总体计划](2026-11-19-radray-raynium-coevolution-master-plan.md) 的阶段一安排，结合 2026-11-19 对 `Raynium` 构建/测试脚本、Native 源码与上游 `RadRay` RHI / Runtime 契约的核查结论定稿；当前线上契约见 [互操作与加载](../architecture/interop.md) 与 [Native runtime](../architecture/native-runtime.md)

## 0. 前置条件与可变性说明

1. **上游 `RadRay` 前置交付物**：本计划依赖上游 `RadRay` 先行落地第一阶段四项通用底座（`T1`～`T4`）并通过其双后端测试，具体包含：
   - RHI 已内建的 `gpu::Device::GetOrCreateSampler`，外加全链路贯通块布局的 `Cube` / `Dim3D` / BCn 纹理拷贝、D3D12 Tier 1 百万级资源描述符堆与 Sampler Table 引用计数去重、D3D12 自持输入字节副本的原生 `gpu::PipelineCache`；
   - `RadRay::runtime` 新增的全能力 `GpuTexture`（含可选显式 `Usage`、子资源 `TextureView` 缓存）与 RDG 原生 Cube 上传、内聚于 `FrameSlot` 且严格在 `ResetCommandStorage()` 立即解绑的 `TransientParameterSetPool`、绑定唯一 `AssetManager` 且联合保活布局退役的 `PsoCache`、三路所有权分流子视图、单 mip 整资源 `AddClearColor` / `AddClearDepthStencil`、`RDGBlackboard`、具备 `Reserved -> Enqueued -> Submitted -> Consumed` 四态管理与指针/容量精确轮询语义的 `AsyncReadbackQueue`，以及支持 `Off` / `FrameOnly` / `FrameAndPasses` 三档模式与 `UserSequence` 快照的内建 GPU 时间戳采样。
2. **中途调整许可**：实施过程中若发现 ABI 18 边缘行为需要微调适配层细节，允许按实测结果就地收敛，以双后端视觉一致性和性能不回退为准绳。

## 1. 第一阶段边界收敛决策（防返工原则）

1. **外部边界与 `RN_FRAME_FLIP_Y` 语义保留**：`src/main/java` 与 `src/client/java` 的 Java 生产代码、FFM 绑定及 `native/include/raynium/bridge.h` 的 21 个 C ABI 18 入口保持不变（`src/test/java` 仅更新 `NativeShaderPackTest.java` 的 Schema 10 记录步长夹具，并在 `NativeCaptureTest.java` 通过同包底层 FFM `rn_poll_capture` 句柄补充边界轮询断言）。保留 ABI 18 的 `RN_FRAME_FLIP_Y` 对地形裁剪空间 Y 与正面绕序的逐帧成对控制，双后端负高度正向视口统一留至阶段二与新三通道 ABI 同步落地。
2. **沿用既有 `AcquireNext()` 前输入准备与 `RunFrame()` fail-fast 边界**：严格对齐 [互操作与加载](../architecture/interop.md) 与 [Native runtime](../architecture/native-runtime.md#失败处理) 现行契约——`AcquireNext()` 之前执行全部调用方输入合法性校验与宿主准备阶段（`PrepareTerrain`、`PrepareCapture` 含截图 `ReadBack` 缓冲预留、`PrepareGui`、`PrepareScene` 持久纹理参数准备），校验或宿主准备失败时在未获取交换链令牌的状态下清理未提交预留并返回 `false`；一旦通过校验进入 `AcquireNext()` 后的 `RunFrame()`，RDG 瞬态纹理物化、`UploadArena`、命令缓冲获取及 RDG `Prepare` 回调统一沿用既有 `RunFrame` fail-fast（`RADRAY_ABORT`）契约。
3. **不在阶段一将旧 `.rnsp` 或 195 个转换管线强迁 `ShaderAsset` / `PsoCache`**：ABI 18 的 `.rnsp` 仅封装裸 `ShaderArtifact` 而不含 `ShaderFamilyLayoutDesc`，且地形/GUI/输出在启动时仅创建少量静态 PSO（无热重载），195 个转换场景管线（`scene_pass.cpp`）又将在阶段二整包删除。因此阶段一不伪造 `ShaderFamilyLayoutDesc` 或槽位重映射层，仅可选将驱动级 `gpu::PipelineCache*` 传入现有静态 PSO 创建；`ShaderAsset` + `PipelineLayoutRegistry` + `PsoCache` 留至阶段二/三随新 Shader 资产体系自然启用。
4. **不在旧 `SolidFramePipeline` 上强拆过渡型 Feature**：`IRenderFeature` + `RDGBlackboard` 留至阶段二作为 `ImmediateReplayFeature` 与常驻世界快轨的原生骨架直接建立。触及与新增的 Native 代码遵守 `AGENTS.md` 硬约束（禁止默认构造后 `Init`，可失败创建采用 `static Create`，显式使用 `radray::` 类型，禁止 `using namespace radray`）。

## 2. 分步实施清单（R1.1 ～ R1.4）

### R1.1 升级 `native/radray.lock.json`、Debug/Release 构建探针、离线打包工具与 Schema 10 解码

- 将 `native/radray.lock.json` 推进至包含阶段一底座的上游 `RadRay` 版本。
- 在同一提交内闭环 CMake 配置（含 Debug `/MTd`）、离线打包工具源码、C++ 解码与测试夹具：
  - **CMake 选项与 DXC 包发现**：更新 `native/CMakeLists.txt` 的源码探针至 `modules/gpu/include/radray/gpu/types.h`；将旧选项 `RADRAY_BUILD_RENDER` 迁移为 `RADRAY_BUILD_GPU_IMPL`、移除已废弃的 `RADRAY_BUILD_SHADER_COMPILER`，将 `raynium_shader_pack` 链接目标改为 `RadRay::shader`；当 `RADRAY_DXC_ROOT` 未显式指定时默认指向 `${RADRAY_SOURCE_DIR}/SDKs/radray_dxc/extracted`（补齐 `CMakePresets.json` 中 `win-x64-debug` 清空 `RADRAY_DEPENDENCY_TRIPLET` 后的 `RadRayDXC` 包发现），并同步修订 `doc/guide/build-test.md` 的依赖准备说明（CMake 配置期需准备 `radray_dxc` 包，运行时 `rayniumruntime.dll` 仍不携带或链接编译器 DLL）；
  - **离线工具与运行时头文件/解码迁移**：将 `native/tools/shader_pack.cpp` 中的 `<radray/shader_compiler/client.h>` 与 `radray::shader_compiler::Client` 迁移至 `<radray/shader/client.h>` 与 `radray::shader::Client`；将全部 `radray::render::*` 迁移至 `radray::gpu::*`；将 `native/src/shader_pack.cpp` 与 `native/src/scene_pass.cpp` 中对 `record.Type` / `binding.Type` 的枚举强转统一改为调用 `radray::shader::DecodeBindingKind(...)`；
  - **打包校验与 Java 负例夹具同步**：更新 `tools/shader_pack.py` 信封校验至 `kShaderMetadataSchemaVersion == 10` 并重新生成内置 `default.rnsp`；同步更新 `src/test/java/dev/raynium/bridge/NativeShaderPackTest.java` 中 `"vertex"` 与 `"constants"` 等篡改用例的记录步长与字段偏移（类型记录步长由 `52` 字节改为 `44` 字节，顶点输入记录改为 `20` 字节内偏移），确保每项负例精确破坏目标字段。

### R1.2 移除私有纹理与采样器封装，直连上游 `GpuTexture`

- **删除 `native/include/raynium/texture_resources.h` 与 `native/src/texture_resources.cpp`** 中的 `raynium::TextureResource` 类及私有采样器缓存。
- 以轻量 `TextureRegistry` 直接管理 `radray::IntrusivePtr<radray::GpuTexture>`：
  - 按 ABI 纹理描述符映射显式 `gpu::TextureUses`，子 mip 采样视图调用 `GpuTexture::GetOrCreateView`，采样器统一调用 `device.GetOrCreateSampler`，立方体贴图直接走 `RDGBuilder::UploadTexture`（移除临时 `Dim2DArray` 中转纹理与拷贝 Pass）；
  - **保留新建单层 2D 颜色附件首帧清屏语义**：在 `TextureRegistry` 跟踪新建单层单 mip 2D `RenderTarget` 纹理的首次入图标记，首次导入 RDG 时插入一次单 mip 整资源 `RDGBuilder::AddClearColor`（透明黑），保障离屏局部更新未覆盖像素与重构前完全一致。

### R1.3 接入 `FrameSlot` 内聚 `TransientParameterSetPool`，清除逐 Draw 绑定清理链

- **静态绑定组布局一次性转为 `shared_ptr` 持有**：将 `SceneProgram::groups`（`scene_pass.h`）、`MeshPassResources`（含 `PassGroupLayout()` 与 `OitGroupLayout()` 访问器，`mesh_pass.h`）、`GuiPassResources` 与 `OutputPassResources` 在创建期产出的 `BindingGroupLayout` 由 `radray::unique_ptr` 一次性转为 `radray::shared_ptr<radray::gpu::BindingGroupLayout>` 长期持有，无需引入 `ShaderAsset` 或 `PipelineLayoutRegistry` 即可向参数池直接传递同一份共享所有者；同时可在 `Renderer` 会话级持有可选驱动 `gpu::PipelineCache` 加速静态 PSO 创建。
- 移除 `SceneDraw::parameters`、`GuiFrameSlot` 纹理参数缓存、`SolidFramePipeline::SlotResources::oitParameters` 及 `OutputBinding` 手工持有的参数集与脏标记。
- 统一通过 `FrameSlot`（宿主准备期绑定持久纹理/采样器）与 `RDGPrepareContext`（图内绑定瞬态纹理视图与缓冲切片）获取瞬态参数集：
  - 同帧同槽内相同绑定内容自动命中池内驻留去重；
  - 依托 `FrameSlot::ResetCommandStorage()` 在正常轮转与 `WaitIdle()` 下“先重置命令分配器并解绑全部参数集、后销毁帧内临时视图与退役资源”的严格顺序，以及上游 D3D12 百万级资源堆与 `SamplerTableCache`，彻底删除 `scene_pass.cpp`、`scene_post.inc`、`gui_pass.cpp` 中所有的 `slot.Emplace<Cleanup>` / `ResetBindingsForReuse()` 析构胶水；
  - 空回调单 mip 清屏 Pass 统一替换为 `RDGBuilder::AddClearColor` / `AddClearDepthStencil`。

### R1.4 四态闭环接入 `AsyncReadbackQueue` 与三档内建 GPU 时间戳采样

- **完整对齐请求期、入图提交前/后与轮询期的 ABI 捕获语义**：
  - **`RequestCapture`（请求期）**：校验 `request.id != 0`，联合检查 `Raynium` 薄待构图表与 `AsyncReadbackQueue::HasRequest` 拒绝重复 ID，联合统计 `pending.size() + readbackQueue.GetActiveRequestCount() < 4096` 上限，并对指定纹理层/mip 与 `CAPTURE_LAST` 立即持有 `IntrusivePtr<GpuTexture>` 保活引用；
  - **`PrepareCapture`、`SetupGraph` 与 `SubmitFrame`（预留、强引用入图移交与 Fence 关联）**：在 `AcquireNext()` 前的 `PrepareCapture` 调用 `AsyncReadbackQueue::ReserveTexture` 分配/复用 `ReadBack` 缓冲（若后续 `PrepareGui` / `PrepareScene` 校验失败或构图未提交即关闭渲染器，调用 `CancelUnsubmitted()` 干净回收已预留/已入图但未提交的缓冲，兼容 `NativeCaptureTest` 关闭语义）；`SetupGraph` 在对应阶段对 `CAPTURE_TEXTURE` 与 `CAPTURE_LAST` **必须先调用 `graph.ImportPersistentOwned(entry.texture)` 将 `IntrusivePtr<GpuTexture>` 强所有权移交给 `CompiledRDG` 保活至本槽 GPU 完成，再调用 `EnqueueReserved` 挂载拷贝并从薄待构图表移除该项**（防止调用方已提前关闭原纹理句柄时源纹理在 GPU 拷贝前提前析构）；`SubmitFrame` 在队列 `Submit` 发出 `_completion` 信号后立即调用 `readbackQueue.MarkSubmitted(frame.completion)` 绑定本次提交序号；
  - **`PollCapture`（入图前与入图后双路径轮询）**：先查薄待构图表——若请求尚未经过下一帧入图，直接填写 `info = {sizeof(info), 0, ...}` 返回 `true`（支持请求后立即轮询返回未就绪）；若已移交上游队列，原样转发 `(sequence, _completion->GetCompletedValue(), outInfo, static_cast<radray::byte*>(pixels), capacity)` 至 `AsyncReadbackQueue::Poll`，精确保持 `pixels == nullptr` 查尺寸不消费、`pixels != nullptr && capacity < requiredBytes`（含 `capacity == 0`）报容量不足且不消费、仅在 Fence 完成且成功复制后消费回收的完整协议。
- **按 `MEASURE` / `PROFILE` 精确映射三档 GPU 时间戳采样**：
  - 删除 `SolidFramePipeline::SubmitFrame` / `Wait` 中的 `before` / `after` 命令缓冲与手工 18 槽时间戳解析；
  - 按帧标志精确映射上游采样档位：未设 `RN_FRAME_MEASURE | RN_FRAME_PROFILE` 时为 `RDGGpuProfilingMode::Off`；仅设 `RN_FRAME_MEASURE` 时为 `RDGGpuProfilingMode::FrameOnly`（仅录制整帧首尾 2 个时间戳，不引入逐 Pass 采样开销）；含 `RN_FRAME_PROFILE` 时为 `RDGGpuProfilingMode::FrameAndPasses`（同时录制存活 Pass 时间戳并输出 120 样本分位数日志）；
  - 向 `RenderGraphFramePipeline` 传入 `frame.sequence` 作为 `UserSequence`，在 `Wait` 读取完成快照，继续**仅在当前帧请求 `RN_FRAME_MEASURE` 时**向 `rn_present_result` 回填已完成帧的 `gpu_sequence` 与 `gpu_nanoseconds`。

## 3. 验收与同步步骤

1. **升级前基线留档**：在推进 `native/radray.lock.json` 前，用当前锁定版本在 D3D12 与 Vulkan 双后端保存标准场景与原生测试的 Native 输出截图基线（覆盖 `RN_FRAME_FLIP_Y` 开/关、GUI 模糊分段、局部更新的新建离屏颜色附件及立方体贴图）。
2. **干净构建与打包验证**：在仅按文档准备依赖与 `radray_dxc` 的干净构建目录下，验证 `win-x64-debug` 与 `win-x64-release` 双预设 CMake 配置、编译、导出表/CRT 身份检查、`raynium_shader_pack` 编译与 `default.rnsp` 生成校验及 Gradle `processClientResources`。
3. **单元与契约测试**：运行 C++ 测试与全部 Gradle 测试（`test` + 双后端 `nativeTest`），重点核查：
   - `NativeShaderPackTest` 全部 Schema 10 字段篡改负例精确命中预期拒绝分支；
   - `NativeSwapchainTest`、`NativeTextureTest`、`NativeWireframeTest`、`NativeSceneFrameTest` 在 `RN_FRAME_FLIP_Y` 开/关及新建离屏附件局部绘制下的像素断言；
   - `NativeCaptureTest` 覆盖“请求后未 `Present` 立即轮询返回未就绪”、“提前销毁源纹理后经 `ImportPersistentOwned` 跨帧捕获”、“预留读回后因后续 GUI 参数非法导致 `Present` 失败并安全关闭渲染器”，以及通过同包底层 FFM `rn_poll_capture` 入口直接验证“就绪后连续两次空指针查询 → 非空指针零/小容量查询失败且不消费 → 正常读取成功 → 再次读取报已消费”。
4. **视觉与性能对照**：将双后端 Native 输出与步骤 1 基线逐场景比对像素一致性，运行 `tools/validate_visual.py` 与 `tools/benchmark.py`（`metcalf-stress`、`tideworn-stress`），核查仅 `RN_FRAME_MEASURE` 与开启 `RN_FRAME_PROFILE` 两档下的 `gpu_sequence`、阶段分位数日志、CPU/GPU 耗时及 D3D12 描述符堆水位无异常退化。
5. **长期文档同步**：实施落地并通过全部验证后，同步更新 `doc/guide/build-test.md`、`doc/architecture/overview.md`、`doc/architecture/native-runtime.md`、`doc/architecture/terrain-rendering.md` 与 `doc/architecture/shader-pack.md`，最后执行 `python tools/check_docs.py` 与 `git diff --check`。
