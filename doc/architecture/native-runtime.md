> - 适用: 修改 MC native 适配、帧生命周期、专用地形提交及共享资源
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界；调度器、RHI 和通用 Mesh Drawing Pipeline 由 RadRay 维护
> - 锚点: `native/include/raynium/environment.h`, `native/include/raynium/runtime.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`

# Native runtime

DLL 从锁定 RadRay 源码构建并静态链接 `RadRay::runtime`，使用 ABI 13。构建身份包含实际源码、版本、dirty 状态与工具链；精确依赖版本由 `native/radray.lock.json` 维护。
Debug/MTd、Release/MT、静态 mimalloc 和加载契约见[互操作与加载](interop.md)。本地 source override 和测试入口见[构建与测试](../guide/build-test.md)。

## 适配层与驻留数据

`Renderer` 拥有 `TerrainResidency` 和双槽 `FrameScheduler`，调度器独占 `SolidFramePipeline`。创建时先准备几何池、shader/管线、槽资源、fence 和调度器，全部成功后交付 renderer。
管线继承 `RenderGraphFramePipeline`，不登记 mesh pass，不调用 `UseMeshPass`，也不依赖 Scene、handle、processor、缓存 MeshDrawCommand 或 runtime Material。
RadRay 保留通用 Scene/Mesh Drawing Pipeline；调度器只提供无参数 `RunFrame()`，输入由具体管线设置。

`TerrainState` 以资源 ID 索引 index、mesh、material 和按值 primitive。mesh 拥有顶点子分配、submesh 描述和预解析绘制模板：完整 VB/IB view、全掩码/有效位、全局 FirstIndex、VertexOffset 及相邻面组合并条件。
局部索引范围不能超过对应子分配；加子分配起点后的索引和顶点偏移必须分别可由 uint32/int32 表示。native 不保留索引 CPU 值，也不逐个扫描索引值。
primitive 缓存模板与材质绑定引用、完整掩码和层。反向索引 index→mesh、mesh→primitive、material→primitive 定位更新影响范围。
索引或网格更新刷新相关模板与 primitive 引用；材质更新刷新相关参数引用与层。纹理重建按保留的材质描述重新绑定。
可见循环对每条 ABI 项只查询一次 primitive，不逐 pass 查询 mesh、material 或命令缓存。重复项、输入顺序、面组掩码及 FirstInstance/InstanceCount 均保留；UINT32_MAX 归一化为完整掩码。
epoch 变更退役旧几何与材质并清空驻留状态。ABI 字节在 PrepareFrame 中复制到 staging/实例缓冲；保存的标量帧参数清空全部 ABI 数组指针。
PrepareFrame 失败不回滚已应用的驻留增量，宿主关闭 renderer 并禁用预览。

## 提交模式与参数

