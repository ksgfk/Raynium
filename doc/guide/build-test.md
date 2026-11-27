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
存档、资源包及 options 由指定游戏目录提供。`benchmarkConfig`/`benchmarkRunDir` 接入路线驱动，`tools/benchmark.py` 支持原版 Vulkan 与 Raynium 两个后端的独立采样。

视觉驱动通过 `-PvalidationConfig=<JSON 路径>` 显式开启，不能与 benchmarkConfig 同时使用。将存档复制到独立游戏目录的 `saves/validation/`，选择 `-PquickPlayWorld=validation -PgameUsername=RayniumTest`；用户名限制为 1–16 个字母、数字或下划线。参考场景配置见 [vanilla-scenes.json](../../tools/validation/vanilla-scenes.json)，包含平台布置、实体、物品、普通透明/OIT、GUI、重载、线框与雨天。配置中的命令会修改测试存档，须使用专用测试存档。
两种启动入口均使用 VisualValidation：服务端命令完成后设置相机，等待指定帧数、时长和区块编译，再经原版截图入口保存 PNG。可指定 gameTime、animationMillis、partialTick、rain/thunder、vignetteBrightness、wireframe、reload、postEffect 和 screen，并支持顶层 defaults。视觉模式固定 GUI 鼠标位置及光照闪烁；动画时钟作用于附魔、sprite、世界边界、实体 tickCount 及末地水晶，gameTime 固定世界年龄与末地闪光状态。维度有默认世界时钟时，服务端将它暂停在同一刻，截图时核对客户端读数并写入 worldClockTicks；切换场景或结束验证时恢复该时钟先前的运行状态。产物在配置目录，包括各场景 PNG、设置和 visual-result.json；每场景五分钟超时，结束后正常关闭游戏。测试驱动完成不代表图像差异已获接受。
设置 animationMillis 的场景在命令和资源重载完成后显式调用一次原版纹理 tick，使冻结世界也应用目标 sprite 帧与插值时刻；正常游戏与性能路线不启用此行为。
[validate_visual.py](../../tools/validate_visual.py) 默认复制存档并依次运行 reference/d3d12/vulkan，输出原图和放大差分图，并按 64 像素区域统计差异；差分步骤依赖 Pillow 和 NumPy，可用 `--compare-only` 重建差分。在 Windows 上指定 `--in-place-world` 时，各会话独立保存世界和玩家元数据，通过目录联接读取原存档的区块、实体和 POI 文件。此模式在验证 JVM 启用 MC 的 DONT_SAVE_WORLD 开关，运行期间锁住所选原存档。[vanilla-effects.json](../../tools/validation/vanilla-effects.json) 包含特殊材质、内置后处理、屏幕覆盖与维度切换。
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

重编内置着色器时，先导出当前 MC 的管线，在仓库构建目录生成 HLSL 和反射目录，再生成双后端包。将示例中的 shader 路径换成对应客户端资源目录。

```powershell
.\gradlew.bat exportVanillaPipelines
python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc
python tools/convert_vanilla_shaders.py --catalog build/vanilla/pipelines.json --shaders <assets/minecraft/shaders> --glslang glslangValidator --spirv-cross spirv-cross --output build/vanilla/immediate
python tools/compile_shader_pack.py
python tools/shader_pack.py src/client/resources/assets/raynium/shaderpacks/default.rnsp
```

默认生成内置 .rnsp 和两份运行时 JSON。仓库随附生成结果。普通构建通过 verifyShaderPack 验证必需双后端程序，check 运行容器测试。用 `python tools/compile_shader_pack.py --install-tools build/shader-sdk` 另行准备资源包着色器编译工具，给游戏 JVM 设置 `-Draynium.shaderToolsDir=<工具目录>`。该工具目录需要 Python 可执行文件在 PATH 上。资产来源与失败处理见[Shader pack](../architecture/shader-pack.md)。

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
Raynium 后端通过游戏的 RenderPearl 入口创建窗口与设备。原版参考客户端沿用 Vulkan。使用 `-PquickPlayWorld=<目录名>` 可直接进入指定存档。benchmark 活跃时自动按批记录 GPU 时间。[Minecraft 接入](../architecture/minecraft-integration.md)

