> - 适用: 查阅 2026-10-30 的runtime 迁入 RadRay历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-30：runtime 迁入 RadRay

- 记录日期：2026-10-30
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：runtime 迁入 RadRay涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-30：runtime 迁入 RadRay”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

从远端恢复锁文件指定的 RadRay 提交，使用干净的 `external/RadRay` 默认依赖路径验证；没有使用本地源码覆盖。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0。
Debug/Release native 均为 C++20、`/MT`、ABI 8，构建元数据中的 `radrayDirty=false`。

| 命令/检查 | 实际结果 |
| --- | --- |
| `python tools/fetch_radray.py restore` | 拉取并切换到锁定提交，受管 checkout 干净 |
| `gradlew.bat nativeTest -PrhiValidation=true -PterrainProfile=true` | 57 个 Debug native/GPU 用例通过，无跳过 |
| `gradlew.bat build nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true` | Java 构建、57 个 Release native/GPU 用例及正式 JAR 打包通过；普通 Java 测试复用 up-to-date 结果 |
| RadRay `test_radray_runtime` / `ctest -R 'Runtime(Scene\|Graph\|Gpu)'` | 同一提交的通用代码在 standalone Debug/Release 各 16 项通过，必测 D3D12/Vulkan，无跳过；standalone 使用动态 CRT preset |
| 独立安装消费 | 锁定源码以 Release `/MT` 安装；外部 C++ 项目使用 `find_package(RadRay CONFIG REQUIRED COMPONENTS runtime)`、`RadRay::runtime` 和 `radray::GraphBuilder` 完成编译、链接及图编译运行 |
| `gradlew.bat verifyNativePackage` | 最终 Release JAR 的结构、ABI 8 manifest、哈希与许可证检查通过 |
| 两仓库 `python tools/check_docs.py`、`git diff --check` | 当前文档引用和差异空白检查通过；未改动已有 history |

native GPU 回归使用独立 Vulkan 消费者覆盖 SOLID/UV、双后端、完整/split mip、direct/indirect、深度、大坐标、纹理变化、resize、epoch 和超时保活。
两配置 native 日志未发现 RHI/驱动校验错误或 Vulkan VUID。未启动游戏、未使用 computer-use，不代表真实游戏交互验证。
本机日志位于忽略目录 `build/verification/runtime-pinned-*.log`，两配置 JUnit XML 分别保存在 `runtime-pinned-debug-junit/` 与 `runtime-pinned-release-junit/`。
通用 runtime 契约与测试由 RadRay 维护，本仓库不再包含 `nativeCoreTest` 或通用 runtime 源码副本。
