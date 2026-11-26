> - 适用: 修改 renderer、命令解码、帧槽、呈现与读回
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界
> - 锚点: `native/src/render_channel.cpp`, `native/src/renderer_frontend.cpp`, `native/src/batch_envelope.cpp`, `native/src/renderer.cpp`, `native/src/command_tape.cpp`, `native/src/modular_frame_pipeline.cpp`, `native/src/immediate_pass.cpp`, `native/src/terrain_feature.cpp`, `native/src/static_geometry_registry.cpp`, `native/src/world_features.cpp`, `native/radray.lock.json`

# Native runtime

## 设备与所有权

每个 runtime 拥有一个 RHI 设备和 Direct 队列。前端线程管理不透明资源句柄，直接创建纹理、缓冲、管线和逻辑时间戳槽。初始上传与待移除网格随下一个批次交给 Raynium Render 线程。该线程创建和销毁 Environment、设备与交换链。几何页分配、窗口操作与排空在 worker 上同步执行。前端在等待期间派发 Win32 同步窗口消息。启用 Tracy 时，前端在线程创建前启动 profiler，worker 进入环境创建后登记名称 `Raynium Render`。前端等待 worker 退出后关闭 profiler，随后可卸载 DLL。当前依赖版本见锁文件。[互操作与加载](interop.md)

## 帧槽、上传与交换链

`rn_submit_frame` 接收命令、传输和实例字节、映射脏区间及地形视图。前端在提交入口仅做 $O(1)$ 顶层描述符与数组边界检查、映射脏区间处理及截图/查询登记，不预扫命令流与视图元素；随后将批次写入交替使用的两个带 PMR 的 `BatchEnvelope`（非借用路径按 8 字节对齐复制命令与上传字节），通过 `RenderChannel` 的原子指针入队并分配序号。worker 取走指针即释放队列容量，并仅在存在前端等待者时触发 `progress` 事件。冷路径通过独立的同步屏障在已入队的批次之后执行。worker 在 `BeginFrame` 之后由 `TapeCompiler` 与 `TerrainResidency` 单遍校验并编译命令流、视图以及截图、映射读回和时间戳资源；任一阶段在提交前失败时通过 `CancelUnsubmitted` 归还未提交的截图暂存缓冲，成功后才申请交换链图像。`ModularFramePipeline` 向同一个 RDG 注册初始上传、光栅与拷贝、查询解析及可选上屏。离屏批次同样提交并取得序号。交换链需要重建时允许离屏工作完成。worker 在 RunFrame 完成并清除输入指针后发布消费和提交水位，再进行呈现。表面丢失或重建失败时保留 RHI 交换链，跳过该表面的获取和呈现。下一次有效 resize 在同一对象上调用 Recreate，成功后恢复呈现。

两个帧槽轮换。槽回收前等待实际完成值，已提交资源由槽或注册表的在途引用持有。前端直接查询硬件 Fence，完成水位以已提交序号及首个待发布读回批次的前一序号为上界。阻塞等待先等目标批提交，再以最多 2 ms 的时间片等待硬件并派发同步窗口消息。查询或映射读回发布后才解除该批的水位上限。worker 仅在有待采集的查询或映射读回时定时唤醒。关闭等待按有限时间片复查设备丢失状态。健康设备内部提交违约触发 fail-fast，丢失态由适配层传播。

## 即时绘制与地形

`TapeCompiler` 在帧槽内生成 `FrameTapePlan`。`FrameOp` 将不大于指针宽度的平凡载荷（如时间戳索引）直接内联存储，其余操作对象分配在帧槽内；`ImmediateDraw` 与 `ShaderResourceBindings` 在帧槽仅保存 16 B/项的紧凑资源句柄条目并原位借用命令流中的 48 B 绑定字节，Push Constant 直接借用批次字节，需要补零时在槽内分配。`RasterPassPlan` 可将全幅清屏折入下一次光栅 Pass 对同一纹理、mip 和层的全幅附件加载操作。中间的纯缓冲写入和缓冲间复制保持原序，允许清屏继续等待合并。纹理传输、读回纹理、时间戳、部分清屏及含模板的深度清屏都会先执行待合并的清屏。上屏全屏绘制以 `DontCare` 加载后备缓冲。编译器根据附件和 render area 建图，并在 Pass 级对 `TerrainVertices`、`TranslucentVertices`、`StaticVertices`、`QuadIndices`、`TranslucentIndices` 几何池读访问及共享实例和间接参数缓冲做单次去重声明。`ImmediateDraw` 保存实际管线、资源切片、纹理视图和绘制参数，在当前图形编码器上绑定并绘制；`ImmediateDraw::Prepare` 与 `ShaderResourceBindings::Prepare` 使用 `RasterProgram` 预计算的 64 位槽位掩码与以栈缓冲为初始存储的 PMR 绑定列表过滤并按 `BindingId` 升序组装参数绑定以命中 RadRay 有序快轨。录制期 `ImmediateDraw` 与 `InstancedGeometryFeature` 共享栈上 `ImmediatePassStateCache` 去重 PSO、Scissor、Stencil、同布局参数组与索引缓冲绑定；`TerrainFeature` 内联回放后立即重置该状态缓存，确保后续绘制完整恢复绑定。

