> - 适用: 实施 Raynium 构建参数收敛、客户端配置系统、测试驱动模块拆分、Java 分层命名与 Native C++ 工程规范整理时的设计与执行参考
> - 权威: 历史架构设计与分步实施计划，按 2026-11-27 记录时状态理解，不构成当前技术契约；当前实现见 `doc/architecture/overview.md` 与 `doc/guide/build-test.md`
> - 锚点: `build.gradle`, `gradle/native.gradle`, `settings.gradle`, `gradle.properties`, `src/client/java/dev/raynium/bridge/*.java`, `src/client/java/dev/raynium/client/**/*.java`, `native/CMakeLists.txt`, `native/include/raynium/*.h`, `native/src/*.cpp`, `tools/benchmark.py`, `tools/validate_visual.py`, `tools/native.py`

# 工程规范统一、构建解耦与测试驱动模块拆分实施计划

- **记录日期**：2026-11-27
- **状态**：经两轮 `/grill-me` 逐项核查源码并由用户确认定稿的完整实施计划，归档时尚未实施
- **范围**：Gradle 构建与本机环境参数收敛、`<gameDir>/config/raynium.json` 纯 JSON 客户端配置与构造注入、`src/harness` 物理隔离与 `launch-spec.json` 直连启动、`dev.raynium.diagnostics` 零分配观测解耦、Java 包单向分层与命名统一、Native C++ 全面对齐上游 `RadRay` 规范、根 `.clang-format` 与全仓格式化刷写，以及配套测试与长期文档同步；不修改 C ABI（保持 ABI 27 布局与导出符号）与上游 `RadRay`
- **来源**：外部会话初稿复盘 + 2026-11-27 当前工作树全量源码、Loom 1.19 缓存配置、Python 脚本与 `RadRay` 规范核查，以及用户逐项拍板决定

---

## 一、背景、源码核查补漏与用户确认决策

### 1.1 核心痛点归因

1. **Gradle 承担过多跨生命周期职责**：`build.gradle` 与 `gradle/native.gradle` 混杂了 17 个扁平 `-P` 参数（构建变体、本机工具路径、客户端渲染开关、测试驱动路径、JFR/Tracy 采集参数），破坏 Configuration Cache 并使同一配置拥有 Gradle `-P`、JVM `-D`、脚本 JSON 三套名称。
2. **自动化测试脚本套壳 Gradle 启动游戏**：`tools/benchmark.py` 与 `tools/validate_visual.py` 编译后再二次（乃至三次）拉起 Gradle 跑 `runClientNative` / `runClientReference`，迫使 `loom.runs` 塞入近 80 行专属于测试驱动的条件分支。
3. **测试驱动进入正式分发包且反向污染底层**：`BenchmarkSession`、`VisualValidation`、`ValidationGalleryScreen` 及 18 个测试专用 Mixin 全部位于 `src/client`，随 `packageNativeMod` 打入发布 JAR；底层 `bridge` 与 `renderpearl` 直接静态调用 `BenchmarkSession` 和 `VisualValidation`。
4. **全局属性散落与跨包环依赖**：8 处业务与测试代码直接读取 `System.getProperty` / `Boolean.getBoolean`；同时 `dev.raynium.bridge` 中的 `NativeBuffer` 反向引用 `client.renderpearl.CommandTape`，`NativeFrameBuffer` 反向引用 `client.terrain.MeshPayload`，形成隐式跨包双向耦合。
5. **Java 与 Native C++ 代码风格不一致**：Java 混用紧凑无空格语法、通配符 `import *`、行内全限定名及扁平的 43 个 Mixin（混用 `*Access` 与 `*Mixin`）；Native `namespace raynium` 混用三种字段命名、`struct`/`class` 边界不清、21 个 `.cpp` 使用 `using namespace radray::gpu` 或 `radray::shader`，且部分头/源文件未严格 1:1 配对。

### 1.2 本次源码审计补齐的关键遗漏点与用户确认决策

相较外部初稿，本次结合源码核实并由用户确认修正以下关键细节：

