> - 适用: 新增或修改 Raynium Java/Native 代码、配置、Mixin 与格式化规则
> - 权威: 本文维护当前生效的代码分层、配置命名、Java/C++ 编码与格式化规范；仓库级红线见根 AGENTS
> - 锚点: `.clang-format`, `local.properties.example`, `build.gradle`, `gradle/native.gradle`, `src/client/java/dev/raynium/client/config/RayniumConfig.java`, `src/client/java/dev/raynium/diagnostics/RayniumTelemetry.java`, `src/harness/resources/fabric.mod.json`, `native/CMakeLists.txt`

# 编码与工程规范

本文记录当前代码树与构建配置已落地的工程约定。仓库级通用红线见根 [AGENTS](../../AGENTS.md)，命令执行与验证入口见[构建与测试](build-test.md)。

## 模块与源码集分层

仓库按物理源码集隔离生产模组与开发测试驱动：

| 源码集 / 包 | 职责与发布边界 |
| --- | --- |
| `src/main` + `src/client`（`raynium`） | 生产客户端模组，打入普通与分发 JAR；不包含 benchmark、视觉验证或原版管线目录导出代码。 |
| `src/harness`（`raynium-harness`） | 独立开发/测试驱动模组，承载 `Benchmark*`、`VisualValidation*`、`ValidationGalleryScreen`、`VanillaPipelineCatalog` 与 `PipelineIdentity`；仅由 `runClientReference`、自动化脚本和测试类路径挂载，不进入任何发布 JAR。 |
| `dev.raynium.diagnostics` | 零上层依赖的观测基础包，提供 JFR 事件（`ClientFrameEvent`、`TerrainFrameEvent`）与无分配遥测回调入口 `RayniumTelemetry`；不依赖 `bridge`、`client`、`harness` 或 Minecraft/RenderPearl。 |
| `dev.raynium.bridge` | FFM 绑定与 native 资源封装层；不反向依赖 `dev.raynium.client` 或 `net.minecraft.*`，通过 `NativeBuffer.StagingWriter`、`NativeFrameBuffer.PinnedPayload`、`NativeRenderer.SubmissionMode` 与 `RayniumTelemetry` 解耦。 |
| `dev.raynium.harness.RayniumHarnessBridge` | Harness 访问核心模组状态与 `RayniumTelemetry` 的唯一软桥接入口，公开签名与字段不直接引用核心模组类型；具体适配收口在包私有 `RayniumCoreBridge`，仅在 `raynium` 模组已加载时实例化。 |

核心与测试驱动各自维护独立的 Access Widener（`src/client/resources/raynium.accesswidener` 与 `src/harness/resources/raynium-harness.accesswidener`）；Loom 在构建期幂等合并到 `build/loom/combined.accesswidener` 供编译使用，打包 `jar` 只收录核心 AW。

## 配置、属性与本机环境

- **用户与运行时配置**：主配置文件为 `<gameDir>/config/raynium.json`（由纯 Gson `RayniumConfig` 严格解析，首次启动缺失时自动写出默认 JSON；未知键、重复键、错误类型、非法枚举、尾随内容或格式损坏直接快速失败）。
  - JSON 字段：`backend`（`d3d12` / `vulkan`，默认 `d3d12`）、`rhiValidation`（默认 `false`）、`terrainSubmission`（`auto` / `direct` / `indirect`，默认 `auto`）、`shaderToolsDir`（可选字符串）。
- **JVM 内存覆盖属性**（只作用于本次进程，不回写 JSON）：
  - `-Draynium.rhi.backend=d3d12|vulkan`
  - `-Draynium.rhi.validation=true|false`
  - `-Draynium.terrain.submission=auto|direct|indirect`
  - `-Draynium.shader.tools-dir=<目录>`
  - `-Draynium.native.dir=<目录>`（显式 native 开发目录）
- **测试与 Harness 内部属性**：
  - `-Draynium.test.surface-layer=<path>`
  - `-Draynium.benchmark.config=<path>`
  - `-Draynium.validation.config=<path>`
- **构造注入原则**：核心运行时类（`RayniumGpuBackend`、`NativeEnvironment`、`NativeRuntime`、`RayniumGpuDevice`、`RayniumPrograms`、`ShaderProgramCache`）不隐式读取 `System.getProperty`，统一由 `RayniumConfig` 或测试代码通过构造函数显式传入参数。
- **本机环境配置**：仓库根忽略 `/local.properties`，模板见 `local.properties.example`，存放 `proxy`、`python`、`parallel`、`radraySourceDir` 与 `gameRunDir`；`settings.gradle` 与 `tools/fetch_radray.py` 均按本机文件读取代理，仓库跟踪文件中不硬编码本机代理地址。
- **Gradle 构建变体属性**：Native 构建只保留单一变体开关 `-Praynium.build.profiler=true`（默认 `false`）；Debug/Release 由对称任务名区分。

