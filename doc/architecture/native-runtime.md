> - 适用: 修改 renderer、命令解码、帧槽、呈现与读回
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界
> - 锚点: `native/src/renderer.cpp`, `native/src/command_tape.cpp`, `native/src/modular_frame_pipeline.cpp`, `native/src/immediate_pass.cpp`, `native/src/terrain_feature.cpp`, `native/src/static_geometry_registry.cpp`, `native/src/world_features.cpp`, `native/radray.lock.json`

# Native runtime

## 设备与所有权

每个 runtime 拥有一个 RHI 设备和 Direct 队列。renderer 管理交换链、缓冲、纹理、常驻模型网格、管线及逻辑时间戳注册表。每个提交返回该 renderer 的单调完成序号。C 入口和设备操作在所属线程执行。当前依赖版本见锁文件。[互操作与加载](interop.md)

## 帧槽、上传与交换链

`rn_submit_frame` 接收命令、传输和实例字节、映射脏区间及地形视图。renderer 准备本批资源，按需获取交换链图像。`ModularFramePipeline` 向同一个 RDG 注册初始上传、光栅与拷贝、查询解析及可选上屏。离屏批次同样提交并取得序号。交换链需要重建时允许离屏工作完成。提交已上队而呈现失败时，结果保留序号和终态标志。

两个帧槽轮换。槽回收前等待实际完成值，已提交资源由槽或注册表的在途引用持有。关闭等待按有限时间片复查设备丢失状态。健康设备内部提交违约触发 fail-fast，丢失态由适配层传播。

## 即时绘制与地形

`ImmediatePass` 根据附件、mip、加载操作和 render area 建图，并在 Pass 级对 `TerrainVertices`、`TranslucentVertices`、`StaticVertices`、`QuadIndices`、`TranslucentIndices` 几何池读访问及共享实例和间接参数缓冲做单次去重声明。`ImmediateDraw` 保存实际管线、资源切片、纹理视图和绘制参数，在当前图形编码器上绑定并绘制；`ImmediateDraw::Prepare` 与 `ShaderResourceBindings::Prepare` 使用 `RasterProgram` 预计算的 64 位槽位掩码与栈上 `radray::InlineVector` 过滤并按 `BindingId` 升序组装参数绑定以命中 RadRay 有序快轨。录制期 `ImmediateDraw` 与 `InstancedGeometryFeature` 共享栈上 `ImmediatePassStateCache` 去重 PSO、Scissor、Stencil、同布局参数组与索引缓冲绑定；`TerrainFeature` 内联回放后立即重置该状态缓存，确保后续绘制完整恢复绑定。

`StaticGeometryRegistry` 在 16 MiB 的顶点页中保存内建四边形和用户网格，通过句柄直接索引。替换和删除的子分配由帧槽保活，槽回收后可释放整张空页。`InstancedGeometryFeature` 将连续同页的多个 run 合并为间接绘制，并按设备上限拆批。单 run 或设备缺少所需间接绘制能力时直接调用 `DrawIndexed`。

整个实例流作为一块借用的 RDG 顶点缓冲导入，同批 OIT 阶段共用实例偏移。纹理和设备缓冲上传也借用批次字节，写入宿主映射缓冲的内容由读回队列持有副本并按序发布。

## 提交模式与参数

`raynium.terrain.submission=auto|direct|indirect` 选择驻留地形的提交方式。每个视图从 `TerrainHotSlot` 构建分页绘制参数。Direct 和 Indirect 共用批次及代表命令，OIT 三个阶段共享透明层参数。[区块地形渲染](terrain-rendering.md)

## 映射、时间戳与读回

使用初始数据或命令内上传的缓冲按实际用途创建。可写持久映射按缓冲归并相交或相邻的脏区间。开放视图每批刷新，关闭视图的字节访问立即失效，已写区间保留到下一次提交。设备缓冲和持久写缓冲按用途和准确字节数复用，退役序号已经完成且槽引用释放后才能取得。待初始上传、宿主读回及超过 16 MiB 的缓冲直接退出注册表。退役池上限 64 MiB，闲置 120 次提交后淘汰。局部设备缓冲映射只回写所映射切片。纹理读回按设备 footprint 搬运，完成后向 Java 发布紧凑行。两个槽写入重叠宿主区间时按序号发布。颜色截图另可按请求编号轮询。

逻辑查询池与物理槽分离。RDG 在命令位置写入时间戳并保活解析，已完成结果带序号。基准显式启用整批 GPU 时间，在上屏批次完成后归并为一帧。等待、准备、RDG 与呈现的 CPU 时长独立记录。
