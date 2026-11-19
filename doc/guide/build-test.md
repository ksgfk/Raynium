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

开发游戏目录默认为 `run/`，日志位于其 `logs/latest.log`；`-PgameRunDir=<目录>` 可选择独立游戏目录。
客户端必须有 native payload；普通 `runClient` 仅在已通过开发目录参数或内置资源提供 payload 时可启动，否则明确失败。

原版参考入口独立于 native 构建与加载：

```powershell
.\gradlew.bat runClientReference -PgameRunDir=build/reference-game
```

该入口保留测试驱动 Mixin，使用原版渲染并以 `--graphicsBackend VULKAN` 选择后端；检查日志中的 `Using graphics backend`，原版回退不算 Vulkan 对照。
存档、资源包及 options 由指定游戏目录提供，运行对照使用存档副本。`benchmarkConfig`/`benchmarkRunDir` 接入路线驱动，`tools/benchmark.py` 支持原版 Vulkan 与 Raynium 两个后端的独立采样。

视觉驱动通过 `-PvalidationConfig=<JSON 路径>` 显式开启，不能与 benchmarkConfig 同时使用。将存档复制到独立游戏目录的 `saves/validation/`，选择 `-PquickPlayWorld=validation -PgameUsername=RayniumTest`；用户名限制为 1–16 个字母、数字或下划线。参考场景配置见 [vanilla-scenes.json](../../tools/validation/vanilla-scenes.json)，包含平台布置、实体、物品、普通透明/OIT、GUI、重载、线框与雨天。配置中的命令会修改测试存档，须使用副本。
两种启动入口均使用 VisualValidation：服务端命令完成后设置相机，等待指定帧数、时长和区块编译，再经原版截图入口保存 PNG。可指定 gameTime、animationMillis、partialTick、rain/thunder、vignetteBrightness、wireframe、reload、postEffect 和 screen，并支持顶层 defaults。视觉模式固定 GUI 鼠标位置及光照闪烁；动画时钟作用于附魔、sprite、世界边界、实体 tickCount 及末地水晶，gameTime 同步末地闪光状态。产物在配置目录，包括各场景 PNG、设置和 visual-result.json；每场景五分钟超时，结束后正常关闭游戏。测试驱动完成不代表图像差异已获接受。
设置 animationMillis 的场景在命令和资源重载完成后显式调用一次原版纹理 tick，使冻结世界也应用目标 sprite 帧与插值时刻；正常游戏与性能路线不启用此行为。
[validate_visual.py](../../tools/validate_visual.py) 依次复制存档、运行 reference/d3d12/vulkan，输出原图和放大差分图，并按 64 像素区域统计差异；差分步骤依赖 Pillow 和 NumPy，可用 `--compare-only` 重建差分。[vanilla-effects.json](../../tools/validation/vanilla-effects.json) 包含特殊材质、内置后处理、屏幕覆盖与维度切换。
`particles` 显式列出原版 ParticleOptions codec 数据、位置、速度及数量；配合 particleSeed/particleTicks，在场景稳定后通过原版粒子工厂和 tick 生成固定状态，并停用该场景的自动粒子添加与推进。种子覆盖粒子实例、引擎以及 SpellParticle 的静态随机源。未指定 particles 时保留原版粒子行为。blockAnimations 按方块位置设置附魔台 time/flip/open/rotation 或折跃门 age/cooldown，原版渲染器继续提取这些固定输入。panorama 和 textureExports 调用原版全景截图及纹理导出入口，等待 PNG 写完后推进场景；差分工具同时比较这些导出图像。结果中的 selectedVanillaPipelines 记录场景选择过的目录管线，供覆盖核对，不代表每个像素或管线变体已获验收。

[vanilla-interactions.json](../../tools/validation/vanilla-interactions.json) 固定物品使用、图腾激活、调试线、地图和方块裂纹输入。useItemTicks/useItemHand 设置原版当前物品的使用时刻与手；activationTicks 固定图腾激活的 0–39 tick；debugEntries 指定原版调试项，breakingBlock 指定位置和阶段。mapPattern 创建并锁定原版地图保存数据、固定颜色并通过原版更新包交给客户端；screen=title 打开标题全景。clearNotifications 仅在显式配置的视觉场景中清除异步聊天和 toast，避免它们的实际到达时间干扰世界像素比较。