## Java 包结构、导入与 Mixin 规范

- **包划分**：
  - 核心 Mixin 按子系统划分在 `dev.raynium.client.mixin.{backend,terrain,world}`，不使用 MixinPlugin，全部 26 项注册于 `src/client/resources/raynium.client.mixins.json`。
  - Harness Mixin 划分在 `dev.raynium.harness.mixin.{benchmark,validation}`，注册于 `src/harness/resources/raynium-harness.mixins.json`，由 `HarnessMixinPlugin` 按是否启用 benchmark/validation 配置决定加载；Mixin 新增字段与辅助方法使用 `rayniumHarness$` 前缀，Accessor 接口统一以 `*Accessor` 命名。
  - `BuiltinShaderPack` 归属 `dev.raynium.client.renderpearl`。
- **导入与类名**：
  - `src/` 下禁止通配符导入（`import ...*;` 与 `import static ...*;`），所有依赖按显式导入声明。
  - 代码体内消除无歧义的冗余全限定类名前缀，仅在存在同名冲突（如 `org.joml.Math` 与 `java.lang.Math`、`com.sun.management.ThreadMXBean` 与 `java.lang.management.ThreadMXBean`）时保留限定名。

## Native C++ 规范（对齐 RadRay）

- **C ABI 边界**：`native/include/raynium/bridge.h` 维持 C ABI 命名（`rn_*` 蛇形类型/字段/函数与 `RN_*` 枚举常量）。不透明句柄实现类型（`struct rn_runtime`、`rn_renderer`、`rn_swapchain`、`rn_buffer`、`rn_texture`、`rn_pipeline`、`rn_query_pool`）保持纯数据薄 `struct`，避免与 `bridge.h` 声明产生 MSVC `/W4` `C4099` 告警；相关辅助操作定义为 `namespace raynium` 内自由函数或内部类方法。
- **命名空间与容器**：
  - 禁止在任何 `.h` / `.cpp` 中使用 `using namespace`（包括 `radray::gpu`、`radray::shader`、`radray`）；显式书写命名空间前缀。
  - 优先直接使用 `radray/types.h` 导出类型（`radray::vector`、`radray::string`、`radray::array`、`radray::unordered_map`、`radray::unique_ptr`、`radray::make_unique` 等），不新增本地别名。
- **`struct` / `class` 与标识符**：
  - 纯数据聚合记录使用 `struct`，公开字段使用 `PascalCase`；协议校验等逻辑写为自由 `constexpr` / 辅助函数。
  - 含显式构造/析构或成员函数的类型使用 `class`，私有成员使用 `_camelCase`，公开成员（如需直接访问）使用 `PascalCase`。
  - 类型与函数/方法使用 `PascalCase`，编译期 `constexpr` 常量使用 `kPascalCase`。
  - 禁止默认构造后再调 `Init()` 的二段式构造；可失败构造统一使用 `static Create(...)`。
- **文件配对与外提**：
  - `.h` 与 `.cpp` 保持同名 1:1 配对（`device_info.{h,cpp}`、`exception.{h,cpp}`、`resource_handles.{h,cpp}` 等）；`frame_plan.h` 与 `raster_records.h` 为纯模板/协议数据头。
  - 头文件自包含所需 `#include`；非模板、非 `constexpr`、非 `= default` 的函数与方法统一外提至同名 `.cpp`。
  - 避免在函数内引入共享 `static const` 对象（如临时 `GpuDeviceCapabilities` 直接栈上默认构造）。

## 代码格式化

仓库根 `.clang-format` 统一维护 C++（对齐锁定 `RadRay` 配置）与 Java 格式化规则（4 空格缩进、禁用 Tab、`ColumnLimit: 0`、附着大括号、Java 自动排序导入且 `static` 导入置前）。

使用工具链中的 `clang-format` 就地格式化或校验（不触及忽略的 `external/RadRay`）：

```powershell
$cf = 'clang-format' # 或 VS 内置 LLVM 路径下的 clang-format.exe
$files = @(Get-ChildItem -Path src -Filter *.java -Recurse) + @(Get-ChildItem -Path native -Include *.h,*.cpp -Recurse)
& $cf -i @($files.FullName)
& $cf --dry-run --Werror @($files.FullName)
```
