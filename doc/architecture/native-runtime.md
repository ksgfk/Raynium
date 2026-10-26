> - 适用: 修改已有 RHI 设备会话及其创建、关闭和所有权
> - 权威: 本文维护 native C++ 职责与生命周期；C ABI/FFM 和 MC 适配分别由互操作与接入文档负责
> - 锚点: `native/include/raynium/runtime.h`, `native/src/runtime.cpp`, `native/CMakeLists.txt`, `native/radray.lock.json`, `src/test/java/dev/raynium/bridge/NativeRuntimeTest.java`

# Native runtime 设备会话

当前 native runtime 通过 RadRay RHI 创建设备及一条 Direct 队列。Raynium 没有资源上传、管线、帧执行或输出接口。

客户端生命周期见 [Minecraft 接入](minecraft-integration.md)，C ABI 与 FFM 见 [互操作与加载](interop.md)，已有验证记录见[构建与测试](../guide/build-test.md#当前验证范围)。

## 当前实现：设备会话

`native/include/raynium/runtime.h` 与 `native/src/runtime.cpp` 定义不可复制、不可移动的 `raynium::RayniumRuntime`，编译进 `rayniumruntime.dll`，通过 RadRay RHI 创建一个设备及一条 Direct 队列。`static bool Create(backend, output, error)` 是唯一创建入口：先在局部 RAII 对象中准备 Factory/环境、Device 和 Queue，全部成功后才调用私有构造函数。没有默认构造或 Initialize；成功返回 true 并交付有效对象，失败返回 false 和诊断字符串，不产生半初始化 runtime。output 必须为空，失败不改动它。本阶段没有 swapchain、资源表、渲染管线或帧提交，MC 继续原版绘制。

Java `NativeRuntime` 拥有不透明的 `rn_runtime*` 和 DLL。`RayniumClient` 在 Fabric `CLIENT_STARTED` 创建它，在 `CLIENT_STOPPING` 关闭它。两处均在 MC 渲染线程；创建不依赖世界加载，资源重载、进出世界不重建 runtime。`CLIENT_STARTED` 位于 `Minecraft.run()` 的异常捕获之前，所以 Java 初始化失败时显式构造 CrashReport 并调用 `emergencySaveAndCrash`，不依赖主循环捕获。

默认 D3D12，`raynium.rhi.backend=d3d12|vulkan` 显式选择；所选后端失败时清理创建过程的局部资源并终止启动，不自动切换后端。普通 Java 开发运行未选择 native payload 时保持原有可选模式。初始化/销毁 C ABI 与参数规则以[当前 C ABI](interop.md#当前-c-abi) 和 `bridge.h` 为准。

| 后端 | 创建顺序 | 清理顺序 |
| --- | --- | --- |
| D3D12 | DXGI Factory → 高性能适配器 → Device → Direct Queue | 队列 Wait → Device → Factory |
| Vulkan | InitEnv → 高性能物理设备 → Device → Direct Queue | 队列 Wait → Device → ShutdownEnv |

队列是设备所有的借用对象，不单独 delete。Create 的局部 guard 只在本次取得 Vulkan 环境所有权后负责 ShutdownEnv；成功构造后将该责任交给 runtime；RadRay 目前只允许一个活跃 Vulkan 环境。宿主契约是每客户端一个活跃 runtime，释放后可以重新创建。当前没有全局 registry，也不支持跨线程并发访问 RHI。

Debug 启用 RadRay 默认 RHI 校验，Release 关闭；驱动调试层、同步验证层和 GPU validation 均未开启。日志包含创建阶段、后端、GPU 名称和队列等待，底层具体失败原因仍由 RadRay 输出。当前没有 Java 日志 upcall，runtime 和 bridge 也没有 C++ try/catch；RHI 返回的可恢复失败转换为状态码，底层异常或不可恢复驱动错误不承诺生成 Java 崩溃报告。

正常退出严格先销毁 runtime，再关闭 library arena。当前没有 native 工作线程或回调。强杀进程、JVM/native 崩溃及其他模组提前终止生命周期事件不保证执行关闭回调；不通过 shutdown hook 在未知线程操作 RHI。

## 依赖能力与项目边界

2026-10-26 调查时，`native/radray.lock.json` 指定的 RadRay 公共 RHI 定义了纹理、命令、读回 buffer、`CopyTextureToBuffer`、Map/Invalidate 和 fence 同步接口。Raynium 当前仅消费设备与队列创建/关闭，没有调用这些读回或绘制接口。

该版本的公共 RHI 没有完整的共享图像/同步对象导入导出接口；`ResourceHint::External` 的存在不表示已有跨设备共享服务。源码位置见 RadRay 仓库中的 [rhi.h](https://github.com/ksgfk/RadRay)（`modules/render/include/radray/render/rhi.h`）、[D3D12 实现](https://github.com/ksgfk/RadRay)（`modules/render/src/d3d12/d3d12_impl.cpp`）与 [Vulkan 实现](https://github.com/ksgfk/RadRay)（`modules/render/src/vk/vulkan_impl.cpp`）。

资源、帧、管线与输出的设计讨论见[思考历史](../history/2026-10-26-rendering-design-notes.md#当时的-native-runtime-草案)，不属于本文的已实现契约。