| 决策维度 | 源码核查发现的边界/隐患 | 用户最终确认方案 |
| --- | --- | --- |
| `harness` 物理隔离与 `RenderMode.REFERENCE` | ① `VanillaPipelineCatalog` 与 `PipelineIdentity` 在正式渲染零调用，仅供 `VisualValidation`/测试/导出工具使用；② `raynium.accesswidener` 前 3 行（`SpriteContents` 动画帧）仅供 `VisualValidationSpriteMixin` 使用；③ `BenchmarkSession` 直接持有 `NativeRenderer` 与 `NativeBuffer.WriteSource` 字段，纯原版未加载 `raynium` 时会触发 `NoClassDefFoundError` | 彻底删除 `RenderMode.REFERENCE` 与置空的 `RayniumClient`；`VanillaPipelineCatalog`、`PipelineIdentity` 及专属 3 条 AW 迁入 `src/harness`；`harness` 通过可选桥接类按 `FabricLoader.isModLoaded("raynium")` 隔离核心类型引用 |
| `loom.runs` 日常入口 | 去掉测试驱动分支后，需明确日常 IDE/CLI 运行项集合及 Release 手动验证入口 | 保留纯核心 `runClient`、`runClientNative`（Debug）、`runClientNativeRelease`（Release）以及仅挂载 `raynium-harness` 的纯原版 Vulkan `runClientReference` |
| `bridge` 拆环与 `diagnostics` 钩子 | `NativeBuffer` 直接实现 RenderPearl `GpuBuffer`（刻意零包装设计），但反向引用了 `CommandTape`；`NativeFrameBuffer` 反向引用了 `MeshPayload`；此外 `RayniumCommandEncoderBackend` 与 `TerrainAnchorMixin` 也静态调用了测试驱动 | 顶层建 `dev.raynium.diagnostics`（仅依赖 JDK/JFR，收拢 JFR 事件与轻量观测钩子）；`bridge` 保留直接实现 `GpuBuffer`/`GpuFormat` 零额外分配，通过内部函数式/Pin 接口切掉对 `client.*` 与 `net.minecraft.*` 的全部依赖 |
| `RayniumConfig` 实现方式 | 初稿曾提议接入 Mojang `RecordCodecBuilder`；且前后对 `shaderToolsDir` 归属及非法配置容错表述不一致 | **不使用任何 Mojang Codec**，直接基于 Gson 解析 `<gameDir>/config/raynium.json`（仅含 `backend`、`rhiValidation`、`terrainSubmission`；缺文件生成默认、缺字段填默认、未知/非法值或坏 JSON 直接 fail-fast）；开发/测试路径参数统一走全小写点分 `-Draynium.*` 单次内存覆盖且不回写文件 |
| `local.properties` 与代理机制 | 原生 Gradle 不会自动读取 `local.properties`；且 `tools/fetch_radray.py` 第 67 行同样硬编码了 `127.0.0.1:10808` | 从受管 `gradle.properties` 与 `fetch_radray.py` 移除硬编码 `127.0.0.1:10808`；在 `settings.gradle` 的 `pluginManagement` 阶段显式加载未跟踪的 `local.properties`（支持代理与本机路径/并发，附 `local.properties.example`）；`fetch_radray.py` 按 `--proxy` > `local.properties` > 环境变量读取代理，未配置则直连 |
| Native 任务矩阵与发布门禁 | `tools/native.py` 的 `package_mod` / `verify_package` 此前未校验 `profilerEnabled == false`，存在误打入 Tracy 插桩产物的风险 | 取消 `-PnativeConfig`，提供 `Debug`/`Release` 对称任务对与独立测试报告目录，仅保留 `-Praynium.build.profiler=true`；新增 `writeLaunchSpec`；在 `package_mod` 与 `verify_package` 中强制拒绝 `profilerEnabled=true` 的快照 |
| Native C++ 对齐 `RadRay` | ① `bridge.h` 声明 `typedef struct rn_*`，直接改 `class` 在 MSVC `/W4` 下触发 `C4099`；② 21 个 `.cpp` 含 `using namespace radray::gpu`/`shader`；③ 5 处头/源文件非严格 1:1 或含非模板内联 | `rn_*` 保持纯数据薄 `struct`，方法外提为内部类或自由函数；全面删除 `using namespace radray::gpu`/`shader`，统一显式写 `radray::gpu::` 与 `radray::shader::`；补齐 `device_info.h` 等实现严格 1:1 配对（纯模板头除外） |
| 代码格式化与规范门禁 | 初稿未定 Java 列宽、存量代码刷写范围，且 `tools/validation/pipeline_coverage.py` 硬编码了旧 Mixin 路径和已改名的 `model_mesh_registry.cpp` | 只配根 `.clang-format`（Cpp 与 Java 均 4 空格、`ColumnLimit: 0`）与 `doc/guide/conventions.md`，暂不加静态门禁脚本；修正 `pipeline_coverage.py` 路径；展开全部 Java 通配符 `import *` 与冗余行内全限定名，并在收尾阶段对全仓 `src/` 与 `native/` 执行一次统一格式化 |

---

## 二、目标架构设计

### 2.1 模块与源码集物理隔离

仓库划分为三个生产/验证相关的源码集与一个单元测试源码集：

1. **`main` + `client`（正式模组 `raynium`）**
   - `src/main/resources/fabric.mod.json`：`id` 为 `"raynium"`，移除已置空的 `entrypoints.client`，保留 `raynium.accesswidener` 与 `raynium.client.mixins.json`。
   - `src/client/resources/raynium.accesswidener`：删除前 3 行仅供视觉验证使用的 `SpriteContents` 条目，仅保留核心渲染与快轨所需的访问拓宽。
   - `src/client/resources/raynium.client.mixins.json`：移除 `plugin`（所有核心 Mixin 无条件生效，彻底删除 `RayniumMixinPlugin` 与 `RenderMode`）。
   - `packageNativeMod` 仅打包 `jar`（即 `main` + `client`）与无 profiler 的 Release DLL，确保分发包零测试驱动类、零测试 Mixin、零测试 AW 条目。

