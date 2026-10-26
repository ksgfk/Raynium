> - 适用: 恢复依赖、构建、测试、启动与分发 Raynium
> - 权威: 本文维护可执行命令和当前验证范围；ABI、内容缓存和加载契约归互操作文档
> - 锚点: `build.gradle`, `gradle/native.gradle`, `gradle.properties`, `native/CMakeLists.txt`, `native/CMakePresets.json`, `native/radray.lock.json`, `tools/fetch_radray.py`, `tools/native.py`, `src/test/java/dev/raynium/bridge/*.java`

# 构建与测试

以下命令在 Raynium 根目录执行。Java/IDEA 设置见[开发环境](dev-environment.md)。
普通 `build` 只构建 Java 并执行纯 Java 测试；native 必须通过显式任务准备。
payload、ABI、缓存和关闭顺序见[互操作与加载](../architecture/interop.md)。

## 当前验证范围

2026-10-26，基于当时的源码和 `native/radray.lock.json` 指定的 RadRay，
使用 Windows 11 x64、Temurin 25.0.4.1、Gradle 9.8.2、MSVC 19.51.36260.0 和 RTX 4080 完成以下验证。
以下是上述环境的运行记录，不保证其他机器或后续源码自动满足同样结果。

| 实际命令（均附 `--console=plain --no-parallel`） | 结果 |
| --- | --- |
| `gradlew.bat assemble testClasses` | Java 源码与测试类编译通过 |
| `gradlew.bat nativeStage packageNativeMod` | Debug/Release DLL、stage 与 Release mod JAR 构建成功 |
| `gradlew.bat build nativeTest verifyNativePackage` | 纯 Java 3 个、Debug native 22 个用例通过，包结构及哈希检查通过 |
| `gradlew.bat nativeTest -PnativeConfig=Release` | Release native 22 个用例通过 |

共 47 次执行，0 失败、0 跳过。两种配置均在 RTX 4080 上创建/销毁 D3D12/Vulkan 设备与 Direct 队列，
覆盖 ABI 参数边界、线程关闭限制、重复关闭与 Vulkan 失败清理。Debug RHI validation=true，Release=false。
Release 的 RadRay D3D12/Vulkan 实现各有一处 C4702 不可达代码警告，未导致失败。
产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
原始日志、两配置报告副本与汇总保存在本机忽略目录 `build/verification/build-test-20261026/`，不随仓库分发。

该轮验证未启动 MC 客户端、未进入世界、未验证正式启动器安装，也未执行最终 JAR 的独立加载探针。
旧 ABI 客户端验证不代表当前 ABI 3 已完成同样验证；当前没有 swapchain/绘制实现。

## Java 构建与运行

```powershell
.\gradlew.bat build
.\gradlew.bat runClient
```

开发游戏目录为 `run/`，日志为 `run/logs/latest.log`。没有指定 native payload 时，当前代码记录
`No native payload selected; use runClientNative to build and load it`，继续原版渲染。

## 显式准备依赖

需要 Windows x64、Java 25、Python 3.10+、Git、CMake、Ninja 和 Visual Studio C++ x64 工具链。Java/Gradle 同步不下载 RadRay；native configure、build、stage 和游戏启动不恢复第三方包。

在 Raynium 根目录执行：

```powershell
python tools/fetch_radray.py restore
```

版本由 `native/radray.lock.json` 锁定，仓库为 `https://github.com/ksgfk/RadRay.git`，检出到忽略的 `external/RadRay/`。默认代理为 `http://127.0.0.1:10808`，没有 submodule；工作树修改、错误 origin、非受管目录会使脚本停止。已有正确提交时不联网。`python tools/fetch_radray.py status` 只读；更新锁后显式 restore，不执行强制 checkout/reset/clean。

在 x64 Developer PowerShell 中显式准备依赖：

```powershell
$env:HTTP_PROXY = 'http://127.0.0.1:10808'
$env:HTTPS_PROXY = 'http://127.0.0.1:10808'
$env:ALL_PROXY = 'http://127.0.0.1:10808'
$env:NO_PROXY = ''
python external/RadRay/tools/fetch_third_party.py restore
python external/RadRay/tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Debug --parallel 8
python external/RadRay/tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Release --parallel 8
```