[vanilla-specials.json](../../tools/validation/vanilla-specials.json) 提供风灵、狼盔甲、纹饰、拴绳、临时方块、闪电及龙的输入。animationMillis 同时固定实体 tick 和闪电几何种子；entityHurtTicks/dragonDeathTicks 固定对应提取状态。visualLightning 使用原版闪电的仅视觉标志，避免随机点火改变场景光照；clouds 选择 fast/fancy/off；requiredEntities 在截图时检查客户端实体已到达，结果保存实体类型计数。runner 拒绝 MC 场景解码警告、native abort 和 GPU validation 错误，防止原版将无效输入回退为默认内容后仍通过采集。

`tools/validation/create_resource_pack.py` 生成隔离的模型、不同尺寸纹理、插值动画和 bitmap 字体输入；使用其输出的 options.txt 时，视觉 runner 同时复制相邻 resourcepacks。`--truetype-font <本地字体路径>` 将字体复制到生成的测试包以覆盖灰度字形，`--oversized-gui-item` 将钻石 GUI 模型放大并设置 oversized_in_gui，触发原版超尺寸物品绘制；字体文件不加入源码。`--translucent-compass` 修改原版指南针纹理的 alpha，由原版材质分析选择透明特殊闪光变体。
[vanilla-gallery.json](../../tools/validation/vanilla-gallery.json) 的 screen=gallery 使用原版提取 API 绘制皮肤、书本、旗帜、图表和物品，galleryPhase 固定皮肤旋转与翻书状态，场景包含重复帧和连续资源重载。该夹具按 640×360 GUI 布局使用，1280×720 窗口选 guiScale=2，并配合上述超尺寸物品测试包。
视觉驱动从原版画中画 blit 入口记录 pictureInPictureTypes，requiredPictureInPicture 声明必须出现的类型；缺少类型时失败，缓存命中也记录实际 blit 类型。
[vanilla-fonts.json](../../tools/validation/vanilla-fonts.json) 配合 TrueType 与透明指南针测试包，覆盖世界文字、告示牌、see-through 字形、透明物品、第一人称特殊闪光及反胃覆盖；options.txt 中设 screenEffectScale=0 以选择原版反胃覆盖路径。
[vanilla-entity-variants.json](../../tools/validation/vanilla-entity-variants.json) 覆盖同队隐身实体的半透明显示、潜影贝深度偏移、画的正反面及移动实心/镂空方块，并分别选择普通透明和 OIT。
[vanilla-translucent-sorting.json](../../tools/validation/vanilla-translucent-sorting.json) 配合资源包生成器的 `--translucent-sorting-items`，通过两个重叠透明 quad 检查物品 atlas 和超尺寸物品的正交排序及全局 OIT 条件；沿用 gallery 的窗口与 GUI 缩放设置。
[vanilla-water-mask.json](../../tools/validation/vanilla-water-mask.json) 覆盖船内水体遮罩的普通透明/OIT 合成和 OIT 世界边界。
[vanilla-outlines.json](../../tools/validation/vanilla-outlines.json) 使用 `highContrastBlockOutline:true` 的 options.txt，覆盖实心/透明方块选中框及移动方块的发光轮廓，包含普通透明/OIT；相机对准可拾取的目标方块。
[vanilla-resource-animation.json](../../tools/validation/vanilla-resource-animation.json) 配合生成的模型/纹理测试包，固定两个动画关键帧及中间插值，连续重载两次，并为每个场景导出方块 atlas 的所有 mip。
`tools/validation/pipeline_coverage.py --minecraft-source <MC 客户端源码目录> --captures <visual-result.json...> --output <构建目录中的 JSON>` 将当前管线目录、适配源码、双后端 shader 产物和采集记录关联；没有明确适配或缺少产物时失败，选择证据与视觉审查状态分别保存。

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
python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc
python tools/build_debug_dependencies.py --parallel 8
python external/RadRay/tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Release --parallel 8
```

Debug 用于默认开发启动，Release 用于分发；只运行相应步骤也可以。构建本身离线，缺包则停止。
Debug 和 Release 配置都从 `RADRAY_DXC_ROOT` 发现 RadRayDXC，默认位置为 RadRay 源码树的 `SDKs/radray_dxc/extracted`。普通构建通过 Python 校验并复制已提交的 `.rnsp`。发布的 runtime DLL 使用静态 shader 解码代码，离线打包工具在编译 shader 时动态加载 DXC。
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
默认编译八个地形、三个 GUI 及一个输出程序；`--program` 可显式指定完整程序集合，生成结果仍需满足全部必需条目；`--output` 指定输出，`--radray-source` 指定同一依赖工作树。
`--clean` 重新配置并全量重编离线工具。原生 DLL 不包含编译器；格式、反射校验和来源规则见[Shader pack](../architecture/shader-pack.md)。
`verifyShaderPack` 随 `processClientResources` 检查包和十二程序双后端条目；`testShaderPackTools` 随 `check` 运行容器拒绝测试。

`.\gradlew.bat exportVanillaPipelines` 导出当前 MC 管线到 `build/vanilla/pipelines.json`，包含字段别名、OIT 变体和完整状态身份。
离线转换工具 `python tools/convert_vanilla_shaders.py --help` 列出所需路径：目录清单、原版 `assets/minecraft/shaders`、glslang、SPIRV-Cross 和输出目录。
工具生成构建产物及 `interfaces.json`，失败项保留诊断并返回非零。随后运行 `python tools/generate_scene_catalog.py --help` 对应的目录生成命令，更新 native 栅格描述与 Java 接口资源。
默认 shader pack 包含转换程序；重编默认包前必须准备 `build/vanilla/shaders`，也可用 `compile_shader_pack.py --vanilla-shaders` 指定仓库内的转换目录。缺失或包含失败项时拒绝生成不完整的默认包。普通构建只使用已提交二进制。

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
.\gradlew.bat runClientNative -PrhiBackend=vulkan -PrhiValidation=true
.\gradlew.bat runClientNative -PgameRunDir=runs/takeover -PquickPlayWorld=acceptance
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeParallel=8
.\gradlew.bat nativeStage -PnativePython=C:/path/to/python.exe
.\gradlew.bat nativeBuild -PradraySourceDir=C:/path/to/RadRay
```