2. **`harness`（独立开发测试驱动模组 `raynium-harness`，不进发布包）**
   - 目录：`src/harness/java` 与 `src/harness/resources`。
   - 依赖关系：`harnessCompileOnly` 依赖 `sourceSets.client.output`（编译期可见核心接口，运行期不把 `raynium` 捆绑进 `sourceSets.harness.runtimeClasspath`），`harnessImplementation` 继承 Minecraft 与 Fabric 依赖。
   - 资源清单：
     - `src/harness/resources/fabric.mod.json`：`id` 为 `"raynium-harness"`，`accessWidener` 指向 `"raynium-harness.accesswidener"`，`entrypoints.client` 指向 `dev.raynium.harness.HarnessClient`，`mixins` 指向 `"raynium-harness.mixins.json"`。
     - `src/harness/resources/raynium-harness.accesswidener`：承载 `SpriteContents$AnimatedTexture`、`SpriteContents$FrameInfo` 与 `frames` 三条拓宽。
     - 编译期 Loom AW 合成：因 Loom 单工程 `loom.accessWidenerPath` 仅接收单个文件，在 `build.gradle` 配置阶段将 `raynium.accesswidener` 与 `raynium-harness.accesswidener` 按内容摘要幂等合并至 `build/loom/combined.accesswidener` 供编译期拓宽，而打包与运行期各自读取独立资源文件。
   - 按需核心桥接：在 `dev.raynium.harness.RayniumHarnessBridge` 内集中封装对 `dev.raynium.diagnostics.*`、`RayniumGpuDevice`、`TerrainCapture`、`NativeBuffer.WriteSource` 的访问；仅当 `FabricLoader.getInstance().isModLoaded("raynium")` 为 `true` 时加载并挂载遥测/管线回调。纯原版 Reference 运行时只加载 `raynium-harness`，完全不触碰 `dev.raynium.*` 类，彻底规避 `NoClassDefFoundError`。

3. **`test`（JUnit 与离线导出工具）**
   - `testImplementation` 同时包含 `sourceSets.client.output` 与 `sourceSets.harness.output`，使 `Benchmark*Test`、`VanillaPipelineCatalogTest`、`NativeRenderPearlTest` 与 `ExportVanillaPipelines` 无缝编译运行。

### 2.2 `dev.raynium.diagnostics` 观测层与单向包分层

Java 核心层确立严格无环的单向包拓扑：

```text
dev.raynium.diagnostics
        ↑
dev.raynium.bridge
        ↑
dev.raynium.client.config
        ↑
dev.raynium.client.{renderpearl, terrain, world}
        ↑
dev.raynium.client.mixin.{backend, terrain, world}
        ↑ (compileOnly + runtime optional via isModLoaded)
dev.raynium.harness.{benchmark, validation, catalog, mixin.benchmark, mixin.validation}
```

1. **`dev.raynium.diagnostics`（零 MC / 零 `bridge` / 零 `client` 依赖）**
   - `ClientFrameEvent`（自 `dev.raynium.client.diagnostics` 迁入，保持 `@Name("dev.raynium.ClientFrame")` 不变）。
   - `TerrainFrameEvent`（自 `dev.raynium.bridge` 迁入，保持 `@Name("dev.raynium.TerrainFrame")` 不变）。
   - `RayniumTelemetry`：提供无分配静态挂载点（默认 `volatile TelemetryListener listener = null`，未挂载时仅一次空判断）：
     - `boolean isMeasuring()`
     - `void onMappedWrites(int writeSourceOrdinal, long bytes)`
     - `void onPresentedFrames(long count)`
     - `long syncWaitStart()` / `void syncWaitEnd(long startedNs)`
     - `void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs)`
     - `void onPipelineSelected(Object renderPipeline)`（供 `TerrainAnchorMixin` 在地形录制点上报实际选用的 `RenderPipeline`，参数用 `Object` 以维持 `diagnostics` 零 RenderPearl 依赖，`harness` 侧强转回 `RenderPipeline`）

2. **`dev.raynium.bridge` 零包装拆环与类型收拢**
   - `NativeBuffer`：保留 `implements GpuBuffer`，将构造参数 `Supplier<CommandTape> batch` 替换为 `bridge` 内部定义的函数式接口 `@FunctionalInterface public interface StagingWriter { void writeBuffer(NativeBuffer target, long offset, MemorySegment source); }`（由 `RayniumCommandEncoderBackend` 传入 `(target, offset, src) -> tape().writeBuffer(target, offset, src)`），消除对 `CommandTape` 的反向 import。
   - `NativeFrameBuffer`：移除对 `MeshPayload` 和 `RetainedTerrain.ViewAccess` 的 import；构造器改为 `NativeFrameBuffer(boolean pooledTerrain)`，新增轻量 `@FunctionalInterface public interface PinnedPayload { void unpin(); }`，将 `indexPooled` / `meshPooled` 改为接收 `MemorySegment`、布局数组与 `PinnedPayload` 引用（`MeshPayload` 与 `MeshPayload.IndexSnapshot` 直接 `implements NativeFrameBuffer.PinnedPayload`，零额外对象分配）。
   - `NativeLibraryLocator`：保留按 `Path` 或资源流定位（可将 `ModContainer` 查找收敛为 `UnaryOperator<Path>` 或 `Function<String, Path>`，或仅依赖 `fabricloader` API，严禁依赖 `dev.raynium.client.*` 与 `net.minecraft.*`）。
   - `TerrainSubmission`：收拢为 `NativeRenderer.SubmissionMode` 枚举（对齐 `NativeEnvironment.Backend`），保持顶层公开类全部以 `Native*` 命名。

