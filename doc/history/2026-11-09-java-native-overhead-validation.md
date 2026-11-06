> - 适用: 查阅 2026-11-09 的Java 帧准备、持久 FFM 缓冲与掩码直传历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-09：Java 帧准备、持久 FFM 缓冲与掩码直传

- 记录日期：2026-11-09
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：Java 帧准备、持久 FFM 缓冲与掩码直传涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-09：Java 帧准备、持久 FFM 缓冲与掩码直传”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

已接入 ABI 10、扁平可见采集、按需上传排序、事件驱动驻留/延迟回收、持久堆外帧缓冲，以及 native 按值 primitive 和几何指针缓存。
当前契约分别见 [地形预览](../architecture/terrain-preview.md)、[互操作与加载](../architecture/interop.md) 和 [Native runtime](../architecture/native-runtime.md)。
原有历史计划保持原样。

Windows x64、Java 25、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44：

| 实际检查 | 结果 |
| --- | --- |
| `test` | 36 项纯 Java 测试通过，无失败、错误或跳过 |
| Debug `nativeTest -PrhiValidation=true` | 95 项双后端 native/GPU 测试通过；RHI wrapper 与驱动校验开启，未启用 timestamp |
| Release `nativeTest -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true` | 95 项双后端 native/GPU 测试通过；驱动校验、Tracy 和 timestamp 开启 |
| 两配置定向重跑 `rejectsZeroAndOutOfRangeElementMasks` | 各 2 项通过，补充检查重建 renderer 后 epoch 大于此前 owner 和帧标记 |
| Release `packageNativeMod verifyNativePackage -PnativeProfiler=true` | ABI 10、十三个导出；Mod JAR 结构、哈希、许可证与静态 CRT 检查通过 |

Gradle 均附 `--console=plain --no-parallel`。本机受管 RadRay checkout 与锁文件不同，未改动该 checkout；
验证显式使用干净的 `build/overhead-radray/` 锁定版 worktree，通过 `-PradraySourceDir=C:/Users/xiaoxs/Desktop/Raynium/build/overhead-radray` 指定。
Debug 依赖已用 `python tools/build_debug_dependencies.py --radray-source build/overhead-radray --parallel 8` 显式增量准备。
测试日志未发现 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Release 仍有既有 Tracy SymInitialize 诊断，上游两处 C4702 编译警告保留。
未验证调用栈符号解析。测试 XML、日志及比较数据位于 `build/verification/java-native-overhead-20261109/`。

随后复用原高压 Newisle 存档、90 秒预热和同一 60 秒路线进行 JFR + Tracy 联合采样。
前后 options 逐字节相同，RadRay 版本/clean 状态、工具链和 native 配置相同；均为 Release/MT、D3D12 native / MC Vulkan、
1280×720 窗口、320×180 native RT、视距 32/模拟距离 8，关闭驱动及 RHI 校验，开启 Tracy/JFR 和 terrain timestamp。
采集完成并正常关闭 runtime、环境与 DLL；JFR 和 Tracy 已读回，33,971 个路线帧逐一对齐（基线 26,831 帧）。

| 同路线指标 | 基线 → 本轮 |
| --- | --- |
| 渲染主线程 Raynium 估算分配 | 8,240.05 → 57.75 MiB，下降 99.3% |
| TerrainCapture 的排序执行样本 | 265 → 0 |
| 每帧 ABI 可见项均值 | 1,878.0 → 639.1；section 实例均值 638.7 → 639.1 |
| VisibleBatches 均值 / P99 | 0.0790 / 0.2552 → 0.0287 / 0.0810 ms |
| native Render 均值 / P99 | 0.3441 / 0.8129 → 0.2700 / 0.6215 ms |
| 帧间隔均值 / P99 | 2.2343 / 6.9634 → 1.7647 / 5.7135 ms |
| GC 停顿次数 / 累计 | 74 / 539.36 → 46 / 434.00 ms |

本轮未采到 TerrainCapture.capture、TerrainPreview.draw 的堆分配，也未采到主线程 Unsafe.allocateMemory0/freeMemory0；
分配样本权重与执行样本只是估算，未出现样本不等于绝对零分配。
超过 16.67 ms 的帧间隔仅从 15 次变为 14 次，最长 GC 停顿从 31.28 增至 44.11 ms，最大客户端/native 帧也增加。
这是各一轮的前后对比，不能据此保证最坏延迟或将整帧变化全部归因于本次重构。
本轮采样及逐帧分析位于 `build/benchmarks/java-native-overhead-20261109/20261109-152625-704294/`，
基线位于 `build/benchmarks/runtime-jfr-20261109/20261109-114422-669000/`；比较摘要为验证目录的 `comparison.json`。