`rhiBackend` 默认 `d3d12`，支持 `d3d12|vulkan`，传递给游戏 JVM 的 `raynium.rhi.backend`。
正式启动器用 `-Draynium.rhi.backend=vulkan` 选择 Vulkan；不自动切换后端。
客户端始终使用无原版 GPU 设备的接管路径，不需要选择 MC 图形后端。必须有 native payload；
启动、纹理、GUI 和地形职责见[Minecraft 接入](../architecture/minecraft-integration.md)。
`-PquickPlayWorld=<目录名>` 追加快速进世界参数。接管统计默认每 5 秒输出，可用 JVM 属性 `raynium.takeover.statsSeconds=0` 关闭。
benchmark 活跃时自动启用 GPU timestamp。独立运行可显式给游戏 JVM 设置 `raynium.terrain.profile=true`，不提供对应 Gradle 属性转发。
`-PterrainSubmission=auto|direct|indirect` 对应 `raynium.terrain.submission`，在 renderer 创建时生效。
Debug RHI/driver validation 会增加 CPU 开销，性能对比应记录配置且固定视点、RT 尺寸和场景。

Debug 默认启用 RadRay RHI 检查。`rhiValidation=true`（JVM 属性 `raynium.rhi.validation`）
显式启用 native 驱动诊断，默认 false。客户端初始化入口通过 `rn_init_env` 准备所选后端的环境，
D3D12 在此创建 factory 并启用调试层，早于窗口创建；首次设备信息查询复用环境创建 runtime。
也可通过独立 `nativeTest -PrhiValidation=true` 验证。
启动日志中的 `Raynium native manifest=` 与 `Raynium native library loaded:` 记录内容缓存和 ABI/build ID、RadRay 身份、dirty、配置、CRT 与工具链。
Debug CRT 为 MTd，Release 为 MT；静态分配器随 bridge 加载与卸载。当前要求 ABI 18 和二十一个导出。
`Raynium runtime created` 表示设备与队列完成。GUI 和世界的实际显示须通过窗口观察；F2 使用原版文件写入保存 native 主颜色目标，全景入口通过独立尺寸离屏帧输出，纹理导出使用按子资源请求读回。