3. **Java 核心与 Harness 类归位及 Mixin 命名规则**
   - `BuiltinShaderPack` 从 `dev.raynium.client.terrain` 迁入 `dev.raynium.client.renderpearl`。
   - `VanillaPipelineCatalog` 与 `PipelineIdentity` 从 `dev.raynium.client.renderpearl` 迁入 `dev.raynium.harness.catalog`（同步更新 `VisualValidation`、`NativeRenderPearlTest`、`VanillaPipelineCatalogTest`、`ExportVanillaPipelines` 引用）。
   - 原 `MinecraftBenchmarkMixin` 拆分为三处单一职责 Mixin：
     - 核心 `dev.raynium.client.mixin.backend.MinecraftFrameMixin`：仅负责 `TerrainCapture.beginFrame()` 与 `ClientFrameEvent` JFR 采样；
     - Harness `dev.raynium.harness.mixin.benchmark.BenchmarkMinecraftMixin`：负责原版调试计时开关打开、`BenchmarkSession.beforeFrame/afterFrame` 与纯原版帧计数；
     - Harness `dev.raynium.harness.mixin.validation.VisualValidationMinecraftMixin`：负责 `VisualValidation.beforeFrame/afterFrame`。
   - Mixin 子包划分与后缀统一：
     - 核心 `dev.raynium.client.mixin` 下拆分为：
       - `backend`：`RayniumBackendMixin`、`MinecraftFrameMixin`、`MappableRingBufferMixin`、`StagedBufferPoolMixin`
       - `terrain`：`CompiledSectionMeshMixin`、`LevelExtractorMixin`、`LevelTerrainHookMixin`、`RenderSectionIndicesMixin`、`RenderSectionStateMixin`、`SectionCompilerMixin`、`SectionOcclusionGraphMixin`、`SectionResultsMixin`、`SectionUploadMixin`、`TerrainAnchorMixin`、`TerrainProjectionMixin`、`UberBufferDrainMixin`
       - `world`：`LevelWorldFeatureScopeMixin`、`ModelFeatureRendererMixin`、`ModelManagerReloadMixin`、`ModelPartCompileMixin`、`PoseStackPoseMixin`、`QuadParticleFeatureMixin`、`QuadParticleRenderStateMixin`、`RenderTypeFeatureRendererMixin`、`RenderTypeGroupMixin`、`ShadowFeatureRendererMixin`
     - Harness `dev.raynium.harness.mixin`（配 `HarnessMixinPlugin`）下拆分为：
       - `benchmark`：`BenchmarkMinecraftMixin`、`BenchmarkReferenceTimerMixin`（仅当 `raynium.benchmark.config != null` 时启用）
       - `validation`：`VisualValidationMinecraftMixin`、其余 11 个 `VisualValidation*Mixin`，以及将原 5 个 `*Access` 接口统一重命名为标准 `*Accessor`：`VisualValidationGatewayAccessor`、`VisualValidationPanoramaAccessor`、`VisualValidationSpellAccessor`、`VisualValidationTitleAccessor`、`VisualValidationUseItemAccessor`（仅当 `raynium.validation.config != null` 时启用）。
   - 注入成员命名：所有 Mixin 新增字段/方法统一 `raynium$camelCase`（Harness 内可使用 `rayniumHarness$camelCase` 以避免与核心同目标类 `Minecraft` 上的注入方法重名冲突！）。

### 2.3 客户端配置系统 `RayniumConfig` 与构造注入

1. **配置文件 `<gameDir>/config/raynium.json`（纯 Gson 实现，零 Mojang Codec）**
   - 位置：`dev.raynium.client.config.RayniumConfig`（不可变 `record`）。
   - 持久化字段（仅此 3 项）：
     ```json
     {
       "backend": "d3d12",
       "rhiValidation": false,
       "terrainSubmission": "auto"
     }
     ```
   - 解析与写盘契约：
     - 文件不存在时：自动创建父目录并写出上述默认格式化的 `config/raynium.json`。
     - 文件存在时：用 `JsonParser` 严格解析 JSON 对象；缺省字段回退默认值；**出现未知字段名、字段类型不匹配、枚举值非法（如拼错 `"vulkn"`）或损坏 JSON 时直接抛出异常（fail-fast）**，严禁静默回退默认后端。

2. **统一全小写点分 `-Draynium.*` 系统属性（单次内存覆盖，不回写文件）**
   - 用户/开发运行期覆盖属性（仅在 `RayniumConfig.load(Path gameDir)` 与 `NativeLibraryLocator` 入口读取，业务类一律构造传参）：
     - `-Draynium.rhi.backend=d3d12|vulkan`
     - `-Draynium.rhi.validation=true|false`
     - `-Draynium.terrain.submission=auto|direct|indirect`
     - `-Draynium.shader.tools-dir=<path>`（替代原驼峰 `raynium.shaderToolsDir`，封装进 `RayniumConfig` 的 `Optional<Path> shaderToolsDir` 字段，经 `RayniumGpuBackend` → `RayniumGpuDevice` → `RayniumPrograms` → `ShaderProgramCache` 构造函数传递）
     - `-Draynium.native.dir=<path>`（仅 `NativeLibraryLocator` 与 `NativeTestSupport` 读取）
   - 测试与 Harness 内部属性：
     - `-Draynium.test.surface-layer=<path>`（替代原 `raynium.test.surfaceLayer`）
     - `-Draynium.benchmark.config=<path>`
     - `-Draynium.validation.config=<path>`
   - JUnit 与 Native 测试注入：`NativeEnvironment`、`NativeRuntime`、`ShaderProgramCache` 等移除无参读取 `System.getProperty`/`Boolean.getBoolean` 的便利重载或将其收敛至 `RayniumConfig`；`NativeRenderPearlTest`、`TerrainSubmissionTest` 等测试直接通过构造函数传入显式参数（不再手工 `System.setProperty` 改全局状态）。`nativeTest*` 若需从命令行全局开启驱动校验，仅透传 `-Draynium.rhi.validation=true` 供 `NativeTestSupport` 读取。

