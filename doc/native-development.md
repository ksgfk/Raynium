# Native 构建、加载与分发

已实现 RadRay 固定版本拉取、`/MT` native 库、Gradle 显式 native 任务、Java 25 FFM loader 与 Release mod JAR 打包。库名和 CMake target 为 `rayniumruntime`，已实现 ABI/构建身份检查、RHI 设备/Direct 队列和 Java 生命周期；游戏继续使用原版渲染，资源、管线及渲染接管尚未实现。FFM 实施建议见 [互操作设计](ffm-interop.md)。

## 本机验证范围

2026-10-26，Windows x64、Temurin JDK 25.0.4、MSVC 19.51.36260.0、固定 RadRay `95f725a35ced0a52ae0706207582788c011914ca`：

- core/render 所需的 15 个第三方包已通过代理恢复并准备 Debug/Release 静态 CRT SDK。
- `nativeStage packageNativeMod` 成功，包含 Java 编译、Debug/Release DLL 构建和 Release JAR 打包检查。新增 runtime create/destroy；临时探针保持删除。
- 标准 JUnit `test` 的 3 个纯 Java 用例通过；ABI 3 / 静态 Create 重构后，Debug/Release `nativeTest` 各 22 个用例通过（含新增 query 诊断与失败不改写 info 的检查），无失败、无跳过：在 NVIDIA GeForce RTX 3060 上分别创建、销毁和重新创建 D3D12/Vulkan 设备与 Direct 队列，验证错误参数、输出清空、诊断截断、空指针销毁、Java 错误线程关闭和重复关闭。Vulkan 环境被占用时返回初始化失败，失败对象不破坏已有实例，释放后可重建。Debug 日志确认 RHI validation=true，Release=false。
- JUnit 迁移时，普通 `build` 通过且未触发 native 构建；确认 JAR 不含测试类/JUnit。相同 Release `nativeTest` 再次运行复用了 Gradle 配置缓存，但仍实际执行测试。最后细化 create 参数诊断后，Debug/Release 各重跑 15 个 `NativeAbiTest` 用例通过。
- 独立 Java 进程使用最终 Release JAR 中的 Java 类和 DLL，通过 ZIP 文件系统提供 `ModContainer.findPath`，验证 `rayniumruntime.dll`、ABI `3`、内置 payload 解包、缓存复用、两次加载/关闭、D3D12/Vulkan 真实 RHI 创建/销毁，以及构建身份不符、缓存被修改、缺少 native access 的拒绝路径。Debug/Release 导出均只有版本、查询、create 和 destroy 四个入口。
- 此前 ABI 2 的 MC 26.3 开发客户端分别用 D3D12/Vulkan 完成真实 RHI 初始化、运行 100 个 tick 并正常退出（进程码 0）。忽略目录中的测试 mod 调用 `client.stop()`；日志确认渲染线程 CLIENT_STOPPING → 等待队列 → runtime destroyed → DLL closed，没有桌面输入或生产测试钩子。
- 此前 ABI 2 额外通过完整 `runClientNative -PrhiBackend=vulkan` Gradle 链启动并正常退出，确认参数转发；测试 fixture 仅通过忽略目录中的 init script 注入。
- 此前 ABI 2 的隔离客户端由测试 fixture 预先占用 Vulkan 环境，真实 RHI 创建失败后确认关闭本次 DLL 引用，MC 生成包含后端、DLL/build ID、RadRay commit 和当时 status=3 的崩溃报告（当前统一为 FAILED=1）并退出（-1），没有退回原版启动。
- 此次 ABI 3 / 静态 Create 重构未重跑 MC 客户端。未进入世界、未验证 swapchain/绘制、设备丢失或正式启动器安装。独立 JAR 测试不替代正式启动器验证。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。本机验证脚本、日志和截图保存在忽略目录 `build/verification/`；不是随 mod 分发的文件。

此前开发账号的 Realms 认证错误来自离线开发令牌；RadRay 完整 Release 编译中的 C4702 不可达代码警告未造成构建失败。当前初始化会执行真实 RadRay RHI 设备/队列创建，但尚无渲染提交；不能据此声称已替换 MC 渲染。

## 文件入口

