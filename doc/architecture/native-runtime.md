> - 适用: 修改 renderer、命令解码、帧槽、呈现与读回
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费与所有权边界
> - 锚点: `native/src/renderer.cpp`, `native/src/command_tape.cpp`, `native/src/modular_frame_pipeline.cpp`, `native/src/immediate_pass.cpp`, `native/src/terrain_feature.cpp`, `native/radray.lock.json`

# Native runtime

## 设备与所有权

每个 runtime 拥有一个 RHI 设备和 Direct 队列。renderer 管理交换链、缓冲和纹理注册表、管线及逻辑时间戳池。每个提交返回该 renderer 的单调完成序号。C 入口和设备操作在所属线程执行。当前依赖版本见锁文件。[互操作与加载](interop.md)

## 帧槽、上传与交换链

`rn_submit_frame` 接收命令、上传字节、映射脏区间及地形视图。renderer 准备本批资源，按需获取交换链图像。`ModularFramePipeline` 向同一个 RDG 注册初始上传、光栅与拷贝、查询解析及可选上屏。离屏批次同样提交并取得序号。交换链需要重建时允许离屏工作完成。提交已上队而呈现失败时，结果保留序号和终态标志。

两个帧槽轮换。槽回收前等待实际完成值，已提交资源由槽或注册表的在途引用持有。关闭等待按有限时间片复查设备丢失状态。健康设备内部提交违约触发 fail-fast，丢失态由适配层传播。

## 即时绘制与地形

`ImmediatePass` 根据附件、mip、加载操作和 render area 建图。`ImmediateDraw` 保存实际管线、资源切片、纹理视图和绘制参数，在当前图形编码器上绑定并绘制。`TerrainFeature` 在同一 Pass 声明依赖，并使用同一编码器回放驻留几何。后续即时绘制重新应用完整绑定。

## 提交模式与参数

`raynium.terrain.submission=auto|direct|indirect` 选择驻留几何的提交方式。模板、排序、可见面组与按槽上传分别由地形驻留、参数生成和帧录制代码管理。普通透明保留顺序，OIT 各阶段读取该次地形命令携带的视图。[区块地形渲染](terrain-rendering.md)

## 映射、时间戳与读回

使用初始数据或命令内上传的缓冲按实际用途创建。可写持久映射提交时刷新，局部设备缓冲映射只回写所映射切片。纹理读回按设备 footprint 搬运，完成后向 Java 发布紧凑行。两个槽写入重叠宿主区间时按序号发布。颜色截图另可按请求编号轮询。

逻辑查询池与物理槽分离。RDG 在命令位置写入时间戳并保活解析，已完成结果带序号。基准显式启用整批 GPU 时间，在上屏批次完成后归并为一帧。等待、准备、RDG 与呈现的 CPU 时长独立记录。
