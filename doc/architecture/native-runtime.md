> - 适用: 修改 MC native 适配、帧生命周期、专用地形提交及共享资源
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界；调度器、RHI 和通用 Mesh Drawing Pipeline 由 RadRay 维护
> - 锚点: `native/src/bridge.cpp`, `native/include/raynium/exception.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`

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
扩容准备新子分配并上传，刷新所有相关模板与绑定；旧索引交给当前 FrameSlot::RetireResource，避免覆盖在途版本。
透明排序索引按透明 mesh ID 独立更新；删除 mesh 同时退役顶点及透明索引。

顶点池 stride 为 28，默认页容量 `(32 MiB)/28`；uint32 索引池默认每页 8,388,608 个索引。
BestFitAllocator 复用子区间；新版本使用新区间，旧 owner 在槽完整轮转或 WaitIdle 后归还。物理页在 renderer 寿命内保留。
整页 VB/IB view 加子分配 FirstIndex/VertexOffset 形成模板；使用宽整数检查偏移和计数可表示性。

图集、lightmap 和各向异性来自每帧全局参数。绑定按参数变化复用，版本身份和完整 VB/IB view 生成绑定状态编号。
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
OIT 三次透明几何 pass 共用一次参数构建；Group 2 在命令存储回收后重置，再绑定当帧瞬态 view。

## 帧槽、上传与交换链

BeginFrame 等待该槽 completion，重置命令存储、退役 owner 和 arena，然后才写入该槽上传/实例/indirect/常量/GUI/计时资源。
PrepareFrame 同步复制 ABI 字节并构建参数，RunFrame 完成录制及队列提交；提交后清空保存的借用输入。
Wait、回收和析构不读取 Java 数组。准备失败由 Finish/WaitIdle 清理尚未提交的准备窗口。

几何上传通过 BatchedBufferUpload.CommitToGraph，实例与 indirect 上传按 HostWrite 导入并恢复 HostWrite。
几何池边界为 Common。纹理和几何上传目标标为图输出，隐藏窗口及空可见帧仍消费增量。
GPU 计时启用时，一次提交包含图前 timestamp、图命令和图后 resolve；样本在槽完成后发布。

交换链固定 BGRA8，申请三张后备缓冲，不持有预创建 View。
Renderer 先 BeginFrame 并准备上传/GUI，再检查窗口可呈现性，以 100 ms 获取后备缓冲。
后备缓冲按 Undefined → Present 导入；所有颜色 pass 使用同一 handle，RDG 自动创建并同帧复用 attachment view。
View owner 属于当前槽 arena，等待 GPU 且重置命令存储后才销毁。
已获取帧沿用 RunFrame fail-fast 契约，并随后 Present，不遗留获取令牌。

resize/destroy 先 Finish（含帧槽 WaitIdle、命令重置和 View 销毁），再等待 Direct 队列，最后重建/销毁交换链。
队列等待覆盖帧 completion signal 之后排队的 Present。窗口 HWND 由宿主持有。
OK 的隐藏/最小化/获取超时帧仍消费上传；FAILED 后不得在原 renderer 重试，结果缓冲保持原值。

## 上传纹理

`UploadedTextures` 属于 renderer，以 16 及以上的 uint32 编号管理 RGBA8/R8 纹理、完整 mip 视图及已用编号集合。世界 epoch 不影响纹理表。
上传入口校验 mip、区域、行距和字节容量，立即把有效像素复制到 native 暂存；下一帧在获取后备缓冲之前按 RHI 行距和放置对齐写入当前槽的 MappedUploadBuffer。
纹理拷贝作为 RDG Copy pass 执行，写入资源标为输出，因此隐藏窗口和空可见集仍执行上传；纹理边界从 Undefined 或 ShaderRead 到 ShaderRead。
每槽保存拷贝资源的 shared owner，直到该槽 GPU 完成且命令已重置。材质绑定分别持有上传图集与光照图的 owner；删除编号后在途绑定继续保活，新帧查找该编号失败。
纹理编号 0–15 保留，材质和带纹理的 GUI 只接受 16 及以上的上传编号。未上传的像素内容未定义，由调用方初始化需要采样的区域和 mip。

## GUI pass

GuiPassResources 独立拥有三个程序、布局、采样器和十二种混合状态的BGRA8 输出 PSO。每槽 GuiFrameSlot 持有顶点、四边形索引、常量、纹理参数集及本帧纹理 owner。
准备发生在获取后备缓冲之前：验证完整四边形范围、纹理编号、采样器位、逻辑尺寸与裁剪，复制顶点并生成索引，准备纹理参数集。
常量的两个对齐区域分别提供普通与灰度文字模式，native 按后端设置 GUI 的 Y 方向。帧描述中的 clear_color 决定输出清屏色。
GUI 在普通地形或 OIT 合成之后单独加入无深度 raster pass，按输入顺序绘制；逻辑裁剪缩放到物理输出。
参数集先重置绑定，再释放上一帧纹理 owner；关闭时也保持此顺序，避免销毁仍被绑定的 view。
