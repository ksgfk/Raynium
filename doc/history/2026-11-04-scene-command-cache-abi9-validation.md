> - 适用: 查阅 2026-11-04 的ABI 9 与持久 Scene 命令缓存历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-04：ABI 9 与持久 Scene 命令缓存

- 记录日期：2026-11-04
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：ABI 9 与持久 Scene 命令缓存涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-04：ABI 9 与持久 Scene 命令缓存”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

环境为 Windows x64、Temurin 25.0.4.1、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0。
锁文件升级到上游 2026-11-04 的“帧概念”版本，显式 restore 干净受管源码，并通过
`python tools/build_debug_dependencies.py --clean` 重建 Debug `/MTd` 依赖。
`python tools/compile_preview_shaders.py` 重新生成地形 DXIL/SPIR-V，绑定拆分为 Pass Group 0 与 Material Group 1。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java、61 项 Debug native/GPU 测试通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 61 项 Release native/GPU 测试通过，无失败、错误或跳过；分发包生成并通过结构、哈希与许可证检查 |
| 两配置 DLL 与 stage | ABI 9、十三个 C 导出、RadRay 干净；Debug 为 MTd，Release 为 MT，均无动态 CRT 导入 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双后端测试启用 native 驱动诊断，独立 Vulkan 消费者启用校验及同步验证；Debug 另启用 RadRay RHI wrapper 检查。
新增双槽像素回归覆盖同一 mesh 的多个 submesh、不同材质、单材质更新、索引扩容替换、网格替换、显式删除及 epoch 切换；
两个 native 槽均在宿主发出输入 signal 前提交。既有回归继续覆盖多实例与 FirstInstance、各 mip、两种纹理传输、
反向深度、大坐标、输入/输出重建、失败更新和消费者超时保活。共享清屏回归使用固定雾色，并检查复制区域外像素。
日志没有 RHI/驱动校验错误或 Vulkan VUID。Release 上游 D3D12/Vulkan 源码各有一处 C4702 警告，未导致失败；
DXIL 编译忽略 HLSL 的 Vulkan binding 属性，SPIR-V 编译使用这些属性。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
证据位于本机忽略目录 `build/verification/runtime-rewrite-20261104/`，包含两配置日志、JUnit XML/报告、manifest 与 dumpbin 输出。
本轮未启动真实游戏、重采性能或注入 device lost/GPU 分配失败；自动测试不替代这些验证。
以下较早日期记录反映当时实现与配置。
