> - 适用: 修改 MC native 适配、帧生命周期、专用地形提交及共享资源
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界；调度器、RHI 和通用 Mesh Drawing Pipeline 由 RadRay 维护
> - 锚点: `native/src/bridge.cpp`, `native/include/raynium/exception.h`, `native/src/renderer.cpp`, `native/src/terrain_residency.cpp`, `native/src/terrain_draw.cpp`, `native/src/solid_pipeline.cpp`, `native/src/frame_upload.cpp`, `native/src/frame_record.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`

# Native runtime

DLL 静态消费锁定的 `RadRay::runtime`，当前 ABI 18。精确依赖版本由锁文件维护；构建身份和加载见[互操作与加载](interop.md)。

## 设备与所有权

环境和设备会话直接使用 `radray::Environment` 与 `radray::DeviceContext`，不设置 Raynium 类型别名。
环境创建 DXGI factory 或 Vulkan instance。`DeviceContext` 按高性能偏好选择适配器，创建设备和一条 Direct 队列。Raynium 不匹配宿主 GPU，也不调用 `Device::CreateExternalResourceExtension()` 创建外部资源扩展。
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

BeginFrame 等待该槽 completion，重置命令分配器，随即由 FrameSlot 的参数池解除上一轮绑定。之后才逆序析构 Emplace 对象并重置帧内存。
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
RDG Prepare 按物理缓冲、偏移和范围构造常量绑定。FrameSlot 以共享布局及完整绑定内容去重，同槽本帧的相同参数共享只读参数集。
Record 只消费准备好的数据并编码命令，不创建资源、不上传、不写参数集；几何提交统计在构图期计算。
GPU 计时由 RenderGraphFramePipeline 在图命令缓冲内录制。槽的提交 fence 完成后，运行时发布带原始 output_sequence 的纳秒快照。
仅设置 MEASURE 时录制整帧首尾时间戳。PROFILE 同时测量存活 pass，地形阶段按名称累计 120 个样本后输出分位数。两个标志皆空时关闭采样，仅请求 MEASURE 的调用返回 gpu_sequence 与 gpu_nanoseconds。

交换链固定 BGRA8，申请三张后备缓冲，不持有预创建 View。
Renderer 先 BeginFrame 并校验、准备上传描述及 GUI，再检查窗口可呈现性，以 100 ms 获取后备缓冲。
每槽持有可采样、可读回的 RGBA8 主颜色目标，地形与 GUI 写入该目标；OutputPassResources 以无采样器的 Load 输出到 BGRA8 后备缓冲。
后备缓冲按 Undefined → Present 导入；RDG 自动创建 attachment view。
输出 pass 在 RDG Prepare 解析主颜色采样视图，从槽参数池获取匹配布局和该视图的参数集。
后备缓冲的附件 view 属于当前槽的图。GPU 完成并重置命令、解除参数绑定之后，图释放该 view。
已获取帧沿用 RunFrame fail-fast 契约，并随后 Present，不遗留获取令牌。

resize/destroy 先 Finish（含帧槽 WaitIdle、命令重置和 View 销毁），再等待 Direct 队列，最后重建/销毁交换链。
队列等待覆盖帧 completion signal 之后排队的 Present。窗口 HWND 由宿主持有。
OK 的隐藏/最小化/获取超时帧仍消费上传；FAILED 后不得在原 renderer 重试，结果缓冲保持原值。

## 上传纹理

`TextureRegistry` 属于 renderer，以 16 及以上的 uint32 编号管理 ABI 描述、GpuTexture 强引用及已用编号集合。世界 epoch 不影响纹理表。
颜色格式包括 RGBA8/R8/RGBA16F/RGBA32F；可创建 D32F 深度附件，但不支持 CPU 深度上传。
GpuTexture 持有底层纹理及采样视图。ABI 的附件、读回和立方体标志映射到显式用途和维度。首次采样或局部写入新的单层单 mip 二维颜色附件前，图插入一次透明黑整面清屏。
提交状态不表示全部区域已初始化或 GPU 已完成。上传入口校验数组层、mip、区域、行距和字节容量，立即复制有效像素；构图时将已有字节数组移给 OwnedBytes，不额外复制快照。
RDG Prepare 负责源行距、staging pitch/placement 对齐与最终映射写入，自动上传节点负责复制与屏障，图末恢复 ShaderRead。
拥有式导入在图登记时保活目标，包含已删除编号但仍待上传的纹理，直至 GPU 完成且命令已重置。
所有维度均通过 RDGBuilder::UploadTexture 直接写入指定层与 mip。立方体面沿用 ABI 的层编号，上传目标显式标为图输出。
待上传项、材质和待绘制记录持有 IntrusivePtr<GpuTexture>。删除编号后，新的查找失败，已登记的图及截图请求继续保活原纹理。
纹理编号 0–15 保留。除上述首次透明清屏的附件外，调用方需初始化将要采样的区域和 mip。
地形及 GUI shader 接受单层颜色 2D 纹理。场景程序也可采样立方体。完整视图及指定 mip 范围的视图由 GpuTexture 提供，采样器由 Device 按描述符复用。

## 按请求读回

登记截图时检查待入图表及 AsyncReadbackQueue，拒绝重复 ID，同时最多保留 4096 个请求。指定纹理和上一帧颜色在登记时取得强引用。AcquireNext 前预留读回缓冲，后续准备失败时取消未提交预留。构图时先将来源以 ImportPersistentOwned 移交 RDG，再登记拷贝。当前帧截图在 GUI 之后、交换链输出之前，上一帧截图在清屏之前，世界截图在第一人称之后、轮廓与后处理之前。指定层和 mip 在上传及离屏更新后复制。
队列以本次提交的 completion fence 关联已入图请求。待入图请求可立即轮询，返回未就绪。空目标指针可重复查询尺寸，非空但容量不足的调用失败并保留请求。完成后成功复制才消费请求，队列负责刷新映射和移除行距填充。
普通帧不创建读回资源、不读取像素。隐藏窗口上的截图请求仍执行所需离屏绘制与复制；关闭等待在途工作后回收未消费截图。