`-PterrainSubmission=auto|direct|indirect` 对应 `raynium.terrain.submission`，在 renderer 创建时生效。
Debug RHI/driver validation 会增加 CPU 开销，性能对比应记录配置且固定视点、RT 尺寸和场景。

Debug 默认启用 RadRay RHI 检查。`rhiValidation=true`（JVM 属性 `raynium.rhi.validation`）
显式启用 native 驱动诊断，默认 false。图形后端加载时通过 `rn_init_env` 准备环境，
D3D12 在此创建 factory 并启用调试层，早于窗口创建；createDevice 复用环境创建 runtime。
也可通过独立 `nativeTest -PrhiValidation=true` 验证。
启动日志中的 `Raynium native manifest=` 与 `Raynium native library loaded:` 记录内容缓存和 ABI/build ID、RadRay 身份、dirty、配置、CRT 与工具链。
Debug CRT 为 MTd，Release 为 MT；静态分配器随 bridge 加载与卸载。当前要求 ABI 27，导出清单见 `tools/native.py`。
`Raynium runtime created` 表示设备与队列完成。GUI 和世界的实际显示须通过窗口观察；F2 使用原版文件写入保存 native 主颜色目标，全景入口通过独立尺寸离屏帧输出，纹理导出使用按子资源请求读回。

## 自动跑图与性能采样

`tools/benchmark.py` 读取 JSON 配置，通过每场游戏目录里的 Windows 目录联接直接打开配置的存档，由 Minecraft 持有会话锁。选项、日志与采样文件按场隔离，游戏保存和场景命令会写回原存档。运行结束后脚本移除联接。Raynium 运行使用 Release/MT。默认关闭 validation、Tracy 与 JFR，使用双后端接管或原版 Vulkan 参考；`renderer=vanilla` 要求 `backend=vulkan`。等待配置的 playerMode（默认 spectator）及实际帧后预热，按路线插值移动，完成后正常关闭游戏。

```powershell
python tools/benchmark.py --config tools/benchmark.example.json --check
python tools/benchmark.py --config tools/benchmark.example.json
python tools/benchmark.py --config tools/benchmark.example.json --profiler --tracy-capture C:/tools/tracy-capture.exe --jfr
```

配置相对路径以配置文件目录为基准；`world` 指向含 level.dat 的源存档。`profilerEnabled` 或 `--profiler` 开启独立诊断运行，才要求匹配版本的 tracyCapture 并等待采集器连接。`jfrEnabled` 或 `--jfr` 开启 JVM profile JFR；任一诊断开关开启时 summary 标记为 diagnostic。`--no-profiler --no-jfr` 可覆盖配置中的诊断开关，适用于主采样。
默认视距 32、模拟距离 8、窗口 1280×720、预热 20 秒；optionsFile 的视距、窗口尺寸及垂直同步由配置覆盖。terrainSubmission 默认 auto。route 包含秒数、位置和朝向；commands、screen 和 oit 描述测试场景。playerMode 默认为 spectator，也可选择 creative 或 survival；GUI 人物预览可使用 survival，避免旁观角色的隐藏模型语义。

`tools/validation/create_benchmark_scenes.py --world <存档> --options <选项文件> --output <构建目录>` 生成实体密集、粒子与透明密集、GUI/画中画的三种 renderer 配置。请使用测试专用存档，重复布景会清理平台上的方块以及已加载且带有 raynium_bench 标签的实体。正式输入使用 1920×1080、32 区块、60 秒预热和 240 秒路线。`--smoke` 将预热与路线缩短到 2/8 秒，仅用于检查驱动和场景，不能作为性能验收结果。

