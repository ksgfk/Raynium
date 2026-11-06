> - 适用: 查阅 2026-11-02 的固定 MT 构建历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-02：固定 MT 构建

- 记录日期：2026-11-02
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：固定 MT 构建涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-02：固定 MT 构建”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

本节记录当时两配置均为 MT 的验证；当前 Debug 已改为 MTd，操作步骤见下文。

保留更新后的 RadRay 锁定源码，将 mod 的 Debug/Release 固定为 `/MT`，静态链接 mimalloc。
按 `windows-x64-msvc-staticcrt` 显式 `--clean` 重建所需的 16 个依赖包，再构建 bridge 和 stage；
移除 Fabric preLaunch 分配器加载器，恢复 schema 1 的 MT 单 DLL payload。

| 检查 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 18 个纯 Java、61 个 Debug native/GPU 用例通过，无失败或跳过 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 61 个 Release native/GPU 用例通过；分发 JAR 生成并通过结构/哈希检查 |
| 固定 GPU 场景图像 | 两配置各 48 张 PNG 与 FramePipeline 迁移基线逐像素及哈希一致 |
| 固定 CRT 约束 | CMake 拒绝 `/MD`、`/MTd` 和 MD SDK 三元组；stage 拒绝旧 MD DLL 的动态 CRT 导入 |
| 最终 JAR | 仅有 `rayniumruntime.dll`；manifest 为 MT/schema 1，无 mimalloc/redirect DLL、preLaunch 入口或加载器 class |

两配置链接命令使用 whole-archive 静态 mimalloc，DLL 导入仅含已分类的 Windows/图形依赖，无动态 CRT。
GPU 回归日志无 RHI/驱动校验错误或 Vulkan VUID；本轮未重新启动游戏或测量 FPS。
证据保存在本机忽略目录 `build/verification/mt-runtime-20261102/`，包含依赖重建、约束拒绝、两配置构建测试日志、JUnit XML 和 PNG。