`StaticGeometryRegistry` 在 16 MiB 的顶点页中保存内建四边形和用户网格，通过句柄直接索引。替换和删除的子分配由帧槽保活，槽回收后可释放整张空页。`InstancedGeometryFeature` 将连续同页的多个 run 合并为间接绘制，并按设备上限拆批。单 run 或设备缺少所需间接绘制能力时直接调用 `DrawIndexed`。

世界快轨实例流作为一块借用的 RDG 动态顶点缓冲导入，超过 64 KiB 时使用 `HostOnly`，其余使用 `PreferDevice`，同批 OIT 阶段共用实例偏移。每槽 `StagingBufferPool` 承载复制源与上传，`DynamicBufferArena` 承载管线直读切片。纹理和设备缓冲上传也借用批次字节，写入宿主映射缓冲的内容由读回队列持有副本并按序发布。

## 提交模式与参数

`raynium.terrain.submission=auto|direct|indirect` 选择驻留地形的提交方式。每个视图从 `TerrainHotSlot` 构建分页绘制参数。Direct 和 Indirect 共用批次及代表命令，OIT 三个阶段共享透明层参数。[区块地形渲染](terrain-rendering.md)

## 映射、时间戳与读回

围栏轮转的瞬态块、可写环缓冲与暂存顶点源直接写入持久映射的上传内存，前端提交批次时刷新映射区间。纯复制源选择系统内存 `HostUpload`。管线直读的轮转缓冲选择 `DeviceUploadWithFallback`，由 RadRay 按可见显存能力和 85% 预算阈值决定物理内存域。RDG 通过 `ImportPersistentOwned` 持有统一的 `GpuBuffer` 至帧槽退役。设备缓冲使用 `DeviceLocal`，GPU 读回暂存使用 `HostReadBack`。瞬态 GPU 块内切片偏移对齐到请求对齐与主机缓存原子大小的最小公倍数。普通可写映射将脏区间冻结为包内字节，再上传至设备缓冲。可写持久映射按缓冲归并相交或相邻的脏区间。持久映射与主机读回缓冲通过 `MemoryUtil` 直接返回原生视图，不再逐次开闭子 `Arena`；瞬态写入视图直接登记脏区间且关闭为空操作。普通可写缓冲保持开放视图跨提交持续追踪脏区间，关闭后已写区间保留到下一次提交。仅无原生映射地址的设备缓冲可写回退路径在映射期使用独立暂存 `Arena`，关闭时通过 `writeBuffer` 提交并释放。设备缓冲和持久写缓冲按用途和准确字节数复用，退役序号已经完成且槽引用释放后才能取得。宿主读回及超过 16 MiB 的缓冲不进入退役池。退役池上限 64 MiB，闲置 120 次提交后淘汰。局部设备缓冲映射只回写所映射切片。GPU 读回完成后，worker 使映射缓存失效并按批次发布。前端在同步点将已完成的行直接复制到仍存活的宿主映射，重叠区间按提交顺序覆盖。尚有宿主发布引用的暂存缓冲在槽重用时另行分配。颜色截图直接复用上游 `radray::AsyncReadbackQueue`（上限 4096 项，单把互斥锁保护跨线程登记、预留、入图、取消与轮询），查询就绪状态不消耗截图且不额外分配临时 `Arena`，就绪后单次失效缓存并逐行解包到调用方目标同时回收暂存缓冲；设备丢失时轮询直接返回失败。

逻辑查询池与物理槽分离。RDG 在命令位置写入时间戳并保活解析，槽中暂存缓冲在创建时映射。worker 在缓存失效成功后发布结果，只有序号仍与前端逻辑槽匹配的时间戳才能写回。基准显式启用整批 GPU 时间，按入队序号归入对应 Java 帧，全部测量批次都具有 GPU 样本时才追加整帧 GPU 耗时。等待、准备、RDG 与呈现的 CPU 时长独立记录。帧槽退役时确定 GPU 样本是否有效，并按提交顺序发布遥测。
