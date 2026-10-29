> - 适用: 恢复依赖、构建、测试、启动与分发 Raynium
> - 权威: 本文维护可执行命令和当前验证范围；ABI、内容缓存和加载契约归互操作文档
> - 锚点: `build.gradle`, `gradle/native.gradle`, `gradle.properties`, `native/CMakeLists.txt`, `native/CMakePresets.json`, `native/radray.lock.json`, `tools/fetch_radray.py`, `tools/native.py`, `src/test/java/dev/raynium/bridge/*.java`

# 构建与测试

以下命令在 Raynium 根目录执行。Java/IDEA 设置见[开发环境](dev-environment.md)。
普通 `build` 只构建 Java 并执行纯 Java 测试；native 必须通过显式任务准备。
payload、ABI、缓存和关闭顺序见[互操作与加载](../architecture/interop.md)。

## 当前验证范围

### 2026-10-29：SOLID 地形与 ABI 7

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

### 2026-10-28：C++ bridge 与固定底层枚举

在下节单例环境的同一机器和工具链上，将 bridge 改为 C++ 头文件后，重新执行
`build nativeTest -PrhiValidation=true`、
`nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` 和 `verifyNativePackage`，
均附 `--console=plain --no-parallel`。Java 构建成功，Debug/Release 各 39 个 native 用例通过，
Release 打包与包校验通过；纯 Java 测试复用本轮先前的通过结果。
两配置的 `dumpbin /exports` 均确认九个 C 导出名称保持不变，日志无驱动校验错误。
已拉取 RadRay 远端 develop，其版本与锁文件和干净工作树一致，无需更新依赖。
本次仅在后台构建和测试，未启动 MC；证据位于 `build/verification/bridge-fixed-enums-20261028/`。

### 2026-10-28：单例环境与 ABI 6

本轮在 Windows x64 / Temurin 25.0.4.1 / MSVC 19.51.36260.0 / RTX 3060 / NVIDIA 591.44 /
Vulkan SDK 1.4.357.0 上验证；RadRay 使用锁定的干净源码，未修改依赖。
以下 Gradle 命令均附 `--console=plain --no-parallel`。

| 命令 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java/native 构建、4 个纯 Java 与 39 个 Debug native 测试通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 39 个 Release native 测试通过，ABI 6 Release stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构与哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | ABI 6 加载、早期环境初始化及 D3D12 设备创建成功；交互验证未完成，随后主动终止本次验证进程 |

测试覆盖单例重复初始化、环境描述和诊断参数、无环境创建 runtime、环境在活跃 runtime 下拒绝销毁、
错误线程、幂等关闭、失败后原所有者继续使用、同一环境内设备重建及环境销毁后重新初始化。
D3D12 专项分别关闭/开启 driver validation，在环境初始化后创建独立 Vulkan 宿主，
同一环境内创建两次 runtime，每次实际共享并逐像素检查两帧 17×9 UV。
原有双后端三种尺寸、连续帧、导入失败和同步重建测试继续通过。
两配置测试日志无 DXGI/D3D12 或 Vulkan 校验错误；Release 上游代码仍有两处 C4702 警告。

游戏日志显示 11:25:59 创建唯一 DXGI factory 并完成环境初始化，早于 MC 图形后端初始化；
11:26:01 创建 D3D12 设备，driver validation 为 true。MC 此次使用 OpenGL，预览按规则禁用。
为避免干扰用户前台工作，停止窗口操作并终止本次启动；Gradle 因进程被终止返回失败。
本轮没有完成 MC Vulkan 预览、资源重载、窗口切换或游戏正常退出验证，不能用自动测试替代这些结论。

构建/游戏日志和两配置测试 XML 位于本机忽略目录 `build/verification/native-environment-20261028/`。
旧版 ABI 5 的运行记录保留在下一节，不代表本轮重新完成了其中的游戏操作。

### 2026-10-28：D3D12 校验提前启用与 ABI 5

在本次工作树、锁定的 RadRay、Windows 11 x64 / Temurin 25.0.4.1 / MSVC 19.51.36260.0 /
RTX 4080 / NVIDIA 621.01 / Vulkan SDK 1.4.363.0 上执行以下命令，均附 `--console=plain --no-parallel`。

| 命令 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java/native 构建、4 个纯 Java 与原有 26 个 Debug native 用例通过 |
| `gradlew.bat nativeTest -PrhiValidation=true` | 补充早期入口与库所有权用例后，30 个 Debug native 用例通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 30 个 Release native 用例通过，ABI 5 Release DLL/stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构与哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | D3D12 → MC Vulkan，进入已有开发世界、UV 显示、资源重载、全屏/窗口切换、返回标题及正常退出通过 |

新增用例检查早期入口的诊断缓冲区约束、重复启用、已准备 library 转交 runtime 后的寿命和失败清理。
游戏日志中，08:40:05 记录早期 D3D12 校验已启用，08:40:07 MC 选择 Vulkan，08:40:10 native
记录 `driver validation=true` 并创建共享预览；窗口切换后预览在 213×120 与 640×360 间重建。
08:42:24 runtime/DLL 按顺序关闭，Gradle 成功退出。游戏日志有实际 D3D12 诊断消息，无 DXGI/D3D12 或 Vulkan 校验错误，
无 `DEVICE_RESET` / `DEVICE_LOST`。native 测试中两次 `vk has actived VkInstance` 来自预期的占用环境失败用例。
MC Services 网络和开发账户 Realms 鉴权错误不影响本次本地世界验证。

