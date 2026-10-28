> - 适用: 修改 Fabric 生命周期、共享预览显示和原版命令流接入
> - 权威: 本文记录 Raynium 已有的 MC 接入；原版源码事实归渲染流程，设备和加载契约归各自页面
> - 锚点: `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/java/dev/raynium/client/preview/*.java`, `src/client/java/dev/raynium/client/mixin/*.java`, `src/client/resources/raynium.client.mixins.json`, `build.gradle`

# Minecraft 接入

## 生命周期与入口

`RayniumClient.onInitializeClient` 定位 native payload、读取后端与校验配置并注册生命周期事件。
选定 payload 后，在此加载 DLL 并创建 `NativeEnvironment`，准备所选后端的 factory/instance 与诊断；失败报告 Fabric 初始化失败。
MC 26.3 / Fabric Loader 0.20.1 的 `EntrypointPatch` 在 Minecraft 构造函数中、设置 gameThread 的
`Thread.currentThread()` 调用前插入 `Hooks.startClient`，早于 `RenderSystem.initBackendSystem`、设备和窗口创建。
该入口不访问尚未初始化的 MC 图形对象。对应 MC 源码获取见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)。

CLIENT_STARTED 在渲染线程取得 MC Vulkan GPU 的 LUID/node mask 并创建 `NativeRuntime`；
通过已初始化环境的 `createRuntime` 创建设备，环境和 DLL 所有权始终由 `NativeEnvironment` 持有。
随后创建 `PreviewOverlay`，通过 `PreviewHost` 附到 Minecraft；预览资源在首次有效帧创建。
CLIENT_STOPPING 先关闭预览和取消挂接，再依次关闭 runtime、环境和 DLL。启动失败执行相同清理顺序；清理失败保留剩余所有者并记录为附带异常。
资源重载、切换世界不重建设备。

未选择 payload 且非 required 模式时记录提示并继续游戏；无效 payload、ABI 不匹配、runtime 创建失败
仍由入口转为 MC 崩溃报告。CLIENT_STARTED 早于 Minecraft.run 的异常处理，入口显式调用 emergencySaveAndCrash。
预览不可用属于单独的降级路径，不改变原有加载错误处理。

## 开发预览模块

共享显示位于 `client/preview`、对应 Mixin 和 bridge 预览入口。
`VulkanFeatureSetsMixin` 为 MC 设备的可选 FeatureSet 增加 Win32 external memory/semaphore 扩展，
不强制设备支持；`raynium.preview.enabled=false` 时不添加。
`VulkanCommandEncoderAccessor` 暴露 encoder 所属设备。
`MinecraftPreviewMixin` 在 MC 26.3 `Minecraft.renderFrame()` 中 `GameRenderer.render()` 返回之后、
原有 surface blit 之前调用预览。

`VulkanPreviewImage` 直接管理导入的 VkImage/VkDeviceMemory 和两条 timeline semaphore，
不包装 GpuTexture，不创建 image view 或 MC shader。导入检查扩展、GPU 身份、支持位和内存要求。
D3D12Resource 用 handle properties 与 image requirements 选择内存类型；OpaqueWin32 使用
导出端的 allocation size/type，并检查 device/driver UUID、format/usage/flags/tiling。
所有 Win32 句柄仍由 native 持有，Java 不 CloseHandle。

MC 26.3 使用的 LWJGL 3.4.3-snapshot 对 `VkImportSemaphoreWin32HandleInfoKHR` 错误地同时要求
handle 和 name 非空。导入函数检查函数地址和句柄后，通过 LWJGL JNI 调用该 Vulkan 入口，
保持 name=NULL 的句柄导入契约；未关闭全局 LWJGL 检查。

小 RT 每轴为主 RT 的四分之一（最小 1），显示在右上角，边距为 16 个 framebuffer 像素，
小窗口按剩余空间缩小边距。MC 26.3 `VulkanGpuSurface.blitFromTexture` 的 dstOffsets 翻转 Y，
因此复制坐标为 `(W−w−marginX, H−h−marginY)`，UV shader 同步补偿 V。
最终可见左上为黑，右上红，左下绿，右下黄。
源码命名基线与生成入口见 [Minecraft 渲染流程](minecraft-rendering.md)。

## 逐帧交接

```text
runtime: 等待自身上一轮完成 → GPU wait consumed(n−1) → acquire → 绘制 → release → ready(n)
MC:      原版绘制 → wait ready(n) → acquire → copy → release → consumed(n) → 原有 blit/submit/present
```

首帧不等待 consumed。MC 使用原有 transient command buffer，通过 encoder.execute 接入，
wait/signal 添加到同一提交构建器，不增加正常帧中途 submit。
小 RT acquire/release 只改变外部所有权；源和主 RT 在复制时均为 GENERAL。
复制前后使用 ALL_COMMANDS / MEMORY_READ|WRITE 内存屏障衔接原版绘制和最终读取。
runtime 的单槽复用与导出对象见 [Native runtime](native-runtime.md#共享预览与-uv-绘制)。

尺寸改变时，先调用 backend encoder.submit 排出待执行工作，通过原有 MC 提交 fence 确认整次提交完成，再等待最后 consumed 和 ready，
销毁 Java 导入对象及 native 预览，再以新尺寸创建，序号归零。backend submit 不触发 frontend 的 endFrame。
最小化或无效尺寸暂停预览。退出执行相同排空顺序，随后关闭 runtime/DLL。
不建立多槽、异步退役或 generation 协议。

MC 非 Vulkan、显式关闭、能力不足、创建或导入失败时记录原因，本会话禁用预览并继续原版游戏。
清理只在完成等待成功后释放资源；超时或 device lost 导致清理失败向上传播，保留活跃所有者，
不把失败当作完成或继续卸载 DLL。

## 当前边界与验证

原版世界、手持物、后处理、GUI 和呈现继续执行。预览仅覆盖指定矩形，内容是 native GPU 绘制的 UV。
没有世界采集、区块映射、通用资源服务或窗口接管；运行路径不经过 CPU 像素读回。
开关、启动命令、实际环境和覆盖限制见[构建与测试](../guide/build-test.md)。