产物位于 outputDir 的独立时间目录，包含配置、构建/游戏日志、client-result.json、summary.json、frames.csv 与 gpu.csv；native 运行另保存 payload 清单，诊断按开关保存 session.tracy/session.jfr。脚本检查路线完成和资源关闭；诊断额外检查采集器退出和文件保存。
`frames.csv` 的 native 指标按本世界帧的实际批次汇总，包括上传字节、驻留几何记录与绘制、即时绘制和绑定数。即时绘制含 GUI 和后处理；独立的旧 GUI 计数列保留为零。原版缺少的 native 指标为 -1。`javaPrepareNs` 计量提交前封存瞬态内存的时间，`nativeCallNs` 计量 Java 进入命令带提交到该方法返回的时间。`nativeSyncWaitNs` 汇总同一测量帧内显式等待和轮询 `rn_wait_submit` 的墙钟时间，单位为纳秒。原版帧记为 -1。`renderWallNs` 涵盖整帧渲染。几何池容量和 Windows 进程显存分别采样。
客户端以固定容量的原始数组记录测量区间，结束后写文件及计算最近秩 p50/p95/p99。clientFrameInterval 包括两次帧入口之间的客户端工作，renderWall 是 renderFrame 的 CPU 侧墙钟时间；它包含等待，不等于线程忙碌时间。Windows 线程 CPU 计数粒度较粗，只报告总量并保留原始列。native 另返回槽等待、资源准备、RDG RunFrame 和呈现时间；RDG RunFrame 包含建图、准备、录制及提交，不与参考 renderer 的不同职责阶段直接等同。
GPU 样本均在完成后读取：native 按批测量并在上屏边界汇总，参考使用原版 TimerQuery 并保留实际样本数量，不重复其滑动平均值。进程内存由 Windows 进程计数器轮询，summary 保存峰值工作集、提交量和采样私有内存；GPU Process Memory 计数器按客户端 PID 汇总适配器的专用、共享和提交量采样峰值，同时保存有效样本数；不可用时记录原因，不用零代替。它们是操作系统进程计数，与 native 几何池预留/已分配容量分别报告。客户端保存区间分配量及 GC 次数/时间。普通游戏帧不开启这些测量。

### 真实跑图环境预设

每组对照必须通过目录联接依次使用同一份原存档。上场结束时的天气和世界时钟会随存档保留，每场都须在 `commands` 中显式指定天气和时刻、关闭自然天气和昼夜推进，再开始预热。新增路线、派生后端配置及重复采样均须保留同一组环境预设。专门测量雨天等场景时，各场统一使用该场景的目标天气。

三张主世界城市地图均固定晴天和 6000 tick（正午）。MC 26.3 使用 `gamerule advance_weather false` 和 `gamerule advance_time false`。重复设置相同规则值或相同绝对时刻会触发命令错误，配置先查询规则、按条件设置，并在主世界时钟偏离 6000 时执行 `time set 6000`。可直接沿用以下配置中的 `commands` 和航线。主采样显式传入 `--no-profiler --no-jfr`。[Metcalf](../../tools/benchmarks/metcalf-stress.json) · [Tideworn](../../tools/benchmarks/tideworn-stress.json) · [Newisle](../../tools/benchmarks/newisle-stress.json)

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

所有 benchmark 客户端关闭 Java 断言和 LWJGL 的参数与函数指针检查。启动器在子进程环境中禁用 Vulkan 附加层，采样结束后核对客户端记录的 Java Tracy、JFR、LWJGL 和断言状态，以及日志中的实际后端。Raynium 须报告 RHI 和驱动校验均已关闭。
Release 默认关闭 RadRay RHI wrapper 检查；rhiValidation=false 关闭 native 驱动诊断。
ABI 参数检查及资源寿命保护始终执行。benchmark 将每个测量批次按提交序号归入录制它的 Java 帧，全部批次都返回有效计时时才记入该帧的 GPU 样本。
该数值覆盖 native 命令的 timestamp 区间，在回收槽时发布，不是 MC 全帧 GPU 耗时，也不是发布时那一帧的即时耗时。