### 2.4 Gradle 构建配置、`local.properties` 与 `launch-spec.json` 直连启动

1. **本机环境配置 `local.properties`（Git 忽略，提供 `local.properties.example`）**
   - 从 `gradle.properties` 移除硬编码的 `systemProp.http.proxyHost/Port` 与 `https.proxyHost/Port`。
   - `.gitignore` 新增 `/local.properties`；仓库根新增受跟踪的 `local.properties.example` 说明支持项：
     - `proxy=http://127.0.0.1:10808`（可选；解析 host/port，未填则不设代理）
     - `python=python`
     - `parallel=8`
     - `radraySourceDir=`（可选本地 RadRay 覆盖路径）
     - `gameRunDir=run`
   - 在 `settings.gradle` 的 `pluginManagement` 块首部读取 `local.properties`：若配置了 `proxy` 且当前 JVM 未显式传入 `-Dhttp.proxyHost`，设置 `http/https.proxyHost` 与 `proxyPort`；其余键值暴露给根工程（命令行 `-P` 若显式传入同名覆盖或 `~/.gradle/gradle.properties` 仍可按优先级工作，但日常无需任何本机 `-P`）。
   - `tools/fetch_radray.py`：移除 `--proxy` 的 `"http://127.0.0.1:10808"` 硬编码默认值；按 `--proxy` 命令行参数 > 仓库根 `local.properties` 的 `proxy` > 环境变量 `HTTPS_PROXY`/`HTTP_PROXY`/`ALL_PROXY` 解析代理；均未配置时直接无代理执行 `git`（保留仓库隔离环境变量清理逻辑）。

2. **清理后的 `build.gradle` 与 `gradle/native.gradle`**
   - **彻底删除的 16 个旧 `-P` 参数**：`rhiBackend`、`rhiValidation`、`terrainSubmission`、`quickPlayWorld`、`gameUsername`、`captureTracyImages`、`benchmarkConfig`、`benchmarkRunDir`、`validationConfig`、`validationReadOnlyRegions`、`jfrFile`、`nativeConfig`、`nativeProfiler`、`nativePython`、`nativeParallel`、`radraySourceDir`（后四者与 `gameRunDir` 归入 `local.properties`）。
   - **唯一保留的构建变体 CLI 属性**：`-Praynium.build.profiler=true`（默认 `false`）。
   - **`loom.runs` 精简为 4 个纯净开发配置**（均通过 `providers.systemPropertiesPrefixedBy("raynium.")` 支持命令行临时透传 `-Draynium.*`，通过原生 `--args="..."` 支持临时传 `--quickPlaySingleplayer` 或 `--tracy`）：
     - `client`：挂载 `raynium`，`--gameDir <gameRunDir>`，`--enable-native-access=ALL-UNNAMED`。
     - `clientNative`：继承 `client`，注入 `raynium.native.dir` 指向 `Debug` stage 目录，依赖 `nativeStageDebug`，生成 IDEA 运行配置。
     - `clientNativeRelease`：继承 `client`，注入 `raynium.native.dir` 指向 `Release` stage 目录，依赖 `nativeStageRelease`。
     - `clientReference`：仅挂载 `raynium-harness`（`source(sourceSets.harness)`，且在任务配置中确保 `fabric.classPathGroups` 与 `classpath` 不含 `main`/`client`），传入 `--graphicsBackend VULKAN`。
   - **对称 Native 任务矩阵与打包门禁**：
     - 构建链：`nativeConfigureDebug` → `nativeBuildDebug` → `nativeStageDebug`，以及 `nativeConfigureRelease` → `nativeBuildRelease` → `nativeStageRelease`；`nativeConfigure` / `nativeBuild` / `nativeStage` 作为固定指向 `Debug` 的轻量生命周期别名。
     - 测试链：`buildVulkanSurfaceTestLayer`（测试层本身固定 `/MT` 独立输出至 `build/native/vulkan-surface-test/`），`vulkanSurfaceTestDebug` / `vulkanSurfaceTestRelease`，`nativeTestDebug` / `nativeTestRelease`（各自独立设置 `reports.html.outputLocation`、`reports.junitXml.outputLocation` 与 `binaryResultsDirectory` 至 `.../nativeTestDebug` 与 `.../nativeTestRelease`，彻底解决报告互相覆盖！）；`vulkanSurfaceTest` 与 `nativeTest` 默认关联 `Debug`，并通过 `providers.systemPropertiesPrefixedBy("raynium.")` 透传临时 `-Draynium.rhi.validation=true`。
     - 发布包门禁：`tools/native.py` 的 `package_mod` 与 `verify_package` 增加 `manifest.get('profilerEnabled') is not False` 检查，强制拒绝将开启 profiler 的快照打入正式发布 JAR。

