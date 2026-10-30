> - 适用: 2026-10-30 确认的 SOLID 可编程渲染 runtime 设计快照
> - 权威: 用户授权保存的实施计划；记录时尚未实现，不构成当前接口契约
> - 锚点: `native/CMakeLists.txt`, `native/include/raynium/bridge.h`, `native/src/renderer.cpp`

# 可编程渲染 Runtime

日期：2026-10-30。来源：本次会话中用户明确要求实施的计划，以及当时包含未提交修改的 Raynium 工作树。下述范围与取舍均由用户确认。实现及验证结果另由当前事实文档维护，本记录保持原样。

## 来源与目标

职责划分参考本机 Unreal Engine 5.8.4 源码：`Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h`、`MeshBatch.h`；`Renderer/Public/MeshPassProcessor.h`、`MeshPassProcessor.inl`；`Renderer/Private/PrimitiveSceneInfo.cpp`、`SceneVisibility.cpp`、`MeshDrawCommands.cpp`、`BasePassRendering.cpp`；`RenderCore/Public/RenderGraphBuilder.h` 和 `RenderCore/Private/RenderGraphBuilder.cpp`。这些源码支持“持久场景代理 → MeshBatch → pass processor → 缓存命令 → 当帧可见命令 → RDG → RHI”的职责分离。这里提取架构，不移植 UE 实现，也不以既有临时代码作为目标架构。

在 native 工程新增依赖 `RadRay::render` 的 `raynium_runtime` 静态 target；现有 `rayniumruntime` DLL 是 MC 适配层，保持文件名、加载方式和 ABI 8。runtime 位于 `native/runtime/`，公共接口使用 `raynium::runtime`，不引用 C bridge、MC program 枚举或后端实现类型。

首版保留 SOLID 共享小 RT，包括场景、Mesh Drawing Pipeline、C++ 可编程管线、单队列 raster/compute/copy RDG、资源管理和帧提交。排除 Java 管线配置、脚本、材质图、GPUScene、GPU 剔除、并行录制、异步计算、多队列、内存别名和窗口接管。

## 数据与职责

- Environment/DeviceContext 管理环境、设备、队列、能力和设备级资源。
- Scene 通过 SceneUpdate 暂存几何、材质、primitive 增删改；ViewSubmission 提供当前视图、可见项和实例范围。成功提交才发布增量。
- MeshPassProcessor 由 MC native 实现，描述 shader、布局、固定状态与绑定规则，不录制命令。MeshDrawCommand 持久缓存稳定管线、资源绑定与范围；PreparedMeshPass 组织当帧命令、排序合批并自动汇集几何、纹理、参数、实例与 indirect 资源访问。
- IRenderPipeline 在 renderer 创建时注入，由 C++ BuildGraph 构建资源和 pass。对应 PassContext 只解析已声明资源并录制 draw/dispatch/copy；runtime 负责 pass 开闭、屏障和提交。
- ResourceStore/FrameContext 管理驻留、上传、临时资源和完成前保活。Scene 与帧基础设施分离。
- shader 离线编译。PSO 身份包含 shader、布局、固定状态、attachment signature，不包含输出尺寸。

## 最小 RDG

提供 CreateTexture/CreateBuffer、ImportTexture/ImportBuffer、AddRasterPass/AddComputePass/AddCopyPass、AddMeshPass、输出标记、extraction、Compile 和可检查执行计划。句柄限当前图；持久命令不得保存图句柄。回调数据保活至录制结束，GPU 资源保活至完成。

整资源追踪，按声明顺序建立 RAW/WAR/WAW，读取依赖此前最近有效写入或导入内容。输出、extraction、外部资源写入与显式副作用是保留根，反向保留依赖；稳定顺序，不做覆盖写消除、激进重排或 pass 合并。校验未初始化读取、跨图句柄、不兼容访问、attachment、pass 类型及 load/store。

用途生成 buffer/texture 状态与 shader stage；连续 UAV 写依赖即使状态相同也生成 UAV 屏障。图内屏障统一生成。外部 acquire/release 是图边界，timeline wait/signal 是提交边界；空可见集仍清屏并交接，交接不能被剔除。

仅分配存活资源，计算使用区间，不做同图别名；GPU 完成后按完整描述符复用临时对象。extraction 向外部 owner 提供资源、最终状态与完成信息。诊断输出图结构、使用区间及屏障摘要。

## SOLID 与生命周期

生产图：导入持久几何、共享输入输出 → SceneUpload → 内容变化时 TextureAssemble → SolidRaster → 输出状态及外部交接。深度为图内临时资源。首版无生产后处理，compute 由独立测试管线验证。

MC 适配负责 28 字节顶点、quad 索引、面组、图集/lightmap、雾、淡入、RGSS、大坐标和上传预算；runtime 只消费声明布局和不透明参数。UV 只是 MC 测试策略。

几何、材质、管线、布局或资源身份变更使关联命令失效；相机、雾、位置、可见范围与排序键只改变帧数据。纹理内容更新与对象替换分开。direct/CPU indirect 共用 packets；能力不足回退 direct，不修改 RadRay。合批必须严格匹配几何、绑定与状态。

两帧槽持有分配器、参数、上传、实例和 indirect；复用前等待本槽 completion。编译、准备或录制失败不提交、不发布场景、不推进序号。内部 completion 与共享 timeline 分离；超时保留 owner。resize 重建输出，纹理重载更新绑定，世界 epoch 清理场景而保留设备。可失败 owner 采用 static Create + RAII，ABI 只返回 OK/FAILED 并写诊断。

## 实施与未执行验收计划

依次保存设计、建立静态库与类型边界、实现 CPU 图编译、资源池/执行/提交、接入 Mesh Drawing Pipeline 和 SOLID、完成测试与事实文档。

测试直接链接静态库，不依赖 bridge；覆盖缓存/失效/严格合批/失败事务/退役，RAW/WAR/WAW/剔除/副作用/非法读取与句柄/attachment，临时分配/完成前禁止复用/extraction/导入去重/最终状态；GPU 验证 raster→采样、copy→compute→copy、连续 UAV 写读及读回。替换测试管线即可添加中间纹理与 pass，无需修改执行器。

SOLID 回归覆盖双后端、两 mip 共享方式、direct/indirect、深度、大坐标、纹理变化、可见范围、连续帧、替换/卸载、resize、输入重建、epoch、关闭与超时。使用独立 Vulkan 消费者和程序化像素/缓冲读回，不使用 computer-use 或手动游戏操作，不将自动结果称为真实游戏交互验证。

维持 Windows x64、C++20、/MT、Debug/Release；构建身份覆盖 runtime；执行 native 测试、Release 打包、分发与文档检查。
