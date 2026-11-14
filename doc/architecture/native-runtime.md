> - 适用: 修改 MC native 适配、帧生命周期、专用地形提交及共享资源
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界；调度器、RHI 和通用 Mesh Drawing Pipeline 由 RadRay 维护
> - 锚点: `native/src/bridge.cpp`, `native/include/raynium/exception.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/solid_pipeline.cpp`, `native/src/frame_upload.cpp`, `native/src/frame_record.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`

# Native runtime

DLL 静态消费锁定的 `RadRay::runtime`，当前 ABI 17。精确依赖版本由锁文件维护；构建身份和加载见[互操作与加载](interop.md)。

## 设备与所有权

环境和设备会话直接使用 `radray::Environment` 与 `radray::DeviceContext`，不设置 Raynium 类型别名。
环境创建 DXGI factory 或 Vulkan instance，不创建设备。runtime 创建高性能适配器设备和一条 Direct 队列，不开启外部共享或匹配宿主 GPU。
设备信息来自普通 RHI DeviceDetail/Capabilities，包含 GPU 名称、厂商、后端原生驱动版本字符串及能力。
环境晚于设备和资源关闭；后端与 driver validation 在环境创建时固定，不自动切换后端。

Renderer 持有 TerrainResidency 和双槽 FrameScheduler；调度器独占 SolidFramePipeline。
创建先准备资源，全部成功后交付有效对象；C++ 不使用二段式初始化。
Renderer 析构先 WaitIdle，再清空驻留；设备析构等待 Direct 队列。

## 失败处理

可安全拒绝的输入、能力、shader 或容量错误，以及仍可安全清理的创建与准备失败，在发现处记录 ERROR 并返回失败。
内部函数不传递错误字符串；调用者只传播成功/失败，不重复记录同一原因。shader decoder 的诊断在消费结果处记录，包含程序名和目标后端。
提交模式选择函数记录实际模式及正常 auto fallback 原因；最小化、零客户区、获取超时和请求重建保持正常帧结果。