## GUI pass

GuiPassResources 持有三个程序、共享绑定组布局和十二种混合状态的 RGBA8 输出 PSO。
GuiFrameSlot 保存本帧绘制记录。每条贴图绘制记录保活 GpuTexture，并保存由纹理和设备分别提供的 mip view 与 sampler。
每个 GUI pass 以拥有式导入声明所读纹理。Prepare 从槽池获取本段常量与纹理参数集。
获取后备缓冲前验证四边形范围、纹理编号、采样器位、逻辑尺寸与裁剪，并解析持久采样视图和采样器。
GUI 顶点借用到 RDG Prepare；quad 索引及普通/灰度文字两份常量直接生成到动态切片，常量偏移由 RDG 对齐。
native 按后端设置 GUI 的 Y 方向。帧描述中的 clear_color 决定输出清屏色。
GUI 在普通地形或 OIT 合成之后加入无深度 raster pass，仅声明自身使用的纹理，按输入顺序绘制；逻辑裁剪缩放到物理输出。末地传送门/折跃门 GUI 使用原版场景程序，有序标记把平面 GUI 拆段并在对应位置插入场景 pass，保留模糊边界；各段持有独立、按槽复用的绘制列表，特殊 GUI 使用独立深度。
命令存储重置后，槽池统一解绑 GUI 参数集，之后释放图及其持有的资源。

## 场景绘制

GpuTexture 按维度、格式、子资源范围和用途缓存视图，视图的寿命受纹理所有者约束。
ScenePassResources 按完整原版管线编号及离屏/GUI 反射方向缓存 PSO，按 shader 程序复用 shader 和 binding group layout。离屏投影反射与正面绕序一起切换，保留物品正面的法线和光照。几何、常量和绑定经一组帧表提交，资源创建和参数写入发生在准备阶段。
SceneFrameSlot 准备几何与纹理描述。每个存活 pass 持有各自的参数集借用，Prepare 从同一帧槽按完整绑定内容获取，支持同一深度拷贝管线在一帧内读取不同来源。
RDG 按实际引用声明各 blob 的顶点、索引或常量用途，借用至同步 Prepare 消费；未引用 blob 不上传。几何块不携带无关的常量用途与范围限制。quad 与三角扇索引直接写入上传区间。Prepare 解析 Buffer/Offset/Size，Record 只绑定与编码。各阶段只声明实际批次的纹理。
持久场景几何由 ScenePassResources 注册表持有 GpuBuffer，独立上传入口复制为 Owned 来源；隐藏帧仍执行 MarkOutput 上传。帧槽持有绘制引用，RDG 持有上传引用，注册表删除不使在途缓冲失效。状态由实际提交后的 RDG 发布。天空与标题全景顶点、索引不逐帧重传；世界边界沿用原版失效条件更新顶点。
普通场景使用主 RGBA8/D32 目标并保留输入顺序；always-on-top 与第一人称阶段分别清理深度。存在世界后处理时，两阶段各用独立深度并按原版 INTEGRATE_DEPTH 规则将非零像素写回主深度，各有独立参数集。没有世界后处理时直接清理主深度。动态对象与地形共享 OIT 范围、系数和累积目标，即使没有透明地形也执行动态 OIT 合成。
普通透明在地形透明层之前把水体遮罩写入主深度。OIT 通过原版深度 blit 将主深度写入独立地形深度，再绘制遮罩；范围裁剪修改主深度后重复这一过程。动态 OIT 使用主深度，遮罩仅影响地形。仅写深度的原版管线移除颜色附件；这也避免向锁定 RadRay 的 Vulkan 颜色掩码转换传入未覆盖的零值。云 OIT 使用独立深度，见下文。
OIT 多次绘制复用借用几何和前缀索引；所有 view、切片和参数在 Prepare 解析，Record 仅编码。管线声明绑定和编译产物实际绑定分别验证，允许编译器消除已声明但未使用的常量块或整组纹理，拒绝未声明编号。只有实际保留的图内纹理绑定才准备对应采样器和参数绑定，覆盖 additive 透射率阶段被编译器消除的采样输入。

场景离屏表按连续绘制范围更新持久采样纹理。每个更新使用池内 RGBA8/D32 tile，整 tile 清屏后绘制，再复制到 atlas 或画中画缓存的指定区域；未更新区域和后续帧复用原有内容。图内更新显式 MarkOutput，窗口隐藏时仍执行。新建单层颜色附件首次导入时整面清屏，满足 D3D12 首次使用初始化要求，状态仅在提交后发布。
云面使用持久 R8_SINT texel buffer，参数池按绑定位置、缓冲、格式与范围区分绑定。RDG 声明 ShaderRead。云 OIT 在裁剪后复制主深度，随后在独立云深度上执行 bounds、transmittance、accumulate。

内置后处理从链表及 std140 常量描述构建 RDG。槽内瞬态池提供临时颜色目标，场景程序持有 PSO，设备共享采样器。Prepare 按切片与采样视图获取参数集，Record 编码全屏绘制。实体轮廓的 sobel、模糊、合成与 GUI 模糊共用这条路径，GUI 两个范围共享一次顶点、索引和常量上传。
