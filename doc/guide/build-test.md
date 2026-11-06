> - 适用: 恢复依赖、构建、测试、启动与分发 Raynium
> - 权威: 本文维护可执行命令和当前验证范围；ABI、内容缓存和加载契约归互操作文档
> - 锚点: `build.gradle`, `gradle/native.gradle`, `gradle.properties`, `native/CMakeLists.txt`, `native/CMakePresets.json`, `native/radray.lock.json`, `native/cmake/profiler-dependency/CMakeLists.txt`, `tools/fetch_radray.py`, `tools/build_debug_dependencies.py`, `tools/native.py`, `src/test/java/dev/raynium/bridge/*.java`

# 构建与测试

以下命令在 Raynium 根目录执行。Java/IDEA 设置见[开发环境](dev-environment.md)。
普通 `build` 只构建 Java 并执行纯 Java 测试；native 必须通过显式任务准备。
payload、ABI、缓存和关闭顺序见[互操作与加载](../architecture/interop.md)。

## 当前验证范围

### 2026-11-06：可见面组批量化

在同一锁定且干净的 RadRay 上，适配层按 runtime ElementMask 合并可见面组，单次查找并收集几何访问，
双槽复用可见 batch/几何列表；排序、缓存提取和绘制提交仍由 runtime 执行。顺序、实例覆盖及列表借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
显式运行 `python tools/build_debug_dependencies.py --parallel 8` 更新 Debug 依赖身份并完成增量准备。
Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01：