| 文件 | 职责 |
|---|---|
| `native/radray.lock.json`、`tools/fetch_radray.py` | 固定上游提交、经代理显式拉取普通 Git checkout |
| `native/CMakeLists.txt`、`native/CMakePresets.json` | C++20 bridge、静态 CRT、独立 Debug/Release 目录与构建身份 |
| `native/include/raynium/bridge.h`、`native/src/bridge.cpp` | C ABI 查询与 runtime create/destroy |
| `native/include/raynium/runtime.h`、`native/src/runtime.cpp` | 设备/Direct 队列、后端初始化及逆序清理 |
| `src/test/java/dev/raynium/bridge/` | 标准 JUnit 测试；后端配置、C ABI 边界与真实 RHI 生命周期 |
| `tools/native.py` | MSVC 环境发现、调用 CMake、stage、依赖检查和 JAR 合并 |
| `gradle/native.gradle` | Gradle native 任务，配置阶段不执行外部命令 |
| `src/client/java/dev/raynium/bridge/` | manifest、开发/JAR 定位、解包缓存、FFM 和库寿命 |
| `src/client/java/dev/raynium/client/RayniumClient.java` | CLIENT_STARTED 加载/创建、CLIENT_STOPPING 销毁/卸载、初始化崩溃报告 |

## 显式准备依赖

需要 Windows x64、Java 25、Python 3.10+、Git、CMake、Ninja 和 Visual Studio C++ x64 工具链。Java/Gradle 同步不下载 RadRay；native configure、build、stage 和游戏启动不恢复第三方包。

在 Raynium 根目录执行：

```powershell
python tools/fetch_radray.py restore
```

当前固定 `95f725a35ced0a52ae0706207582788c011914ca`，仓库为 `https://github.com/ksgfk/RadRay.git`，检出到忽略的 `external/RadRay/`。默认代理为 `http://127.0.0.1:10808`，没有 submodule；工作树修改、错误 origin、非受管目录会使脚本停止。已有正确提交时不联网。`python tools/fetch_radray.py status` 只读；更新锁后显式 restore，不执行强制 checkout/reset/clean。

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

本机已加入忽略提交的 IDEA Gradle 运行配置 **Minecraft Client (Raynium Native)**，调用 `:runClientNative`。重新导入工程后也可直接运行 Gradle 面板中的同名任务。Loom 的 native run 配置已声明；生成配置时执行 `ideaSyncTask`。IDE 中若选择绕过 Gradle 直接启动 JVM 的配置，需先执行 `nativeStage`。

可选参数：

```powershell
.\gradlew.bat runClientNative -PrhiBackend=vulkan
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeParallel=8
.\gradlew.bat nativeStage -PnativePython=C:/path/to/python.exe
.\gradlew.bat nativeBuild -PradraySourceDir=C:/Users/xiaoxs/Desktop/RadRay
```

`rhiBackend` 默认 `d3d12`，支持 `d3d12|vulkan`，传递给游戏 JVM 的 `raynium.rhi.backend`。正式启动器用 `-Draynium.rhi.backend=vulkan` 选择 Vulkan；不自动切换后端。Debug 默认启用 RHI 校验，驱动调试层/GPU validation 均关闭。

`radraySourceDir` 用于联调本地 checkout，仍要求其静态 CRT SDK 已准备；默认不使用旁边的 RadRay。构建会记录实际 RadRay commit 和 dirty 状态。发布只接受锁定提交的干净 Release 构建。

普通 `runClient` 和 Java `build` 不触发 native 构建。Java 开发 classpath 没有内置 payload 且未指定开发目录时，客户端记录提示并继续原版渲染。`runClientNative` 设置 `raynium.native.required=true` 和开发 stage 目录，缺失/错误 payload 会使初始化失败，不回退到另一份旧 DLL。

Loom 的两个客户端运行配置都给 Minecraft JVM 添加 `--enable-native-access=ALL-UNNAMED`。这是游戏 JVM 参数，不是 Gradle daemon 参数；正式启动器也需添加它。loader 检查实际模块的 native access，权限未启用时报告启动参数要求。