Debug 用于默认开发启动，Release 用于分发；只运行相应步骤也可以。构建本身离线，缺包则停止。当前关闭 shader compiler/tools，不要求 DXC SDK。不要复用旧 `windows-x64-msvc` 动态 CRT 包。若手动缩小依赖集合，stage 所需许可证来自 core/render 使用的第三方源码目录，需保留这些源码。

## 开发启动与 IDEA

完成依赖准备后，在 Raynium 根目录执行：

```powershell
.\gradlew.bat runClientNative
```

任务链是 `nativeConfigure → nativeBuild → nativeStage → runClientNative`。脚本沿用已有 x64 MSVC 环境；普通终端/IDEA 中通过 `vswhere` 找到 C++ 安装并读取 `VsDevCmd` 环境，只影响子进程。找不到工具链会给出明确错误。CMake/Ninja 负责增量编译，Gradle 不根据少量文件自行跳过 native build。

可在 IDEA 中建立调用 `:runClientNative` 的 Gradle 运行配置，或从 Gradle 面板运行同名任务。忽略目录中的既有 IDE 配置不保证在其他机器存在。Loom 的 native run 配置已声明；生成配置时执行 `ideaSyncTask`。IDE 中若选择绕过 Gradle 直接启动 JVM 的配置，需先执行 `nativeStage`。

可选参数：

```powershell
.\gradlew.bat runClientNative -PrhiBackend=vulkan
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeParallel=8
.\gradlew.bat nativeStage -PnativePython=C:/path/to/python.exe
.\gradlew.bat nativeBuild -PradraySourceDir=C:/path/to/RadRay
```

`rhiBackend` 默认 `d3d12`，支持 `d3d12|vulkan`，传递给游戏 JVM 的 `raynium.rhi.backend`。正式启动器用 `-Draynium.rhi.backend=vulkan` 选择 Vulkan；不自动切换后端。Debug 默认启用 RHI 校验，驱动调试层/GPU validation 均关闭。

`radraySourceDir` 用于联调本地 checkout，仍要求其静态 CRT SDK 已准备；默认不使用旁边的 RadRay。构建会记录实际 RadRay commit 和 dirty 状态。发布只接受锁定提交的干净 Release 构建。

普通 `runClient` 和 Java `build` 不触发 native 构建。Java 开发 classpath 没有内置 payload 且未指定开发目录时，客户端记录提示并继续原版渲染。`runClientNative` 设置 `raynium.native.required=true` 和开发 stage 目录，缺失/错误 payload 会使初始化失败，不回退到另一份旧 DLL。

Loom 的两个客户端运行配置都给 Minecraft JVM 添加 `--enable-native-access=ALL-UNNAMED`。这是游戏 JVM 参数，不是 Gradle daemon 参数；正式启动器也需添加它。loader 检查实际模块的 native access，权限未启用时报告启动参数要求。

加载成功应记录 DLL 来源与实际路径，以及 ABI/build ID/RadRay revision/dirty/config/CRT/toolchain。当前要求 ABI `3` 和 create/destroy 导出，不再输出探针结果。随后记录 RHI 初始化阶段、后端和 GPU；只有 `Raynium runtime created` 表示设备与队列初始化完成，仍不代表管线或呈现完成。

## 发布 mod JAR

准备 Release 静态 CRT 依赖后执行：

```powershell
.\gradlew.bat packageNativeMod
```

该任务依赖普通 Loom `jar` 与独立的 `nativeStageRelease` 链。MC 26.3 当前使用未混淆的 `net.fabricmc.fabric-loom`，其 Java 产物就是本工程 `jar`；没有调用不存在的 `remapJar`。分发产物为 `build/libs/raynium-<version>-windows-x86_64.jar`，普通 Java JAR 保留。

