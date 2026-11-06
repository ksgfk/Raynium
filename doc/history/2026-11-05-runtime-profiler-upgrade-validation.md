> - 适用: 查阅 2026-11-05 的依赖升级与深度 Tracy 插桩历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-05：依赖升级与深度 Tracy 插桩

- 记录日期：2026-11-05
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：依赖升级与深度 Tracy 插桩涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-05：依赖升级与深度 Tracy 插桩”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到当前锁文件版本；适配合并后的 `runtime/material.h`，并在本机对受管源码做了未入库的本地 Tracy 插桩。
修正专用 Tracy SDK 的 delayed-init 编译与导出定义后，使用 Windows x64、MSVC 19.51.36260.0、
RTX 3060 / NVIDIA 591.44，Release `/MT`、Tracy 0.14.1 进行验证。

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：69 项 native/GPU 用例通过，无失败、错误或跳过。
- 同配置连接采集器，运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；采集器保存约 915 ms、21 个帧标记、1,830 个 CPU zones，CSV 工具成功读回 84 种 zone 名。
- 短采集确认新增的资源创建、Map/复制、槽回收、缓存同步、draw 提交和 D3D12 队列 wait/signal 区间可见；native 驱动与 RHI wrapper 日志均为 validation=false。
- `python tools/check_docs.py`、`git diff --check` 通过。

测试中的独立 Vulkan 消费者仍按测试实现启用校验，不代表正式游戏配置；本轮未验证 Debug 或打包。
Tracy 重复初始化仍有 Windows SymInitialize 诊断，已验证 zones 保存和读回，未验证调用栈符号解析。
证据位于本机忽略目录 `build/verification/tracy-session-20261105/`，包含构建测试日志、69 项测试 XML、短采集、CSV、构建清单及采集配置。

随后由用户启动开发客户端并进入本地世界操作：native D3D12 / MC Vulkan，Release `/MT`，
RHI wrapper、native 驱动与 MC Vulkan 校验关闭，启用 terrain timestamp 和 JFR。
游戏日志确认 ABI 9、2048×2048 五级 mip 图集直接共享与 213×120 预览 RT，最后 runtime、环境和 DLL 依次正常关闭，Gradle 成功退出。
Tracy 采集器正常退出并保存约 75.7 秒、11,828 个帧标记、1,009,107 个 CPU zones，文件约 5.84 MiB；
CSV 工具读回 82 种 zone 名，确认资源创建、数据写入、槽回收、缓存同步与 draw/队列提交的新增区间存在。
同目录 `session.jfr` 约 10.60 MiB，JFR summary 成功读回 85 秒及 12,219 个 `dev.raynium.ClientFrame` 事件。
完整文件为同目录 `session.tracy`、`session.jfr`、`zones.csv`、`game.log`、`game-console.log`、`capture-summary.json` 与 `jfr-summary.txt`。
本轮操作场景没有与旧采样对齐，不作为同视点版本性能比较。