加载成功应记录 DLL 来源与实际路径，以及 ABI/build ID/RadRay revision/dirty/config/CRT/toolchain。当前要求 ABI `3` 和 create/destroy 导出，不再输出探针结果。随后记录 RHI 初始化阶段、后端和 GPU；只有 `Raynium runtime created` 表示设备与队列初始化完成，仍不代表管线或呈现完成。

## Stage、缓存和生命周期

构建目录为 `build/native/windows-x64-msvc-staticcrt/<Debug|Release>/`，DLL 与 `native-build.json` 位于其 `bin/`。

`nativeStage` 用 `dumpbin /dependents` 检查直接 DLL 导入：发现动态 CRT 或未列入规则的私有 DLL 时停止。当前只分发一个 bridge DLL，系统/驱动依赖不复制。以后需要私有动态库时，先补明确的 stage 和加载规则。

stage 目录为：

```text
build/native/stage/windows-x86_64/<Config>/
  current.json
  <manifest-sha256>/
    native-manifest.json
    rayniumruntime.dll
    licenses/<dependency>/...
```

先生成并检查完整快照，再发布目录和 `current.json`。同一构建目录不要由多个进程同时 configure/build/stage。manifest 记录平台、ABI、布局尺寸、build ID、RadRay commit/dirty、配置、CRT、工具链、DLL 与许可证的大小/hash，以及分类后的系统依赖。build ID 来自 native/RadRay 源码、配置和工具链指纹；DLL SHA-256 标识实际二进制，两者用途不同。

Java 优先读取 `-Draynium.native.dir=<目录>`，该目录可以是带 `current.json` 的配置根，也可以是具体快照。否则读取 mod JAR 内 payload。两条入口都复制到游戏目录 `.raynium/natives/windows-x86_64/<manifest-sha256>/`，检查大小和 SHA-256，并以文件锁协调多个进程；仅完整副本会发布。已有损坏缓存会报错，不覆盖可能正在使用的 DLL。

加载副本不会占用 linker 输出文件。重新编译后重启客户端即可加载新快照；不提供运行中 DLL 热切换，也不自动删除旧快照/游戏缓存。哈希用于一致性检查，不代替代码来源认证。

