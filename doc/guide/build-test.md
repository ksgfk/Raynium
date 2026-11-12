> - 适用: 恢复依赖、构建、测试、启动与分发 Raynium
> - 权威: 本文维护当前可执行命令、测试覆盖与产物边界；ABI、内容缓存和加载契约归互操作文档
> - 锚点: `build.gradle`, `gradle/native.gradle`, `gradle.properties`, `native/CMakeLists.txt`, `native/CMakePresets.json`, `native/radray.lock.json`, `native/cmake/profiler-dependency/CMakeLists.txt`, `tools/fetch_radray.py`, `tools/build_debug_dependencies.py`, `tools/native.py`, `src/test/java/dev/raynium/bridge/*.java`

# 构建与测试

以下命令在 Raynium 根目录执行。Java/IDEA 设置见[开发环境](dev-environment.md)。
普通 `build` 只构建 Java 并执行纯 Java 测试；native 必须通过显式任务准备。
payload、ABI、缓存和关闭顺序见[互操作与加载](../architecture/interop.md)。

开发、验证与采样的按日期记录见[历史索引](../history/README.md#开发验证与采样记录)。

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
python tools/build_debug_dependencies.py --parallel 8
python external/RadRay/tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Release --parallel 8
```

Debug 用于默认开发启动，Release 用于分发；只运行相应步骤也可以。构建本身离线，缺包则停止。
普通构建验证并复制已提交的 `.rnsp` 资源，native 只链接 RadRay shader decoder，关闭 compiler/tools，不要求 DXC SDK。资源验证使用 Python。
Debug 使用 `/MTd`、Debug CRT 和默认 STL iterator 检查；Release 使用 `/MT`。两配置均静态链接 mimalloc，不复用 MD 包。
锁定 RadRay 的 `staticcrt` 三元组在 Debug 也固定 MT，因此 Debug 使用 Raynium 的显式离线脚本，
按上游依赖 catalog 准备当前所需的 16 个包；构建和 SDK 位于 `build/native-dependencies/windows-x64-msvc-mtd/`。
该脚本自动获取 x64 MSVC 环境，普通终端也可执行；源码仍由 fetch 阶段准备，缺失即失败。
Debug 的 mimalloc 关闭 CRT 覆盖并保留显式 `mi_*` 分配，Release 保留静态覆盖；详见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。
依赖源码或配方升级后显式加 `--clean` 重建；两种脚本只清理各自的输出目录。
Release 如只准备当前 core/render/runtime 所需集合及测试库，可向上游 build 命令添加
`--only fmt,magic_enum,eigen,sigslot,stdexec,spdlog,xxHash,yyjson,pugixml,mimalloc,Vulkan-Headers,volk,VulkanMemoryAllocator,DirectX-Headers,D3D12MemoryAllocator,googletest`。
stage 所需许可证来自这些第三方源码目录，需保留源码。

修改 `native/shaders/` 中的 HLSL 或共享 include 后显式准备锁定 DXC 并生成 shader pack：

```powershell
python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc
python tools/compile_shader_pack.py
python tools/shader_pack.py src/client/resources/assets/raynium/shaderpacks/default.rnsp
```

生成器在 `build/shader-tools/Release/` 编译离线工具，输出并提交 `src/client/resources/assets/raynium/shaderpacks/default.rnsp`。
默认编译八个必需程序；`--program` 可显式指定完整程序集合，生成结果仍需满足全部必需条目；`--output` 指定输出，`--radray-source` 指定同一依赖工作树。
`--clean` 重新配置并全量重编离线工具。原生 DLL 不包含编译器；格式、反射校验和来源规则见[Shader pack](../architecture/shader-pack.md)。
`verifyShaderPack` 随 `processClientResources` 检查包和八程序双后端条目；`testShaderPackTools` 随 `check` 运行容器拒绝测试。

MSVC 升级后，清理对应配置的 `build/native/` CMake 输出及旧 stage，再用依赖脚本的 `--clean` 重编相应静态库。
删除前解析并核对目标在构建输出目录内；不删除源码、下载 SDK 或游戏数据。
启用 profiler 的配置还需运行 `python tools/build_profiler_dependencies.py --config Debug --clean` 或对应 Release 命令。
CMake 子进程使用隐藏控制台，使本地化 MSVC `/showIncludes` 与 Ninja 的依赖扫描使用同一输出代码页。

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
.\gradlew.bat runClientNative -PrhiBackend=vulkan -PrhiValidation=true -PvulkanValidation=true
.\gradlew.bat runClientNative -PpreviewEnabled=false
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeParallel=8
.\gradlew.bat nativeStage -PnativePython=C:/path/to/python.exe
.\gradlew.bat nativeBuild -PradraySourceDir=C:/path/to/RadRay
```

`rhiBackend` 默认 `d3d12`，支持 `d3d12|vulkan`，传递给游戏 JVM 的 `raynium.rhi.backend`。
正式启动器用 `-Draynium.rhi.backend=vulkan` 选择 Vulkan；不自动切换后端。
MC 自身须在视频设置中选择 Vulkan 并重启，runtime 后端参数不替 MC 切换图形 API。
预览默认开启，`previewEnabled=false` 对应 `-Draynium.preview.enabled=false`。
内容为完整区块地形。`-PterrainTextureTransfer=auto|direct|split`
选择自动降级、强制多 mip 共享或强制逐 mip fallback（JVM 属性 `raynium.terrain.textureTransfer`）。
`-PterrainDisableFaceCull=true` 关闭 Java 面组剔除以进行对照。
`-PterrainProfile=true` 记录 CPU/GPU 分阶段 p50/p95/p99、上传字节和驻留统计；GPU 需支持 timestamp query。
对应 JVM 属性为 `raynium.terrain.disableFaceCull`、`raynium.terrain.profile`。
`-PterrainSubmission=auto|direct|indirect` 对应 `raynium.terrain.submission`，在 renderer 创建时生效。
Debug RHI/driver validation 会增加 CPU 开销，性能对比应记录配置且固定视点、RT 尺寸和场景。

Debug 默认启用 RadRay RHI 检查。`rhiValidation=true`（JVM 属性 `raynium.rhi.validation`）
显式启用 native 驱动诊断，默认 false。客户端初始化入口通过 `rn_init_env` 准备所选后端的环境，
D3D12 在此创建 factory 并启用调试层，早于 MC 图形初始化；CLIENT_STARTED 复用环境创建 runtime。
也可通过独立 `nativeTest -PrhiValidation=true` 验证。
`vulkanValidation=true` 给 MC 添加 `--vulkanValidation` 参数，检查 MC Vulkan 命令流。
两者含义不同；native Vulkan 开关同时启用同步验证。GPU validation 未开启。

`radraySourceDir` 用于联调本地 checkout；Debug 先用 `tools/build_debug_dependencies.py --radray-source <路径>` 准备 MTd 依赖，Release 准备该 checkout 的静态 CRT SDK。
默认不使用旁边的 RadRay。构建会记录实际 RadRay commit 和 dirty 状态。发布只接受锁定提交的干净 Release 构建。

普通 `runClient` 和 Java `build` 不触发 native 构建。Java 开发 classpath 没有内置 payload 且未指定开发目录时，客户端记录提示并继续原版渲染。`runClientNative` 设置 `raynium.native.required=true` 和开发 stage 目录，缺失/错误 payload 会使初始化失败，不回退到另一份旧 DLL。

Loom 的两个客户端运行配置都给 Minecraft JVM 添加 `--enable-native-access=ALL-UNNAMED`。这是游戏 JVM 参数，不是 Gradle daemon 参数；正式启动器也需添加它。loader 检查实际模块的 native access，权限未启用时报告启动参数要求。

加载成功应记录 DLL 来源与实际路径，以及 ABI/build ID/RadRay revision/dirty/config/CRT/toolchain。
日志的 CRT 在 Debug 为 `MTd`、Release 为 `MT`；没有 allocator DLL 的预加载或 redirected 状态。静态分配器随 bridge 加载与卸载。
当前要求 ABI `13` 和十三个导出。`Raynium runtime created` 表示设备与队列完成；
`Raynium shared preview ready` 表示输出资源已创建和导入；`Raynium terrain inputs ready` 记录输入模式及 mip 数。实际显示仍须检查游戏画面。

## 纯地形截图对照

```powershell
.\gradlew.bat runClientNative -PterrainCompareDir=build/terrain-compare -PterrainCompareInterval=120
```

`terrainCompareDir` 为配对截图与误差 JSON 的输出目录；对照行为见[地形预览](../architecture/terrain-preview.md#诊断与限制)。
普通透明和改进透明度分别跟随游戏设置。手动修改 options.txt 时使用 graphicsPreset 为 custom，避免画质预设覆盖 improvedTransparency。
此开关增加参考绘制和读回开销，常规性能采样不启用它。

## 自动跑图与保存 Tracy

`tools/benchmark.py` 读取 JSON 配置，先在持有源存档锁时复制存档，再构建 Release/MT、启用 Tracy 的 native payload 与 Java 客户端。
它在独立游戏目录使用 MC Vulkan、指定 native 后端、关闭垂直同步和两侧驱动校验，通过 quick play 打开副本，
等待旁观模式、native 帧和采集器连接后预热，按路线插值移动，完成后正常关闭游戏并保存采样。

```powershell
python tools/benchmark.py --config tools/benchmark.example.json --check
python tools/benchmark.py --config tools/benchmark.example.json
# 对已有配置临时开启 JFR，同时保留 Tracy。
python tools/benchmark.py --config tools/benchmark.example.json --jfr
```

配置中的相对路径以配置文件目录为基准；`world` 指向含 `level.dat` 的源存档，`tracyCapture` 指向同版本命令行采集器。
默认及示例视距为 32 区块、模拟距离 8 区块；MC 26.3 的 `Options` 在最大堆不少于 1 GB 时将原版视距上限设为 32。
`optionsFile` 中的视距会被配置值覆盖。默认窗口 1280×720、预热 20 秒；示例路线 60 秒。
`terrainSubmission` 选择并记录提交策略，默认 auto；`warmupSeconds`、视距、窗口尺寸、超时和包含位置、yaw、pitch、秒数的 `route` 可在配置中设置。
采样使用现有地形预览，native RT 仍是窗口宽高的四分之一；原版绘制继续执行。

每轮产物位于 `outputDir` 下的独立时间目录，包含存档副本、解析后的配置、构建/游戏/采集日志、payload 清单、
`client-result.json`、`summary.json` 和 `session.tracy`。脚本检查路线完成、native 帧数量、资源关闭日志和 Tracy 保存结果。
配置 `jfrEnabled` 默认为 `false`，示例设为 `true`；也可用 `--jfr` 对本次运行强制开启。
开启后通过已有的 `-PjfrFile` 入口在游戏 JVM 启动时录制 `profile` JFR，正常退出时将 `session.jfr` 保存到同一轮目录。
脚本检查该文件存在、长度和 JFR 文件头，`summary.json` 记录开关及文件字节数；完整读回可运行 `jfr summary <session.jfr>`。
JFR 事件范围及磁盘缓冲上限见下节。源存档、原游戏选项和已有采样目录不被实际跑图写入。

## Tracy 与整段客户端采样

`-PnativeProfiler=true` 启用 native Tracy 标记，默认关闭；对应 `tools/native.py configure --profiler`
和 CMake `RAYNIUM_ENABLE_PROFILER`。开关计入构建身份，清单记录 `profilerEnabled`，开启时 stage 包含 Tracy 许可证。
先用 `python external/RadRay/tools/fetch_third_party.py restore --only tracy` 准备源码，再运行
`python tools/build_profiler_dependencies.py --config Release`（Debug 使用 `--config Debug`）。
脚本显式离线构建当前配置的静态 CRT Tracy SDK，位于 `build/native-dependencies/tracy/<配置>/sdk/`；native 构建不隐式准备该 SDK。
Tracy 源码或配方升级后，准备命令加 `--clean` 清理并重建所选配置的 build 与 SDK。
当前锁定依赖使用 Tracy 0.15.0，客户端为静态、on-demand、仅 localhost，关闭广播和 crash handler。
专用 SDK 开启 delayed init/manual lifetime：合法环境创建前启动，创建失败时清理，环境销毁后、FFM 卸载 DLL 前停止并回收线程。
`native/cmake/profiler-dependency/CMakeLists.txt` 显式向 Tracy client 及其导出 target 添加 `TRACY_DELAYED_INIT`；
不能仅传同名 CMake 变量，因为当前第三方 CMake 没有将它声明为编译选项。依赖脚本使用独立的 `build-manual-lifetime/` 构建目录。
它不使用上游默认 SDK 的静态析构退出方式，避免在 Windows loader lock 内等待 Tracy 工作线程。

先启动同版本 `tracy-capture`，再启动游戏；下例中的工具路径和输出目录由调用方准备：

```powershell
tracy-capture.exe -a 127.0.0.1 -o C:/captures/session.tracy
# 另一个终端：
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true '-PjfrFile=C:/captures/session.jfr'
```

Release 默认关闭 RadRay RHI wrapper 检查；上面的两个 validation 参数分别关闭 native 驱动诊断和 MC Vulkan 校验。
ABI 参数检查及资源寿命保护仍然执行。`terrainProfile=true` 开启已有 GPU timestamp 与阶段统计，并将已完成帧的 GPU 毫秒数写入 Tracy plot。
该数值覆盖 native 命令的 timestamp 区间，在回收槽时发布，不是 MC 全帧 GPU 耗时，也不是发布时那一帧的即时耗时。

`Raynium.Render` 覆盖完整 native Render，包括 BeginFrame 槽回收、驻留更新、参数准备及 RunFrame；`Raynium.RunFrame` 仅覆盖 Extract、RDG Record 和队列提交。
`Raynium.Terrain.ResolveVisible`、`Sort`、`BuildArguments`、`UploadArguments` 和 `Record` 分别记录可见解析、排序、参数构建、参数上传和几何录制；没有逐 draw profiling scope。
`Raynium.Visible.Solid/Cutout/Translucent` 记录各层输入项；`Raynium.Terrain.<层>.SelectedGroups/Records/Batches` 记录各层选中面组、参数记录及批次。
`Raynium.Terrain.BuiltRecords/BuiltBatches/ArgumentBytes` 只计参数生成一次的量，`OitParameterBuilds` 在有 OIT 透明输入时为 1。
`Raynium.Frame.SubmittedRecords/SubmittedBatches/SubmittedIndicesInstances` 汇总实际几何 pass，OIT 透明部分计三次。
`Raynium.Frame.DirectCalls/IndirectCalls/BindingChanges` 记录几何提交的实际 API 调用和绑定操作；记录数与原生 indirect 调用数分别观察。
`Raynium.Frame.Sequence` 标识当前输入帧；`Raynium.GpuFrame.Sequence` 与同时发布的 `Raynium.GpuMilliseconds` 对应已完成的原始帧，不能归给发布时正在准备的帧。
`Raynium.Geometry.VertexPages/IndexPages` 记录池保留页数，`Raynium.Terrain.BindingStates` 记录活跃精确绑定状态数。启用 Tracy 时，每 120 帧发布 `Raynium.Geometry.Vertex/IndexReservedBytes`、`Vertex/IndexFreeBytes` 与 `Vertex/IndexAllocatedBytes`；Allocated 为保留容量减空闲量，包含等待退役的区间。这些稀疏样本不能把未发布帧当成零，也不表示最大连续空闲区间。runtime 另提供几何池、上传、槽回收、RDG 编译/屏障/录制区间。
父 CPU 区间包含子区间，不直接相加；GPU 时间线独立读取。
Tracy frame mark 在一次 native Render 调用结束时发出；间隔包含两次调用之间的宿主工作，不等同于 native CPU 用时。
客户端是 on-demand，因此 native 库加载前、采集器连接前的工作不在 Tracy 中；它也不提供 Java 调用栈。

`-PjfrFile=<路径>` 在游戏 JVM 启动时启用 `profile` JFR，正常退出时保存，磁盘缓冲上限 1 GiB；超出上限会淘汰较早数据。
JFR 包含 JVM 执行采样、GC 等事件；`dev.raynium.TerrainFrame` 按原始序号记录输入准备、Java 驻留准备及 renderPrepared/FFM 包络。FFM 包络包含 native Render，不能再次与 native 时间相加。`dev.raynium.ClientFrame` 记录每次正常返回的 `Minecraft.renderFrame` 耗时及是否加载世界。
未启用 JFR 事件时不创建逐帧 event。异常退出的未完成帧不会提交该 event；强制结束进程也不保证完成最终文件保存。
完整游戏的 Java 与 native 分析分别读取 JFR 和 Tracy，不能将采样次数直接当作精确函数耗时。

## 发布 mod JAR

准备 Release 静态 CRT 依赖后执行：

```powershell
.\gradlew.bat packageNativeMod
```

该任务依赖普通 Loom `jar` 与独立的 `nativeStageRelease` 链。MC 26.3 当前使用未混淆的 `net.fabricmc.fabric-loom`，其 Java 产物就是本工程 `jar`；没有调用不存在的 `remapJar`。分发产物为 `build/libs/raynium-<version>-windows-x86_64.jar`，普通 Java JAR 保留。

打包拒绝脏 RadRay、非锁定提交、非 Release payload、重复/保留路径和已签名输入 JAR；检查 `fabric.mod.json`、payload 哈希与许可证后才发布最终文件。
包内容、许可证位置和调试文件边界见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。

单独检查已经生成的包，不触发构建；与 `packageNativeMod` 联合执行时，校验排在打包之后：

```powershell
.\gradlew.bat verifyNativePackage
```

将最终的 `-windows-x86_64.jar` 放入客户端 mods 目录，并给启动器的游戏 JVM 设置 native access 参数。无开发目录参数时，loader 自动解包并加载内置 DLL。Raynium 自身分发许可证仍未选择；当前收集的是 RadRay 与 native 依赖许可证。
Raynium 自身无需动态 VC++ CRT 或额外 mimalloc DLL；系统与显卡驱动依赖仍由运行环境提供。

## Java 与 RHI 测试

测试统一放在 `src/test/java/`，使用 `gradle.properties` 固定版本的 JUnit Jupiter 与 JUnit Platform launcher。Loom 自动为标准 test 源码集提供 client 类及依赖。测试入口是标准 Gradle Test 任务。

在 Raynium 根目录运行：

```powershell
.\gradlew.bat test
.\gradlew.bat nativeTest
.\gradlew.bat nativeTest -PrhiValidation=true
.\gradlew.bat nativeTest -PnativeConfig=Release
.\gradlew.bat nativeTest --tests "*.NativeRuntimeTest"
```

| 测试类 | 内容 | 任务 |
| --- | --- | --- |
| `TerrainSubmissionTest` / `NativeRuntimeBackendTest` | 提交模式/后端配置解析和非法值 | `test` |
| `BenchmarkRouteTest` | 路线位置与最短 yaw 插值、端点限制、非法时间与角度拒绝 | `test` |
| `NativeManifestTest` | Debug/MTd 与 Release/MT 单 DLL 清单；CRT/配置错配、MD/MDd、schema 2、缺失/多余项、非法路径和哈希拒绝 | `test` |
| `PreviewRegionTest` | 显示矩形、MC Y 翻转后的复制位置和小窗口边界 | `test` |
| `BuiltinShaderPackTest` | 内置来源筛选、缺失资源与流关闭 | `test` |
| `NativeShaderPackTest` | 互斥/未知提交 flags 拒绝、双后端容器/artifact/反射拒绝、ABI 10 描述符拒绝、不可变快照和创建失败后重建 | `nativeTest` |
| `NativeAbiTest` | 参数校验、输出清空、诊断截断和空句柄销毁 | `nativeTest` |
| `NativeRuntimeTest` | 双后端环境/设备创建、重建、线程限制、幂等关闭、单例保护及早期 factory 共享绘制 | `nativeTest` |
| `MeshPayloadTest` / `TextureTransferTest` | 保守面组、紧凑元素位、越界模型、ByteBuffer 顶点拥有权和失败降级 | `test` |
| `TerrainCaptureTest` / `TerrainResidencyTest` | 采集引用释放、超预算近处优先与超大网格、关闭回退延迟回收、零掩码、提交边界与预览重建 | `test` |
| `NativeFrameBufferTest` | 持久帧指针/数量复位、超过 8 MiB 的上传扩容、对齐及多实例掩码 | `test` |
| `NativeTransparencyTest` | 双后端、direct/indirect 与两传输模式的镂空阈值、排序索引更新、OIT 顺序无关、高叠层、窄深度范围、模式切换与尺寸往返 | `nativeTest` |
| `NativeTerrainTest` | 双后端 direct/indirect 的 host-written 多记录参数、非零参数偏移与 firstInstance，以及两传输模式的 mip、深度、大坐标、epoch、重建、双槽在途参数/资源隔离、驻留字典增长、多材质/多 submesh 缓存、填满 32 MiB 索引页后的跨页绑定、非零 firstIndex、索引扩容与局部增删改、稠密 primitive 删除交换及资源更新、多几何池页与 base vertex、分页 staging 增长后复用、删除后区间重用、空可见帧上传后跨槽及尺寸往返绘制 | `nativeTest` |
| `NativeRendererTest` | 双后端雾色清屏/区域外像素、连续交接、输出重建保留场景、错误线程、超时保留 owner 和导入失败清理 | `nativeTest` |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native；FFM 缓冲测试使用 JVM native access。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

专用参数单元测试是可选 CMake 目标。已准备匹配 CRT 的 GTest 后，在 x64 开发终端执行：

```powershell
cmake --preset win-x64-debug -S native -DRAYNIUM_BUILD_TESTS=ON
cmake --build build/native/windows-x64-msvc-mtd/Debug --target test_raynium_terrain --parallel 8
ctest --test-dir build/native/windows-x64-msvc-mtd/Debug -R Terrain --output-on-failure
```

该目标覆盖不连续/最高位/全掩码、重复项、稳定排序、完整 VB/IB 与材质版本身份、IB 交错合批、超过旧 12 位容量的状态集合、状态引用回收及编号耗尽、primitive 查找表的扩容/删除/下标更新与完整 uint64 ID、排序 payload 与独立展开结果比较、多实例、合并边界、计数溢出、小上限拆分及模式能力选择，不需要 GPU。
像素对照可分别以 `-PterrainSubmission=direct` 保存 `testImages`，再以 `indirect` 和 `testBaseline` 使用相同输入与容差比较。

通用 runtime 测试位于 RadRay 的 `modules/runtime/tests/`，由 `test_radray_runtime` / GTest 注册，
直接链接 `RadRay::runtime`，不包含 MC bridge。在已准备依赖并完成 configure 的 RadRay checkout 执行：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-msvc/Debug --config Debug
cmake --build --preset win-x64-debug --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug -R 'FrameScheduler|GeometryBufferPool|GeometryUploadGpuTest|MappedUploadBuffer|BatchedBufferUpload|RDG|Material' --output-on-failure
```

RadRay standalone 的测试依赖需在该 checkout 显式准备，包括 GTest；runtime 的 CPU 用例使用 fake，GeometryUploadGpuTest 需要显式准备 shader fixture 和双后端 GPU。
`nativeTest` 只运行 Raynium 的适配/共享交接回归，不构建上游 GTest。

native 测试 JVM 使用 Java 25、`--enable-native-access=ALL-UNNAMED` 与指定配置的 stage 路径。仅一个 worker，禁用 JUnit 并行；每个 runtime 的创建和销毁在同一线程，错误线程用例从额外线程调用预期被拒绝的创建、渲染或关闭。GPU/驱动状态不属于普通 Gradle 输入，因此 `nativeTest` 每次显式执行，不使用 up-to-date 或构建缓存结果。

JUnit `@TempDir` 管理每个用例的 DLL 缓存目录。测试 fixture 和 try-with-resources 保证断言失败时也先释放 native runtime 和环境，再卸载 DLL，最后清理临时目录。用例无顺序依赖，测试观测与句柄跟踪仅存在于测试源码，不加入生产对象。

预览测试在三种尺寸上各交接 8 帧，使用测试专属读回 buffer 校验每个像素；生产路径无读回。
消费者启用 Khronos validation 和同步验证，依赖本机安装的 Vulkan SDK/layer；缺失时测试失败。
原生后端诊断日志与 JUnit 结果一起检查，游戏内生命周期仍须单独运行。

`-PtestImages=<绝对目录>` 可保存 NativeTerrainTest 已有固定像素场景的 PNG；`-PtestBaseline=<绝对目录>` 对对应图片逐像素比较，缺失基线也视为失败。
文件名包含 backend、测试名、尺寸与序号；direct/split 输入路径预期相同，因此共用基线。双槽隔离用例使用独立的逐像素预期，不依赖这些基线文件。
这些选项只影响测试 fixture，不进入生产渲染路径或分发包。

报告分别位于 `build/reports/tests/test/index.html`、`build/reports/tests/nativeTest/index.html`，XML 位于 `build/test-results/<任务名>/`。IDEA 导入 Gradle 后会识别标准测试源码；native 用例通过 Gradle 的 `nativeTest` 任务运行，以获得构建依赖和 JVM 参数。测试类不打入普通或 native mod JAR。JUnit 任务不启动游戏，不能替代真实 MC 生命周期或绘制验证。

## 直接使用 CMake

x64 开发终端中也可从 `native/` 执行：

```powershell
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug --parallel 8
cmake --install ../build/native/windows-x64-msvc-mtd/Debug --component runtime
```

install 只复制 bridge 和构建元数据，不生成 stage 或 mod JAR。完成直接构建后，可在项目根运行 `python tools/native.py stage --config Debug`。使用本地源码覆盖时 configure 与 stage 传递相同的源码目录；Gradle 的 `-PradraySourceDir` 向两步统一传递该目录。所有下载仍通过显式 fetch 操作和配置的代理进行。

## 任务与产物边界

`gradle/native.gradle` 只调度显式 configure/build/stage，增量编译交给 CMake/Ninja；配置缓存不代表跳过 native 构建。联合运行测试与 Release 打包时，打包配置链等待 nativeTest，避免同一 Ninja 目录被并发写入。
Debug 的 CMake 目录为 `build/native/windows-x64-msvc-mtd/Debug/`，Release 为 `build/native/windows-x64-msvc-staticcrt/Release/`；
DLL 与构建元数据在各自 `bin/`，上游 `radrayruntime.lib` 位于该配置的 `radray/_build/<Config>/` 子目录。
不要同时向同一配置目录执行 configure/build/stage；构建完成后再执行测试，不并行运行构建与测试。
Debug 和 Release 的 `nativeTest` 共用默认报告目录，连续验收时在切换配置前将报告复制到不同的本机验证目录。
stage 内容布局、库依赖限制与加载缓存见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。