3. **`writeLaunchSpec` 任务与 Python 自动化直连启动**
   - 在 `gradle/native.gradle` 注册 `writeLaunchSpec` 任务，输出 `build/native/launch-spec.json`，包含：
     - `javaExecutable`：Java 25 Toolchain 的 `java` 可执行文件绝对路径；
     - `mainClass`：Loom 开发启动主类（`net.fabricmc.loader.impl.launch.knot.KnotClient`）；
     - `commonJvmArgs` / `commonSystemProperties`：`--enable-native-access=ALL-UNNAMED`、`fabric.development=true`、`log4j.configurationFile`、`fabric.defaultModDistributionNamespace=official`、`fabric.defaultMixinRemapType=static`、`fabric.gameJarPath`、`fabric.gameJarPath.client` 等 Loom 基础运行参数；
     - `commonProgramArgs`：`--assetIndex`、`--assetsDir`、`--version` 等基础游戏参数；
     - `baseClasspath`：Minecraft 客户端及依赖库类路径（剔除本仓库 `main`/`client`/`harness` 输出目录）；
     - `mods`：`{ "raynium": [...main与client的classes/resources目录...], "rayniumHarness": [...harness的classes/resources目录...] }`；
     - `nativeStageDirs`：`{ "Debug": ".../Debug", "Release": ".../Release" }`。
   - `tools/benchmark.py` 改造：
     - 构建阶段只调一次 Gradle：`harnessClasses` +（若 `renderer == 'raynium'` 则加 `clientClasses`、`nativeStageRelease`）+ `writeLaunchSpec`（若 `profilerEnabled` 则传 `-Praynium.build.profiler=true`）。
     - `prepare_run` 在 `<session>/game/config/raynium.json` 直接写出 `{ "backend": ..., "rhiValidation": false, "terrainSubmission": ... }`。
     - 启动阶段读取 `launch-spec.json` 直接 `subprocess.Popen([javaExecutable, ...])`：
       - `vanilla` 模式：仅挂载 `mods["rayniumHarness"]`（`classpath = baseClasspath + rayniumHarness`，`-Dfabric.classPathGroups=...`），程序参数传 `--graphicsBackend VULKAN`；
       - `raynium` 模式：挂载 `raynium` + `rayniumHarness`，注入 `-Draynium.native.dir=<Release stage>`；
       - 统一内聚拼装 `-da`、`-Dorg.lwjgl.util.NoChecks=true`、`-Dorg.lwjgl.util.NoFunctionChecks=true`、`-Draynium.benchmark.config=...`、可选 `-XX:StartFlightRecording=...`、`--gameDir`、`--username RayniumTest`、`--quickPlaySingleplayer benchmark`。
   - `tools/validate_visual.py` 改造：
     - 在 `for mode in args.modes` 循环前一次性调用 Gradle 完成所需构建与 `writeLaunchSpec`（包含 `harnessClasses`；若含 `d3d12`/`vulkan` 则加 `clientClasses` 与对应 `nativeStageDebug` 或 `nativeStageRelease`）。
     - 每个 `mode` 会话在 `<game>/config/raynium.json` 写出对应 `backend` 与 `rhiValidation`，直接拉起 `javaExecutable`（`--in-place-world` 时由 Python 直接附加 `-DMC_DEBUG_ENABLED=true -DMC_DEBUG_DONT_SAVE_WORLD=true`，`--tracy-images` 时直接附加 `--tracy`）。

### 2.5 Native C++ 规范全面对齐 `RadRay`

1. **边界原则**
   - `native/include/raynium/bridge.h` 作为 C ABI 边界，完整保留 `rn_*` 类型/函数/结构体蛇形字段与 `RN_*` 枚举常量，ABI 版本（27）与跨语言二进制布局零变更。
   - C ABI 不透明句柄的实现结构体（`struct rn_env`、`struct rn_runtime`、`struct rn_renderer`、`struct rn_buffer`、`struct rn_texture`、`struct rn_pipeline`、`struct rn_query_pool`）保持纯数据薄 `struct` 定义，避免与 `bridge.h` 中的 `typedef struct rn_*` 产生 MSVC `/W4` `C4099` 告警；原 `rn_buffer` 上的 `IsHostVisible`、`IsReadback`、`Contains`、`Import` 等成员函数外提为内部辅助函数或收归 `raynium` 内部类。