`Raynium.Frame.AcquireNext` 标出取得交换链图像的调用，`Raynium.Frame.Present` 覆盖提交呈现与结果检查。测量批次的统计工作记为 `Raynium.Benchmark.BatchMetrics`，可通过首末标记定位正式路线。
`Raynium.Frame.QueueSubmit` 覆盖队列提交。槽回收时由 `RadRay.RDG.GpuNanoseconds` 发布单批 GPU 时间。`Raynium.Frontend.Freeze` 包含主线程冻结输入与入队，其中 `Raynium.Frontend.QueueCapacity` 记录等待队列腾位的区间。
`Raynium.Terrain.Prepare` 覆盖驻留更新、可见解析及参数构建；`Raynium.Terrain.Sort`、`BuildArguments` 分别记录排序和参数构建，`Raynium.Terrain.Record` 记录几何 pass 的内联录制；没有逐 draw profiling scope。上传字节的复制与生成计入 `RadRay.RDG.FillUploads`。
每次执行 `Raynium terrain instance copies` 时，`Raynium.Terrain.InstanceCopies.ResolveBuffers` 记录源与目标缓冲的解析耗时，`Raynium.Terrain.InstanceCopies.RecordRanges` 记录整批 `CopyBufferToBuffer` 循环的 CPU 耗时。
同一父区间结束前，`Raynium.Terrain.InstanceCopies.RangeCount` 和 `Raynium.Terrain.InstanceCopies.Bytes` 各采样一次，分别给出合并后的拷贝段数和实际拷贝总字节数（含合并带入的间隙）。可按段数分组、对照字节数比较两端的录制耗时。采样单位是执行的多段拷贝 pass，走单段上传或跳过上传的帧没有该项样本。
锁定 RadRay 的 `CompiledRDG::Record` 按 Upload execution group 记录一个 `RDG UploadBatch` GPU 计时区，逐上传节点保留 CPU zone。D3D12 的光栅 GPU 区间覆盖整个原生 render pass，合并组使用首个 RDG 光栅 pass 名。该上传 GPU 区间包含组内拷贝和重叠写屏障，不包含组入口屏障；完整 native GPU 耗时仍读取上述帧外围 timestamp，不能用上传子区间之和替代。实现来源为 RadRay `modules/runtime/src/rdg.cpp`。
`Raynium.Visible.Solid/Cutout/Translucent` 记录各层输入项；`Raynium.Terrain.<层>.SelectedGroups/Records/Batches` 记录各层选中面组、参数记录及批次。
`Raynium.Terrain.BuiltRecords/BuiltBatches/ArgumentBytes` 统计本帧生效参数总量，`Raynium.Terrain.UploadedArgumentBytes` 仅统计本帧实际上传的间接参数字节（稳态缓存命中时为 0），`OitParameterBuilds` 在有 OIT 透明输入时为 1。
`Raynium.Resident.Meshes`、`Raynium.Uploads.Bytes` 与 `Raynium.Geometry.VertexPages/IndexPages` 记录驻留网格数、上传字节和池保留页数。启用 Tracy 时，定期发布 `Raynium.Geometry.Vertex/IndexReservedBytes`、`Vertex/IndexFreeBytes` 与 `Vertex/IndexAllocatedBytes`；Allocated 为保留容量减空闲量，包含等待退役的区间。这些稀疏样本不能把未发布帧当成零，也不表示最大连续空闲区间。runtime 另提供几何池、上传、槽回收、RDG 编译/屏障/录制区间。
父 CPU 区间包含子区间，不直接相加；GPU 时间线独立读取。
窗口帧数和帧间隔读取 client-result.json 与 frames.csv。当前提交路径没有逐窗口帧发送 Tracy frame mark；同一窗口帧可以包含多个 native 提交批次。
客户端是 on-demand，因此 native 库加载前、采集器连接前的工作不在 Tracy 中；它也不提供 Java 调用栈。

`-PjfrFile=<路径>` 在游戏 JVM 启动时启用 `profile` JFR，正常退出时保存，磁盘缓冲上限 1 GiB；超出上限会淘汰较早数据。
JFR 包含 JVM 执行采样、GC 等事件；`dev.raynium.TerrainFrame` 按实际提交序号汇总批内各视图的采集、驻留准备及 rn_submit_frame 的 FFM 时长。该 FFM 区间包含冻结输入和队列回压，工作线程的 RDG 执行与 Present 在 Tracy 中单独计时。`dev.raynium.ClientFrame` 记录每次正常返回的 `Minecraft.renderFrame` 耗时及是否加载世界。
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

