> - 适用: 查阅 2026-10-28 的D3D12 校验提前启用与 ABI 5历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-28：D3D12 校验提前启用与 ABI 5

- 记录日期：2026-10-28
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：D3D12 校验提前启用与 ABI 5涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-28：D3D12 校验提前启用与 ABI 5”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在本次工作树、锁定的 RadRay、Windows 11 x64 / Temurin 25.0.4.1 / MSVC 19.51.36260.0 /
RTX 4080 / NVIDIA 621.01 / Vulkan SDK 1.4.363.0 上执行以下命令，均附 `--console=plain --no-parallel`。

| 命令 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java/native 构建、4 个纯 Java 与原有 26 个 Debug native 用例通过 |
| `gradlew.bat nativeTest -PrhiValidation=true` | 补充早期入口与库所有权用例后，30 个 Debug native 用例通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 30 个 Release native 用例通过，ABI 5 Release DLL/stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构与哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | D3D12 → MC Vulkan，进入已有开发世界、UV 显示、资源重载、全屏/窗口切换、返回标题及正常退出通过 |

新增用例检查早期入口的诊断缓冲区约束、重复启用、已准备 library 转交 runtime 后的寿命和失败清理。
游戏日志中，08:40:05 记录早期 D3D12 校验已启用，08:40:07 MC 选择 Vulkan，08:40:10 native
记录 `driver validation=true` 并创建共享预览；窗口切换后预览在 213×120 与 640×360 间重建。
08:42:24 runtime/DLL 按顺序关闭，Gradle 成功退出。游戏日志有实际 D3D12 诊断消息，无 DXGI/D3D12 或 Vulkan 校验错误，
无 `DEVICE_RESET` / `DEVICE_LOST`。native 测试中两次 `vk has actived VkInstance` 来自预期的占用环境失败用例。
MC Services 网络和开发账户 Realms 鉴权错误不影响本次本地世界验证。

完整日志、两配置的 native 测试报告和游戏截图保存在本机忽略目录 `build/verification/dx12-validation-20261028/`。
此次游戏运行使用 Debug payload；Release 验证范围为 native 测试和打包，未运行 Release 游戏或其他 GPU/驱动环境。
