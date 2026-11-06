> - 适用: 查阅 2026-11-03 的Release Tracy 客户端采集历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-03：Release Tracy 客户端采集

- 记录日期：2026-11-03
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：Release Tracy 客户端采集涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-03：Release Tracy 客户端采集”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

环境为 Windows x64、Temurin 25.0.4.1、MSVC 19.51、RTX 3060 / NVIDIA 591.44；MC 使用 Vulkan，native 使用 D3D12。
显式准备 Release `/MT` 依赖并启用 Tracy 0.14.1，RHI wrapper、native 驱动与 MC Vulkan 校验关闭；开启 terrain timestamp 统计。
客户端由用户操作，Tracy 保存约 90 秒、1,962 个帧标记、37,247 个 CPU zones；其中 1,959 次 native Render 调用。
存档末段采集时间 82–89 秒的 530 帧均有 3,436 条 draw、没有网格更新：

| 项目 | 平均 ms | p95 ms |
| --- | --- | --- |
| native Render CPU | 9.58 | 11.03 |
| Scene Prepare（包含排序与 batch 构造） | 6.72 | 7.75 |
| 可见项排序 | 5.12 | 5.94 |
| Frame Prepare | 1.10 | 1.41 |
| draw 命令录制 | 0.57 | 0.71 |
| native GPU timestamp 区间（回收时发布的 529 个样本） | 0.63 | 0.70 |

这些 CPU zones 有父子嵌套关系，不能逐行相加。帧槽复用等待平均约 0.0014 ms。
JFR 最初没有随继承的 Loom 配置进入 JVM，随后手工启用；退出卡住后用户强制结束，最终 JFR 文件为空。
从临时仓库保留了一个约 46 秒的完整尺寸 chunk：在独立副本中仅将偏移 64 的未完成状态改为完成状态后，JDK 工具可读取其中事件。
该恢复副本不能作为完整启动/正常退出记录。12:17:27–12:17:33 的 455 个世界帧平均 13.02 ms、p95 15.25 ms；该区间 GC pause 合计 14.69 ms。

随后修正客户端 JVM 参数继承，并用 Gradle 实际启动参数检查确认包含 JFR 开关；Tracy 改为专用 manual-lifetime SDK，
在 DLL 卸载前显式结束采样线程。Release 采样配置的 `NativeRuntimeTest` 和 `NativeRendererTest` 共 16 项通过，无失败或跳过，测试 JVM 正常退出。
另在采集器连接状态下运行 `NativeRendererTest.failedConsumerWaitRetainsOwners` 的两个后端用例，均通过；采集器完成 5.57 秒 trace 保存，测试 JVM 正常退出。
该修复未重新采集用户游戏全过程。证据位于本机忽略目录 `build/verification/tracy-session-20261103/`，包含原始 Tracy、CSV、JFR 原块/恢复副本、统计与测试日志。
