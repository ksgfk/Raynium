# Native 构建、加载与分发

已实现 RadRay 固定版本拉取、`/MT` bridge、Gradle 显式 native 任务、Java 25 FFM loader 与 Release mod JAR 打包。当前只做 ABI、RadRay 调用和分配器探针，游戏继续使用原版渲染；runtime 和渲染 Mixin 尚未实现。本机编译与启动验证结果见下文，命令供后续复现。

## 本机验证范围

2026-10-26，Windows x64、Temurin JDK 25.0.4、MSVC 19.51.36260.0、固定 RadRay `95f725a35ced0a52ae0706207582788c011914ca`：

- 经 `127.0.0.1:10808` 恢复 core/render 所需的 15 个第三方源码，并分别构建 Debug/Release 静态 CRT SDK。
- `build`、`nativeStage`、`packageNativeMod`、`verifyNativePackage` 均成功；Gradle configuration cache 正常写入，后续 native 增量构建报告无须重编译。
- `runClientNative` 启动 MC 26.3，Python 只读窗口截图确认进入首次欢迎界面，进程与窗口响应正常。日志确认 Debug DLL 加载、ABI `1`、RadRay 顶点 stride `12`、mimalloc mask `7`，实际 revision 与锁一致。
- 独立 Java 进程使用最终 Release JAR 中的 Java 类和 DLL，通过 ZIP 文件系统提供 `ModContainer.findPath`，验证内置 payload 解包、内容缓存复用、FFM 探针、两次加载/关闭，以及构建身份不符、缓存被修改、缺少 native access 的拒绝路径。这覆盖 loader 的 JAR 路径，不等于另行安装到正式启动器后的整机测试。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。本机验证脚本、日志和截图保存在忽略目录 `build/verification/`；不是随 mod 分发的文件。

本次未进入世界或验证渲染替换。开发账号的 Realms 认证错误来自离线开发令牌；不影响客户端欢迎界面和 native 初始化。Release 编译存在 RadRay D3D12/Vulkan 实现中的 C4702 不可达代码警告，未造成构建失败。

## 文件入口

| 文件 | 职责 |
|---|---|
| `native/radray.lock.json`、`tools/fetch_radray.py` | 固定上游提交、经代理显式拉取普通 Git checkout |
| `native/CMakeLists.txt`、`native/CMakePresets.json` | C++20 bridge、静态 CRT、独立 Debug/Release 目录与构建身份 |
| `native/include/raynium/bridge.h`、`native/src/bridge.cpp` | 固定尺寸 C ABI 和不创建设备的探针 |
| `tools/native.py` | MSVC 环境发现、调用 CMake、stage、依赖检查和 JAR 合并 |
| `gradle/native.gradle` | Gradle native 任务，配置阶段不执行外部命令 |
| `src/client/java/dev/raynium/bridge/` | manifest、开发/JAR 定位、解包缓存、FFM 和库寿命 |
| `src/client/java/dev/raynium/client/RayniumClient.java` | 客户端初始化加载、退出时关闭库 |

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
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeParallel=8
.\gradlew.bat nativeStage -PnativePython=C:/path/to/python.exe
.\gradlew.bat nativeBuild -PradraySourceDir=C:/Users/xiaoxs/Desktop/RadRay
```

`radraySourceDir` 用于联调本地 checkout，仍要求其静态 CRT SDK 已准备；默认不使用旁边的 RadRay。构建会记录实际 RadRay commit 和 dirty 状态。发布只接受锁定提交的干净 Release 构建。

普通 `runClient` 和 Java `build` 不触发 native 构建。Java 开发 classpath 没有内置 payload 且未指定开发目录时，客户端记录提示并继续原版渲染。`runClientNative` 设置 `raynium.native.required=true` 和开发 stage 目录，缺失/错误 payload 会使初始化失败，不回退到另一份旧 DLL。

Loom 的两个客户端运行配置都给 Minecraft JVM 添加 `--enable-native-access=ALL-UNNAMED`。这是游戏 JVM 参数，不是 Gradle daemon 参数；正式启动器也需添加它。loader 检查实际模块的 native access，权限未启用时报告启动参数要求。

加载成功应出现三类日志：DLL 来源与实际路径；ABI/build ID/RadRay revision/dirty/config/CRT/toolchain；顶点 stride 为 `12`、分配器 mask 为 `7`。这只是 native 接入完成标志，不表示渲染接管。

## Stage、缓存和生命周期

构建目录为 `build/native/windows-x64-msvc-staticcrt/<Debug|Release>/`，DLL 与 `native-build.json` 位于其 `bin/`。

`nativeStage` 用 `dumpbin /dependents` 检查直接 DLL 导入：发现动态 CRT 或未列入规则的私有 DLL 时停止。当前只分发一个 bridge DLL，系统/驱动依赖不复制。以后需要私有动态库时，先补明确的 stage 和加载规则。

stage 目录为：

```text
build/native/stage/windows-x86_64/<Config>/
  current.json
  <manifest-sha256>/
    native-manifest.json
    raynium_bridge.dll
    licenses/<dependency>/...