| 实际检查（Gradle 均附 `--console=plain --no-parallel`） | 结果 |
| --- | --- |
| `gradlew.bat nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true` | 93 项通过；RHI wrapper 与 native 驱动校验开启 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true` | 93 项通过；最终 Release/MT、Tracy payload 已 stage |
| 同 Release 配置，连接采集器运行 `NativeTerrainTest.groupedVisibleSubmeshesKeepOrderAndInstanceOverrides` | 4 项参数化用例通过；采集器自动保存约 368 ms、13 个帧标记、879 个 CPU zones 后退出 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

新增双后端 × direct/split 像素用例覆盖相邻合并、逆序与重复元素、全元素哨兵、不同排序键与实例起点/数量，
并在双槽在途下切换空可见帧后恢复绘制；已有高位掩码用例新增第 0/31 个元素的相邻合并。
短采集 CSV 逐帧读回确认可合并输入从 2 项变为 1 条 runtime 可见命令，其余顺序/实例边界保持预期计数，
且不再出现旧的 `Raynium.Frame.GatherGeometryAccesses` 二次遍历区间。这是功能和插桩验证，不作为实机场景性能结论。
测试日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Tracy 重复初始化的 SymInitialize 诊断仍出现，未验证调用栈符号解析。
测试消费者和显式诊断用例有各自的校验设置。本轮未启动游戏、重跑纯 Java 测试或发布打包。
证据在本机忽略目录 `build/verification/visible-batches-20261106/`，包括配置日志、三轮完整测试及短测 XML、构建清单、采集和逐帧计数摘要。

### 2026-11-06：批量绘制命令升级与采样准备

RadRay 更新到锁文件版本，受管源码保持干净。Raynium 将 ABI submesh 可见项转换成 ElementMask，
按同一掩码声明几何访问，并对齐上游每 batch 最多 32 个 submesh 的限制；缓存及绑定池边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01、Release `/MT`、Tracy 0.14.1：

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：89 项 native/GPU 用例通过，无失败、错误或跳过；新增双后端 × direct/split 用例逐像素验证第 0/31 个 submesh、全部元素掩码与中间空元素。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；采集器自动保存并退出，约 809 ms、21 个帧标记、1,793 个 CPU zones，CSV 成功读回 106 种区间，含 `RadRay.MeshDraw.Submit`。
- `python tools/check_docs.py`、`git diff --check`：通过。

日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Tracy 重复初始化仍有 SymInitialize 诊断，本轮未验证调用栈符号解析。
测试消费者和显式诊断用例的校验设置与游戏采样不同。本轮未重跑纯 Java 测试、Debug 或发布打包。
证据在本机忽略目录 `build/verification/tracy-session-20261106-222653/`，含测试日志、两轮测试 XML、短采集及构建清单。

随后用户以同一 Release/MT 构建启动游戏并操作本地世界，native D3D12 / MC Vulkan，关闭 RHI wrapper、native 驱动与 MC Vulkan 校验，启用 terrain timestamp。
日志确认干净 RadRay、2048×2048 五级 mip 图集直接共享及 213×120 预览 RT；退出时 runtime、环境和 DLL 依次正常关闭。
Tracy 自动保存约 101.4 秒、19,342 个帧标记、1,602,135 个 CPU zones，`session.tracy` 为 10,902,033 字节，采集器和游戏均已退出。
CPU 总耗时、CPU self time 与 GPU 事件 CSV 成功读回，同目录保存 `game.log` 和 `capture-summary.json`。本次未录制 JFR，也未与旧采样对齐场景或视点。
游戏日志有开发账号认证/Realms 诊断及缺少 post effect 的警告，未见 native 渲染失败。

### 2026-11-06：命令绑定池升级

RadRay 更新到锁文件版本，受管源码干净且无上游补丁；Raynium 的 BuildMeshDrawCommands 调用保持兼容。
缓存绑定改由上游独立池持有，命令和绑定存储的借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Release `/MT`、Tracy 0.14.1：

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：85 项 native/GPU 用例通过，无失败、错误或跳过。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；短采集保存约 708 ms、21 个帧标记、1,793 个 CPU zones，CSV 读回 106 种区间，含 `RadRay.MeshDraw.Submit`。

日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；已有的重复初始化 SymInitialize 诊断仍出现。
测试消费者和显式诊断用例的校验设置与游戏采样不同。本轮未重跑纯 Java 测试、Debug 或发布打包。
证据在本机忽略目录 `build/verification/tracy-session-20261106-191241-binding-pools/`，含测试日志、两轮测试 XML、短采集与构建清单。

### 2026-11-06：缓存命令借用升级与 Tracy 细分

RadRay 更新到锁文件版本且受管源码保持干净（`radrayDirty=false`），没有上游补丁。
Raynium 适配带只读 Scene 参数的 SetupGraph；命令借用边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
适配层补充几何/上传/材质/图声明和共享交接插桩，结合上游已有 runtime/RDG 分段。

Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44，Release `/MT`、Tracy 0.14.1：

- `gradlew.bat test nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：85 项 native/GPU 用例通过，无失败、错误或跳过；19 项纯 Java 测试复用 up-to-date 结果。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`，4 项参数化用例通过；短采集保存约 599 ms、21 个帧标记、1,792 个 CPU zones，CSV 成功读回 105 种 CPU 区间及 16 个 GPU 区间事件。
- 读回确认 Raynium 的实例/staging 复制、几何访问收集及 acquire/release 录制，以及 RadRay 的 RDG 依赖、附件解析、槽 arena 回收和 MeshDraw 提交区间存在。

测试日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；独立 Vulkan 测试消费者及显式测试诊断开关的用例仍启用校验，
不代表游戏采样配置。已有的重复初始化 SymInitialize 诊断仍出现；本次验证 zones 保存和读回，没有验证调用栈符号解析。
本轮未执行 Debug 或发布打包。证据在本机忽略目录 `build/verification/tracy-session-20261106-175340-deep/`，
包括 `release-final.log`、`release-results/`、`smoke.tracy`、CPU/GPU CSV 和 `native-build.json`。

随后用户手动启动游戏并进入本地世界：Release/MT、native D3D12 / MC Vulkan，关闭 RHI wrapper、native 驱动与 MC Vulkan 校验，
启用 Tracy、terrain timestamp 和 JFR。日志确认锁定的干净 RadRay、2048×2048 五级 mip 直接共享及 213×120 预览 RT，
退出时 runtime、环境和 DLL 正常关闭。Tracy 自动保存约 115.1 秒、27,833 个帧标记、2,140,458 个 CPU zones，
`session.tracy` 为 15,620,521 字节，CPU/GPU CSV 读回成功；`session.jfr` 为 12,875,855 字节，
JFR summary 读回 124 秒和 28,379 个 `dev.raynium.ClientFrame` 事件。文件及 `game.log`、`capture-summary.json` 均在同一证据目录。
日志另有系统 Perflib 计数器和开发账号 Realms 认证诊断；未见 native 渲染失败。本次场景没有与旧采样对齐，不作为同视点性能比较。

### 2026-11-06：RenderGraphFramePipeline 接入

RadRay 更新到锁文件版本，受管源码保持干净（`radrayDirty=false`），原有本机插桩未回放。
SolidFramePipeline 改用 runtime RDG，消费边界和共享交接见 [Native runtime](../architecture/native-runtime.md)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44；显式执行
`python tools/build_debug_dependencies.py --parallel 8` 更新 Debug 依赖身份并完成增量准备。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat test nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java 测试、85 项 native/GPU 用例通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 85 项 native/GPU 用例通过，无失败、错误或跳过 |
| `gradlew.bat packageNativeMod verifyNativePackage --console=plain --no-parallel` | 干净 Release/MT、未启用 profiler 的分发包构建及结构、哈希校验通过 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双配置覆盖 D3D12/Vulkan、direct/split 纹理、双槽在途资源、几何 arena、深度与大坐标。
新增 4 项参数化用例：在无可见 draw 时上传几何及纹理，两槽空帧后再绘制，随后输出尺寸按 32/36/40/32 往返，
每次均在空帧后复用驻留资产并逐像素验证，覆盖上传输出保活、槽池复用与资源交接。
两配置均启用 native 驱动校验；Debug 启用 RHI wrapper，Release 按上游默认关闭 wrapper。
日志无 RHI 拒绝、D3D12 警告/错误、Vulkan VUID 或同步验证错误。
非阻塞诊断包括 LWJGL 的 Unsafe 弃用提示、Vulkan loader 的包映射调试消息、上游 D3D12 的 C4702 不可达代码警告，
以及启用 Tracy 时已有的 SymInitialize 错误；本轮未采集 Tracy、未验证符号解析，也未启动游戏客户端。

本机证据在忽略目录 `build/verification/radray-upgrade-20261106/`：`debug-final.log`、`release.log`、`package.log`，
以及分别保存的 `debug-results/`、`release-results/`、`java-results/` JUnit XML。
分发包为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。

随后用户以 Release/MT、native D3D12 / MC Vulkan 启动开发客户端并进入本地世界操作，启用 Tracy 和 terrain timestamp，
关闭 RHI wrapper、native 驱动及 MC Vulkan 校验。游戏日志确认干净 RadRay、ABI 9、2048×2048 五级 mip 图集直接共享及 213×120 预览 RT。
游戏正常退出后，runtime、环境和 DLL 依次关闭，采集器自动结束并保存约 73.9 秒、21,524 个帧标记、456,455 个 CPU zones，文件 5,001,314 字节。
`tracy-csvexport` 成功读回，确认 `Raynium.Frame.SetupGraph` 和 `Raynium.Frame.RecordDraws` 等 zone 存在。
证据在本机忽略目录 `build/verification/tracy-session-20261106-151006/`，含 `session.tracy`、`zones.csv`、`game.log`、构建清单、采集配置及摘要。
本次未录制 JFR，场景未与旧采样对齐，不作为同视点版本性能比较。

### 2026-11-05：帧内 GPU 资源持久化

每槽帧资源、每槽 staging 与几何 arena 落地后的验证。资源归属与退役边界见 [Native runtime](../architecture/native-runtime.md#上传帧槽与资源退役)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44；RadRay 基于锁文件提交，但工作树含本机改动（`radrayDirty=true`）。
Debug `/MTd` 依赖按当前 RadRay 提交重建：`python tools/build_debug_dependencies.py --clean --parallel 8`。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 81 项 Debug native/GPU 用例通过，无失败、错误或跳过；含 RadRay RHI wrapper 检查 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 81 项 Release native/GPU 用例通过，无失败、错误或跳过 |
| 两配置日志 | 双后端 driver validation 开启；无 RHI 校验拒绝、D3D12 警告/错误、Vulkan VUID 或同步验证报告，无 `[warn]`/`[error]` 行 |
| 两配置 stage manifest | ABI 9、Debug MTd / Release MT、`radrayDirty=true`；Release 另含 profiler |

新增三组双后端 × 完整/split 用例：驻留顶点跨两个 arena 块且第二块含非零 base vertex、
单帧上传超过初始 8 MiB staging 并在替换后由两槽复用、删除 mesh 后区间在槽复用后才被同尺寸新 mesh 重新分配（同块邻居全程可见）。
既有 78 项继续覆盖双槽在途参数/资源隔离、准备失败保活、输出按 32/36/40 逐代重建（槽 0 遇尺寸变化重建深度）、增量驻留、材质/mesh 更新、反向深度与大坐标。

本轮未启动游戏客户端、未重采 Tracy，也没有实机性能结论；`packageNativeMod` / `verifyNativePackage` 未执行，
因为受管 RadRay 工作树处于 dirty 状态，打包会按要求拒绝。
本机日志位于忽略目录 `build/_dtest_full2.log`、`build/_rtest_final.log`，JUnit XML 在 `build/test-results/nativeTest/`。

### 2026-11-05：依赖升级与深度 Tracy 插桩

RadRay 更新到当前锁文件版本；适配合并后的 `runtime/material.h`，并在本机对受管源码做了未入库的本地 Tracy 插桩。
修正专用 Tracy SDK 的 delayed-init 编译与导出定义后，使用 Windows x64、MSVC 19.51.36260.0、
RTX 3060 / NVIDIA 591.44，Release `/MT`、Tracy 0.14.1 进行验证。

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：69 项 native/GPU 用例通过，无失败、错误或跳过。
- 同配置连接采集器，运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；采集器保存约 915 ms、21 个帧标记、1,830 个 CPU zones，CSV 工具成功读回 84 种 zone 名。
- 短采集确认新增的资源创建、Map/复制、槽回收、缓存同步、draw 提交和 D3D12 队列 wait/signal 区间可见；native 驱动与 RHI wrapper 日志均为 validation=false。
- `python tools/check_docs.py`、`git diff --check` 通过。

测试中的独立 Vulkan 消费者仍按测试实现启用校验，不代表正式游戏配置；本轮未验证 Debug 或打包。
Tracy 重复初始化仍有 Windows SymInitialize 诊断，已验证 zones 保存和读回，未验证调用栈符号解析。
证据位于本机忽略目录 `build/verification/tracy-session-20261105/`，包含构建测试日志、69 项测试 XML、短采集、CSV、构建清单及采集配置。

随后由用户启动开发客户端并进入本地世界操作：native D3D12 / MC Vulkan，Release `/MT`，
RHI wrapper、native 驱动与 MC Vulkan 校验关闭，启用 terrain timestamp 和 JFR。
游戏日志确认 ABI 9、2048×2048 五级 mip 图集直接共享与 213×120 预览 RT，最后 runtime、环境和 DLL 依次正常关闭，Gradle 成功退出。
Tracy 采集器正常退出并保存约 75.7 秒、11,828 个帧标记、1,009,107 个 CPU zones，文件约 5.84 MiB；
CSV 工具读回 82 种 zone 名，确认资源创建、数据写入、槽回收、缓存同步与 draw/队列提交的新增区间存在。
同目录 `session.jfr` 约 10.60 MiB，JFR summary 成功读回 85 秒及 12,219 个 `dev.raynium.ClientFrame` 事件。
完整文件为同目录 `session.tracy`、`session.jfr`、`zones.csv`、`game.log`、`game-console.log`、`capture-summary.json` 与 `jfr-summary.txt`。
本轮操作场景没有与旧采样对齐，不作为同视点版本性能比较。

### 2026-11-04：FrameSlot 资源退役接口升级

更新锁定 RadRay，并适配返回 void 的 Extract/Record/RunFrame 及 FrameSlot::RetireResource。
资源转交与失败后的保活边界见 [Native runtime](../architecture/native-runtime.md#上传帧槽与资源退役)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01，Release `/MT`，启用 Tracy 0.14.1。

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel`：65 项现有 native/GPU 用例通过。
- 随后同配置运行 `nativeTest --tests '*.NativeTerrainTest.failedPreparationRetainsResourcesForPendingFrame'`：新增 4 项双后端/direct-split 用例通过；更新中途失败后不重试，完成在途帧像素检查并正常关闭。
- 连接采集器的短测保存了 0.607 秒、81 个 CPU zones 的 Tracy；采集器及测试 JVM 均正常退出。

