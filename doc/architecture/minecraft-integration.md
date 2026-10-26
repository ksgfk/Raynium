> - 适用: 定位当前 Fabric 生命周期、后端配置和原版渲染边界
> - 权威: 本文记录 Raynium 已有的 MC 接入；原版源码事实归渲染流程，设备和加载契约归各自页面
> - 锚点: `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/resources/raynium.client.mixins.json`, `build.gradle`, `gradle.properties`

# Minecraft 接入

## 当前入口

`RayniumClient` 实现 `ClientModInitializer`，注册 Fabric `CLIENT_STARTED` 和 `CLIENT_STOPPING`。
当前接入只有 native 生命周期，客户端 Mixin 列表为空。

| 节点 | 当前行为 |
| --- | --- |
| `onInitializeClient()` | 创建 logger，注册启动/停止事件 |
| `CLIENT_STARTED` | 定位 native payload；选中 payload 时创建 `NativeRuntime` |
| 后端配置 | 读取 `raynium.rhi.backend`，缺省为 `d3d12`，支持 `d3d12` 与 `vulkan` |
| 无 payload | `raynium.native.required=true` 时启动失败；否则记录提示并继续运行 |
| 加载或初始化失败 | 捕获 IO、运行时和链接错误，记录后端/payload 信息，构造 CrashReport 并调用 `emergencySaveAndCrash` |
| `CLIENT_STOPPING` | 有 runtime 时调用 `close()` 并清空 Java 字段 |

`CLIENT_STARTED` 发生在 `Minecraft.run()` 的异常捕获之前，因此启动失败由入口显式转为崩溃报告。
创建和关闭在 MC 渲染线程执行；当前没有为切世界或资源重载注册 runtime 重建逻辑。
DLL/runtime 所有权见[互操作与加载](interop.md#会话和库的寿命)，设备创建与清理见[Native runtime](native-runtime.md)。

## 当前渲染边界

Raynium 没有注册渲染注入，也没有调用原版纹理上传、render pass、surface 或帧呈现接口。
原版世界、手持物、后处理、GUI、后台上传和窗口呈现继续由 Minecraft 执行。
当前不存在场景采集器、区块资源映射、FramePacket、离屏结果传输或预览显示器。

MC 26.3 的主帧循环、RenderPearl 纹理/输出 API 和源码生成入口见[Minecraft 渲染流程](minecraft-rendering.md)。
自有 RT 显示与场景适配的讨论保存在[思考历史](../history/2026-10-26-rendering-design-notes.md)，未计入当前能力。

## 运行与验证

普通运行和 native 运行的任务、JVM 参数、payload 选择及验证覆盖范围见[构建与测试](../guide/build-test.md)。
设备创建成功只证明 native 会话可用，不代表自有绘制、RT 显示或游戏内画面对照已经实现。
