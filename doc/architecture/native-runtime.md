> - 适用: 修改 MC native 适配、设备会话、共享资源及关闭顺序
> - 权威: 本文维护 Raynium 对 RadRay runtime 的消费边界；通用场景/RDG 契约由 RadRay 维护
> - 锚点: `native/include/raynium/environment.h`, `native/include/raynium/runtime.h`, `native/src/renderer.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/src/render_resources.cpp`, `native/CMakeLists.txt`

# Native runtime

`rayniumruntime` DLL 消费上游 `RadRay::runtime` 静态库；公共头使用 `<radray/runtime/...>`，类型直接位于 `radray`。
Raynium 不再定义通用 runtime target 或保留 `native/runtime` 源码副本；本仓库的 MC 适配类型仍位于 `raynium`。
DLL 文件名、加载方式与 ABI 8 不变。依赖版本由 `native/radray.lock.json` 锁定。
通用场景、RDG、资源池与帧提交的权威说明在 RadRay 的 `docs/architecture/runtime.md`。
本项目验证见[构建与测试](../guide/build-test.md#当前验证范围)，MC 数据见[地形预览](terrain-preview.md)，跨语言寿命见[互操作与加载](interop.md)。

## 适配层职责

`Renderer::Render` 将 C ABI 帧数据转换为 `radray::SceneUpdate` / `ViewSubmission`，并保持这些借用数据直到本帧录制完成。
创建 renderer 时注入 `SolidRenderPipeline`；其 Processor 是 MC 的 `SolidPassProcessor`。
processor 提供 shader、布局、固定状态与资源绑定规则；BuildGraph 声明输入装配、颜色/深度 attachment 和 mesh pass。
图的 pass 开闭、屏障、参数保活、两帧槽复用、completion 和场景事务由 RadRay runtime 管理。

28 字节顶点、quad 索引、面组、图集/lightmap、雾、淡入、RGSS、大坐标和上传预算属于适配层。
UV 是适配层的测试 program；runtime 没有 UV/SOLID 分支。MC ABI 不开放 Java pass 配置、脚本或材质图。
纹理内容变化只触发内容装配，绑定对象身份替换才使相关命令失效；相机、雾、实例和可见范围只更新帧数据。
resize 不使稳定绘制命令失效，世界 epoch 切换清理场景并保留设备会话。
图编译、资源准备或录制失败不提交、不发布场景增量，也不推进共享交接序号。
`RADRAY_DUMP_GRAPH` 非空时输出通用 runtime 的图结构、使用区间与屏障摘要。

## 环境与设备

DLL 对外保留 `RayniumEnvironment` / `RayniumRuntime` 名称，它们是 `radray::Environment` / `radray::DeviceContext` 的类型别名。
环境创建 DXGI factory 或 Vulkan instance，不创建设备；设备会话借用环境，创建 Device 和一条 Direct 队列。
两者均由 `static Create` 先准备局部 RAII 资源，全部成功后交付有效对象；没有默认构造后 Initialize 的路径。
环境必须晚于设备和所有资源销毁。设备析构等待队列后释放设备。

DLL 每份加载实例最多一个环境和一个活跃设备会话；生命周期与渲染调用在环境初始化线程串行执行。
后端与 driver validation 在环境创建时固定，`raynium.rhi.backend=d3d12|vulkan` 默认 D3D12，不自动切换。
Debug/Release 分别采用 RadRay 默认启用/关闭的 RHI 检查；driver validation 在 Vulkan 启用调试和同步验证，在 D3D12 启用 DXGI/D3D12 诊断，未开启 GPU validation。
D3D12 环境在 MC 设备创建前初始化，设备重建借用同一个 factory，不重复开启调试层；环境销毁不关闭进程级调试层。
Vulkan 创建失败只清理本次取得的 instance；已有环境不会被失败创建关闭。

设备开启 external resource 扩展。传入宿主身份时按 LUID/node mask 匹配 GPU；无法匹配时记录警告并返回首选设备，导入端拒绝跨 GPU 共享。
未指定宿主时使用 RadRay 高性能适配器。`DeviceContext::SupportsIndirect` 综合 RHI 的 IndirectDraw 与 IndirectFirstInstance 能力，适配层不查询后端内部对象。
队列、timestamp、限制等通过设备通用能力查询取得。没有 native 工作线程、异步计算或额外提交队列。

## 共享输出与输入

DLL 一个 renderer 同时允许一个 RenderTarget 和一个 SharedTextures 集合。
RenderTarget 只持有 RGBA8_UNORM 共享颜色、导出和 ready/consumed timeline；attachment view 由图执行对象持有，深度来自 SolidRenderPipeline 的图内 D32 临时纹理。
SolidRaster 清雾色和 depth=0，SOLID 使用 GreaterEqual，UV 禁用深度；UV/SOLID 可以共用一个可见列表。
生产图为 SceneUpload → 按需 TextureAssemble → SolidRaster → 外部 release；开启 profiling 时多一个 TimestampResolve。没有生产后处理。

D3D12 导出 D3D12Resource/D3D12Fence，Vulkan 导出 OpaqueWin32。句柄由 native RAII 持有，Java 仅借用导入。
输入支持完整 mip 共享或每 mip 一张物理图像；split 模式采样私有完整 mip 图像，仅 changedTextures 标记变化时添加 TextureAssemble copy pass。
首次使用须初始化所有逻辑纹理；无内容变化和空可见集仍交接所有共享图像，并清屏输出。

```text
创建输入: release → inputReleased(1)
MC:      wait inputReleased(n) → 写输入 → inputAvailable(n)
renderer: wait inputAvailable(n) + outputConsumed(previous)
          RDG 上传/装配/清屏/绘制/acquire/release
          signal rendererCompletion(k) + inputReleased(n+1) + outputReady(m)
MC:      wait outputReady(m) → copy/release → outputConsumed(m)
```

三组 timeline 分开，每条只有一个 signal 方。正常帧不 CPU 等待尚未提交的宿主输入工作，不调用整队列 Wait。
输出首帧从 Undefined 初始化，以后从 Common acquire。resize 排空旧输出后重建颜色与交接，图深度按新尺寸取得；稳定命令不失效。
输入重建排空后递增绑定身份版本；世界 epoch 清理场景，设备保留。完整宿主排空顺序见 [MC 接入](minecraft-integration.md#逐帧交接)。
关闭 target 同时确认 ready/consumed；关闭 inputs 确认 native 已释放，宿主先完成写入。超时保留句柄、Java owner 和 DLL，父 owner 有子 owner 时拒绝关闭。
