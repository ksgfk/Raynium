> - 适用: 查阅 2026-11-04 的Tracy 瓶颈增量更新优化历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-04：Tracy 瓶颈增量更新优化

- 记录日期：2026-11-04
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：Tracy 瓶颈增量更新优化涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-04：Tracy 瓶颈增量更新优化”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

移除每帧 Scene/TerrainState 备份与回滚、驻留资源引用全表校验和逐索引 CPU 扫描，驻留字典改为哈希表；
共享 quad 索引首次至少准备 65,536 顶点容量。增量更新与资源寿命契约见 [Native runtime](../architecture/native-runtime.md#适配层职责)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44，使用锁文件对应的干净 RadRay 源码。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java、65 项 Debug native/GPU 测试通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 65 项 Release native/GPU 测试通过，无失败、错误或跳过；Mod JAR 生成并通过结构、哈希与许可证检查 |
| 两配置 DLL 与 stage | ABI 9、十三个 C 导出，Debug/MTd、Release/MT，无动态 CRT 导入；本轮未启用 native Tracy 客户端 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双后端及 direct/split 路径启用 native 驱动诊断，Vulkan 消费者启用校验及同步验证；Debug 另启用 RHI wrapper 检查。
新增四组像素回归覆盖在途帧期间批量新增 96 组资源、哈希表增长后旧 batch 保持有效、纯材质更新、
primitive 替换同帧删除旧材质与几何、批量删除、零更新帧与 epoch 切换。
既有测试继续覆盖双槽参数/资源隔离、多 submesh、索引与网格同 ID 替换、输入/输出重建及消费者超时保活；
已移除依赖中途失败后继续重试场景的断言。日志无 RHI/驱动校验错误、Vulkan VUID 或同步错误。
Release 上游 D3D12/Vulkan 源码各有一处 C4702 警告，未导致失败。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
证据保存在本机忽略目录 `build/verification/tracy-optimization-20261104/`，包含两配置日志、JUnit XML/报告、构建与 stage 清单及结果摘要。
本轮未启动真实游戏、重采 Tracy 性能或注入 device lost/GPU 分配失败；此前采集的耗时不代表优化后实测。
