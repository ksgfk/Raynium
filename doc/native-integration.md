# RadRay 引入 Raynium 的构建与加载方案

Raynium 已建立独立的 CMake native 工程，目标产物为 `rayniumruntime.dll`，静态链接 RadRay 所需模块。Java 25 使用 FFM 调用该 DLL 的 C ABI，Gradle 调度显式 native 构建和打包，运行时 loader 负责检查平台、解包、加载和版本。

状态：native CMake 工程、RadRay 拉取脚本、Gradle native 任务、FFM loader 和 Release 分发代码已接入。本机编译、开发客户端启动与 Release payload 加载检查已通过，操作和具体范围见 [Native 工程操作](native-development.md)。当前固定 RadRay `95f725a35ced0a52ae0706207582788c011914ca`，目标为 Windows x86_64；runtime 本身的设计保留在 [Native runtime 功能设计](native-runtime-design.md)。

## Java 工程引入 native 的基本流程

Java classpath 负责 Java 类和资源。C++ 静态库由 native linker 消费，动态库由操作系统加载；把文件放进 Gradle 依赖或 JAR，只完成了文件分发。

实际接入分为四层：

```text
C++ 源码与 RadRay 静态库
    → CMake + 编译器 + linker
    → rayniumruntime.dll
    → Gradle 打包为 JAR 资源
    → Java 解包到实际文件系统并加载
    → JNI 或 FFM 调用导出函数
```

| 层次 | 解决的问题 |
| --- | --- |
| 构建与链接 | native 源码如何编译、依赖哪些库、生成什么架构与配置 |
| 跨语言接口 | Java 如何描述参数、调用函数、处理内存与错误 |
| 分发与加载 | DLL 放在哪里、如何选择架构、如何找到其依赖 |
| 生命周期与版本 | Java 和 DLL 是否匹配、何时初始化/关闭、是否可卸载 |

`System.load` 接收动态库的绝对路径；`System.loadLibrary` 接收不带路径和平台扩展名的库名。FFM 的 `SymbolLookup.libraryLookup(Path, Arena)` 可以按文件路径加载并查找 C 符号，库的寿命与 arena 关联。因此内置在 JAR 中的 DLL 需要先形成真实文件。[JDK 25 System](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/System.html)、[SymbolLookup](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/SymbolLookup.html)

## 已有模组怎么做

以下来源均固定到已调查提交，描述其加载和构建机制，不表示 Raynium 要照搬其目录或全局状态。

| 项目 | 构建和打包 | Java 调用与加载 |
| --- | --- | --- |
| RTX Remix Minecraft | Gradle 的 `compileNativeWin64` 调批处理生成 bridge DLL；`packageJar` 加入按架构组织的 native payload | 解包 native 目录；`System.load` 加载 `remixbridge.dll`，Java `native` 方法进入 JNI |
| Radiance | Java 编译以 `javac -h` 生成 JNI 头；Gradle 消费已安装到 resources 的 native 库，并检查变体 | 从资源复制 `core.dll` 到游戏目录，随后 `System.load`；Java proxy 中声明 native 方法 |
| C2ME native math | Gradle CMake 插件按 target 构建，把平台名放进资源文件名 | 解包到文件，通过 `SymbolLookup.libraryLookup`、`Linker.downcallHandle` 使用 FFM |
| wgpu-mc | Gradle 调 Cargo，复制 DLL 与 C 头，使用 jextract 生成 Java 绑定 | 从 JAR 资源复制 DLL 并 `System.load`；当前代码中可见 jextract 的 `WM.*` 调用，也保留 JNI 声明，不能仅凭 DLL 名判定全项目都是 JNI |

具体源码入口：