两轮测试均无失败、错误或跳过，日志无 D3D12 校验错误、Vulkan VUID 或同步错误。
Tracy 重复初始化时仍出现 Windows SymInitialize 诊断，已插桩 zones 可采集；未验证调用栈符号解析。
自动测试、短采集及 stage 清单位于本机忽略目录 `build/verification/tracy-session-20261104-231831/`。

随后用户以 Release、native D3D12 / MC Vulkan 进入开发世界并操作，RHI wrapper、native 驱动与 MC Vulkan 校验关闭，开启 terrain timestamp 统计。
实机日志确认 ABI 9、2048×2048 五级 mip 图集直接共享和 213×120 预览 RT；游戏正常退出后保存 74.8 秒、25,324 个帧标记、544,251 个 CPU zones 的 Tracy，约 3.84 MiB。
同目录 `session.tracy` 已由 CSV 工具读回，runtime、环境和 DLL 按顺序正常关闭，采集器退出。
本轮未验证 Debug 或打包；两次实机操作场景未对齐，不作为同视点的版本性能比较。

### 2026-11-04：Tracy 瓶颈增量更新优化

移除每帧 Scene/TerrainState 备份与回滚、驻留资源引用全表校验和逐索引 CPU 扫描，驻留字典改为哈希表；
共享 quad 索引首次至少准备 65,536 顶点容量。增量更新与资源寿命契约见 [Native runtime](../architecture/native-runtime.md#适配层职责)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44，使用锁文件对应的干净 RadRay 源码。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java、65 项 Debug native/GPU 测试通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 65 项 Release native/GPU 测试通过，无失败、错误或跳过；Mod JAR 生成并通过结构、哈希与许可证检查 |
| 两配置 DLL 与 stage | ABI 9、十三个 C 导出，Debug/MTd、Release/MT，无动态 CRT 导入；本轮未启用 native Tracy 客户端 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双后端及 direct/split 路径启用 native 驱动诊断，Vulkan 消费者启用校验及同步验证；Debug 另启用 RHI wrapper 检查。
新增四组像素回归覆盖在途帧期间批量新增 96 组资源、哈希表增长后旧 batch 保持有效、纯材质更新、
primitive 替换同帧删除旧材质与几何、批量删除、零更新帧与 epoch 切换。
既有测试继续覆盖双槽参数/资源隔离、多 submesh、索引与网格同 ID 替换、输入/输出重建及消费者超时保活；
已移除依赖中途失败后继续重试场景的断言。日志无 RHI/驱动校验错误、Vulkan VUID 或同步错误。
Release 上游 D3D12/Vulkan 源码各有一处 C4702 警告，未导致失败。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
证据保存在本机忽略目录 `build/verification/tracy-optimization-20261104/`，包含两配置日志、JUnit XML/报告、构建与 stage 清单及结果摘要。
本轮未启动真实游戏、重采 Tracy 性能或注入 device lost/GPU 分配失败；此前采集的耗时不代表优化后实测。

### 2026-11-04：ABI 9 与持久 Scene 命令缓存

环境为 Windows x64、Temurin 25.0.4.1、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0。
锁文件升级到上游 2026-11-04 的“帧概念”版本，显式 restore 干净受管源码，并通过
`python tools/build_debug_dependencies.py --clean` 重建 Debug `/MTd` 依赖。
`python tools/compile_preview_shaders.py` 重新生成地形 DXIL/SPIR-V，绑定拆分为 Pass Group 0 与 Material Group 1。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java、61 项 Debug native/GPU 测试通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest packageNativeMod verifyNativePackage -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 61 项 Release native/GPU 测试通过，无失败、错误或跳过；分发包生成并通过结构、哈希与许可证检查 |
| 两配置 DLL 与 stage | ABI 9、十三个 C 导出、RadRay 干净；Debug 为 MTd，Release 为 MT，均无动态 CRT 导入 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双后端测试启用 native 驱动诊断，独立 Vulkan 消费者启用校验及同步验证；Debug 另启用 RadRay RHI wrapper 检查。
新增双槽像素回归覆盖同一 mesh 的多个 submesh、不同材质、单材质更新、索引扩容替换、网格替换、显式删除及 epoch 切换；
两个 native 槽均在宿主发出输入 signal 前提交。既有回归继续覆盖多实例与 FirstInstance、各 mip、两种纹理传输、
反向深度、大坐标、输入/输出重建、失败更新和消费者超时保活。共享清屏回归使用固定雾色，并检查复制区域外像素。
日志没有 RHI/驱动校验错误或 Vulkan VUID。Release 上游 D3D12/Vulkan 源码各有一处 C4702 警告，未导致失败；
DXIL 编译忽略 HLSL 的 Vulkan binding 属性，SPIR-V 编译使用这些属性。

产物为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。
证据位于本机忽略目录 `build/verification/runtime-rewrite-20261104/`，包含两配置日志、JUnit XML/报告、manifest 与 dumpbin 输出。
本轮未启动真实游戏、重采性能或注入 device lost/GPU 分配失败；自动测试不替代这些验证。
以下较早日期记录反映当时实现与配置。

### 2026-11-04：Release Tracy 缓存路径采集

在同日 ABI 9 重写后的源码上补充分阶段 Tracy zones 与缓存/驻留 plots。Release `/MT`、静态 Tracy 0.14.1，
native D3D12、MC Vulkan；RHI wrapper、native 驱动与 MC Vulkan 校验关闭，开启 terrain timestamp 统计。
本配置的 `nativeTest` 61 项通过，无失败或跳过；客户端由用户按上述配置自行启动并操作，采集器先行等待连接。
游戏日志确认 ABI 9、Release/MT、2048×2048 五级 mip 图集直接共享及 213×120 输出 RT。

客户端正常退出后，采集器自动保存 63.3 秒、5,658 个帧标记、207,284 个 CPU zones 的记录，约 1.34 MiB；
Tracy CSV 工具成功读回，游戏日志确认 runtime、环境及 bridge DLL 依次正常关闭。
其中 5,655 次 native Render 调用，包含区块加载和场景增长，全段平均 3.92 ms、p95 10.86 ms。

选择 trace 时间 49.306–51.831 秒的连续 240 次调用：驻留 mesh/primitive 均为 3,155，可见命令 3,361，
缓存命令 17,345；mesh 更新与脏 batch 数均为 0，期间没有 BuildBatch zone。
该区间由资源/可见数量固定条件选择，未记录相机是否固定。

| 项目 | 平均 ms | p95 ms |
| --- | --- | --- |
| native Render | 6.849 | 8.619 |
| Scene Prepare | 1.514 | 2.109 |
| Scene 备份 | 1.381 | 1.835 |
| Scene 增量应用 | 1.293 | 1.912 |
| draw 命令录制 | 0.950 | 1.553 |
| 可见命令排序 | 0.076 | 0.094 |
| 帧槽复用等待 | 0.0011 | 0.0015 |
| 已完成 native GPU timestamp 区间 | 0.458 | 0.550 |

CPU zones 为包含子 zone 的墙钟耗时，不能相加；GPU 样本在回收帧槽时发布，不对应发布时这帧的即时 GPU 耗时。
本轮没有采集 JFR 或 MC 全帧 GPU 数据，场景与此前采样不同，不作为同视点的版本性能对比。
证据位于本机忽略目录 `build/verification/tracy-session-20261104/`：`session.tracy`、游戏/构建测试日志、
manifest、测试 XML、CSV、采集设置及分析脚本/JSON。文档与 diff 检查通过。

### 2026-11-03：Release Tracy 客户端采集

环境为 Windows x64、Temurin 25.0.4.1、MSVC 19.51、RTX 3060 / NVIDIA 591.44；MC 使用 Vulkan，native 使用 D3D12。
显式准备 Release `/MT` 依赖并启用 Tracy 0.14.1，RHI wrapper、native 驱动与 MC Vulkan 校验关闭；开启 terrain timestamp 统计。
客户端由用户操作，Tracy 保存约 90 秒、1,962 个帧标记、37,247 个 CPU zones；其中 1,959 次 native Render 调用。
存档末段采集时间 82–89 秒的 530 帧均有 3,436 条 draw、没有网格更新：

| 项目 | 平均 ms | p95 ms |
| --- | --- | --- |
| native Render CPU | 9.58 | 11.03 |
| Scene Prepare（包含排序与 batch 构造） | 6.72 | 7.75 |
| 可见项排序 | 5.12 | 5.94 |
| Frame Prepare | 1.10 | 1.41 |
| draw 命令录制 | 0.57 | 0.71 |
| native GPU timestamp 区间（回收时发布的 529 个样本） | 0.63 | 0.70 |

这些 CPU zones 有父子嵌套关系，不能逐行相加。帧槽复用等待平均约 0.0014 ms。
JFR 最初没有随继承的 Loom 配置进入 JVM，随后手工启用；退出卡住后用户强制结束，最终 JFR 文件为空。
从临时仓库保留了一个约 46 秒的完整尺寸 chunk：在独立副本中仅将偏移 64 的未完成状态改为完成状态后，JDK 工具可读取其中事件。
该恢复副本不能作为完整启动/正常退出记录。12:17:27–12:17:33 的 455 个世界帧平均 13.02 ms、p95 15.25 ms；该区间 GC pause 合计 14.69 ms。

随后修正客户端 JVM 参数继承，并用 Gradle 实际启动参数检查确认包含 JFR 开关；Tracy 改为专用 manual-lifetime SDK，
在 DLL 卸载前显式结束采样线程。Release 采样配置的 `NativeRuntimeTest` 和 `NativeRendererTest` 共 16 项通过，无失败或跳过，测试 JVM 正常退出。
另在采集器连接状态下运行 `NativeRendererTest.failedConsumerWaitRetainsOwners` 的两个后端用例，均通过；采集器完成 5.57 秒 trace 保存，测试 JVM 正常退出。
该修复未重新采集用户游戏全过程。证据位于本机忽略目录 `build/verification/tracy-session-20261103/`，包含原始 Tracy、CSV、JFR 原块/恢复副本、统计与测试日志。

### 2026-11-02：固定 MT 构建

本节记录当时两配置均为 MT 的验证；当前 Debug 已改为 MTd，操作步骤见下文。

保留更新后的 RadRay 锁定源码，将 mod 的 Debug/Release 固定为 `/MT`，静态链接 mimalloc。
按 `windows-x64-msvc-staticcrt` 显式 `--clean` 重建所需的 16 个依赖包，再构建 bridge 和 stage；
移除 Fabric preLaunch 分配器加载器，恢复 schema 1 的 MT 单 DLL payload。

| 检查 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 18 个纯 Java、61 个 Debug native/GPU 用例通过，无失败或跳过 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 61 个 Release native/GPU 用例通过；分发 JAR 生成并通过结构/哈希检查 |
| 固定 GPU 场景图像 | 两配置各 48 张 PNG 与 FramePipeline 迁移基线逐像素及哈希一致 |
| 固定 CRT 约束 | CMake 拒绝 `/MD`、`/MTd` 和 MD SDK 三元组；stage 拒绝旧 MD DLL 的动态 CRT 导入 |
| 最终 JAR | 仅有 `rayniumruntime.dll`；manifest 为 MT/schema 1，无 mimalloc/redirect DLL、preLaunch 入口或加载器 class |

两配置链接命令使用 whole-archive 静态 mimalloc，DLL 导入仅含已分类的 Windows/图形依赖，无动态 CRT。
GPU 回归日志无 RHI/驱动校验错误或 Vulkan VUID；本轮未重新启动游戏或测量 FPS。
证据保存在本机忽略目录 `build/verification/mt-runtime-20261102/`，包含依赖重建、约束拒绝、两配置构建测试日志、JUnit XML 和 PNG。

### 2026-11-02：动态 CRT 与 mimalloc DLL

本节记录当时实际执行的 MD 验证；该构建路线现已由固定 MT 路线替换，当前操作步骤见下文。

使用更新后的锁文件和干净受管 RadRay 源码，按 `windows-x64-msvc` 三元组显式 `--clean` 重建当前所需的 16 个依赖包（含 GTest），
分别生成 Debug `/MDd` 与 Release `/MD` SDK；core/render/runtime 和 bridge 使用新的独立 CMake 目录重新构建。
环境为 Windows x64、Temurin 25.0.5.8、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01。

| 检查 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 17 个纯 Java、64 个 Debug native/GPU 用例通过，无失败或跳过 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 64 个 Release native/GPU 用例通过；三 DLL 分发 JAR 生成并通过结构/哈希检查 |
| 两配置固定 GPU 场景图像 | 各 48 张 PNG 与此前 FramePipeline 迁移基线逐像素相同，PNG 哈希相同 |
| 分配器探针 | 两配置 `mi_malloc` 分配属于 mimalloc；CRT `malloc` 不属于 mimalloc，`mi_is_redirected() == false`；bridge 关闭后分配器仍有效 |
| 缓存与配置拒绝 | 旧 schema、MT/MD 配置不符、缺失文件、非法路径/哈希、损坏 allocator 缓存及同进程切换 allocator 身份均被拒绝 |
| Release `runClientNative`，关闭 RHI/MC Vulkan 校验和 terrain profile | 标题画面显示并通过“退出游戏”正常结束；日志确认 runtime → environment → bridge 关闭 |

真实客户端 23:39:32 在 Fabric `preLaunch` 加载 mimalloc/redirect，23:39:39 加载 bridge，随后创建 MC Vulkan 和 native D3D12 设备。
实际状态仍为 `redirected=false`：redirect 诊断表明 JVM 的 CRT 已先初始化。这是进程级重定向未生效，不能据此声称全部 CRT/C++ 分配已切换到 mimalloc。
没有修改 Java 可执行文件或安装 JVM 启动前注入。DLL 搜索和寿命契约见[互操作与加载](../architecture/interop.md#dll-的依赖搜索)。
本轮没有进入世界或测量 FPS；地形回归来自自动 GPU 图像用例。GPU 回归日志无 RHI/驱动校验错误或 Vulkan VUID；redirect 自身的未接管诊断保留。
证据保存在本机忽略目录 `build/verification/md-runtime-20261102/`，包含两配置日志、JUnit XML、PNG 和 `game-release.log`。

### 2026-11-02：FrameScheduler 与 Raynium 专用 FramePipeline

环境为 Windows x64、Temurin 25.0.5.8、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01、Vulkan SDK 1.4.363.0。
先恢复迁移前锁文件对应的 RadRay 源码并重建基线，再更新锁文件、显式 restore 新版；最终使用受管 `external/RadRay`，没有本地源码覆盖。
Debug/Release 的受影响 core/render/runtime 静态库、bridge DLL 与 stage 均重新编译/生成；两份元数据均匹配当前锁文件、`radrayDirty=false`、C++20、`/MT`、ABI 8。
本轮没有修改 RadRay 源码。通用 RenderSystem、SceneRenderer、View/Pass 与 Render Graph 未接入。

以下 Gradle 命令均附 `--console=plain --no-parallel`；图片目录参数使用本机绝对路径。

| 命令/检查 | 实际结果 |
| --- | --- |
| 迁移前 `gradlew.bat nativeTest -PrhiValidation=true -PterrainProfile=true` | 57 个 native/GPU 用例通过，无跳过 |
| 迁移前 `nativeTest --tests '*.NativeTerrainTest' -PrhiValidation=true -PtestImages=<baseline-pixels>` | 保存 48 张固定场景 PNG，完整/split 共享路径使用同一逐像素预期 |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | Java 构建、8 个纯 Java 用例与 61 个 Debug native/GPU 用例通过 |
| 最终 `nativeTest -PrhiValidation=true -PterrainProfile=true -PtestImages=<final-debug-pixels> -PtestBaseline=<baseline-pixels>` | 61 个 Debug 用例通过，0 失败、0 跳过；48 张 PNG 与基线逐像素一致，PNG 文件哈希也一致 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true -PtestImages=<final-release-pixels> -PtestBaseline=<baseline-pixels>` | 61 个 Release 用例通过，无跳过；48 张 PNG 与基线一致；生成正式 native JAR |
| `gradlew.bat verifyNativePackage` | 包结构、manifest、ABI 和哈希检查通过 |
| 锁定 RadRay standalone Debug `/MT` 构建 `test_radray_runtime`；`ctest -R 'FrameLoop\|MeshDrawing\|Material'` | 35 项通过，无跳过；FrameLoop 实际运行 D3D12/Vulkan，包括失败恢复、部分提交、arena 回收与退出顺序 |

新增双槽测试先提交两帧，再提供宿主输入 signal，确保两槽同时在途。连续 8 对帧替换同 ID 网格、索引和材质，
交替相机小数偏移、顶点颜色、direct/indirect 和纹理变化掩码；同一可见项展开两个不同原点的实例。
逐像素验证无参数串帧、无旧版本提前释放；非法索引更新在输入 signal 尚不存在时立即拒绝，随后相同序号可继续。
最后验证移除全部场景资源、缺失引用拒绝、epoch 切换和正常 owner 关闭。既有回归继续覆盖 mip、反向深度、大坐标、混合 UV/SOLID、resize、输入重建与消费者超时保活。
两配置日志未发现 RHI/驱动校验错误或 Vulkan VUID。Release 上游 D3D12/Vulkan 各有一处 C4702 不可达代码警告，未导致失败。
GPU validation 未开启；未主动注入 device lost 或 GPU 分配失败。

迁移前实际运行了 D3D12 native / MC Vulkan：进入已有“新的世界”，观察 SOLID 预览，保存截图后返回标题并正常退出，日志确认 runtime、环境和 DLL 顺序关闭。
基线为 854×480 主画面、213×120 小 RT、FOV 配置 0、renderDistance=16、mipmapLevels=4、gamma=0.5、窗口模式，使用 `/time set noon` 固定光照时段。
截图、设置和退出后的世界副本均保存在本机忽略目录，可用同一世界存档中的玩家视点复现。
迁移后的开发客户端已启动并创建设备，但用户按 Escape 停止 computer-use，因此没有继续进入世界、截图比较或交互退出。
这次固定 GPU 场景像素相同不等于最终游戏画面已确认相同，也没有本轮实机性能结论。

本机证据目录为 `build/verification/frame-runtime-20261102/`：`baseline-game.png`、`baseline-options.txt`、`baseline-world/`、
三组像素目录、`final-debug-junit/`、`final-release-junit/`、`runtime-tests.xml` 和相应日志。standalone 的实际 configure/build 脚本也保存在该忽略目录。
未新增开发历史，既有 history 保持原样。

### 2026-10-30：runtime 迁入 RadRay

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

### 2026-10-30：迁移前的可编程 runtime 与 RDG

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

### 2026-10-30：统一 mesh draw pipeline 与 ABI 8

本轮使用 Windows x64、Temurin 25.0.4.1、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0。
RadRay 为锁定的干净源码；本轮未修改 RHI。以下 Gradle 命令均附 `--console=plain --no-parallel`。

| 命令 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | Java 构建、8 个纯 Java、57 个 Debug native/GPU 用例通过；核心 CTest 通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true` | 57 个 Release native/GPU 用例、核心 CTest、Release DLL/stage/JAR 构建通过 |
| `gradlew.bat verifyNativePackage` | 包结构、ABI 8 manifest 与哈希检查通过 |
| `python tools/check_docs.py`、`git diff --check` | 文档引用与差异空白检查通过 |

核心测试覆盖缓存复用、视图/实例不重建、几何/材质/index/纹理版本失效、UV 不受无关纹理重建影响、准备失败不发布、epoch 清除、连续范围合并、严格实例化和 direct/indirect 参数一致。
GPU 测试覆盖 D3D12/Vulkan、完整与分 mip 传输、所有 mip 像素、反向深度、大坐标、连续帧及 epoch。
新增用例在同一帧混合 UV 与 textured program，分别检查 direct/indirect 的完整像素；输入与输出同时重建后，不重新上传场景即可继续绘制。
无输入 signal 时先提交非法可见引用会立即失败；修正后同一序号可正常提交。消费者未 signal 导致输出关闭超时后，target、renderer、runtime、环境仍保持所有权，完成消费后可继续绘制并关闭。
旧 preview/terrain 导出已删除；两配置 stage 的 `/exports` 检查确认十三个 ABI 8 入口。
最终两配置日志未发现 RHI/驱动校验错误或 Vulkan VUID；GPU validation 未开启。硬件覆盖仍仅限本机 GPU/驱动。

本轮启动过 D3D12 native / MC Vulkan 开发客户端，日志确认 ABI 8、早期环境初始化与设备创建。
按用户要求停止 computer-use 后只读取日志：客户端随后进入本地世界，创建 213×120 地形输出，记录连续帧与几何上传统计，最后正常关闭 runtime、环境和 DLL。
日志未出现 RHI/驱动校验错误；启动期间有 OSHI/Perflib 与开发账户 Realms 鉴权错误。未观察游戏画面，移动、区块更新、resize、资源重载、UV 切换及禁用预览的完整交互验收仍未完成。
该客户端启动早于最后的 pass 文件拆分与关闭路径收尾，不能代表最终代码的实机验证。
CPU/GPU 耗时与上传统计保留。上述运行中，约 60 个可见网格的稳定窗口显示 command_builds 保持 82、geometry_upload_bytes 为 0，CPU 帧调用 p50 约 1.8–2.1 ms，GPU 光栅 p50 约 0.026 ms。
这些样本不是固定场景基准，且采自收尾前版本；本轮未完成同场景同配置的迁移前后性能对比，不据此推断帧率变化。
因此本轮自动验证已通过，完整的实机和性能验收尚未完成；旧日期记录不是本轮重测结果。
日志与两配置 XML 位于本机忽略目录 `build/verification/mesh-pipeline-20261030/`，没有新增 history。

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
python tools/build_debug_dependencies.py --parallel 8
python external/RadRay/tools/build_third_party.py build --triplet windows-x64-msvc-staticcrt --config Release --parallel 8
```

Debug 用于默认开发启动，Release 用于分发；只运行相应步骤也可以。构建本身离线，缺包则停止。
普通构建使用已嵌入的 shader 字节码，关闭 RadRay shader compiler/tools，不要求 DXC SDK。
Debug 使用 `/MTd`、Debug CRT 和默认 STL iterator 检查；Release 使用 `/MT`。两配置均静态链接 mimalloc，不复用 MD 包。
锁定 RadRay 的 `staticcrt` 三元组在 Debug 也固定 MT，因此 Debug 使用 Raynium 的显式离线脚本，
按上游依赖 catalog 准备当前所需的 16 个包；构建和 SDK 位于 `build/native-dependencies/windows-x64-msvc-mtd/`。
该脚本自动获取 x64 MSVC 环境，普通终端也可执行；源码仍由 fetch 阶段准备，缺失即失败。
Debug 的 mimalloc 关闭 CRT 覆盖并保留显式 `mi_*` 分配，Release 保留静态覆盖；详见[构建身份与 payload](../architecture/interop.md#构建身份与-payload)。
依赖源码或配方升级后显式加 `--clean` 重建；两种脚本只清理各自的输出目录。
Release 如只准备当前 core/render/runtime 所需集合及测试库，可向上游 build 命令添加
`--only fmt,magic_enum,eigen,sigslot,stdexec,spdlog,xxHash,yyjson,pugixml,mimalloc,Vulkan-Headers,volk,VulkanMemoryAllocator,DirectX-Headers,D3D12MemoryAllocator,googletest`。
stage 所需许可证来自这些第三方源码目录，需保留源码。

修改 `native/shaders/terrain.hlsl` 后显式准备锁定 DXC 并重新生成头文件，再运行 native 构建：

```powershell
python external/RadRay/tools/fetch_sdks.py restore --only radray_dxc
python tools/compile_preview_shaders.py
```

脚本默认使用 `external/RadRay/SDKs/radray_dxc/extracted/bin/dxc.exe`，也可通过 `--dxc` 指定路径。
地形 HLSL 编译为 DXIL/SPIR-V，输出到 `native/include/raynium/terrain_bytecode.h`；运行时不打包编译器。

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
内容为 SOLID 地形。`-PterrainTextureTransfer=auto|direct|split`
选择自动降级、强制多 mip 共享或强制逐 mip fallback（JVM 属性 `raynium.terrain.textureTransfer`）。
`-PterrainDisableFaceCull=true` 关闭 Java 面组剔除以进行对照。
`-PterrainProfile=true` 记录 CPU/GPU 分阶段 p50/p95/p99、上传字节和驻留统计；GPU 需支持 timestamp query。
对应 JVM 属性为 `raynium.terrain.disableFaceCull`、`raynium.terrain.profile`。
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
当前要求 ABI `9` 和十三个导出。`Raynium runtime created` 表示设备与队列完成；
`Raynium shared preview ready` 表示输出资源已创建和导入；`Raynium SOLID terrain inputs ready` 记录输入模式及 mip 数。实际显示仍须检查游戏画面。

## Tracy 与整段客户端采样

`-PnativeProfiler=true` 启用 native Tracy 标记，默认关闭；对应 `tools/native.py configure --profiler`
和 CMake `RAYNIUM_ENABLE_PROFILER`。开关计入构建身份，清单记录 `profilerEnabled`，开启时 stage 包含 Tracy 许可证。
先用 `python external/RadRay/tools/fetch_third_party.py restore --only tracy` 准备源码，再运行
`python tools/build_profiler_dependencies.py --config Release`（Debug 使用 `--config Debug`）。
脚本显式离线构建当前配置的静态 CRT Tracy SDK，位于 `build/native-dependencies/tracy/<配置>/sdk/`；native 构建不隐式准备该 SDK。
当前锁定依赖使用 Tracy 0.14.1，客户端为静态、on-demand、仅 localhost，关闭广播和 crash handler。
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

Tracy zones 区分环境/设备创建与销毁、`Scene.Prepare`、按增量删除的 `Scene.UpdateResources`、`Scene.StageGeometry`、
`Scene.MaterialBindings`、`Scene.Apply`、`Scene.VisibleBatches` 与 `Frame.StageInstances`。
`Scene.RetireEpoch` 只在切换已有 epoch 时记录，`Scene.RefreshBatches` 只在已有 mesh/index 被覆盖后扫描存活 primitive 时记录。
管线另记录帧槽等待、帧数据提取、SetupGraph、可见缓存命令指针收集、几何上传录制、纹理 acquire/release、draw 录制和 queue submit；以上 zone 名省略 `Raynium.` 前缀。
`Scene.StageGeometry` 内的 `Geometry.StageUpload` 覆盖写入槽 staging 的每一步；`Raynium.Cache.BuildBatch` 只在 Processor 实际重建脏 batch 时记录；
`Raynium.RunFrame` 包含上游缓存同步与槽回收，其他阶段用子 zone 区分。
plots 包含可见项、网格更新、draw 数量、已发布场景的驻留数量、RunFrame 前的脏 batch 数、同步后的缓存命令数及 GPU 毫秒数。
`Frame.WaitReusable` 检查槽完成状态，深度创建与复用、常量上传、排序、barrier 和 encoder 开闭已由 runtime 管理；`Raynium.Buffer.Create` 只覆盖适配层显式创建的缓冲。
另有 timestamp resolve、fence 等待和 GPU timing 读回区间；
plots 记录 index/material/primitive 更新、primitive 删除、实例、纹理变化位掩码、上传次数/字节、待退役资源数量和几何 arena 块数。
`Raynium.Arena.FreeBytes`、`Raynium.Frame.StagingCapacity` 与 `Raynium.Frame.InstanceCapacity` 只在启用 `RADRAY_ENABLE_PROFILER` 时发布。
Raynium 还细分几何 arena 分配/回收/增块、staging 与实例 memcpy、UploadBuffer 创建/扩容/flush、材质绑定创建、
图常量/上传/纹理/光栅访问声明，以及共享 acquire/release 命令录制与提交。
可见项掩码合并与几何访问去重统一计入 `Raynium.Scene.VisibleBatches`，已移除单独的 `Raynium.Frame.GatherGeometryAccesses` 二次遍历；
对比旧采样时应将旧版这两个区间合计。`Raynium.Visible` 仍记录 ABI 面组项数，`RadRay.Scene.VisibleBatches` 和 `Raynium.DrawCommands` 记录合并后的输入与实际可见命令数。
锁定 RadRay 自带 `RadRay.FrameScheduler`、`FrameSlot`、`FramePipeline`、`Scene`、`DrawList`、`MeshDraw` 与 `RDG` 区间，
包含缓存同步、槽回收、排序、图依赖/裁剪/调度、资源池命中或创建、屏障、附件及逐 Pass 录制；不需要修改上游源码。
这些 CPU 父区间包含子区间，不能直接求和；RDG 录制耗时和后端 GPU 时间线分别读取。
Tracy frame mark 在一次 native Render 调用结束时发出；间隔包含两次调用之间的宿主工作，不等同于 native CPU 用时。
客户端是 on-demand，因此 native 库加载前、采集器连接前的工作不在 Tracy 中；它也不提供 Java 调用栈。

`-PjfrFile=<路径>` 在游戏 JVM 启动时启用 `profile` JFR，正常退出时保存，磁盘缓冲上限 1 GiB；超出上限会淘汰较早数据。
JFR 包含 JVM 执行采样、GC 等事件；`dev.raynium.ClientFrame` 记录每次正常返回的 `Minecraft.renderFrame` 耗时及是否加载世界。
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
| `NativeRuntimeBackendTest` | 后端配置解析和非法值 | `test` |
| `NativeManifestTest` | Debug/MTd 与 Release/MT 单 DLL 清单；CRT/配置错配、MD/MDd、schema 2、缺失/多余项、非法路径和哈希拒绝 | `test` |
| `PreviewRegionTest` | 显示矩形、MC Y 翻转后的复制位置和小窗口边界 | `test` |
| `NativeAbiTest` | 参数校验、输出清空、诊断截断和空句柄销毁 | `nativeTest` |
| `NativeRuntimeTest` | 双后端环境/设备创建、重建、线程限制、幂等关闭、单例保护及早期 factory 共享绘制 | `nativeTest` |
| `MeshPayloadTest` / `TextureTransferTest` | 保守面组、越界模型、顶点拥有权和失败降级 | `test` |
| `NativeTerrainTest` | 双后端/两传输模式的 mip、深度、大坐标、epoch、重建、双槽在途参数/资源隔离、驻留字典增长、多材质/多 submesh 缓存、索引扩容与局部增删改、多几何 arena 块与 base vertex、staging 增长后复用、删除后区间重用、空可见帧上传后跨槽及尺寸往返绘制 | `nativeTest` |
| `NativeRendererTest` | 双后端雾色清屏/区域外像素、连续交接、输出重建保留场景、错误线程、超时保留 owner 和导入失败清理 | `nativeTest` |

`test` 排除 `native` 标签，随标准 `check/build` 运行，不加载 DLL、不构建 native。`nativeTest` 是标准 Gradle `Test` 任务，只包含 `native` 标签；它复用 `testClasses`，依赖 `nativeStage`，在构建/stage 完成后启动测试 JVM。依赖仍须显式准备，执行需要 Windows x64 和 D3D12/Vulkan 驱动；缺少设备或 DLL 时明确失败，不跳过。

通用 runtime 测试已迁入 RadRay 的 `modules/runtime/tests/`，由 `test_radray_runtime` / GTest 注册，
直接链接 `RadRay::runtime`，不包含 MC bridge。在已准备依赖并完成 configure 的 RadRay checkout 执行：

```powershell
cmake --build --preset win-x64-debug --target test_radray_runtime --parallel 8
ctest --preset win-x64-debug -R 'FrameLoop|MeshDrawingPipeline|Material' --output-on-failure
```

RadRay standalone 的测试依赖需在该 checkout 显式准备，包括 GTest；当前 runtime 用例不需要编译 shader fixture。
`nativeTest` 只运行 Raynium 的适配/共享交接回归；不再隐式构建上游 GTest。

native 测试 JVM 使用 Java 25、`--enable-native-access=ALL-UNNAMED` 与指定配置的 stage 路径。仅一个 worker，禁用 JUnit 并行；每个 runtime 的创建和销毁在同一线程，错误线程用例从额外线程调用预期被拒绝的创建、渲染或关闭。GPU/驱动状态不属于普通 Gradle 输入，因此 `nativeTest` 每次显式执行，不使用 up-to-date 或构建缓存结果。

JUnit `@TempDir` 管理每个用例的 DLL 缓存目录。测试 fixture 和 try-with-resources 保证断言失败时也先释放 native runtime 和环境，再卸载 DLL，最后清理临时目录。用例无顺序依赖，测试观测与句柄跟踪仅存在于测试源码，不加入生产对象。

预览测试在三种尺寸上各交接 8 帧，使用测试专属读回 buffer 校验每个像素；生产路径无读回。
消费者启用 Khronos validation 和同步验证，依赖本机安装的 Vulkan SDK/layer；缺失时测试失败。
原生后端诊断日志与 JUnit 结果一起检查，游戏内生命周期仍须单独运行。

`-PtestImages=<绝对目录>` 可保存 NativeTerrainTest 已有固定像素场景的 PNG；`-PtestBaseline=<绝对目录>` 对对应图片逐像素比较，缺失基线也视为失败。
文件名包含 backend、测试名、尺寸与序号；direct/split 输入路径预期相同，因此共用基线。新双槽隔离用例使用独立的逐像素预期，不依赖这些基线文件。
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