`RayniumClient` 拥有 `NativeRuntime`，后者持有不透明 native handle 和 `NativeLibrary`。库使用 shared arena；runtime 的创建与关闭受 Java 所有者线程检查约束。CLIENT_STARTED 创建，CLIENT_STOPPING 先销毁 runtime 再关闭库；不使用 Cleaner/shutdown hook。初始化失败清理部分 native 对象和 DLL 后显式生成 MC 崩溃报告。详细顺序和限制见 [设备会话](native-runtime-design.md#当前实现设备会话)。

## 发布 mod JAR

准备 Release 静态 CRT 依赖后执行：

```powershell
.\gradlew.bat packageNativeMod
```

该任务依赖普通 Loom `jar` 与独立的 `nativeStageRelease` 链。MC 26.3 当前使用未混淆的 `net.fabricmc.fabric-loom`，其 Java 产物就是本工程 `jar`；没有调用不存在的 `remapJar`。分发产物为 `build/libs/raynium-<version>-windows-x86_64.jar`，普通 Java JAR 保留。

JAR 内包含：

```text
natives/windows-x86_64/native-manifest.json
natives/windows-x86_64/rayniumruntime.dll
META-INF/licenses/raynium-native/<dependency>/...
```

打包拒绝脏 RadRay、非锁定提交、非 Release payload、重复/保留路径和已签名输入 JAR；检查 `fabric.mod.json`、payload 哈希与许可证后才发布最终文件。不包含 PDB、静态/import `.lib`、SDK 全目录或本机绝对路径。PDB 保留在本地构建目录。

单独检查已经生成的包，不触发构建：

```powershell
.\gradlew.bat verifyNativePackage
```

将最终的 `-windows-x86_64.jar` 放入客户端 mods 目录，并给启动器的游戏 JVM 设置 native access 参数。无开发目录参数时，loader 自动解包并加载内置 DLL。Raynium 自身分发许可证仍未选择；当前收集的是 RadRay 与 native 依赖许可证。

## C ABI 与内存边界

| 导出 | 契约 |
|---|---|
| `rn_get_abi_version()` | ABI 版本 `3` |
| `rn_query_abi(info, capacity, error_message, error_capacity)` | 填充调用方的 240 字节结构；失败不改动 info，原因写入诊断字符串 |
| `rn_create_runtime(desc, out_runtime, error_message, error_capacity)` | 通过 RayniumRuntime::Create 创建完整对象；失败清理局部资源并将有效输出槽清零，原因写入诊断字符串 |
| `rn_destroy_runtime(runtime)` | 等待 Direct 队列并销毁对象，NULL 无操作；非空对象只能销毁一次 |

`rn_query_abi` 与 `rn_create_runtime` 都只返回 OK/FAILED；具体原因写入 UTF-8 诊断字符串，不从返回码区分参数、容量或设备失败。query 也允许 NULL 诊断指针配零容量；诊断缓冲区必须与 info 分离。

`rn_runtime_desc` 是 8 字节、4 字节对齐的两个 uint32：`struct_size` 必须为 8，`backend` 为 1（D3D12）或 2（Vulkan）。描述、输出指针槽和诊断缓冲区不能重叠，均只在调用期间借用。诊断使用 UTF-8，容量大于零时截断并 NUL 终止；NULL 诊断指针必须配零容量。当前诊断为 ASCII，截断不会切断多字节字符。native 对象只能由同一 DLL 的 destroy 释放。

ABI 结构布局以 `bridge.h` 为准，包含四个 uint32 和固定容量 UTF-8 字符串。Java 检查尺寸、指针宽度、flags、build ID、RadRay revision、配置、平台和工具链与 manifest 一致。结构和临时内存由 Java arena 管理，不跨边界 free/delete。`NativeBindings` 使用普通 FFM downcall，不使用 critical 调用。

当前 ABI 为 `3`：所有返回状态的函数只使用 `RN_STATUS_OK=0` 和 `RN_STATUS_FAILED=1`，query 增加诊断缓冲区参数；240 字节查询结构布局保持不变。Java 在调用新 query 前核对版本，manifest 也只接受 ABI 3；旧 ABI 1/2 payload 必须重新构建，不能复用旧签名。开发与发布均需重新构建/stage，旧 ABI 的 manifest 会被拒绝。stage 和 JAR 只收集 `rayniumruntime.dll`，不自动删除旧构建目录或旧内容缓存。

Debug/Release 均使用 `/MT` 和静态 mimalloc；Debug 保留调试信息和 RadRay 调试宏，不启用 `/MTd` 的 Debug CRT。mimalloc 自带分配符号替换，bridge 不包含 `mimalloc-new-delete.h`。覆盖标准 CPU 堆分配入口，不覆盖 GPU 显存、系统/驱动内部或直接调用系统内存 API 的分配。

## Java 与 RHI 测试

测试统一放在 `src/test/java/`，使用 `gradle.properties` 固定版本的 JUnit Jupiter 与 JUnit Platform launcher。Loom 自动为标准 test 源码集提供 client 类及依赖。项目不再使用独立 smoke main、自定义 JavaCompile 或 JavaExec 测试入口。

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

报告分别位于 `build/reports/tests/test/index.html`、`build/reports/tests/nativeTest/index.html`，XML 位于 `build/test-results/<任务名>/`。IDEA 导入 Gradle 后会识别标准测试源码；native 用例通过 Gradle 的 `nativeTest` 任务运行，以获得构建依赖和 JVM 参数。测试类不打入普通或 native mod JAR。真实 MC 启动/退出仍由客户端生命周期验证覆盖，本次 JUnit 任务不启动游戏。

## 直接使用 CMake

x64 开发终端中也可从 `native/` 执行：

```powershell
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel 8
cmake --install ../build/native/windows-x64-msvc-staticcrt/Debug --component runtime
```

install 只复制 bridge 和构建元数据，不生成 stage 或 mod JAR。完成直接构建后，可在项目根运行 `python tools/native.py stage --config Debug`。使用本地源码覆盖时 configure 与 stage 应传递相同的源码目录；优先用 Gradle 的 `-PradraySourceDir` 统一传递。所有下载仍通过显式 fetch 操作和配置的代理进行。
