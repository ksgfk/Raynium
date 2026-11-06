> - 适用: 查阅 2026-10-30 的迁移前的可编程 runtime 与 RDG历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-30：迁移前的可编程 runtime 与 RDG

- 记录日期：2026-10-30
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：迁移前的可编程 runtime 与 RDG涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-30：迁移前的可编程 runtime 与 RDG”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在 Windows x64、Temurin 25.0.4.1、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0 上执行。
RadRay 保持锁文件对应的干净源码；Debug/Release 都使用 C++20 和 `/MT`。以下 Gradle 命令均附 `--console=plain --no-parallel`。

| 命令 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | 构建通过，4 个 Debug CTest、57 个 native/GPU JUnit 用例通过，无跳过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true` | 4 个 Release CTest、57 个 native/GPU JUnit 用例通过，无跳过；Release DLL/stage/JAR 生成成功 |
| `gradlew.bat verifyNativePackage` | 分发结构、ABI 8 manifest、payload 哈希和许可证检查通过 |
| `gradlew.bat test --rerun-tasks` | 8 个纯 Java 用例重新执行通过 |
| `python tools/check_docs.py`、`git diff --check` | 文档引用与差异空白检查通过 |

当时三个 C++ 测试程序直接链接 `raynium_runtime`，不包含 MC bridge；其中 GPU 程序分别运行 D3D12 和 Vulkan。
覆盖持久命令缓存/失效/严格合批、失败更新不发布、RAW/WAR/WAW、剔除/副作用、非法句柄/访问/attachment、UAV 屏障、临时资源池、完成前禁止复用、extraction 和重复导入 owner 保活。
GPU 读回验证 raster→采样和 copy→compute→compute→copy；只替换测试管线即可增加中间纹理与 pass，执行器不变。
注入构图、编译、录制和资源解析失败，确认不提交、不发布场景。连续帧检查缓存复用、资源退役及尺寸变化。

SOLID 使用独立 Vulkan 输入生产者/输出消费者，覆盖双后端、完整/split mip、direct/indirect、所有 mip、反向深度、大坐标、可见范围、纹理更新、几何替换/卸载、输入输出重建、epoch、关闭及超时保活。
本轮修正测试消费者的 `VkSubmitInfo.waitSemaphoreCount`：LWJGL 3.4.3 的等待数组 setter 不自动填写此字段，原测试存在未真正等待共享 timeline 的竞态。最终异步提交没有额外 CPU completion 等待。
两配置最终日志未发现 RHI/驱动校验错误或 Vulkan VUID；driver/synchronization validation 已开启，GPU validation 未开启。

本轮没有启动游戏，没有使用 computer-use 或手动操作；这些结果不代表真实 MC 游戏交互验收，也不构成性能对比结论。
日志位于本机忽略目录 `build/verification/final-debug.log`、`final-release.log`、`final-java.log`、`final-package-verify.log`；JUnit 报告入口见本文测试章节。
设计快照已按用户授权保存到 history，当前实现见 [Native runtime](../architecture/native-runtime.md)。下方为此前实施阶段的验证记录。
