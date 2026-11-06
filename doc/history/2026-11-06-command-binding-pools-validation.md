> - 适用: 查阅 2026-11-06 的命令绑定池升级历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-06：命令绑定池升级

- 记录日期：2026-11-06
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：命令绑定池升级涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-06：命令绑定池升级”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到锁文件版本，受管源码干净且无上游补丁；Raynium 的 BuildMeshDrawCommands 调用保持兼容。
缓存绑定改由上游独立池持有，命令和绑定存储的借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Release `/MT`、Tracy 0.14.1：

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：85 项 native/GPU 用例通过，无失败、错误或跳过。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；短采集保存约 708 ms、21 个帧标记、1,793 个 CPU zones，CSV 读回 106 种区间，含 `RadRay.MeshDraw.Submit`。

日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；已有的重复初始化 SymInitialize 诊断仍出现。
测试消费者和显式诊断用例的校验设置与游戏采样不同。本轮未重跑纯 Java 测试、Debug 或发布打包。
证据在本机忽略目录 `build/verification/tracy-session-20261106-191241-binding-pools/`，含测试日志、两轮测试 XML、短采集与构建清单。
