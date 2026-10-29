> - 适用: 修改 RHI 设备会话、共享预览绘制及其所有权
> - 权威: 本文维护 native C++ 职责与生命周期；C ABI/FFM 和 MC 适配分别由互操作与接入文档负责
> - 锚点: `native/include/raynium/environment.h`, `native/src/environment.cpp`, `native/include/raynium/runtime.h`, `native/src/runtime.cpp`, `native/include/raynium/preview.h`, `native/src/preview.cpp`, `native/shaders/uv.hlsl`, `native/radray.lock.json`

# Native runtime

runtime 通过 RadRay RHI 创建设备和一条 Direct 队列，支持一个共享预览 RT 和一个独立 SOLID 地形会话。
地形资源与绘制见[地形预览](terrain-preview.md)；没有 swapchain 或通用场景资源服务。MC 接入见 [Minecraft 接入](minecraft-integration.md)，
接口和借用规则见 [互操作与加载](interop.md)，实际验证范围见[构建与测试](../guide/build-test.md#当前验证范围)。

## 单例环境与设备会话

每份已加载 DLL 持有一个 `RayniumEnvironment`，由 `rn_init_env` 创建、`rn_destroy_env` 销毁。
`RayniumEnvironment::Create` 在局部 RAII 中准备 DXGI factory 或 Vulkan instance，全部成功后
才构造并发布有效环境，不创建 GPU 设备。重复初始化失败，不替换原环境。
Vulkan 创建失败的 guard 只清理本次成功取得的环境，不关闭已有环境。

后端与 driver validation 在环境初始化时固定。`raynium.rhi.backend=d3d12|vulkan`
默认 D3D12，不自动切换。Debug/Release 分别使用 RadRay 默认启用/关闭的 RHI 检查；
driver validation 在 Vulkan 启用调试和同步验证，在 D3D12 启用 DXGI/D3D12 诊断，GPU validation 未开启。

D3D12 factory 及其诊断上下文一直由环境持有，runtime 创建与重建借用同一个 factory，
不会再次创建 factory 或调用 EnableDebugLayer。游戏在 MC 图形初始化前初始化环境；
CLIENT_STARTED 获取宿主 GPU 后创建设备。环境销毁不会关闭进程级 D3D12 调试层。
调试层必须在 D3D12 设备创建前启用，见
[Microsoft EnableDebugLayer 契约](https://learn.microsoft.com/en-us/windows/win32/api/d3d12sdklayers/nf-d3d12sdklayers-id3d12debug-enabledebuglayer)。

`RayniumRuntime::Create` 借用环境，在局部 RAII 对象中准备 Device 和 Direct Queue，
全部成功后才调用私有构造函数。output 必须为空，失败不改变它，也不销毁环境。
环境和 runtime 均无默认构造或 Initialize；队列由设备所有，runtime 仅借用。

环境最多有一个活跃 runtime；销毁后可在同一环境内重建。无环境或已有 runtime 时创建失败，
runtime 存活时环境销毁失败并保留资源。所有生命周期调用在环境初始化线程串行执行。
环境初始化、设备创建及关闭的日志分别记录各自阶段；runtime ready 日志包含两种 validation 状态。

两种设备均开启 RadRay external resource 扩展。传入宿主 GPU 身份时，按 LUID/node mask
匹配适配器；无法匹配时保留可用设备并记录警告，导入端拒绝不同 GPU 的预览。
无宿主身份时使用 RadRay 的高性能设备选择。设备创建失败仍属于游戏启动失败。

| 后端 | 环境初始化 | runtime 创建/关闭 | 环境销毁 |
| --- | --- | --- | --- |
| D3D12 | 创建 DXGI factory、配置诊断 | 借用 factory → Device/Direct Queue；关闭时 Wait → Device | 释放 factory 和诊断上下文 |
| Vulkan | InitEnv、配置诊断 | 借用 instance → Device/Direct Queue；关闭时 Wait → Device | ShutdownEnv |

资源重载与切换世界不重建设备。退出依次关闭输出预览、地形会话、runtime、环境，再卸载 DLL。
环境不是运行期切换后端的入口；没有 native 工作线程、回调或 GC/Cleaner 回收路径。

## 共享预览与 UV 绘制

`Preview::Create` 使用局部 `Resources` RAII 准备全部资源，成功后才构造预览对象。
一个 runtime 只允许一个预览；创建不提交 GPU 工作。尺寸范围为每轴 1..16384。
资源包括一张 RGBA8_UNORM、单 mip/层/采样、颜色附件和复制源用途的纹理，
两条可导出 timeline fence（ready/consumed），以及两组 allocator/command buffer、view、UV shader 和 pipeline；另有私有 D32 深度附件供地形绘制。

| 后端 | 纹理导出 | fence 导出 |
| --- | --- | --- |
| D3D12 | D3D12Resource | D3D12Fence |
| Vulkan | OpaqueWin32 | OpaqueWin32 |

导出 Win32 句柄由预览中的 RadRay RAII 对象持有，Java 仅借用导入。
Vulkan 导出还携带精确分配大小、memory type、device/driver UUID 和图像创建参数。
两种路径均使用 dedicated allocation，offset=0；输出交接只转移小 RT；地形输入有独立的所有权与 timeline。

`native/shaders/uv.hlsl` 使用全屏三角形与 SV_Position 实际绘制，
DXIL/SPIR-V 均由同一源离线生成到 `uv_bytecode.h`，嵌入 DLL。
显示结果为 `(u,v,0,1)`，左上原点。预览 shader 的 V 分量反向以补偿 MC 最终 blit 的 Y 翻转；
这是当前开发显示路径的坐标约定。运行时不依赖 DXC，不读回像素。

每次 Draw 的序号从 1 开始严格递增。复用命令槽前，CPU 等待该槽前次 ready(n−2) 完成（5 秒上限）；
GPU 提交等待 consumed(n−1)，acquire 小 RT、绘制、release 到 Common/GENERAL，再 signal ready(n)。
首帧从 Undefined 初始化，不等待 consumed。每条 timeline 仅有一个 signal 方。
正常帧不调用整队列 Wait；等待、提交和复制关系见 [MC 接入](minecraft-integration.md#逐帧交接)。

销毁先由宿主完成消费并销毁导入对象，native `Finish` 确认自身提交完成后才释放资源。
超时返回失败且保留预览；不能把超时当完成，不能随后卸载 DLL。
C ABI 不捕获底层异常，RadRay 的不可恢复错误不保证转换成 FAILED；强杀或设备丢失不保证正常关闭。

## 依赖边界

2026-10-29 更新到 `native/radray.lock.json` 锁定的 RadRay develop 源码，
本项目消费 `ExternalResourceExtension` 的支持查询、导出纹理/fence、身份和 external ownership barrier。
上游入口为 [RadRay 仓库](https://github.com/ksgfk/RadRay)，接口位于
`modules/render/include/radray/render/rhi.h`，实现位于 `modules/render/src/d3d12/` 和 `modules/render/src/vk/`。
Raynium 不维护通用共享资源系统；Java 的 Vulkan 导入服务预览输出和地形输入；当前锁定源码支持多 mip 的 ExternalTextureDescriptor，仍需按实际描述、用途、设备和驱动查询支持。