打包拒绝脏 RadRay、非锁定提交、非 Release payload、重复/保留路径和已签名输入 JAR；检查 `fabric.mod.json`、payload 哈希与许可证后才发布最终文件。
包内容、许可证位置和调试文件边界见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。

单独检查已经生成的包，不触发构建：

```powershell
.\gradlew.bat verifyNativePackage
```

将最终的 `-windows-x86_64.jar` 放入客户端 mods 目录，并给启动器的游戏 JVM 设置 native access 参数。无开发目录参数时，loader 自动解包并加载内置 DLL。Raynium 自身分发许可证仍未选择；当前收集的是 RadRay 与 native 依赖许可证。

## Java 与 RHI 测试

测试统一放在 `src/test/java/`，使用 `gradle.properties` 固定版本的 JUnit Jupiter 与 JUnit Platform launcher。Loom 自动为标准 test 源码集提供 client 类及依赖。测试入口是标准 Gradle Test 任务。

在 Raynium 根目录运行：

```powershell
.\gradlew.bat test
.\gradlew.bat nativeTest
.\gradlew.bat nativeTest -PnativeConfig=Release
.\gradlew.bat nativeTest --tests "*.NativeRuntimeTest"
```

| 测试类 | 内容 | 任务 |
| --- | --- | --- |
| `NativeRuntimeBackendTest` | 后端配置解析和非法值 | `test` |
| `NativeAbiTest` | 参数校验、输出清空、诊断截断和空句柄销毁 | `nativeTest` |
| `NativeRuntimeTest` | 双后端创建/销毁/重建、线程限制、幂等关闭及 Vulkan 失败清理 | `nativeTest` |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

native 测试 JVM 使用 Java 25、`--enable-native-access=ALL-UNNAMED` 与指定配置的 stage 路径。仅一个 worker，禁用 JUnit 并行；每个 runtime 的创建和销毁在同一线程，错误线程用例仅从额外线程调用预期被拒绝的 close。GPU/驱动状态不属于普通 Gradle 输入，因此 `nativeTest` 每次显式执行，不使用 up-to-date 或构建缓存结果。

JUnit `@TempDir` 管理每个用例的 DLL 缓存目录。测试 fixture 和 try-with-resources 保证断言失败时也先释放 native runtime，再卸载 DLL，最后清理临时目录。用例无顺序依赖，测试观测与句柄跟踪仅存在于测试源码，不加入生产对象。

报告分别位于 `build/reports/tests/test/index.html`、`build/reports/tests/nativeTest/index.html`，XML 位于 `build/test-results/<任务名>/`。IDEA 导入 Gradle 后会识别标准测试源码；native 用例通过 Gradle 的 `nativeTest` 任务运行，以获得构建依赖和 JVM 参数。测试类不打入普通或 native mod JAR。JUnit 任务不启动游戏，不能替代真实 MC 生命周期或绘制验证。

## 直接使用 CMake

x64 开发终端中也可从 `native/` 执行：

```powershell
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel 8
cmake --install ../build/native/windows-x64-msvc-staticcrt/Debug --component runtime
```

install 只复制 bridge 和构建元数据，不生成 stage 或 mod JAR。完成直接构建后，可在项目根运行 `python tools/native.py stage --config Debug`。使用本地源码覆盖时 configure 与 stage 传递相同的源码目录；Gradle 的 `-PradraySourceDir` 向两步统一传递该目录。所有下载仍通过显式 fetch 操作和配置的代理进行。

## 任务与产物边界

`gradle/native.gradle` 只调度显式 configure/build/stage，增量编译交给 CMake/Ninja；配置缓存不代表跳过 native 构建。
Debug/Release 的 CMake 目录分别位于 `build/native/windows-x64-msvc-staticcrt/<Config>/`，DLL 与构建元数据在其 `bin/`。
不要同时向同一配置目录执行 configure/build/stage；构建完成后再执行测试，不并行运行构建与测试。
Debug 和 Release 的 `nativeTest` 共用默认报告目录，连续验收时在切换配置前将报告复制到不同的本机验证目录。
stage 内容布局、库依赖限制与加载缓存见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。