创建策略为 auto、direct、indirect。auto 仅在 `IndirectDraw`、非零 `IndirectFirstInstance` 均支持且 `MaxDrawIndirectCount>1` 时使用 indirect，否则使用专用 direct。
强制 indirect 缺少任何条件时创建失败，诊断说明原因；分配、映射、flush 或录制失败不静默切换模式。创建日志记录请求/实际模式、能力、原生记录上限和 auto 回退原因。
Java 枚举与配置入口见[地形预览](terrain-preview.md#绘制与-oit)，ABI flags 布局归[互操作与加载](interop.md#abi-13-布局)。

每帧每层使用紧凑可见记录。SOLID、CUTOUT、小波 OIT 使用 StateFirst，普通透明使用 DepthFirst；均稳定排序，相同完整排序键保留输入顺序。
primitive 缓存由材质版本与完整顶点 view 产生的 32 位压缩状态键，可见记录组合状态键和原始深度/顺序键。StateFirst 先比较状态，DepthFirst 先比较原始深度；索引 view 不作为附加排序键。
小列表使用稳定插入排序，大列表使用稳定基数排序并复用每槽 scratch。压缩键冲突保留输入顺序，批次兼容性仍检查完整 view 与参数组。
掩码按面组位序展开。只有连续、被选中、索引范围相接、顶点偏移兼容、三角形完整且单实例的面组可合并；普通透明禁用合并，多实例逐面组展开。
合并计数使用宽整数检查并在 uint32 上限前拆开；提交索引数乘实例数的总和溢出则准备失败。

参数构建输出一个连续 `DrawIndexedIndirectArguments` CPU 数组和指向区间的批次表。只有排序后相邻且材质参数组、完整 VB view、完整 IB view 均相同的记录才归为一批，不跨排序边界重排。
direct 遍历同一普通 CPU 参数数组调用 `DrawIndexed`；indirect 绑定批次状态，按原生上限拆分 `DrawIndexedIndirect`，参数字节偏移为记录下标乘 20。
OIT 的三次几何 pass 共用同一份透明排序、参数区间和批次；各自绑定对应 PSO 和 Group 2。普通透明使用自己的 DepthFirst/不合并规则。

Group 0 为每槽 256 字节 Upload CBuffer 与参数集。Group 1 为每个材质版本的图集/lightmap/sampler 参数集；Group 2 为每槽四套 OIT 参数集，命令存储回收后重置，并在 RDG 回调绑定当前瞬态 view。
`MeshPassResources` 拥有解码 artifact、shader、布局及八个 PSO；不构造 runtime Material。shader、纹理及 OIT 规则见[地形预览](terrain-preview.md)和[Shader pack](shader-pack.md)。

## 上传、帧槽与资源退役

| 对象 | 所有者与边界 |
| --- | --- |
| 设备、Direct 队列 | C bridge 持有环境与 DeviceContext；环境晚于设备销毁 |
| 管线、shader、布局、PSO | FrameScheduler 拥有管线；先回收所有槽的命令及退役资源，再销毁管线 |
| 驻留资源及模板/绑定引用 | TerrainResidency 持有；替换/删除时把旧 owner 交给当前 FrameSlot::RetireResource |
| 顶点/索引池与子分配 | 两池晚于驻留状态析构；页在 renderer 寿命内保留，子区间在 owner 退役后归还 |
| 可见记录、CPU 参数数组、批次表 | 每槽独立、清空后保留容量，仅在该槽 BeginFrame 完成等待和重置后写入 |
| staging、实例、indirect 上传缓冲、常量、OIT 参数和计时资源 | 每槽独立；GPU 完成且命令存储重置后才复用、扩容或释放 |
| 深度及 OIT 附件/view | RDG 每槽瞬态池按完整描述符复用，保留实际终态 |
| 共享输出/输入 | C bridge 子句柄持有，提交期间借用；关闭前完成宿主交接并排空槽 |

顶点池使用 28 字节 stride，默认页容量 `(32 MiB)/28`；索引池使用 uint32，默认页容量 300,000。整页 view 配合子分配起点与 submesh 偏移绘制，BestFitAllocator 复用连续空闲区间，超大请求使用大页。
新资源版本分配新区间，不覆写在途版本。旧 owner 在当前槽完整轮转或 WaitIdle 后释放；GPU 资源必须跨越 GPU 完成和全部相关命令 epoch 重置边界。

Renderer 先调用 BeginFrame，再 PrepareFrame，完成 ABI 复制、排序、参数构建、扩容、写入和 flush 后，才 SetInput 并调用 RunFrame；作用域清理 ClearInput。
外部输入在 RunFrame 返回前有效且只读。SubmitFrame 使用后清除 FrameData 的借用输入，仅保留原始序号、flags、完成值、计数和 timing mask；Wait、回收和析构不访问借用输入。
准备失败保留当前准备窗口和退役 owner；WaitIdle 取消准备。FrameData 由槽 arena 持有，不复制驻留状态。

每槽持有独立 MappedUploadBuffer，indirect 用 `Upload + Indirect`，记录步长 20 字节。indirect 参数扩容与上传独立于几何 staging 预算；direct 读取普通 CPU 数组。
参数和实例以 HostWrite 导入，消费 pass 分别声明 Indirect/Vertex 读取，并回到 HostWrite。常量由 RDG UploadBuffer 写入，几何由 BatchedBufferUpload.CommitToGraph 录制复制。
几何池保留全部页的读取声明，导入/终态为 Common。几何上传目标、已变化 split 私有纹理及共享输出标为图输出；空可见帧仍上传、交接和清屏。
共享 ownership acquire/release 由 RDG 录制，队列 timeline 等待/信号由 SubmitFrame 组织。
不开启 GPU 计时时提交图命令；开启时在图前后增加 timestamp/resolve 命令，一次队列提交包含三条命令，覆盖所有 native GPU 工作。

Wait 等待管线 completion 并读取槽内计时结果；样本携带原始 output sequence 发布。Finish 先以 5 秒超时检查 completion，再 WaitIdle；超时保留 owner。
Renderer 析构先 WaitIdle，再清空驻留状态，最后释放管线和调度器。共享输入销毁先排空槽，再释放纹理参数绑定；CPU 材质描述与几何保留。
帧回调失败通过 RADRAY_ABORT 诊断终止；C ABI 可失败准备继续使用 OK/FAILED 和调用方错误字符串契约。

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
地形 auto 模式按设备已启用能力选择 indirect 或专用 direct，不创建渲染工作线程、异步计算或额外提交队列；启用 profiler 时，Tracy 的采样线程由环境生命周期管理。
设备析构等待 Direct 队列，环境必须晚于设备及所有 GPU 资源销毁。native 不持有宿主窗口或交换链，最终呈现仍归 MC。

## 共享输出与输入

一个 renderer 同时允许一个 RenderTarget 和一个 SharedTextures 集合。
RenderTarget 持有 RGBA8_UNORM 共享颜色、其 attachment view、导出和 ready/consumed timeline；D32 深度及其 view 由 runtime 的每槽瞬态池持有。
首个地形 pass 清雾色、alpha=0 和 depth=0；不透明、镂空与普通透明使用 GreaterEqual，并启用背面剔除和深度写入。OIT 附件和绘制规则见[地形预览](terrain-preview.md)。
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
世界 epoch 切换清理驻留状态但保留设备会话。

关闭 target 同时确认 ready/consumed，关闭 inputs 确认 native 已释放，随后 Finish/WaitIdle 释放槽内引用，再销毁共享资源。
宿主先提交并完成自己的读写，关闭 Java 导入后才销毁 native 子句柄；超时保留句柄、Java owner 和 DLL。
父 owner 有活跃子 owner 时拒绝关闭。完整宿主顺序见[MC 接入](minecraft-integration.md#逐帧交接)，FFM 所有权见[互操作与加载](interop.md)。