## 自动跑图与性能采样

`tools/benchmark.py` 读取 JSON 配置，在持有源存档锁时复制存档，再构建 Release/MT 和 Java 客户端。默认关闭 validation、Tracy 与 JFR，使用双后端接管或原版 Vulkan 参考；`renderer=vanilla` 要求 `backend=vulkan`。等待配置的 playerMode（默认 spectator）及实际帧后预热，按路线插值移动，完成后正常关闭游戏。

```powershell
python tools/benchmark.py --config tools/benchmark.example.json --check
python tools/benchmark.py --config tools/benchmark.example.json
python tools/benchmark.py --config tools/benchmark.example.json --profiler --tracy-capture C:/tools/tracy-capture.exe --jfr
```

配置相对路径以配置文件目录为基准；`world` 指向含 level.dat 的源存档。`profilerEnabled` 或 `--profiler` 开启独立诊断运行，才要求匹配版本的 tracyCapture 并等待采集器连接。`jfrEnabled` 或 `--jfr` 开启 JVM profile JFR；任一诊断开关开启时 summary 标记为 diagnostic。`--no-profiler --no-jfr` 可覆盖配置中的诊断开关，适用于主采样。
默认视距 32、模拟距离 8、窗口 1280×720、预热 20 秒；optionsFile 的视距、窗口尺寸及垂直同步由配置覆盖。terrainSubmission 默认 auto。route 包含秒数、位置和朝向；commands、screen 和 oit 描述测试场景。playerMode 默认为 spectator，也可选择 creative 或 survival；GUI 人物预览可使用 survival，避免旁观角色的隐藏模型语义。存档和选项仅写入副本。

`tools/validation/create_benchmark_scenes.py --world <存档> --options <选项文件> --output <构建目录>` 生成实体密集、粒子与透明密集、GUI/画中画的三种 renderer 配置，正式输入使用 1920×1080、32 区块、60 秒预热和 240 秒路线。`--smoke` 将预热与路线缩短到 2/8 秒，仅用于检查驱动和场景，不能作为性能验收结果。

产物位于 outputDir 的独立时间目录，包含配置、构建/游戏日志、client-result.json、summary.json、frames.csv 与 gpu.csv；native 运行另保存 payload 清单，诊断按开关保存 session.tracy/session.jfr。脚本检查路线完成和资源关闭；诊断额外检查采集器退出和文件保存。
`frames.csv` 的 native 计数包含向图声明的有效上传字节（不含 staging 对齐）、地形记录及调用、场景/GUI 调用、几何与后处理的 PSO/参数/顶点/索引绑定次数，以及几何池保留和已分配字节。已分配区间包含等待双槽退役的部分；容量不表示整个进程的显存用量。场景调用包含深度和内置后处理，绑定计数不包含最终输出及 OIT 全屏合成。调用计数在准备阶段产生，Record 不修改计数。原版对应列使用 -1 表示未采集；字节和计数的分位数保持原单位，不转换为毫秒。
客户端以固定容量的原始数组记录测量区间，结束后写文件及计算最近秩 p50/p95/p99。clientFrameInterval 包括两次帧入口之间的客户端工作，renderWall 是 renderFrame 的 CPU 侧墙钟时间；它包含等待，不等于线程忙碌时间。Windows 线程 CPU 计数粒度较粗，只报告总量并保留原始列。native 另返回槽等待、资源准备、RDG RunFrame 和呈现时间；RDG RunFrame 包含建图、准备、录制及提交，不与参考 renderer 的不同职责阶段直接等同。
GPU 样本均在完成后读取：native 使用整帧两个时间戳，参考使用原版 TimerQuery 并保留实际样本数量，不重复其滑动平均值。进程内存由 Windows 进程计数器轮询，summary 保存峰值工作集、提交量和采样私有内存；GPU Process Memory 计数器按客户端 PID 汇总适配器的专用、共享和提交量采样峰值，同时保存有效样本数；不可用时记录原因，不用零代替。它们是操作系统进程计数，与 native 几何池预留/已分配容量分别报告。客户端保存区间分配量及 GC 次数/时间。普通游戏帧不开启这些测量。