2. **`namespace raynium` 命名、类型与文件组织规范**
   - **彻底移除 `using namespace`**：删除全部 21 个 `.cpp`（含 `native/src/`、`native/tests/`、`native/tools/`）中的 `using namespace radray::gpu;` 与 `using namespace radray::shader;`，统一显式书写 `radray::gpu::` 与 `radray::shader::`。
   - **标识符命名对齐 `RadRay`**：
     - 类型（`class` / `struct` / `enum`）与函数/方法：`PascalCase`；
     - 公开数据成员（含 `FrameExecution`、`ShaderResourceBindings`、`wire::*` 协议头、`ResourceHandle`、`QuerySlot`、`TerrainView` 等纯数据记录字段）：统一改为 `PascalCase`（保持 `wire::*` 字段顺序与类型不变，确保 `static_assert(sizeof(...))` 完全通过）；
     - `class` 私有成员变量：`_camelCase`；
     - `constexpr` 编译期常量：`kPascalCase`（如 `kInline` 等）。
   - **`struct` 与 `class` 严格区分**：
     - 仅纯数据聚合记录使用 `struct`（如 `wire::Pass`/`Draw`/`Geometry`/`Terrain` 将校验方法改为自由 `constexpr bool ValidWireHeader(...)`，使其回归纯 POD `struct`）；
     - 凡含显式构造函数、析构函数或成员函数的类型（如 `HostReadStorage`、`QueryPoolStorage`、`ShaderResourceBindings`、`TerrainView`、`TerrainFeature`、`ImmediatePipeline` 等）一律改为 `class`（公开数据成员如需直接访问可在 `public:` 段使用 `PascalCase`）。
   - **`.h` / `.cpp` 同名 1:1 配对与非模板函数外提**：
     - 新增 `native/include/raynium/device_info.h` 与 `native/src/device_info.cpp` 配对（从 `render_resources.h` 迁出 `QueryDeviceInfo`）；
     - 新增 `native/src/exception.cpp` 与 `exception.h` 配对（外提 `AbortUnexpectedException`）；
     - 新增 `native/src/resource_handles.cpp` 与 `resource_handles.h` 配对（外提非模板构造与 `rn_buffer` 相关辅助函数，模板 `ResourceHandle` / `HandleList` / `ResolveHandle` 留在头中）；
     - `frame_plan.h`：将非模板 `FramePushConstants` 迁入唯一消费侧或配对 `.cpp`，使 `frame_plan.h` 成为纯模板头；
     - `terrain_feature.h`：外提 `SubmitPreparedTerrainDirectDraws` 至 `terrain_feature.cpp`（并消除头文件函数内 `static const` 对象，改为栈上局部默认构造 `const radray::gpu::GpuDeviceCapabilities direct{};` 以符合 `RadRay` 避免共享静态对象的规则）；
     - 排查其余 26 对 `.h`/`.cpp`，将非模板、非 `constexpr`、非 `= default` 的类内简短方法/访问器外提至同名 `.cpp`，补齐头文件自包含 `#include`，并在 `native/CMakeLists.txt` 的 `rayniumruntime` 与 `test_raynium_terrain` 目标中完整同步源文件与头文件列表。

### 2.6 代码格式化 `.clang-format` 与规范文档

1. **根目录 `.clang-format`**
   - `Language: Cpp` 段：完整对齐 `external/RadRay/.clang-format`（4 空格缩进、`UseTab: Never`、`ColumnLimit: 0`、`BreakBeforeBraces: Attach`、`PointerAlignment: Left`、`AccessModifierOffset: -4`、`SortIncludes: false`、`AlwaysBreakTemplateDeclarations: Yes`）。
   - `Language: Java` 段：4 空格缩进、`UseTab: Never`、`ColumnLimit: 0`、`BreakBeforeBraces: Attach`、`BreakAfterJavaFieldAnnotations: false`、`SpaceBeforeParens: ControlStatements`、`SpacesInParentheses: false`、`SortIncludes: true`、`SortJavaStaticImport: Before`。
2. **Java 代码卫生清理**
   - 清理 `src/`（含 `client`、`harness`、`test`）中所有通配符 `import ...*;`（包括 `import static java.lang.foreign.ValueLayout.*;` 等统一按清晰显式导入展开），并消除方法体内无歧义的冗余全限定类名前缀。
3. **长期规范文档 `doc/guide/conventions.md`**
   - 在代码落地阶段新增 `doc/guide/conventions.md`（带标准三行头部），集中记录已生效的仓库模块分层、配置与命令行属性命名、Java 包与类/Mixin 命名规则、Native C++ 对齐 `RadRay` 规范及 `.clang-format` 使用说明，并在根 `AGENTS.md` 与 `doc/architecture/overview.md` 补充导航入口。

---

## 三、分阶段实施路线与逐步验证

按依赖底向上拆分为 6 个有序阶段，确保每一步均可独立编译与回归测试：

### 阶段 S1：Diagnostics 观测层抽取、`bridge` 零包装拆环与 `RayniumConfig` 落地
1. 创建 `dev.raynium.diagnostics`（`ClientFrameEvent`、`TerrainFrameEvent`、`RayniumTelemetry`）；重构 `NativeBuffer`（`StagingWriter`）、`NativeFrameBuffer`（`PinnedPayload`）、`NativeRenderer`（内聚 `SubmissionMode`）、`RayniumCommandEncoderBackend` 与 `TerrainAnchorMixin` 改调 `RayniumTelemetry`。
2. 实现 `dev.raynium.client.config.RayniumConfig`（纯 Gson 读写 `<gameDir>/config/raynium.json` + `-Draynium.*` 内存覆盖 + 非法配置 fail-fast），将 `RayniumGpuBackend`、`NativeEnvironment`、`NativeRuntime`、`RayniumGpuDevice`、`RayniumPrograms`、`ShaderProgramCache` 全部改为强类型构造注入；更新 JUnit 测试改为构造传参，统一 `-Draynium.shader.tools-dir` 与 `-Draynium.test.surface-layer`。
3. 阶段检查：`.\gradlew.bat test` 通过。

