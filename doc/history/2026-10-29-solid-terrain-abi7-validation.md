> - 适用: 查阅 2026-10-29 的SOLID 地形与 ABI 7历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-29：SOLID 地形与 ABI 7

- 记录日期：2026-10-29
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：SOLID 地形与 ABI 7涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-29：SOLID 地形与 ABI 7”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在 Windows x64 / Temurin 25.0.4.1 / MSVC 19.51.36260.0 / RTX 4080 / NVIDIA 621.01 /
Vulkan SDK 1.4.363.0 上验证；RadRay 已更新到锁文件指定的干净源码。以下命令均附 `--console=plain --no-parallel`。

| 命令 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | 8 个纯 Java 测试及 49 个 Debug native 测试通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true` | 49 个 Release native 测试通过，ABI 7 分发 JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构与哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | D3D12 → MC Vulkan 直接多 mip 输入；进入开发世界、SOLID 预览、F3+T、全屏/窗口切换、返回标题与正常退出通过 |
| `gradlew.bat runClientNative -PrhiBackend=vulkan -PterrainTextureTransfer=split -PterrainProfile=true -PrhiValidation=true -PvulkanValidation=true` | Vulkan → MC Vulkan 强制 split fallback；世界显示、F3+T、返回标题与正常退出通过；合批修改后再次进入同一世界并正常退出 |

新增 native 用例覆盖双后端 × 直接/逐 mip 两种模式：非方形图集全部 mip 的逐像素结果、直接/间接 draw、
连续替换和移除、反向深度、大整数坐标、无网格更新帧、epoch 清空、采样设置变化、UV 后首次地形和空面组。
注入不兼容 GPU 身份使直接导入失败，验证清理后可创建并使用 split；纯 Java 测试覆盖两条路径均失败时传播错误。
本次测试与游戏日志未出现 RHI、D3D12 或 Vulkan 同步校验错误；GPU validation 未开启。
游戏的 MC Services/开发账户网络鉴权报错不影响本地世界验证。

同一开发世界、相同 213×120 预览 RT、Debug 和驱动校验配置下，约 915 个可见 section、约 2394 个面组 draw：
按距离段/顶点页合批前，CPU 地形帧调用 p50 约 8.6 ms；合批后约 1.5 ms，GPU 光栅 p50 约 0.06 ms。
这是两次运行中稳定采样窗口的观察，世界时间和少量 section 更新仍在变化，不是严格隔离的基准。
CPU 指标包含槽位等待与 FFM/native 录制，原版仍同时绘制，不能据此推算独立渲染器 FPS。

覆盖仍限于本机 GPU/驱动和开发世界，未完成多厂商、复杂资源包、所有视效/模型的逐像素对照或大规模性能矩阵。
直接路径与 fallback 均有自动测试；实机 D3D12 操作早于最后的面组/合批调整，最后版本的实机合批复测使用 Vulkan split。
最后两处边界修正（首次深度状态、零字节写入）由随后两配置 native 测试验证，未再次启动游戏。
日志及两配置 XML 保存在本机忽略目录 `build/verification/terrain-20261029/`。