完整日志、两配置的 native 测试报告和游戏截图保存在本机忽略目录 `build/verification/dx12-validation-20261028/`。
此次游戏运行使用 Debug payload；Release 验证范围为 native 测试和打包，未运行 Release 游戏或其他 GPU/驱动环境。

### 2026-10-27：共享预览验证

2026-10-27，基于本次工作树和 `native/radray.lock.json` 锁定的 RadRay develop，
使用 Windows 11 x64、Temurin 25.0.4.1、Gradle 9.8.2、MSVC 19.51.36260.0、
RTX 4080 / NVIDIA 621.01、Vulkan SDK 1.4.363.0 完成以下验证。
以下是该环境的运行记录，不保证其他机器或后续源码自动满足同样结果。

| 实际命令（均附 `--console=plain --no-parallel`） | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true` | Java 构建、4 个纯 Java 和 26 个 Debug native 测试通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` | 26 个 Release native 测试通过，Release DLL/stage/mod JAR 构建成功 |
| `gradlew.bat verifyNativePackage` | 包结构、manifest 和哈希检查通过 |
| `gradlew.bat runClientNative -PrhiValidation=true -PvulkanValidation=true` | D3D12 → MC Vulkan，世界内 UV、资源重载、全屏/窗口切换、最小化恢复和正常退出通过 |
| `gradlew.bat runClientNative -PrhiBackend=vulkan -PrhiValidation=true -PvulkanValidation=true` | Vulkan → MC Vulkan，同组游戏操作和正常退出通过 |
| `gradlew.bat runClientNative -PnativeConfig=Release -PpreviewEnabled=false -PvulkanValidation=true` | 原版标题画面正常，无预览且未启用共享扩展，退出成功 |
| `gradlew.bat runClientNative -PvulkanValidation=true` | 最终 D3D12 代码复核全屏重建与提交 fence 排空，退出成功 |

自动测试共 56 次执行，0 失败、0 跳过。共享测试在每个后端上以 257×131、1×1、319×177
三种尺寸各交接 8 帧，逐像素检查 UV 与区域外像素，覆盖创建参数、序号、导入失败清理、等待和重建。
两配置的 native 驱动诊断均开启；消费者启用 Vulkan 同步验证。Debug RHI 检查开启，Release 关闭。
最终测试与游戏日志没有 Vulkan 校验错误；测试中的 `vk has actived VkInstance` 是预期失败清理用例。
Release 上游代码有两处 C4702 不可达代码警告，未导致失败。
MC 环境另有 OSHI/Perflib 计数器、开发账户 Realms 鉴权及资源重载 post effect 警告，不影响该轮预览验证。

实际游戏检查确认可见 UV 左上黑、右上红、左下绿、右下黄，显示于右上角，原版区域外画面正常。
游戏验证中修复了 MC 最终 blit 的 Y 翻转补偿、Win32 semaphore 导入的 LWJGL 校验兼容以及等待结构计数。
该轮 D3D12 游戏运行尚未启用 driver validation，其诊断只在独立测试进程验证；早期启用修复的验证见本节 2026-10-28 记录。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
测试报告副本、最终日志和游戏截图保存在本机忽略目录 `build/verification/preview-20261027/`，不随仓库分发。
未验证正式启动器安装、多 GPU 切换或其他厂商 GPU；未主动注入 device lost/超时，也未在缺少共享能力的硬件运行。
无预览开关与 OpenGL 的禁用分支已运行，导入失败资源清理由 Vulkan 验证测试覆盖。

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

Debug 用于默认开发启动，Release 用于分发；只运行相应步骤也可以。构建本身离线，缺包则停止。
普通构建使用已嵌入的 shader 字节码，关闭 RadRay shader compiler/tools，不要求 DXC SDK。
不要复用旧 `windows-x64-msvc` 动态 CRT 包。若手动缩小依赖集合，stage 所需许可证来自 core/render 使用的第三方源码目录，需保留这些源码。

修改 `native/shaders/uv.hlsl` 或 `terrain.hlsl` 后显式准备锁定 DXC 并重新生成头文件，再运行 native 构建：

```powershell
python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc
python tools/compile_preview_shaders.py
python tools/compile_preview_shaders.py --shader terrain
```