### 阶段 S2：`src/harness` 源码集物理拆分与 Java 包/Mixin 归位重命名
1. 配置 `build.gradle` 新增 `sourceSets.harness`、Loom 编译期合并 AW（`build/loom/combined.accesswidener`）与双 mod 定义（`raynium` + `raynium-harness`）。
2. 将 `Benchmark*`、`VisualValidation*`、`ValidationGalleryScreen`、`VanillaPipelineCatalog`、`PipelineIdentity` 及 18 个测试驱动 Mixin/Accessor 迁入 `src/harness`（实现 `HarnessClient`、`HarnessMixinPlugin`、`RayniumHarnessBridge`，并将 5 个 `*Access` 接口统一重命名为 `*Accessor`）；从核心彻底移除 `RenderMode`、`RayniumMixinPlugin`、`RayniumClient` 及 `raynium.accesswidener` 前 3 行。
3. 将 `BuiltinShaderPack` 迁入 `renderpearl`，将核心 26 个 Mixin 按 `mixin.{backend,terrain,world}` 归入子包并拆出 `MinecraftFrameMixin`；同步修正 `tools/validation/pipeline_coverage.py` 中的 Java Mixin 路径及 `native/src/static_geometry_registry.cpp` 路径。
4. 阶段检查：`.\gradlew.bat classes clientClasses harnessClasses test jar` 通过，且核查 `build/libs/raynium-*.jar` 内不含任何 `harness`、`benchmark`、`validation`、`VanillaPipelineCatalog` 类及冗余 AW 条目。

### 阶段 S3：Gradle 构建收敛、`local.properties`、`writeLaunchSpec` 与 Python 自动化直连
1. 新增 `local.properties.example`、更新 `.gitignore`、改造 `settings.gradle` 读取 `local.properties`、移除 `gradle.properties` 与 `tools/fetch_radray.py` 硬编码代理。
2. 重构 `build.gradle` 与 `gradle/native.gradle`：清空 16 个旧 `-P` 参数，建立 `client` / `clientNative` / `clientNativeRelease` / `clientReference` 四个干净运行项，建立 `Debug`/`Release` 对称 Native 构建与测试任务（独立报告目录，仅留 `-Praynium.build.profiler=true`），增强 `tools/native.py` 拒绝打包 `profilerEnabled=true` 产物，并实现 `writeLaunchSpec`。
3. 重构 `tools/benchmark.py`、`tools/validate_visual.py` 及 `tools/tests/test_benchmark.py`、`tools/tests/test_validate_visual.py`：单次调用 Gradle 完成构建并导出 `launch-spec.json`，在隔离目录写出 `config/raynium.json` 后由 Python 直接拉起游戏 JVM。
4. 阶段检查：`.\gradlew.bat check writeLaunchSpec` 通过（覆盖 JUnit `test` 与 Python `testShaderPackTools` 全部单测），核验生成的 `build/native/launch-spec.json` 中 `raynium` 与 `rayniumHarness` 类路径物理隔离正确。

### 阶段 S4：Native C++ 全面对齐 `RadRay` 规范
1. 移除 21 个 `.cpp` 中的 `using namespace radray::gpu` 与 `radray::shader`，显式补全 `radray::gpu::` 与 `radray::shader::`。
2. 补齐 `device_info.h`、`exception.cpp`、`resource_handles.cpp`，整理 `frame_plan.h`、`raster_records.h`、`terrain_feature.h` 及全部 26 对 `.h`/`.cpp` 的非模板函数外提与头文件自包含；保持 `rn_*` 薄 `struct` 无 `C4099` 告警，将 `namespace raynium` 内类型按纯数据 `struct` / 含行为 `class` 区分，公开数据成员统一 `PascalCase`、私有成员 `_camelCase`、常量 `kPascalCase`；同步更新 `native/CMakeLists.txt`（含 `rayniumruntime`、`test_raynium_terrain`、`raynium_shader_pack`）。
3. 阶段检查：执行 `.\gradlew.bat nativeTestDebug`（含 `vulkanSurfaceTestDebug`）与 CMake `test_raynium_terrain`（若本地 GTest 已就绪），确认双后端零编译告警（`/W4`）与全部 GPU 测试通过。

### 阶段 S5：`.clang-format` 引入、Java 导入清理与全仓统一格式化
1. 在仓库根建立 `.clang-format`（Cpp 对齐 `RadRay` + Java 4 空格无列宽限制规则）。
2. 展开 `src/` 全部 Java 文件的通配符 `import *` 并清理冗余行内全限定类名；对 `src/` 下所有 `.java` 与 `native/` 下所有 `.h`/`.cpp` 统一执行 `clang-format -i`。
3. 阶段检查：复跑 `.\gradlew.bat check nativeTestDebug` 确认格式化与导入展开零语义回归。

### 阶段 S6：长期文档同步与仓库门禁总验收
1. 新增 `doc/guide/conventions.md`，同步更新 `AGENTS.md`、`README.md`、`doc/architecture/overview.md`、`doc/architecture/interop.md`、`doc/architecture/minecraft-integration.md`、`doc/guide/build-test.md`、`doc/guide/dev-environment.md` 中涉及的命令、配置路径、包名、类名与代码锚点（严守只记录已落地当前事实的规则，不改动冻结的旧 `doc/history/*.md`）。
2. 总验收门禁：
   - `python tools/check_docs.py` 与 `git diff --check` 零错误；
   - `.\gradlew.bat check nativeTestDebug` 通过；
   - 若本机 Release 静态依赖已就绪，进一步执行 `.\gradlew.bat nativeTestRelease packageNativeMod verifyNativePackage` 验证正式分发包构建与校验闭环。
