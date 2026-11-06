> - 适用: 查阅 2026-11-08 的runtime 状态排序与 RDG 交接接入历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-08：runtime 状态排序与 RDG 交接接入

- 记录日期：2026-11-08
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：runtime 状态排序与 RDG 交接接入涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-08：runtime 状态排序与 RDG 交接接入”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到锁文件版本，接入 StateFirst 自动状态键、连续元素合并、Scene::Reserve、私有 Scene 访问器和 RDG 外部所有权屏障。
受管 RadRay 仅保留本机 profiler Submit 调用计数补丁；具体接入与所有权边界见 [Native runtime](../architecture/native-runtime.md)。
显式运行 `python tools/build_debug_dependencies.py --parallel 8` 完成新版本 Debug 依赖身份准备。
Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01，Gradle 均附 `--console=plain --no-parallel`：

| 实际检查 | 结果 |
| --- | --- |
| `test nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PvulkanValidation=true -PterrainProfile=true` | 93 项 native/GPU 用例通过；21 项纯 Java 测试复用 up-to-date 结果 |
| `nativeTest -PnativeConfig=Debug -PrhiValidation=true -PvulkanValidation=true -PterrainProfile=false` | 93 项 native/GPU 用例通过；开启 RHI wrapper 校验，覆盖无 timestamp 的单图命令提交 |

两轮 native 测试无失败、错误或跳过，覆盖双后端、direct/split 输入、双槽在途、空帧交接、resize、连续面组与实例覆盖。
日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Release 的 Tracy SymInitialize 诊断仍存在，未验证调用栈符号解析。
本机证据位于 `build/verification/radray-upgrade-20261108/`，包含测试日志、JUnit XML 和升级前的本地补丁备份。

同日对上一轮 `build/benchmarks/stress-20261108/` 的三个场景各重采一次，均正常完成并保存 Tracy/JFR。
复用各场景的源世界、路线、预热时间和逐字节相同的 options；视距 32、模拟距离 8，窗口 1280×720、native RT 320×180，
Release/MT、D3D12 native / MC Vulkan、Tracy 与 JFR 开启、驱动及 RHI 校验关闭。原世界预热 60 秒、城市预热 90 秒，正式路线均为 60 秒。

| 场景 | native CPU 均值 ms（前 → 后） | 变化 | native P95 ms（前 → 后） | 路线 FPS（前 → 后） | native GPU 均值 ms（前 → 后） |
| --- | --- | --- | --- | --- | --- |
| 原世界 32 区块 | 1.414 → 1.014 | -28.3% | 2.032 → 1.441 | 169.4 → 176.7 | 0.186 → 0.188 |
| Newisle 穿城 | 1.540 → 1.092 | -29.1% | 2.137 → 1.495 | 100.2 → 123.8 | 0.309 → 0.311 |
| Newisle 固定天际线 | 0.873 → 0.685 | -21.6% | 0.964 → 0.787 | 124.2 → 135.1 | 0.395 → 0.411 |

三场景每帧平均可见面组分别变化 -0.14%、+0.30%、-1.18%；实际 DrawIndexed 分别减少 32.2%、24.4%、34.6%，
顶点缓冲绑定从约 1555/1269/294 次降至 20/23/14 次。穿城场景的 Submit、GatherVisibleHandles、SortVisible 均值分别从
0.417/0.109/0.067 ms 降至 0.223/0.056/0.015 ms；本轮适配层 VisibleBatches 自耗时仍占 native CPU 均值约 37%–43%。
固定天际线 native GPU 均值增加约 4.0%，正式采样前观察游戏与 native 预览均有城市地形输出。

移动路线的 P95/P99 下降，但最大 native 帧从 12.556/8.027 ms 增至 40.087/29.589 ms：
前者含 38.938 ms QueueSubmit，后者含 26.086 ms Buffer.Create；另记录到 Scene.Reserve 8.674 ms 的扩容尖峰。
这些是区间墙钟耗时，现有采样不能进一步分离驱动执行、分配与线程调度影响。
每版本每场景仅一轮，非交错重复 A/B；穿城 GC 总暂停从 1.710 秒变为 1.501 秒，加载与模拟存在波动，不能把整帧 FPS 变化全部归因于 runtime。
本轮证据位于 `build/benchmarks/runtime-upgrade-20261108/`：`comparison.json` 汇总配置一致性和前后指标，
各 session 的 `analysis/native-analysis.json`、CPU inclusive/self CSV 与逐帧数据保留分层证据；三轮 self 耗时之和均与 native 总耗时一致。