## Tracy 与整段客户端采样

`-PcaptureTracyImages=true` 显式传入原版 `--tracy`，启用 Java Tracy 及图像采集；原版 `--tracyNoImages` 仍关闭图像请求。接管模式用 native 缩略图读回，保留原版帧延迟，最多一个在途图像。`validate_visual.py --tracy-images` 可运行同一路径。该选项用于图像诊断，不用于关闭 profiler 的主性能采样。

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
.\gradlew.bat runClientNative -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false '-PjfrFile=C:/captures/session.jfr'
```

Release 默认关闭 RadRay RHI wrapper 检查；rhiValidation=false 关闭 native 驱动诊断。
ABI 参数检查及资源寿命保护仍然执行。benchmark 活跃时开启整帧测量；只有 profilerEnabled 诊断运行或游戏 JVM 设置 `raynium.terrain.profile=true` 才开启分阶段统计及 Tracy GPU plot。
该数值覆盖 native 命令的 timestamp 区间，在回收槽时发布，不是 MC 全帧 GPU 耗时，也不是发布时那一帧的即时耗时。

`Raynium.Present` 覆盖窗口帧的槽回收、驻留/GUI 准备、Acquire、RDG 提交和 Present。
`Raynium.Terrain.Prepare` 覆盖驻留更新、可见解析及参数构建；`Raynium.Terrain.Sort`、`BuildArguments` 分别记录排序和参数构建，`Raynium.Terrain.Record` 记录几何及 OIT 屏幕 pass 的录制；没有逐 draw profiling scope。上传字节的复制与生成计入 `RadRay.RDG.FillUploads`。
锁定 RadRay 的 `CompiledRDG::Record` 按 Upload execution group 记录一个 `RDG UploadBatch` GPU 计时区，逐上传节点保留 CPU zone。该 GPU 区间包含组内拷贝和重叠写屏障，不包含组入口屏障；完整 native GPU 耗时仍读取上述帧外围 timestamp，不能用上传子区间之和替代。实现来源为 RadRay `modules/runtime/src/rdg.cpp`。
`Raynium.Visible.Solid/Cutout/Translucent` 记录各层输入项；`Raynium.Terrain.<层>.SelectedGroups/Records/Batches` 记录各层选中面组、参数记录及批次。
`Raynium.Terrain.BuiltRecords/BuiltBatches/ArgumentBytes` 只计参数生成一次的量，`OitParameterBuilds` 在有 OIT 透明输入时为 1。
`Raynium.Frame.SubmittedRecords/SubmittedBatches/SubmittedIndicesInstances` 汇总实际几何 pass，OIT 透明部分计三次。
`Raynium.Frame.DirectCalls/IndirectCalls/BindingChanges` 记录几何提交的实际 API 调用和绑定操作；记录数与原生 indirect 调用数分别观察。
`Raynium.Frame.Sequence` 标识当前输入帧；`Raynium.GpuFrame.Sequence` 与同时发布的 `Raynium.GpuMilliseconds` 对应已完成的原始帧，不能归给发布时正在准备的帧。
`Raynium.Geometry.VertexPages/IndexPages` 记录池保留页数，`Raynium.Terrain.BindingStates` 记录活跃精确绑定状态数。启用 Tracy 时，每 120 帧发布 `Raynium.Geometry.Vertex/IndexReservedBytes`、`Vertex/IndexFreeBytes` 与 `Vertex/IndexAllocatedBytes`；Allocated 为保留容量减空闲量，包含等待退役的区间。这些稀疏样本不能把未发布帧当成零，也不表示最大连续空闲区间。runtime 另提供几何池、上传、槽回收、RDG 编译/屏障/录制区间。
父 CPU 区间包含子区间，不直接相加；GPU 时间线独立读取。
Tracy frame mark 在一次 native Render/Present 调用结束时发出；间隔包含两次调用之间的宿主工作，不等同于 native CPU 用时。
客户端是 on-demand，因此 native 库加载前、采集器连接前的工作不在 Tracy 中；它也不提供 Java 调用栈。

`-PjfrFile=<路径>` 在游戏 JVM 启动时启用 `profile` JFR，正常退出时保存，磁盘缓冲上限 1 GiB；超出上限会淘汰较早数据。
JFR 包含 JVM 执行采样、GC 等事件；`dev.raynium.TerrainFrame` 按原始序号记录 Java 准备和 presentPrepared/FFM 包络。FFM 包络包含 native Present，不能再次与 native 时间相加。`dev.raynium.ClientFrame` 记录每次正常返回的 `Minecraft.renderFrame` 耗时及是否加载世界。
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
| `TerrainSubmissionTest` / `NativeRuntimeBackendTest` | 提交模式及后端解析 | `test` |
| `BenchmarkRouteTest` / `CpuLightmapTest` | 路线插值、边界和 CPU 光照图规则 | `test` |
| `NativeManifestTest` / `NativeBindingsTest` / `BuiltinShaderPackTest` | ABI 18、旧 DLL 在解析新导出前拒绝、单 DLL 清单、资源来源及拒绝路径 | `test` |
| `TerrainCaptureTest` / `TerrainResidencyTest` | 引用释放、预算、整 section 回退、提交后释放、新 epoch 需要新 payload、透明索引版本 | `test` |
| `NativeFrameBufferTest` / `NativeGuiFrameBufferTest` | ABI 18 布局、数组复位、扩容/对齐、实例与 GUI 编排 | `test` |
| `VanillaPipelineCatalogTest` / `TakeoverGuiTest` | 同名不同宏、OIT 清单、稳定身份和未知管线拒绝 | `test` |
| `TextureClearTest` | 单通道、RGBA、半精度及浮点颜色清色布局 | `test` |
| `NativeAbiTest` | 参数、reserved 拒绝、状态码、输出保护及空句柄销毁 | `nativeTest` |
| `NativeRuntimeTest` / `NativeShaderPackTest` | 双后端设备生命周期、线程/owner、超过 Windows MAX_PATH 的含空格及中文缓存路径加载、shader pack 及提交策略拒绝 | `nativeTest` |
| `NativeSwapchainTest` | 双后端呈现、尺寸/模式、隐藏/最小化/零客户区及失败后关闭重建 | `nativeTest` |
| `NativeTextureTest` | direct/indirect、RGBA8/R8/mip/padding 区域上传、共享索引扩容、透明排序、OIT/普通透明、十二种 GUI 提交、双槽资源退役、透明顶点连续替换并保留索引、在途材质纹理删除和替换、退役后准备失败关闭、连续局部上传、上传后删除目标、跨页增长、epoch 与销毁编号拒绝 | `nativeTest` |
| `NativeTextureTest` 的纹理扩展用例 | 数组层、立方体、浮点颜色、深度附件创建和上传后删除 | `nativeTest` |
| `NativeCaptureTest` | 双后端颜色通道、上下方向、非对齐行距、隐藏帧、上一帧来源、resize 来源尺寸、读回编号消费、未读结果及准备失败后关闭、世界/GUI 采集时点、请求后删除纹理的 mip/层读回、六个离屏帧与正常窗口输出切换、显式 Tracy 缩略图的尺寸、方向和跨槽 resize | `nativeTest` |
| `NativePipelineCatalogTest` | 枚举转换后的原版变体并跨双槽提交，覆盖程序接口、PSO、绑定及附件；原版 bounds cull 由 native OIT 路径替代，不通过普通场景 pass 提交；零几何布局检查不代替视觉验收 | `nativeTest` |
| `NativeSceneTest` | 原版转换程序、非零几何和常量偏移、双槽重复绘制、立方体采样与背面剔除、像素尺寸点展开、无地形动态 OIT、独立水体遮罩、优化掉的常量/加法混合透射率纹理组及局部纹理更新、第一人称/覆盖层深度合成、完整 mip view/LOD/各向异性及在途删除、GUI 特殊场景交错顺序、R8_SINT 云面与云 OIT、透视反向深度下云与动态 OIT 的交叉遮挡、透视距离下信标雾颜色、atlas 局部缓存、内置后处理/GUI 模糊分界及实体轮廓像素断言 | `nativeTest` |
| `SceneUniformsTest` | 堆上暂存矩阵的列主序、偏移及边界 | `test` |
| `SceneGeometryTest` | 持久 quad 与三角扇的原版索引顺序和非法顶点数 | `test` |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native；FFM 缓冲测试使用 JVM native access。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

专用参数及帧上传/录制单元测试是可选 CMake 目标。已准备匹配 CRT 的 GTest 后，在 x64 开发终端执行：

```powershell
cmake --preset win-x64-debug -S native -DRAYNIUM_BUILD_TESTS=ON
cmake --build build/native/windows-x64-msvc-mtd/Debug --target test_raynium_terrain --parallel 8
ctest --test-dir build/native/windows-x64-msvc-mtd/Debug --output-on-failure
```

该目标覆盖七组重排/紧凑掩码、实际 AABB、非有限值、投影/视点、多实例并集，以及稳定排序、完整 VB/IB/绑定身份、状态回收、独立参数展开、合并边界、计数溢出与模式选择。帧上传/录制用例覆盖最终区间索引生成、非零切片偏移、常量绑定复用、direct/indirect 拆分与 GUI 空绘制；场景 probe 验证顶点、实例和 16 位索引的完整切片地址。probe 检查录制没有映射、Flush、设备访问或参数写入，不需要 GPU。

通用 runtime 测试位于 RadRay 的 `modules/runtime/tests/`，由 `test_radray_runtime` / GTest 注册，
直接链接 `RadRay::runtime`，不包含 MC bridge。在已准备依赖并完成 configure 的 RadRay checkout 执行：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-msvc/Debug --config Debug
cmake --build --preset win-x64-debug --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug -R 'FrameScheduler|GeometryBufferPool|GeometryUploadGpuTest|UploadArena|RDG|Material' --output-on-failure
```