脚本默认使用 `external/RadRay/SDKs/radray_dxc/extracted/bin/dxc.exe`，也可通过 `--dxc` 指定路径。
同一 HLSL 编译为 DXIL/SPIR-V，分别输出到 `native/include/raynium/uv_bytecode.h` 和 `terrain_bytecode.h`；运行时不打包编译器。

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
默认内容是 SOLID 地形；`-PpreviewContent=uv` 恢复 UV 诊断。`-PterrainTextureTransfer=auto|direct|split`
选择自动降级、强制多 mip 共享或强制逐 mip fallback（JVM 属性 `raynium.terrain.textureTransfer`）。
`-PterrainDirectDraw=true` 和 `-PterrainDisableFaceCull=true` 提供对照路径。
`-PterrainProfile=true` 记录 CPU/GPU 分阶段 p50/p95/p99、上传字节和驻留统计；GPU 需支持 timestamp query。
对应 JVM 属性为 `raynium.terrain.directDraw`、`raynium.terrain.disableFaceCull`、`raynium.terrain.profile`。
Debug RHI/driver validation 会增加 CPU 开销，性能对比应记录配置且固定视点、RT 尺寸和场景。

Debug 默认启用 RadRay RHI 检查。`rhiValidation=true`（JVM 属性 `raynium.rhi.validation`）
显式启用 native 驱动诊断，默认 false。客户端初始化入口通过 `rn_init_env` 准备所选后端的环境，
D3D12 在此创建 factory 并启用调试层，早于 MC 图形初始化；CLIENT_STARTED 复用环境创建 runtime。
也可通过独立 `nativeTest -PrhiValidation=true` 验证。
`vulkanValidation=true` 给 MC 添加 `--vulkanValidation` 参数，检查 MC Vulkan 命令流。
两者含义不同；native Vulkan 开关同时启用同步验证。GPU validation 未开启。

`radraySourceDir` 用于联调本地 checkout，仍要求其静态 CRT SDK 已准备；默认不使用旁边的 RadRay。构建会记录实际 RadRay commit 和 dirty 状态。发布只接受锁定提交的干净 Release 构建。

普通 `runClient` 和 Java `build` 不触发 native 构建。Java 开发 classpath 没有内置 payload 且未指定开发目录时，客户端记录提示并继续原版渲染。`runClientNative` 设置 `raynium.native.required=true` 和开发 stage 目录，缺失/错误 payload 会使初始化失败，不回退到另一份旧 DLL。

Loom 的两个客户端运行配置都给 Minecraft JVM 添加 `--enable-native-access=ALL-UNNAMED`。这是游戏 JVM 参数，不是 Gradle daemon 参数；正式启动器也需添加它。loader 检查实际模块的 native access，权限未启用时报告启动参数要求。

加载成功应记录 DLL 来源与实际路径，以及 ABI/build ID/RadRay revision/dirty/config/CRT/toolchain。
当前要求 ABI `7` 和十二个导出。`Raynium runtime created` 表示设备与队列完成；
`Raynium shared preview ready` 表示输出资源已创建和导入；`Raynium SOLID terrain inputs ready` 记录输入模式及 mip 数。实际显示仍须检查游戏画面。

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
.\gradlew.bat nativeTest -PrhiValidation=true
.\gradlew.bat nativeTest -PnativeConfig=Release
.\gradlew.bat nativeTest --tests "*.NativeRuntimeTest"
```

| 测试类 | 内容 | 任务 |
| --- | --- | --- |
| `NativeRuntimeBackendTest` | 后端配置解析和非法值 | `test` |
| `PreviewRegionTest` | 显示矩形、MC Y 翻转后的复制位置和小窗口边界 | `test` |
| `NativeAbiTest` | 参数校验、输出清空、诊断截断和空句柄销毁 | `nativeTest` |
| `NativeRuntimeTest` | 双后端环境/设备创建、重建、线程限制、幂等关闭、单例保护及早期 factory 共享绘制 | `nativeTest` |
| `MeshPayloadTest` / `TextureTransferTest` | 保守面组、越界模型、顶点拥有权和失败降级 | `test` |
| `NativeTerrainTest` | 双后端/两传输模式的 mip 像素、连续帧、深度、大坐标、epoch 和导入失败恢复 | `nativeTest` |
| `NativePreviewTest` | 双后端共享到独立 Vulkan 消费者，逐像素 UV/区域外验证、连续交接、等待、重建和导入失败清理 | `nativeTest` |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

native 测试 JVM 使用 Java 25、`--enable-native-access=ALL-UNNAMED` 与指定配置的 stage 路径。仅一个 worker，禁用 JUnit 并行；每个 runtime 的创建和销毁在同一线程，错误线程用例仅从额外线程调用预期被拒绝的 close。GPU/驱动状态不属于普通 Gradle 输入，因此 `nativeTest` 每次显式执行，不使用 up-to-date 或构建缓存结果。

JUnit `@TempDir` 管理每个用例的 DLL 缓存目录。测试 fixture 和 try-with-resources 保证断言失败时也先释放 native runtime 和环境，再卸载 DLL，最后清理临时目录。用例无顺序依赖，测试观测与句柄跟踪仅存在于测试源码，不加入生产对象。

预览测试在三种尺寸上各交接 8 帧，使用测试专属读回 buffer 校验每个像素；生产路径无读回。
消费者启用 Khronos validation 和同步验证，依赖本机安装的 Vulkan SDK/layer；缺失时测试失败。
原生后端诊断日志与 JUnit 结果一起检查，游戏内生命周期仍须单独运行。

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
