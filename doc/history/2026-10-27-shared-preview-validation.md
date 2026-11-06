> - 适用: 查阅 2026-10-27 的共享预览验证历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-27：共享预览验证

- 记录日期：2026-10-27
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：共享预览验证涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-27：共享预览验证”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

2026-10-27，基于本次工作树和 `native/radray.lock.json` 锁定的 RadRay develop，
使用 Windows 11 x64、Temurin 25.0.4.1、Gradle 9.8.2、MSVC 19.51.36260.0、
RTX 4080 / NVIDIA 621.01、Vulkan SDK 1.4.363.0 完成以下验证。
以下是该环境的运行记录，不保证其他机器或后续源码自动满足同样结果。

| 实际命令（均附 `--console=plain --no-parallel`） | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java 构建、4 个纯 Java 和 26 个 Debug native 测试通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 26 个 Release native 测试通过，Release DLL/stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构、manifest 和哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | D3D12 → MC Vulkan，世界内 UV、资源重载、全屏/窗口切换、最小化恢复和正常退出通过 |
| `gradlew.bat runClientNative -PrhiBackend=vulkan -PrhiValidation=true -PvulkanValidation=true` | Vulkan → MC Vulkan，同组游戏操作和正常退出通过 |
| `gradlew.bat runClientNative -PnativeConfig=Release -PpreviewEnabled=false -PvulkanValidation=true` | 原版标题画面正常，无预览且未启用共享扩展，退出成功 |
| `gradlew.bat runClientNative -PvulkanValidation=true` | 最终 D3D12 代码复核全屏重建与提交 fence 排空，退出成功 |

自动测试共 56 次执行，0 失败、0 跳过。共享测试在每个后端上以 257×131、1×1、319×177
三种尺寸各交接 8 帧，逐像素检查 UV 与区域外像素，覆盖创建参数、序号、导入失败清理、等待和重建。
两配置的 native 驱动诊断均开启；消费者启用 Vulkan 同步验证。Debug RHI 检查开启，Release 关闭。
最终测试与游戏日志没有 Vulkan 校验错误；测试中的 `vk has actived VkInstance` 是预期失败清理用例。
Release 上游代码有两处 C4702 不可达代码警告，未导致失败。
MC 环境另有 OSHI/Perflib 计数器、开发账户 Realms 鉴权及资源重载 post effect 警告，不影响该轮预览验证。

实际游戏检查确认可见 UV 左上黑、右上红、左下绿、右下黄，显示于右上角，原版区域外画面正常。
游戏验证中修复了 MC 最终 blit 的 Y 翻转补偿、Win32 semaphore 导入的 LWJGL 校验兼容以及等待结构计数。
该轮 D3D12 游戏运行尚未启用 driver validation，其诊断只在独立测试进程验证；早期启用修复的验证见本节 2026-10-28 记录。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
测试报告副本、最终日志和游戏截图保存在本机忽略目录 `build/verification/preview-20261027/`，不随仓库分发。
未验证正式启动器安装、多 GPU 切换或其他厂商 GPU；未主动注入 device lost/超时，也未在缺少共享能力的硬件运行。
无预览开关与 OpenGL 的禁用分支已运行，导入失败资源清理由 Vulkan 验证测试覆盖。