- Remix：[native 构建任务 451–460](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/build.gradle#L451-L460)、[分发打包 489–546](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/build.gradle#L489-L546)、[加载 bridge 114–134](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/rtxremix-java/src/com/nvidia/rtxremix/RemixAPI.java#L114-L134)、[JNI 声明](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/f2c6fdaa763f45676f1576e63f8e054bf4ef16af/rtxremix-java/src/com/nvidia/rtxremix/RemixBridge.java#L28-L57)。
- Radiance：[JNI 头生成 204–208](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/build.gradle#L204-L208)、[资源处理 301–308](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/build.gradle#L301-L308)、[DLL 复制与加载 61–86](https://github.com/Minecraft-Radiance/Radiance/blob/948b29a35328ee812b9aa566fc8de2789100291a/src/main/java/com/radiance/client/RadianceClient.java#L61-L86)。
- C2ME：[CMake 和资源任务](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/build.gradle)、[FFM 加载 85–104](https://github.com/RelativityMC/C2ME-fabric/blob/361361de8441e0c842d1190067bc1ef4e83b88c4/c2me-opts-natives-math/src/main/java/com/ishland/c2me/opts/natives_math/common/NativeLoader.java#L85-L104)。
- wgpu-mc：[Cargo 与文件复制任务](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/rust/build.gradle)、[Java 资源与 jextract 配置](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/build.gradle#L35-L65)、[DLL 加载](https://github.com/wgpu-mc/wgpu-mc/blob/65a2fe332ace938067afd143b65f59995e7d01e5/fabric/src/main/java/dev/birb/wgpu/rust/WgpuNative.java#L35-L48)。

Raynium 可组合这些做法：采用 C2ME 的 FFM 调用方向、Remix 的独立 native 构建与分发目录，继续让 CMake 管理 RadRay。首版不需要引入专门的 Gradle CMake 插件。

## RadRay 的引入方式

### 脚本拉取固定提交后作为源码子工程

Raynium 使用 `tools/fetch_radray.py` 按 `native/radray.lock.json` 拉取普通 Git checkout，不使用 submodule。native 工程通过 `RADRAY_SOURCE_DIR` 消费该源码；默认路径为 `external/RadRay`，也可显式覆盖为本机独立工作区：

```text
Desktop/
  RadRay/                       可选的本地联调工作区
  Raynium/
    external/RadRay/            脚本拉取的固定版本，默认源码入口
    native/                     CMake 宿主与 C ABI bridge
    build/native/               Raynium 自己的 native 构建目录
```

使用 `add_subdirectory(RADRAY_SOURCE_DIR, 独立binary目录)` 后，CMake 直接建立对 `RadRay::render` 等 target 的依赖。修改 RadRay 源码会参与下一次 native 增量构建，不必每次先安装一份 SDK。RadRay 自有库当前明确为 STATIC，宿主的 SHARED bridge 可以链接这些静态库。[RadRay 子工程规则](../../RadRay/docs/guide/build-test.md#作为子工程引用)、[render target](../../RadRay/modules/render/CMakeLists.txt)、[CMake 静态与共享库](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)

本机绝对路径放入被忽略的 `native/CMakeUserPresets.json` 或命令行参数，不写入公共 `CMakeLists.txt`。独立的 native binary 目录放在 Raynium 下，不复用 RadRay 现有 standalone build 目录，也不向其 `SDKs/` 写 bridge 产物。

CI 显式运行同一个脚本恢复锁定提交，configure 不自动 checkout 最新分支。脚本拒绝覆盖工作树修改，不自动更换 remote 或强制清理。修改依赖锁后显式 restore 完成升级。本地源码覆盖允许联调未提交修改；后续发布构建需使用明确的干净 revision。

### 稳定后可以使用安装包

RadRay 也已提供 `RADRAY_INSTALL`、`RadRayConfig.cmake` 和 `RadRay::` 导出 targets，后续可切换为：

```cmake
find_package(RadRay CONFIG REQUIRED COMPONENTS render)
target_link_libraries(rayniumruntime PRIVATE RadRay::render)
```

安装包只导出 RadRay 自身，消费方仍需找到兼容的第三方包。这个方式适合独立 SDK/CI 缓存，当前一起开发 runtime 时会增加反复安装步骤，因此首版只实现源码引入模式，保留包模式的扩展位置。[RadRay 安装规则](../../RadRay/cmake/Install.cmake)、[包配置](../../RadRay/cmake/RadRayConfig.cmake.in)

## Native 产物和 CMake 配置

### 一个对 Java 暴露的 DLL

```text
rayniumruntime.dll
  ├─ bridge.cpp 的 C 导出接口
  ├─ RadRay::render / RadRay::core
  ├─ RayniumRuntime 的设备与 Direct 队列
  └─ 后续接入资源服务、pipeline 及所需 shader decoder
```

RadRay 的 C++ 类、STL 容器和 RHI 虚接口保留在 DLL 内部。C 导出函数只使用固定宽度类型、字节范围、opaque handle 和明确的版本契约。`bridge.h` 是 bridge ABI 的权威，Java 布局通过版本与尺寸检查匹配它。

动态库只导出需要的 `rn_*` 符号，使用显式 export 宏或导出列表，不把全部 C++ 符号自动导出。FFM 路线无需 `jni.h`、`JNIEnv` 或链接 JVM。

在 Windows 上，RadRay `.lib` 是构建输入；bridge 生成的 import `.lib` 也是供 native linker 使用的构建产物。玩家运行时主要需要 DLL、必要的动态依赖及 shader 资产；不把 `.lib` 放进 mod JAR。PDB 作为同 build ID 的单独调试包保存。

### 配置基线

实际入口已位于 [native/CMakeLists.txt](../native/CMakeLists.txt)，下面保留配置边界的示意；源码默认路径、版本检查、输出目录和安装规则以实际文件为准。

```cmake
cmake_minimum_required(VERSION 3.25)
project(RayniumNative LANGUAGES C CXX)

set(RADRAY_SOURCE_DIR "" CACHE PATH "RadRay source checkout")

set(RADRAY_BUILD_TESTS OFF)
set(RADRAY_BUILD_BENCHMARKS OFF)
set(RADRAY_BUILD_COMPILE_COMMANDS OFF)
set(RADRAY_BUILD_WINDOW OFF)
set(RADRAY_BUILD_RENDER ON)
set(RADRAY_BUILD_SHADER OFF)
set(RADRAY_BUILD_SHADER_COMPILER OFF)
set(RADRAY_BUILD_SHADER_TOOLS OFF)
set(RADRAY_ENABLE_MIMALLOC ON)
set(RADRAY_ENABLE_PROFILER OFF)
set(RADRAY_ENABLE_ZLIB OFF)
set(RADRAY_ENABLE_LIBPNG OFF)
set(RADRAY_ENABLE_LIBJPEG OFF)
set(RADRAY_ENABLE_AUTO_OPTIMIZATION OFF)
set(RADRAY_INSTALL OFF)

add_subdirectory("${RADRAY_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/radray")
add_library(rayniumruntime SHARED src/bridge.cpp)
target_include_directories(rayniumruntime PRIVATE include)
target_compile_features(rayniumruntime PRIVATE cxx_std_20)
target_link_libraries(rayniumruntime PRIVATE RadRay::render)
```

将依赖三元组、compiler、generator、CRT 和 Debug/Release 放在 preset/toolchain 中，保证它们在创建 targets 之前确定。当前使用 Windows x64 MSVC、`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` 和 `windows-x64-msvc-staticcrt` 依赖包；这是编译工具链选择，不改变 RadRay 管理图形后端的边界。

| 项目 | 建议与原因 |
| --- | --- |
| 配置 | 使用独立 Debug 与 Release 目录；当前仓库依赖准备流程只接受这两种配置，不直接套用别人的 RelWithDebInfo |
| CRT | Debug/Release 均使用 `/MT`，依赖仍按 Debug/Release 分目录；不混入动态 CRT 或 `/MTd` 包 |
| DLL 分配器 | 使用 RadRay 传递的静态 mimalloc whole-archive 与 CRT/new/delete 替换，不再包含 `mimalloc-new-delete.h`；具体边界见 [Native 工程操作](native-development.md#c-abi-与内存边界) |
| Profiler | 最小接入阶段关闭，后续显式启用并确认 native profiler 的宿主生命周期 |
| Window | MC 已管理 SDL 窗口，关闭 RadRay window 模块；后续向 runtime 传递 HWND |
| Shader compiler/tools | 当前加载验证不需要；运行时不附带 DXC。后续需要 shader artifact decoder 时启用 shader 模块 |
| 图像解码 | Java 已准备像素，首版不需要 native PNG/JPEG 解码 |
| 自动 CPU 优化 | 关闭按构建机器探测的自动 SIMD/FMA，发布时使用明确的最低 CPU 基线 |

这些开关仅配置 Raynium 的 native 宿主构建，不修改 RadRay 独立工程的默认值。现有 RadRay 的 allocator whole-archive、profiler 和依赖约束见 [构建与测试](../../RadRay/docs/guide/build-test.md)。

依赖恢复、第三方构建、主工程构建继续分阶段。开发者先显式执行 RadRay 的 fetch/prepare；Raynium 的 CMake 配置缺包时给出缺失项，不自动下载或编译 third_party。所有需要联网的恢复使用 `127.0.0.1:10808`。

## Java 调用方式

| 方案 | 形式 | 对 Raynium 的适用性 |
| --- | --- | --- |
| JNI | Java `native` 方法 + JNI 导出或注册 | 成熟，适合大量 Java 对象交互；需要 JNI glue |
| FFM | 普通 C 导出 + `SymbolLookup` + `Linker` | 与 Java 25、连续帧数据和通用 native runtime 匹配，推荐 |
| JNA | 额外 Java 库映射 C 函数 | 可用于一般 native 接口，当前没有引入它的必要 |

推荐 FFM 的原因是接口与对象边界吻合，不预先声称它一定比 JNI 更快。几何和资源仍按批提交，避免把每个顶点或每条 draw 都变成一次跨语言调用。

首版 API 数量少，手写小型 binding 即可；以后若头文件明显增大，再增加针对 `bridge.h` 的显式 jextract 生成任务，输出到 build/generated。不要把整个 RadRay C++ 头文件树交给 Java binding 工具。

客户端入口拥有 `NativeRuntime`，它持有 native handle 与 `NativeLibrary`；后者保存 library arena 和 `NativeBindings`。绑定在加载阶段建立 downcall handles 并检查 ABI/构建身份。CLIENT_STARTED 创建 RHI，CLIENT_STOPPING 先销毁 RHI 再卸载 DLL。线程、内存和失败清理见 [FFM 互操作](ffm-interop.md)。

RenderFrame、上传或 fence 等待可能耗时或阻塞，使用普通 downcall。C2ME 快速数学函数所用的 `Linker.Option.critical` 不适用于这些调用。[JDK critical 调用约束](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.Option.html)

开发运行配置需要在 Minecraft JVM 上启用相应 native access，例如 unnamed module 使用 `--enable-native-access=ALL-UNNAMED`。这与 `org.gradle.jvmargs` 不同，后者只配置 Gradle daemon。正式启动器也需要满足 JDK 的 native access 要求；mod 不能在 JVM 已启动后靠改系统属性补上启动权限。

## Gradle 与 CMake 的分工

Gradle 不重新列举 C++ 编译参数和 RadRay 源文件；它调用 CMake 的 configure/build/stage 入口。IDEA 同步只建立任务模型，不执行外部进程，也不恢复 native 依赖。

已在 `gradle/native.gradle` 和 Loom runs 中接入以下显式任务：

| 任务 | 行为 |
| --- | --- |
| `nativeConfigure` | 配置 Raynium 自己的 CMake binary 目录 |
| `nativeBuild` | 依赖 configure，调用 CMake 增量构建 bridge |
| `nativeStage` | 检查直接 DLL 依赖，收集 bridge、许可证及 manifest，生成不可变的待分发快照 |
| `runClientNative` | 构建并 stage，然后以开发 DLL 目录启动客户端 |
| `nativeConfigureRelease` / `nativeBuildRelease` / `nativeStageRelease` | 专供分发的 Release 链，不受开发配置参数影响 |
| `packageNativeMod` | 构建 Release native，将 payload 合入 Loom `jar` 产物，生成可分发 mod |
| `verifyNativePackage` | 检查已有包的结构、平台、ABI/build ID 字段及哈希，不触发构建或执行 DLL |

普通 `runClient` 不自动选择开发 stage，也不隐式启动 C++ 构建；未指定 `raynium.native.dir` 且 classpath 没有 payload 时继续原版渲染。`runClientNative` 构建 native 并设置开发 stage 路径及 required 标志。普通 Java `build` 产物与包含 native 的正式包使用不同文件名。

当前 MC 26.3 使用未混淆的 `net.fabricmc.fabric-loom`，正式包消费本工程的 `jar` 任务，没有 `remapJar` 步骤。native 资源属于二进制 payload，不参与 Minecraft mappings 重命名。

`packageNativeMod` 将 Java 产物与 native payload 合并。合并时拒绝重复/保留路径和已签名输入，检查 `fabric.mod.json`、manifest、DLL 和许可文件后才发布最终文件。后续需要发布到模组平台时使用这个最终产物。

Gradle 配置使用 lazy task registration 和 Provider；外部命令只在 task 执行阶段运行，保留当前 configuration cache 的使用方向。让 CMake/Ninja 决定 C++ 源码是否需要增量重编译，不能只把 `bridge.cpp` 声明为输入后让 Gradle错误地跳过整个 native build。

开发构建目录不参与跨机器 Gradle build cache 复用；可缓存的是带工具链、配置、RadRay revision 和内容标识的完整 native 发布包。`tools/native.py` 沿用已有 x64 C++ 环境，或通过 `vswhere` 和 `VsDevCmd` 为子进程建立环境，供从桌面启动的 IDEA 使用。

## 产物目录和打包布局

```text
Raynium/
  native/
    CMakeLists.txt
    CMakePresets.json
    CMakeUserPresets.json        本机路径，被忽略
    include/raynium/bridge.h
    src/bridge.cpp
  src/client/java/dev/raynium/bridge/
    NativeLibrary.java
    NativeLibraryLocator.java
    NativeBindings.java
    NativeManifest.java
  build/native/
    windows-x64-msvc-staticcrt/Debug/      CMake binary tree
    windows-x64-msvc-staticcrt/Release/
    stage/windows-x86_64/<Debug|Release>/
      current.json
      <manifest-sha256>/
        rayniumruntime.dll
        native-manifest.json
        licenses/<dependency>/...
```

正式 JAR 内采用独立于 MC asset namespace 的目录：

```text
natives/windows-x86_64/
  rayniumruntime.dll
  native-manifest.json
META-INF/licenses/raynium-native/<dependency>/...
```

首版只携带 Windows x86_64 payload。架构依据当前 JVM 进程的 `os.arch` 选择并规范化 `amd64/x86_64` 等名称，不能只看宿主机器的硬件架构。以后增加平台时扩展目录和匹配表，不改变 Java 场景接口。

manifest 由 stage 任务生成，记录 bridge ABI、布局尺寸/schema 版本、目标平台、Debug/Release、CRT、工具链、build ID、RadRay revision/dirty 标志、DLL 与许可证的文件名/大小/SHA-256，以及分类后的系统依赖。当前只允许一个 bridge DLL，尚不支持私有动态依赖。二进制通过 `rn_query_abi` 入口返回实际 ABI/build 信息，加载后与 manifest 核对。

manifest 的哈希用于识别内容与检测错误/不完整副本，不单独构成代码来源认证。对外发布不包含 Debug CRT 依赖、`.obj`、静态 `.lib`、CMake cache、源码绝对路径配置或本机 SDK 全目录。

## 加载流程

loader 的选择顺序为：显式开发目录 `-Draynium.native.dir=...`，否则读取 mod JAR 中的 payload 并检查平台。开发目录无效时直接报告具体路径，不回退到旧内置版本。

两条来源共用后续流程：

1. 读取 manifest，确认 JVM 架构、ABI 和配置。
2. 将实际 payload 发布到游戏目录下 `.raynium/natives/windows-x86_64/<content-hash>/`。
3. 检查已有完整副本，或写入独立 staging 目录，校验后再发布；多进程使用文件锁协调解包。
4. 检查游戏 JVM native access 权限，以绝对路径调用 `SymbolLookup.libraryLookup`。
5. 先调用版本入口核对 ABI 3，再用带诊断缓冲区的 `rn_query_abi` 检查布局与构建身份。
6. 记录加载结果，通过 `rn_create_runtime` 创建 RHI 设备/Direct 队列；创建失败清理并输出 MC 崩溃报告，仍不接管 MC 绘制。

所有内容路径落在 loader 自己的根目录内，当前仅接受固定 DLL 文件名。已有损坏缓存直接报错，不覆盖、不删除；暂未实现旧缓存自动清理。

开发也加载内容副本，而不是直接加载 linker 输出目录中的 DLL。Windows 占用已加载 DLL 时，这样仍能重编译新的 bridge；重启客户端后加载新 build。Java 热更新不能替代 native ABI 或 DLL 的重新加载。

### DLL 的依赖搜索

主 DLL 使用绝对路径，并不保证其间接依赖会自动从同目录找到。Windows 对依赖 DLL 的搜索和进程已加载模块仍有独立规则。[Windows DLL 搜索顺序](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)

当前静态链接 RadRay、CRT 与第三方实现，保留 Windows/驱动系统依赖。stage 通过 `dumpbin /dependents` 拒绝动态 CRT 和未分类的私有 DLL。未来需要私有动态库时，再扩展 manifest 的依赖列表、按依赖顺序绝对路径预加载并保持寿命；复杂依赖图或运行时按名称加载的库，需要单独设计基于 `LoadLibraryExW` 明确搜索策略的 bootstrap。

不通过修改进程全局 PATH 或 SetDllDirectory 来影响其他 Minecraft 模组。也不保证不同模组使用同名 DLL 的不同版本可以同时隔离：使用有辨识度的库名，并减少私有动态依赖。

Debug/Release 均使用 `/MT`，不分发动态 VC++ CRT。Debug 保留调试信息与 RadRay 调试宏，但不启用 `/MTd`，以满足 mimalloc 的静态 CRT 替换条件。驱动和系统图形 DLL 不从开发机复制进 mod；当前未使用 shader compiler，也不打包 `dxcompiler.dll`。

## 首个前置任务的范围

构建/加载基础与 RHI 初始化已写入源码及任务，验证范围见下方链接：

1. 已建立 native CMake 宿主，默认消费脚本按锁定提交拉取的 RadRay 源码，链接 `RadRay::render`。
2. 已按固定提交切换 `/MT`，移除重复 new/delete 替换头；`rayniumruntime.dll` 提供版本查询、240 字节 ABI/build 描述和 runtime create/destroy。临时探针已删除，ABI 为 `3`。
3. 已建立 Gradle configure/build/stage 和开发启动链，包含桌面 IDEA 所需 MSVC 子进程环境发现。
4. 已建立 Java FFM loader 与 binding，开发目录和 JAR 来源共用内容缓存，由客户端 CLIENT_STARTED 加载并创建 RHI，在 CLIENT_STOPPING 销毁 runtime 后关闭 library arena。
5. 已提供独立 Release 打包入口，合并 DLL、manifest 与上游许可证；独立包检查任务不触发构建。

首版仍使用原版 MC 渲染，Mixin 不必接管世界或窗口。`RayniumRuntime` 实现在本项目 native 工程并调用 `RadRay::render`，不引用不存在的 `RadRay::runtime` target；后续扩展资源、管线和帧接口。

已验证开发目录加载、JAR 解包加载、构建身份不匹配、缓存损坏、重复加载/释放和 native access 缺失等路径；架构不匹配、系统 DLL 缺失、多个游戏进程同时启动及正式启动器安装等场景尚未验证。结果以 [本机验证范围](native-development.md#本机验证范围) 为准。

当前前置能力是：Java 能加载 native 库并核对 ABI/构建身份，DLL 能随 mod JAR 分发加载；runtime 已创建真实 RHI 设备与队列，资源提交和绘制在后续实现中接入。
