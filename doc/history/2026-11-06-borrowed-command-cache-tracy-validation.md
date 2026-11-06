> - 适用: 查阅 2026-11-06 的缓存命令借用升级与 Tracy 细分历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-06：缓存命令借用升级与 Tracy 细分

- 记录日期：2026-11-06
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：缓存命令借用升级与 Tracy 细分涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-06：缓存命令借用升级与 Tracy 细分”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到锁文件版本且受管源码保持干净（`radrayDirty=false`），没有上游补丁。
Raynium 适配带只读 Scene 参数的 SetupGraph；命令借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
适配层补充几何/上传/材质/图声明和共享交接插桩，结合上游已有 runtime/RDG 分段。

Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44，Release `/MT`、Tracy 0.14.1：

- `gradlew.bat test nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：85 项 native/GPU 用例通过，无失败、错误或跳过；19 项纯 Java 测试复用 up-to-date 结果。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`，4 项参数化用例通过；短采集保存约 599 ms、21 个帧标记、1,792 个 CPU zones，CSV 成功读回 105 种 CPU 区间及 16 个 GPU 区间事件。
- 读回确认 Raynium 的实例/staging 复制、几何访问收集及 acquire/release 录制，以及 RadRay 的 RDG 依赖、附件解析、槽 arena 回收和 MeshDraw 提交区间存在。

测试日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；独立 Vulkan 测试消费者及显式测试诊断开关的用例仍启用校验，
不代表游戏采样配置。已有的重复初始化 SymInitialize 诊断仍出现；本次验证 zones 保存和读回，没有验证调用栈符号解析。
本轮未执行 Debug 或发布打包。证据在本机忽略目录 `build/verification/tracy-session-20261106-175340-deep/`，
包括 `release-final.log`、`release-results/`、`smoke.tracy`、CPU/GPU CSV 和 `native-build.json`。

随后用户手动启动游戏并进入本地世界：Release/MT、native D3D12 / MC Vulkan，关闭 RHI wrapper、native 驱动与 MC Vulkan 校验，
启用 Tracy、terrain timestamp 和 JFR。日志确认锁定的干净 RadRay、2048×2048 五级 mip 直接共享及 213×120 预览 RT，
退出时 runtime、环境和 DLL 正常关闭。Tracy 自动保存约 115.1 秒、27,833 个帧标记、2,140,458 个 CPU zones，
`session.tracy` 为 15,620,521 字节，CPU/GPU CSV 读回成功；`session.jfr` 为 12,875,855 字节，
JFR summary 读回 124 秒和 28,379 个 `dev.raynium.ClientFrame` 事件。文件及 `game.log`、`capture-summary.json` 均在同一证据目录。
日志另有系统 Perflib 计数器和开发账号 Realms 认证诊断；未见 native 渲染失败。本次场景没有与旧采样对齐，不作为同视点性能比较。