绘制列表与排序项数量矛盾、内部 payload 索引或绑定状态损坏、非法绑定释放、内部工厂输出已占用等不变量破坏，直接记录 fatal 并 abort；这些检查在 Debug/Release 均生效。
Finish 等待最后提交完成的时限为 5 秒，等待失败后 abort，不回收尚未确认完成的资源。可选 GPU 计时读取失败只记 WARN。
Raynium 的 C ABI 保护器、含可抛出操作的 noexcept 回调和渲染录制回调，在保护边界记录意外 C++ 异常后 abort，包括内存分配异常；不安装进程级 terminate handler。
日志复用 RadRay logger，ERROR/fatal 带源码位置；abort 终止整个宿主进程。跨语言状态与销毁约束见[互操作与加载](interop.md#当前-c-abi)。

## 地形驻留与绑定

Native 按 mesh ID 保存顶点子分配、实际 AABB、七组到紧凑元素位的映射、层、透明索引 owner、绘制模板和绑定状态编号。
网格上传时一次性完成分组/重排；算法及 Java 采集协议见[地形渲染](terrain-rendering.md)。
SOLID/CUTOUT 使用 Native 按需扩容的共享 Quad 索引，首次至少容纳 65,536 顶点。
扩容准备新子分配并上传，刷新所有相关模板与绑定；旧索引 owner 移入当前 FrameSlot::Emplace，避免覆盖在途版本。
透明排序索引按透明 mesh ID 独立更新；删除 mesh 同时退役顶点及透明索引。
驻留表按值保存网格、绘制模板和索引子分配；退役直接将值移动到帧槽。透明网格仅替换顶点时，将原索引子分配移给新网格，索引随后替换或删除时才退役；单 Direct 队列上的该次退役覆盖此前使用。

顶点池 stride 为 28，默认页容量 `(32 MiB)/28`；uint32 索引池默认每页 8,388,608 个索引。
BestFitAllocator 复用子区间；新版本使用新区间，旧 owner 在槽完整轮转或 WaitIdle 后归还。物理页在 renderer 寿命内保留。
整页 VB/IB view 加子分配 FirstIndex/VertexOffset 形成模板；使用宽整数检查偏移和计数可表示性。

图集、lightmap 和各向异性来自每帧全局参数。绑定按参数变化复用，版本身份和完整 VB/IB view 生成绑定状态编号。
材质绑定由 unique_ptr 独占以保持批次借用地址稳定；替换时将整个 unique_ptr 移入帧槽。
编号不截断、不编码物理页排序；最后引用释放后可复用。新帧校验纹理编号仍存活；在途旧参数及纹理由帧槽退役保活。
空可见帧允许只清屏、GUI 和上传，不要求地形纹理。

更新就地应用，准备失败不回滚，调用方关闭 renderer。epoch 变更退役旧网格、共享索引和材质绑定，再建立新驻留状态。

## 提交模式与参数

auto 仅在 IndirectDraw、IndirectFirstInstance 和 MaxDrawIndirectCount>1 时启用 indirect，否则使用 direct；强制模式缺能力时创建失败。
准备、映射或录制失败不会静默改变提交模式。

每槽独立保存可见 payload、16 字节排序项、稳定排序 scratch、普通 CPU DrawIndexedIndirectArguments 数组和批次。
SOLID/CUTOUT/OIT 使用 StateFirst；普通透明使用 DepthFirst。相同完整键保留输入顺序。
排序只移动键与下标；小列表插入排序，大列表基数排序。只合并排序后相邻、完整 view 和参数组一致的批次。
单实例、索引连续且被选中的面组可合并；普通透明和多实例不合并面组。溢出计数在准备期失败。
direct 遍历同一参数数组 DrawIndexed，indirect 按原生上限拆分调用，记录步长 20 字节。
OIT 三次透明几何 pass 共用一次参数构建及透明层资源声明。Group 2 在 RDG Prepare 绑定瞬态 view；槽内 RAII 清理在命令存储重置后解除绑定，早于图及 view 清理。

## 帧槽、上传与交换链

BeginFrame 等待该槽 completion，先重置命令存储，再逆序析构 Emplace 对象并重置线性 PMR。
旧网格、索引子分配和材质 owner 由槽对象保活；整页 GpuBuffer 的引用不代替子区间退役。
帧对象与重排分组数据使用槽内存；阶段局部去重容器使用 scratch PMR，不将其引用交给录制回调。
可见列表、排序 scratch、参数数组和批次按槽保留容量。

PrepareFrame 校验输入、计算分组和可见参数；同步 RunFrame 的 RDG Prepare 消费借用的 ABI 顶点、索引、实例与 GUI 顶点。
提交后清空借用输入；Wait、回收和析构不读取 Java 数组。准备失败由 Finish/WaitIdle 清理尚未提交的准备窗口。
几何页通过 GetGpuBuffer 与持久导入关联状态，边界为 Common；只向各层 pass 声明其实际批次使用的页和材质纹理。
地形顶点按已分析的分组与偏移直接重排到最终上传切片，共享 quad 索引直接生成；透明索引借用至 Prepare 消费结束。
上传目标显式标为图输出，隐藏窗口及空可见帧仍消费增量。持久资源终态在实际队列提交后由 RenderGraphFramePipeline 发布。

每槽 UploadArena 使用 8 MiB 页，统一承载几何 staging、纹理 staging、实例、indirect、常量及 GUI 动态切片。
页持久映射，槽完成并重置命令后复用；RDG 汇总 Flush 和共享页只读状态。大分配可使用独立大页，已有切片不搬移。
所有缓冲绑定和 indirect 地址使用 RDGBufferSlice 的物理 Buffer、Offset 与逻辑 Size；零大小切片不绑定或绘制，清屏仍执行。
Prepare 解析切片并更新参数集，常量绑定在物理页或范围改变时重写。
Record 只消费准备好的数据并编码命令，不创建资源、不上传、不写参数集；几何提交统计在构图期计算。
GPU 计时启用时，一次提交包含图前 timestamp、图命令和图后 resolve；样本在槽完成后发布。

交换链固定 BGRA8，申请三张后备缓冲，不持有预创建 View。
Renderer 先 BeginFrame 并校验、准备上传描述及 GUI，再检查窗口可呈现性，以 100 ms 获取后备缓冲。
后备缓冲按 Undefined → Present 导入；所有颜色 pass 使用同一 handle，RDG 自动创建并同帧复用 attachment view。
View owner 属于当前槽内 RDG 图，等待 GPU 且重置命令存储后才销毁。
已获取帧沿用 RunFrame fail-fast 契约，并随后 Present，不遗留获取令牌。

resize/destroy 先 Finish（含帧槽 WaitIdle、命令重置和 View 销毁），再等待 Direct 队列，最后重建/销毁交换链。
队列等待覆盖帧 completion signal 之后排队的 Present。窗口 HWND 由宿主持有。
OK 的隐藏/最小化/获取超时帧仍消费上传；FAILED 后不得在原 renderer 重试，结果缓冲保持原值。

## 上传纹理

`UploadedTextures` 属于 renderer，以 16 及以上的 uint32 编号管理 RGBA8/R8 GpuTexture 及已用编号集合。世界 epoch 不影响纹理表。
GpuTexture 拥有纹理和完整 mip 采样 view；提交状态只表示单队列交接状态，不表示全部区域已初始化或 GPU 已完成。
上传入口校验 mip、区域、行距和字节容量，立即复制有效像素；构图时将已有字节数组移给 OwnedBytes，不额外复制快照。
RDG Prepare 负责源行距、staging pitch/placement 对齐与最终映射写入，自动上传节点负责复制与屏障，图末恢复 ShaderRead。
拥有式导入在图登记时保活目标，包含已删除编号但仍待上传的纹理，直至 GPU 完成且命令已重置。
纹理表按值保存描述和 GpuTexture owner，查找返回仅在同步准备期间借用的指针。待上传项、材质绑定和 GUI 参数缓存直接持有 RadRay API 的 IntrusivePtr<GpuTexture>，不再包装共享的纹理元数据；删除编号后在途使用继续保活，新帧查找该编号失败。
纹理编号 0–15 保留，材质和带纹理的 GUI 只接受 16 及以上的上传编号。未上传的像素内容未定义，由调用方初始化需要采样的区域和 mip。

## GUI pass

GuiPassResources 独立拥有三个程序、布局、采样器和十二种混合状态的 BGRA8 输出 PSO。
每槽 GuiFrameSlot 复用绘制列表及参数集；纹理参数缓存持有纹理 owner 和 sampler 身份，内容变化才重写，未使用的缓存解除绑定并释放 owner。
绘制记录和图导入借用该槽缓存保活的 GpuTexture；该槽 GPU 工作完成、命令与图数据重置后，才更新或释放缓存中的旧 owner。
获取后备缓冲前验证完整四边形范围、纹理编号、采样器位、逻辑尺寸与裁剪，并准备纹理参数集。
GUI 顶点借用到 RDG Prepare；quad 索引及普通/灰度文字两份常量直接生成到动态切片，常量偏移由 RDG 对齐。
native 按后端设置 GUI 的 Y 方向。帧描述中的 clear_color 决定输出清屏色。
GUI 在普通地形或 OIT 合成之后单独加入无深度 raster pass，仅声明自身使用的纹理，按输入顺序绘制；逻辑裁剪缩放到物理输出。
参数集先重置绑定，再释放旧纹理 owner；关闭时参数集先于 owner 销毁。常量参数集先于所属 UploadArena 关闭。
