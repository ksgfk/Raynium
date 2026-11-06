> - 适用: 查阅 2026-10-28 的单例环境与 ABI 6历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-28：单例环境与 ABI 6

- 记录日期：2026-10-28
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：单例环境与 ABI 6涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-28：单例环境与 ABI 6”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

本轮在 Windows x64 / Temurin 25.0.4.1 / MSVC 19.51.36260.0 / RTX 3060 / NVIDIA 591.44 /
Vulkan SDK 1.4.357.0 上验证；RadRay 使用锁定的干净源码，未修改依赖。
以下 Gradle 命令均附 `--console=plain --no-parallel`。

| 命令 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java/native 构建、4 个纯 Java 与 39 个 Debug native 测试通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 39 个 Release native 测试通过，ABI 6 Release stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构与哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | ABI 6 加载、早期环境初始化及 D3D12 设备创建成功；交互验证未完成，随后主动终止本次验证进程 |

测试覆盖单例重复初始化、环境描述和诊断参数、无环境创建 runtime、环境在活跃 runtime 下拒绝销毁、
错误线程、幂等关闭、失败后原所有者继续使用、同一环境内设备重建及环境销毁后重新初始化。
D3D12 专项分别关闭/开启 driver validation，在环境初始化后创建独立 Vulkan 宿主，
同一环境内创建两次 runtime，每次实际共享并逐像素检查两帧 17×9 UV。
原有双后端三种尺寸、连续帧、导入失败和同步重建测试继续通过。
两配置测试日志无 DXGI/D3D12 或 Vulkan 校验错误；Release 上游代码仍有两处 C4702 警告。

游戏日志显示 11:25:59 创建唯一 DXGI factory 并完成环境初始化，早于 MC 图形后端初始化；
11:26:01 创建 D3D12 设备，driver validation 为 true。MC 此次使用 OpenGL，预览按规则禁用。
为避免干扰用户前台工作，停止窗口操作并终止本次启动；Gradle 因进程被终止返回失败。
本轮没有完成 MC Vulkan 预览、资源重载、窗口切换或游戏正常退出验证，不能用自动测试替代这些结论。

构建/游戏日志和两配置测试 XML 位于本机忽略目录 `build/verification/native-environment-20261028/`。
旧版 ABI 5 的运行记录保留在下一节，不代表本轮重新完成了其中的游戏操作。