```

先生成并检查完整快照，再发布目录和 `current.json`。同一构建目录不要由多个进程同时 configure/build/stage。manifest 记录平台、ABI、布局尺寸、build ID、RadRay commit/dirty、配置、CRT、工具链、DLL 与许可证的大小/hash，以及分类后的系统依赖。build ID 来自 native/RadRay 源码、配置和工具链指纹；DLL SHA-256 标识实际二进制，两者用途不同。

Java 优先读取 `-Draynium.native.dir=<目录>`，该目录可以是带 `current.json` 的配置根，也可以是具体快照。否则读取 mod JAR 内 payload。两条入口都复制到游戏目录 `.raynium/natives/windows-x86_64/<manifest-sha256>/`，检查大小和 SHA-256，并以文件锁协调多个进程；仅完整副本会发布。已有损坏缓存会报错，不覆盖可能正在使用的 DLL。

加载副本不会占用 linker 输出文件。重新编译后重启客户端即可加载新快照；不提供运行中 DLL 热切换，也不自动删除旧快照/游戏缓存。哈希用于一致性检查，不代替代码来源认证。

`NativeLibrary` 实例持有 shared arena，由客户端入口拥有；退出事件关闭 arena。当前只有同步探针，没有 runtime、工作线程和 Java 回调。后续加入这些对象时，必须先停止调用并销毁 runtime，再关闭库。

## 发布 mod JAR

准备 Release 静态 CRT 依赖后执行：

```powershell
.\gradlew.bat packageNativeMod
```

该任务依赖普通 Loom `jar` 与独立的 `nativeStageRelease` 链。MC 26.3 当前使用未混淆的 `net.fabricmc.fabric-loom`，其 Java 产物就是本工程 `jar`；没有调用不存在的 `remapJar`。分发产物为 `build/libs/raynium-<version>-windows-x86_64.jar`，普通 Java JAR 保留。

JAR 内包含：

```text
natives/windows-x86_64/native-manifest.json
natives/windows-x86_64/raynium_bridge.dll
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
| `rn_get_abi_version()` | ABI 版本 `1` |
| `rn_query_abi(info, capacity)` | 填充调用方的 240 字节结构，返回 `0` 成功、`1` 空指针、`2` 容量不足；失败不写输出 |
| `rn_probe_vertex_stride()` | 调用 RadRay `GetVertexFormatSizeInBytes(FLOAT32X3)`，预期 `12` |
| `rn_probe_allocator()` | CRT malloc、RadRay 对齐分配、C++ new 分别对应 bit 0/1/2，预期 `7` |

ABI 结构布局以 `bridge.h` 为准，包含四个 uint32 和固定容量 UTF-8 字符串。Java 检查尺寸、指针宽度、flags、build ID、RadRay revision、配置、平台和工具链与 manifest 一致，再运行两个探针。结构和临时内存由 Java arena 管理，不跨边界 free/delete。`NativeBindings` 使用普通 FFM downcall，不使用 critical 调用。

Debug/Release 均使用 `/MT` 和静态 mimalloc；Debug 保留调试信息和 RadRay 调试宏，不启用 `/MTd` 的 Debug CRT。mimalloc 自带分配符号替换，bridge 不再包含 `mimalloc-new-delete.h`。覆盖标准 CPU 堆分配入口，不覆盖 GPU 显存、系统/驱动内部或直接调用系统内存 API 的分配。运行时探针检查代表性调用，不是所有依赖路径的穷举证明。

## 直接使用 CMake

x64 开发终端中也可从 `native/` 执行：

```powershell
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel 8
cmake --install ../build/native/windows-x64-msvc-staticcrt/Debug --component runtime
```

install 只复制 bridge 和构建元数据，不生成 stage 或 mod JAR。完成直接构建后，可在项目根运行 `python tools/native.py stage --config Debug`。使用本地源码覆盖时 configure 与 stage 应传递相同的源码目录；优先用 Gradle 的 `-PradraySourceDir` 统一传递。所有下载仍通过显式 fetch 操作和配置的代理进行。
