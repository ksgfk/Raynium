> - 适用: 修改 MC native 适配、帧生命周期、共享资源及关闭顺序
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费边界；FrameScheduler/Scene/arena 基础契约由 RadRay 维护
> - 锚点: `native/include/raynium/environment.h`, `native/include/raynium/runtime.h`, `native/src/renderer.cpp`, `native/src/terrain_scene.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`

# Native runtime

`rayniumruntime` DLL 通过 CMake `add_subdirectory` 从锁定的 RadRay 源码编译并静态链接 `RadRay::runtime`，不消费另一份预编译 runtime。
公共头使用 `<radray/runtime/...>`，本仓库的具体适配类型位于 `raynium`。bridge DLL 为 `rayniumruntime.dll`，使用 ABI 10；Debug/MTd、Release/MT 与静态 mimalloc 的构建约束见[互操作与加载](interop.md#构建身份与-payload)。
精确依赖版本由 `native/radray.lock.json` 维护；构建身份包含 RadRay 源码、版本、dirty 状态及工具链。
上游基础契约位于 RadRay `docs/architecture/runtime.md`。本项目的测试入口与覆盖见[构建与测试](../guide/build-test.md#java-与-rhi-测试)。

## 适配层职责

`Renderer` 拥有 `TerrainScene` 和一个双槽 `FrameScheduler`，调度器独占 `SolidFramePipeline`。
`Renderer::Create` 依次创建 `GeometryArena`（预建首块）、`SolidFramePipeline` 和调度器，全部成功后才构造 renderer 并把 arena 移入 `TerrainScene`。
SolidFramePipeline 继承上游 RenderGraphFramePipeline，使用 RDG 声明常量上传、几何复制、按需 split mip 装配和 SolidRaster。
图编译/裁剪、依赖调度、资源屏障、附件和命令缓冲开闭、可见命令排序及每槽瞬态资源池由 runtime 执行。
Raynium 负责输入/输出共享 ownership transfer、timeline 提交和完成等待；没有通用 RenderSystem、SceneRenderer 或 View/Pass 框架。

`TerrainScene` 持有持久的 RadRay Scene、`GeometryArena` 和以 `std::unordered_map` 按 ID 索引的 TerrainState。
`Renderer::Render` 检查输入/输出序号并等待待复用槽就绪后，由 `PrepareFrame` 按当帧 ABI 增量数组就地更新状态与 Scene。
入口检查布局、epoch、数组指针/数量及增量记录的基本字段；资源引用和几何内容由同一模组的 Java 调用方保证，
native 不扫描驻留表校验引用或逐个读取索引值。顶点与索引先按该槽 staging 容量写入当前帧槽持久映射的 Upload 缓冲，
由 Record 一次复制到几何 arena 区间或独立索引 Buffer，CPU 不驻留索引值副本；实例写入同一槽的持久映射实例缓冲。
帧参数记录清空所有 ABI 指针。

删除和替换直接按增量 ID 查找，旧 owner 的保活边界见下文资源退役说明。
材质通过 Add/Update/RemoveMaterial 管理；删除或替换 primitive 时先移除旧 batch，解除其旧材质引用，再处理材质删除与新关联。
仅已有 mesh 或 index 被同 ID 替换时，才遍历存活 primitive 并对受影响项调用 UpdateMeshBatch，保留 batch 句柄。
稳态零更新帧跳过资源与 batch 同步，纯新增几何也不遍历驻留 primitive；可见列表直接从本帧 ABI 数组生成。
有场景增量时，适配层按当前数量加本帧新增上界调用 `Scene::Reserve`，由 runtime 预留对象、关联、命令与绑定池容量。
Scene 存储通过受控增删改接口使用；TerrainPrimitive 按值存储，缓存 batch 句柄、顶点/索引 Buffer 指针和元素数量，在构建或刷新 batch 时同步。
适配层在创建及 epoch 切换后为 meshes/primitives 驻留表各预留 4096 项。
epoch 切换退役旧几何与材质并清空 Scene/状态；共享纹理重建后按保留描述重建材质与 primitive 句柄。
RunFrame 正常返回后推进输入/输出 timeline 序号。PrepareFrame 更新失败不恢复 Scene 或 TerrainState；宿主关闭 renderer 并禁用预览，不重试该场景。

## 命令缓存与参数绑定

SolidFramePipeline 注册一个 MeshPassId 为 0 的 SolidPassProcessor。可变 Scene 的 RunFrame 在 Extract 前同步脏 batch 的缓存命令。
Processor 通过 BuildMeshDrawCommands 为每个非空 batch 生成一条包含 submesh 掩码的命令；未改变的 batch 跨帧复用命令。
SOLID 注册 `StateFirst` 排序，不提供自定义状态键；runtime 根据 PSO、驻留顶点流与材质参数状态生成键，并组合当帧可见项的深度桶。
帧列表排序使用 runtime 的稳定插入排序/基数排序，适配层不把 batch handle 编入基础键。
材质更新由 Scene 标脏引用它的 batch，索引扩容或网格替换由适配层原地更新关联 batch；可见性、相机和实例变化不标脏命令。

Group 0 是每槽独立的 256 字节 Upload CBuffer 与参数集，在管线 Create 中创建并绑定。
Group 1 是每个材质版本的图集/lightmap 与 sampler 参数集，随材质更新或纹理重绑创建。
BuildMeshDrawCommands 将顶点流和参数载荷交给 runtime 按内容去重并引用计数，额外 submesh 由命令独占。
64 字节 MeshDrawCommand 通过 VertexBuffersIndex、PayloadIndex 和 ExtraElementsIndex 引用绑定存储；
每 Pass 的稠密命令数组和 batch 槽表由 runtime 管理。命令及池条目不拥有所引用的 RHI 对象，缓存列表负责释放其绑定引用。
MeshPassResources 的稳定存储拥有 shader、group layouts、pipeline layout、PSO、MaterialResource 与 base Material；
逆序析构保证被依赖布局晚于参数集和 PSO 释放，帧槽常量也晚于自身参数集释放。

VertexInput 声明 28 字节顶点槽 0 和 16 字节实例槽 1；驻留 mesh 只绑定槽 0，允许延迟实例流。
槽 0 的 stream 覆盖整个 arena 块，同一块的网格共享相同 view，绘制时用 submesh 的 base_vertex 加区间首顶点定位。
PrepareFrame 将 ABI 可见项转换为 runtime 的 VisibleMeshBatch，并同时收集几何 Buffer 借用；ExtractFrameData 转交帧参数和列表 span，SetupGraph 声明上传、资源访问和输出。
每条 ABI 可见项直接生成一条 VisibleMeshBatch，不展开或合并掩码；UINT32_MAX 归一化为该 primitive 的全部元素位。
独立记录保留输入顺序、重复、排序键及 FirstInstance/InstanceCount 覆盖；单条掩码中的元素按位序消费。
Processor 开启 runtime 的 `AllowElementMerging`：满足同索引缓冲、顶点偏移、单实例及索引范围连续等条件的相邻选中元素可以合为一次 DrawIndexed。
SOLID shader 不依赖 draw/primitive ID；合并不改变索引序列与实例覆盖，非连续选择和多实例绘制仍由 runtime 分别处理。
可见循环只查询一次按值 primitive，用缓存的元素数量校验掩码，并从缓存指针收集几何访问，不读取 runtime MeshBatch 或 VertexInput。
几何访问按 Buffer/状态对去重，先用上一个顶点/索引指针短路重复项；共享 quad 索引连续出现时只执行第一次列表查重。
Extract 使用 PrepareFrame 提供的列表，不二次遍历 Scene 或逐面组扫描全部 Elements。
图编译后，ExtractMeshPass 使用 GatherVisibleMeshDrawCommands 的帧列表重载，收集缓存命令指针、有效元素掩码及当帧 FirstInstance/InstanceCount；BindingStorage 指针由命令保存。
Scene、命令缓存与绑定存储从同步完成到 RunFrame 返回保持只读且地址稳定；帧槽只借用 MeshDrawCommand 指针。Wait 和槽清理不解引用这些指针。
runtime 的 Record 排序存活 MeshPass；SolidRaster 回调把当帧实例流和 Group 0 参数集传入 MeshPassFrameDrawList::Submit，
由上游执行 PSO/参数绑定和 DrawIndexed，Debug 另执行整批预检；录制直接使用借用的缓存命令。
Raynium 通过可见项覆盖实例范围，由 runtime 提交绘制；适配层不展开实例或另建 indirect 参数。MC 数据与 shader 见[地形预览](terrain-preview.md)。

## 上传、帧槽与资源退役

| 对象 | 所有者与释放边界 |
| --- | --- |
| 环境、设备和 Direct 队列 | C bridge 持有环境与 DeviceContext；队列从设备借用，环境晚于设备销毁 |
| FramePipeline、PSO、shader、布局与 MaterialResource | FrameScheduler 拥有管线；调度器先排空槽录制及退役资源，再销毁管线 |
| 当前 CPU 场景、submesh、mesh、材质与 primitive 描述/句柄 | TerrainScene 持有 TerrainState；mesh 由 shared_ptr 保持 VertexInput 地址稳定，primitive 按值存储 |
| 每个索引的独立 Device Buffer、每个材质的 Group 1 与 sampler | 当前 TerrainState 保活；旧版本先由 TerrainScene 暂存，再交给 FrameSlot::RetireResource |
| 几何 arena 块（32 MiB Device Buffer）与网格顶点区间 | GeometryArena 归 TerrainScene 并晚于驻留状态析构；块在场景寿命内不释放，区间随 mesh owner 退役归还 |
| 每槽的 passConstants/passParameterSet | SolidFramePipeline 固定双槽资源；仅在对应槽完成后写入 |
| 每槽 staging、实例、timestamp/readback | SolidFramePipeline 固定双槽资源；Create 预建，仅在对应槽完成后写入或替换 |
| 每槽可见 batch 与几何访问列表 | SolidFramePipeline 固定双槽 CPU 存储；PrepareFrame 清空后保留容量，可见列表按几何增长扩容 |
| 每槽 D32 深度及 view | SetupGraph 声明瞬态附件；RenderGraphFramePipeline 的槽池按完整描述符复用，保留实际终态，旧尺寸对象保留到池销毁 |
| CommandAllocator / CommandBuffer | FrameSlot 独占；runtime 在图命令中录制所有权屏障，开启 GPU 计时时 SubmitFrame 另借前后两条 timestamp 命令，仍只提交一次 |
| 共享输出及输入 | C bridge 子句柄持有 RenderTarget / SharedTextures；帧借用，销毁前完成宿主交接并排空调度器 |

网格顶点从 GeometryArena 的块内区间分配，28 字节/顶点，按 best fit 选空闲区间并与相邻区间合并；单块 32 MiB，
区间用完时新建块，块在 TerrainScene 寿命内只增不减。每个 index 独占 uint32 Buffer。新版本先写入当前槽 staging，
由 Record 分组复制到目标区间或索引 Buffer，不覆写仍在途的旧版本。
PrepareFrame 在替换或删除 owner 前把它加入 TerrainScene 的待退役集合；
准备成功后将集合移入 TerrainFrame，Extract 通过当前 FrameSlot::RetireResource 转交整批所有者。
该槽保活到下次复用或 WaitIdle，释放前所有引用旧版本的 CommandAllocator 均已重置；
顶点区间在此时归还 arena，之后才可能被新的 PrepareFrame 重新分配，因此在途读取与新写入不会重叠。
若 PrepareFrame 中途失败，待退役集合留在 TerrainScene，排空调度器后由 Reset 或 DetachTextures 清理。

管线维护独立 completion timeline。复用槽时 Wait 确认提交完成，调度器依次重置 allocator、处理退役、清空帧记录与 arena。
FrameData 拥有上传记录、标量帧参数和完成值；可见 batch 与去重几何列表通过 span 借用本槽 CPU 存储，不拥有 GPU 对象、不复制整份 Scene 或持有整份 TerrainState。
列表只在本次 RunFrame 内的 SetupGraph/ExtractMeshPass 使用。旧帧 CPU 录制和该槽 completion 完成后，下一次 PrepareFrame 可以清空复用；
后续旧槽 Wait 和 arena 清理不读取这些 span。RHI 对象仍按原有帧槽退役协议保活。
常量使用 RDGBuilder::UploadBuffer；几何保留每槽持久映射 staging，由 GeometryUpload CopyPass 一次录制各区间复制。
图按整 Buffer 跟踪状态，几何导入/终态统一为 Common，上传 staging、常量和实例在图末回到 HostWrite。
几何上传目标与已变化的 split 私有纹理均标记为图输出，即使没有可见 draw 也会初始化；共享颜色也是图输出，空帧仍清屏。
深度由 runtime 池按描述符复用，并记录真实终态、生成跨帧屏障。

共享图像通过 RDG 的 `RDGExternalOwnership` 声明 acquire/release，队列等待和信号仍由 SubmitFrame 负责。
共享输出首帧从 Undefined 开始且只 release；后续输出和全部物理输入均以 Common 导入/结束并 acquire/release。
所有物理输入均 MarkOutput，包括本帧未采样或未更新的图像，保证空帧与未变化的 split mip 仍完成所有权交接。
RDG 在首次访问时 acquire，在终态转换后 release；共享颜色用于 RenderTarget、direct 输入用于 ShaderRead、split 物理输入用于 CopySource。
私有完整 mip 图像的复制与采样转换也由 RDG 处理。
不开启 GPU 计时时只提交图命令；开启时按 timestamp 起点 → graph → timestamp 终点/resolve 提交三条命令，计时覆盖图内所有屏障及 native GPU 工作。
Extract、Record 与 RunFrame 返回 void；
帧回调中的无效状态、资源或命令准备失败通过 RADRAY_ABORT 记录诊断并终止，不提供回滚或重试。

复用槽及显式 Finish 先以 5 秒超时检查 completion，失败保留活跃所有者；成功后 WaitIdle 完成回收。
Wait 读取槽持有的 timestamp readback，数值样本存入管线固定数组，不保留 FrameData 引用。
Renderer 销毁前先 WaitIdle，再清空持久 Scene/TerrainState，最后释放调度器与管线布局。
共享输入销毁也先排空槽，再清空缓存命令及持久纹理参数绑定，保留 CPU 材质描述与几何；下次挂接纹理集合时重建关联与绑定。

## 环境与设备

`RayniumEnvironment` / `RayniumRuntime` 是 `radray::Environment` / `radray::DeviceContext` 的类型别名。
环境创建 DXGI factory 或 Vulkan instance，不创建设备；设备会话借用环境，创建 Device 和一条 Direct 队列。
可失败创建使用 `static Create`，全部资源准备成功后才交付有效对象，没有默认构造后 Initialize。
上游工厂以日志和空 owner 报告失败，C bridge 返回 `RN_STATUS_FAILED` 并填入调用方诊断缓冲区。

DLL 每份加载实例最多一个环境和一个活跃设备会话；生命周期与渲染调用在环境初始化线程串行执行。
后端与 driver validation 在环境创建时固定，`raynium.rhi.backend=d3d12|vulkan` 默认 D3D12，不自动切换。
Debug/Release 分别采用 RadRay 默认启用/关闭的 RHI 检查；driver validation 在 Vulkan 启用调试和同步验证，在 D3D12 启用 DXGI/D3D12 诊断，未开启 GPU validation。
D3D12 环境在 MC 设备创建前初始化，设备重建借用同一个 factory；环境销毁不关闭进程级调试层。
Vulkan 创建失败只清理本次取得的 instance。

设备开启 external resource 扩展。传入宿主身份时按 LUID/node mask 匹配 GPU；无法匹配时记录警告并返回首选设备，导入端拒绝跨 GPU 共享。
当前 SOLID 路径不依赖 indirect draw 能力，不创建渲染工作线程、异步计算或额外提交队列；启用 profiler 时，Tracy 的采样线程由环境生命周期管理。
设备析构等待 Direct 队列，环境必须晚于设备及所有 GPU 资源销毁。native 不持有宿主窗口或交换链，最终呈现仍归 MC。

## 共享输出与输入

一个 renderer 同时允许一个 RenderTarget 和一个 SharedTextures 集合。
RenderTarget 持有 RGBA8_UNORM 共享颜色、其 attachment view、导出和 ready/consumed timeline；D32 深度及其 view 由 runtime 的每槽瞬态池持有。
SolidRaster 清雾色和 depth=0，SOLID 使用 GreaterEqual，并启用背面剔除和深度写入。
SOLID 使用 256 字节 CBuffer、28 字节顶点和 16 字节 origin/visibility 流，支持图集/lightmap、雾、淡入和 RGSS。

D3D12 导出 D3D12Resource/D3D12Fence，Vulkan 导出 OpaqueWin32。Win32 句柄由 native RAII 持有，Java 借用导入。
输入支持完整 mip 共享或每 mip 一张物理图像；split 模式采样私有完整 mip 图像，只在 changedTextures 标记变化时复制相应完整 mip 链。
所有物理输入每帧均 acquire/release；没有内容变化时不复制。首次使用必须初始化全部逻辑纹理，空可见集仍交接并清屏。

```text
创建输入: 初始化布局 → release → inputReleased(1)
MC:      wait inputReleased(n) → 写输入 → inputAvailable(n)
native:  wait inputAvailable(n) + outputConsumed(previous)
         acquire → RDG 上传/装配/SolidRaster → release
         signal completion(k) + inputReleased(n+1) + outputReady(m)
MC:      wait outputReady(m) → copy/release → outputConsumed(m) → 原有呈现
```

每条 timeline 只有一个 signal 方。正常帧不 CPU 等待尚未提交的宿主输入工作，不调用整队列 Wait。
输出首帧从 Undefined 初始化，后续从 Common acquire；输入 direct 在 Common/ShaderRead 间转换，split 物理输入使用 CopySource，私有图集在 ShaderRead/CopyDestination 间转换。
resize 排空旧输出与帧槽后重建输出，各槽在下一次图执行时按新描述符从池中取得深度（相同尺寸可复用）；共享输入重建也先排空槽，后续材质参数集绑定新 view。
世界 epoch 切换清理持久场景但保留设备会话。

关闭 target 同时确认 ready/consumed，关闭 inputs 确认 native 已释放，随后 Finish/WaitIdle 释放槽内引用，再销毁共享资源。
宿主先提交并完成自己的读写，关闭 Java 导入后才销毁 native 子句柄；超时保留句柄、Java owner 和 DLL。
父 owner 有活跃子 owner 时拒绝关闭。完整宿主顺序见[MC 接入](minecraft-integration.md#逐帧交接)，FFM 所有权见[互操作与加载](interop.md)。