| 测试入口 | 核对内容 |
| --- | --- |
| `test` | 管线身份、驻留预算/稠密槽位回收与采用、地形占位与捕获、刚体模型矩阵列直读法线基向量/池化分组合批与紧凑粒子堆外直写编码（`WorldFastTrackTest`）、后端选择、路线、清单与着色器包。 |
| `NativeAbiTest`、`NativeRuntimeTest`、`NativeShaderPackTest` | ABI 拒绝路径、设备与线程所有权、双后端包和退出顺序。 |
| `NativeCommandTapeTest` | 离屏与上屏提交隔离、顺序复制、映射刷新、瞬态切片的缓存原子对齐、深度行距、时间戳、首次部分访问与保活。 |
| `NativeRenderPearlTest` | 从游戏前端编译管线并绘制，验证线程交接、Push Constant、实际颜色和材质，以及超过复用池上限的上传缓冲在提交后立即关闭时的像素结果。 |
| `NativeWorldFeaturesTest` | 双后端检查用户网格注册、同页实例绘制、粒子与阴影四边形、传输与实例流的独立偏移，以及共享实例切片的像素结果。 |
| `NativeTextureTest`、`NativeCaptureTest`、`NativeSwapchainTest` | 子资源、捕获格式及缓冲容量、尺寸变更与窗口状态。 |
| `NativeWireframeTest` | direct/indirect 实际像素、面掩码、双槽退役及跨页几何。 |
| `NativeTerrainFadeTest` | 在 D3D12 和 Vulkan 上读回三层地形颜色及 OIT 各阶段附件，比较淡入中点、完成、关闭、近处豁免与时钟边界的像素。 |
| `vulkanSurfaceTest` | 真实 Vulkan 设备上的 WSI 恢复、设备丢失后的表面释放与交换链销毁、两批在途时的紧邻字节与查询槽发布。门控测试在 AcquireNext 暂停时核对冷资源创建、已提交批次的 Fence 等待及后续绘制的像素。 |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native；FFM 缓冲测试使用 JVM native access。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

`nativeTest` 也会执行 `vulkanSurfaceTest`。该任务在独立测试 JVM 中启用测试专用 Vulkan 层，把指定错误送入原生提交路径。测试层使用受管依赖中的 Vulkan-Headers 构建，产物位于 `build/native/vulkan-surface-test/`。阶段快照与分发 JAR 均只收录 `rayniumruntime.dll`。

专用参数及帧上传/录制单元测试是可选 CMake 目标。已准备匹配 CRT 的 GTest 后，在 x64 开发终端执行：

```powershell
cmake --preset win-x64-debug -S native -DRAYNIUM_BUILD_TESTS=ON
cmake --build build/native/windows-x64-msvc-mtd/Debug --target test_raynium_terrain --parallel 8
ctest --test-dir build/native/windows-x64-msvc-mtd/Debug --output-on-failure
```

该目标覆盖网格分组、预分析布局、显式面掩码、线框索引与跨面候选缓存，使用 CPU 设备替身检查内建网格、用户句柄复用、稳态缓冲晋升和 Direct/Indirect 对同一参数表的提交。缓冲注册表用例覆盖持久上传导入及映射刷新失败。

通用 runtime 测试位于 RadRay 的 `modules/runtime/tests/`，由 `test_radray_runtime` / GTest 注册，
直接链接 `RadRay::runtime`，不包含 MC bridge。在已准备依赖并完成 configure 的 RadRay checkout 执行：

```powershell
python tools/compile_test_shaders.py --build-dir build/windows-x64-msvc/Debug --config Debug
cmake --build --preset win-x64-debug --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug -R 'FrameScheduler|GeometryBufferPool|GeometryUploadGpuTest|MappedBufferPools|RDG|Material' --output-on-failure
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