RadRay standalone 的测试依赖需在该 checkout 显式准备，包括 GTest；runtime 的 CPU 用例使用 fake，GeometryUploadGpuTest 需要显式准备 shader fixture 和双后端 GPU。
`nativeTest` 只运行 Raynium 的适配、上传与交换链呈现回归，不构建上游 GTest。

native 测试 JVM 使用 Java 25、`--enable-native-access=ALL-UNNAMED` 与指定配置的 stage 路径。仅一个 worker，禁用 JUnit 并行；每个 runtime 的创建和销毁在同一线程，错误线程用例从额外线程调用预期被拒绝的创建、渲染或关闭。GPU/驱动状态不属于普通 Gradle 输入，因此 `nativeTest` 每次显式执行，不使用 up-to-date 或构建缓存结果。

JUnit `@TempDir` 管理每个用例的 DLL 缓存目录。测试 fixture 和 try-with-resources 保证断言失败时也先释放 native runtime 和环境，再卸载 DLL，最后清理临时目录。用例无顺序依赖，测试观测与句柄跟踪仅存在于测试源码，不加入生产对象。

nativeTest 需要 D3D12/Vulkan 设备；开启 driver validation 还需要对应诊断组件及 Vulkan validation layer。
NativeCaptureTest 提供离屏像素读回断言；其余呈现及 validation 检查覆盖提交与资源合法性。视觉工具从独立原版配置生成相同输入的 PNG 对照，采集和差分保存在输出目录；提交通过不能替代逐项视觉审查。
原生诊断日志与 JUnit 结果一起检查；实际 MC 加载、渲染及生命周期单独运行。

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
