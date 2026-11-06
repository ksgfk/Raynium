> - 适用: 查阅 2026-11-06 的可见面组批量化历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-06：可见面组批量化

- 记录日期：2026-11-06
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：可见面组批量化涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-06：可见面组批量化”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在同一锁定且干净的 RadRay 上，适配层按 runtime ElementMask 合并可见面组，单次查找并收集几何访问，
双槽复用可见 batch/几何列表；排序、缓存提取和绘制提交仍由 runtime 执行。顺序、实例覆盖及列表借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
显式运行 `python tools/build_debug_dependencies.py --parallel 8` 更新 Debug 依赖身份并完成增量准备。
Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01：

| 实际检查（Gradle 均附 `--console=plain --no-parallel`） | 结果 |
| --- | --- |
| `gradlew.bat nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true` | 93 项通过；RHI wrapper 与 native 驱动校验开启 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true` | 93 项通过；最终 Release/MT、Tracy payload 已 stage |
| 同 Release 配置，连接采集器运行 `NativeTerrainTest.groupedVisibleSubmeshesKeepOrderAndInstanceOverrides` | 4 项参数化用例通过；采集器自动保存约 368 ms、13 个帧标记、879 个 CPU zones 后退出 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

新增双后端 × direct/split 像素用例覆盖相邻合并、逆序与重复元素、全元素哨兵、不同排序键与实例起点/数量，
并在双槽在途下切换空可见帧后恢复绘制；已有高位掩码用例新增第 0/31 个元素的相邻合并。
短采集 CSV 逐帧读回确认可合并输入从 2 项变为 1 条 runtime 可见命令，其余顺序/实例边界保持预期计数，
且不再出现旧的 `Raynium.Frame.GatherGeometryAccesses` 二次遍历区间。这是功能和插桩验证，不作为实机场景性能结论。
测试日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Tracy 重复初始化的 SymInitialize 诊断仍出现，未验证调用栈符号解析。
测试消费者和显式诊断用例有各自的校验设置。本轮未启动游戏、重跑纯 Java 测试或发布打包。
证据在本机忽略目录 `build/verification/visible-batches-20261106/`，包括配置日志、三轮完整测试及短测 XML、构建清单、采集和逐帧计数摘要。

## 归档前指南中的采样比较口径

来源：同一工作树 `doc/guide/build-test.md` 的“Tracy 与整段客户端采样”节。

可见项掩码校验与几何访问去重统一计入 `Raynium.Scene.VisibleBatches`，已移除单独的 `Raynium.Frame.GatherGeometryAccesses` 二次遍历；
对比旧采样时应将旧版这两个区间合计。

连续元素合并后，一条命令中多个面组可以只产生一次 DrawIndexed；实际调用数读取本机计数补丁的 `RadRay.MeshDraw.DrawIndexedCalls`。
