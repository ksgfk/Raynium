# Raynium

基于 Fabric 的 Minecraft 客户端模组。目标是逐步通过 RadRay RHI 和通用的 native 渲染 runtime 替换 Minecraft 渲染系统：Java 负责场景与数据准备，native 内的渲染管线负责组织渲染并调用 RHI。

当前已有 Java 开发环境、RadRay 固定版本拉取、静态 CRT native bridge、Gradle native 任务、Java 25 FFM 加载和 Release mod JAR 打包代码。已在本机完成编译、客户端启动及 Release payload 加载检查，验证范围见 [Native 工程操作](doc/native-development.md#本机验证范围)。`RayniumRuntime` 已支持 D3D12/Vulkan 设备与 Direct 队列，由 Java 在客户端开始/退出时创建和销毁。Mixin 列表为空，仍使用原版渲染；资源、管线和呈现尚未实现。

## 设计与源码调研

文档入口为 [doc/README.md](doc/README.md)。

- [Native 工程操作](doc/native-development.md)：准备静态 CRT 依赖、IDEA/Gradle 启动、FFM 加载、缓存和 Release 打包。
- [渲染接入设计](doc/rendering-design.md)：已确认的 Java/native 职责边界、候选接口和待定事项。
- [实现设计](doc/implementation-design.md)：Windows + RadRay RHI 的模块、ABI、资源与帧生命周期、MC 接入位置和分阶段方案。
- [Native runtime 功能设计](doc/native-runtime-design.md)：资源服务、完整帧执行、管线支持、状态与寿命管理、输出和功能分级。
- [RadRay 构建与 native 加载](doc/native-integration.md)：CMake 引入、bridge DLL、Gradle 调度、FFM 加载和分发方案。
- [FFM 互操作](doc/ffm-interop.md)：C ABI、内存所有权、线程、错误、回调和 runtime 生命周期的实施建议。
- [Minecraft 26.3 渲染流程](doc/minecraft-26.3-rendering.md)：帧调度、地形与动态对象通路、RenderPearl 边界。
- [已有模组接入调研](doc/native-mod-research.md)：RTX Remix、Radiance、wgpu-mc 等项目的实现、关键 Mixin 与固定提交源码入口。

## 工具链

| 组件 | 固定版本 |
| --- | --- |
| Minecraft Java Edition | 26.3 |
| Java | 25 |
| Fabric Loader | 0.20.1 |
| Fabric API | 0.165.0+26.3 |
| Fabric Loom | 1.19.3 |
| Gradle Wrapper | 9.8.2 |

版本集中在 `gradle.properties`；Gradle 版本和分发包 SHA-256 在 `gradle/wrapper/gradle-wrapper.properties`。使用仓库内的 Wrapper，无须全局安装 Gradle。

本机已有 IntelliJ IDEA 2026.2.3 与 Temurin JDK 25，JDK 路径为：

```text
C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot
```

本地 `.idea/gradle.xml` 使用 `GRADLE_LOCAL_JAVA_HOME`，路径保存在忽略提交的 `.gradle/config.properties`。换机器后在 IDEA 中重新选择本机 JDK 25。

下载统一使用 `http://127.0.0.1:10808` 代理，HTTP/HTTPS 参数已写入 `gradle.properties`。同步、生成源码和首次启动客户端时保持该代理运行；换机器时同步调整这些参数。

## 在 IntelliJ IDEA 中打开

1. 使用 **File → Open** 打开 `C:\Users\xiaoxs\Desktop\Raynium`，按 Gradle 工程导入，等待同步和索引结束。
2. **Settings → Build, Execution, Deployment → Build Tools → Gradle** 中使用项目的 **Wrapper**，Gradle JVM 选择 **JDK 25**。若本机路径未自动解析，选择上面的 JDK 目录。
3. **File → Project Structure → Project SDK** 选择 **JDK 25**。项目使用 Java 25 toolchain。
4. 运行/调试配置为 **Minecraft Client (Raynium)**，由 Loom 生成。若首次导入后暂未显示，可重新加载 Gradle 工程。
5. 可选安装 **Minecraft Development** 插件，便于编辑 Mixin；阅读 MC 源码不依赖此插件。

## 阅读 Minecraft 源码

Minecraft 26.3 使用未混淆的官方名称，本工程使用 `net.fabricmc.fabric-loom`，不配置 Yarn 或旧版 remap 插件。源码由 Loom 在本机反编译生成。

```powershell
.\gradlew.bat genSources ideaSyncTask
```

生成后重新加载 IDEA 的 Gradle 工程。使用 **Ctrl+N** 查类，必要时开启搜索库类；也可从 **External Libraries** 展开 Minecraft 库。**Ctrl+B / Ctrl+单击** 跳转，**Alt+F7** 查找引用。

若打开类时顶部仍出现 **Choose Sources / Attach Sources**，选择本工程 `.gradle/loom-cache/minecraftMaven/net/minecraft/` 下对应库的 `*-sources.jar`：`minecraft-clientOnly-*` 包含客户端渲染代码，`minecraft-common-*` 包含公共代码。源码附件是本机生成物，不放入模组源目录。

建议从这些入口开始梳理渲染链路：

| 类 / 包 | 阅读方向 |
| --- | --- |
| `net.minecraft.client.Minecraft` | 客户端生命周期、主循环、窗口与渲染调度 |
| `net.minecraft.client.renderer.GameRenderer` | 帧级渲染组织、相机与画面输出 |
| `net.minecraft.client.renderer.LevelRenderer` | 世界渲染与场景数据组织 |
| `com.mojang.blaze3d.systems.RenderSystem` | 渲染系统入口与设备访问 |
| `com.mojang.renderpearl` | 26.3 的 GPU API、前端、资源、命令与图形后端 |
| `com.mojang.blaze3d.platform.Window` | SDL 窗口；旧版模组的 GLFW 接入不能直接沿用 |

## 构建与运行

在工程根目录执行：

```powershell
# 生成用于阅读的 MC 源码与 IDEA 运行配置
.\gradlew.bat genSources ideaSyncTask

# 构建模组；产物位于 build/libs/
.\gradlew.bat build

# 启动开发客户端；也可以在 IDEA 中 Run / Debug
.\gradlew.bat runClient
```

开发客户端的游戏目录是 `run/`，日志位于 `run/logs/latest.log`。入口被加载后会输出 `Raynium initialized; vanilla rendering is active.`。

本机已验证 Java/native 构建及开发客户端启动；IDEA 交互、进入世界和正式启动器安装尚未验证。

RadRay 固定于 `95f725a35ced0a52ae0706207582788c011914ca`，Debug/Release 都使用 `/MT` 和静态 mimalloc。在项目根目录运行 `python tools/fetch_radray.py restore`，按 [Native 工程操作](doc/native-development.md) 显式准备 `windows-x64-msvc-staticcrt` 依赖，再执行：

```powershell
# 构建、stage 并启动带 native bridge 的开发客户端
.\gradlew.bat runClientNative

# 构建 Release DLL 并生成包含 native payload 的 mod JAR
.\gradlew.bat packageNativeMod
```

本机 IDEA 配置 **Minecraft Client (Raynium Native)** 调用 `:runClientNative`；普通 `runClient` 与 Java `build` 不触发 native 构建。Gradle native 任务会为子进程建立 MSVC 环境；依赖拉取不使用 submodule，也不由配置或构建自动触发。

## 目录与后续边界

- `src/main/resources/fabric.mod.json`：客户端限定、依赖版本与入口声明。
- `src/client/java/dev/raynium/client/`：客户端模组代码。
- `src/client/java/dev/raynium/bridge/`：manifest、DLL 定位与缓存、FFM 绑定和库生命周期。
- `src/test/java/dev/raynium/bridge/`：JUnit 测试；`test` 执行纯 Java 用例，`nativeTest` 执行 DLL/RHI 集成用例，详见 [测试说明](doc/native-development.md#java-与-rhi-测试)。
- `src/client/resources/raynium.client.mixins.json`：未来渲染接入所需的 Mixin 配置，目前为空。
- `gradle/`、`gradlew`、`gradlew.bat`：Gradle 启动工具及 `native.gradle` 显式 native 任务。
- `native/`：CMake 工程、Debug/Release presets、RadRay commit 锁和最小 C ABI bridge。
- `tools/fetch_radray.py`：经代理显式拉取 RadRay 的脚本。
- `tools/native.py`：MSVC 环境、CMake 调度、stage、依赖与许可证收集、JAR 打包。
- `external/RadRay/`：忽略提交的受管 RadRay checkout。
- `doc/`：当前设计、MC 渲染流程与已有模组源码调研。

native 工程默认使用脚本拉取的 RadRay，也支持显式指定旁边的 RadRay 工作区联调。当前库名为 `rayniumruntime.dll`，已接通 FFM 加载和 ABI 3 检查；临时探针已删除。runtime 已通过静态 Create 工厂创建设备和 Direct 队列；后续资源服务、GUI 和呈现的方案见实现设计。

`.gradle/`、`.idea/`、`build/`、`run/` 已忽略提交，包含本机依赖缓存、生成源码、运行配置与测试世界。尚未为 Raynium 选择分发许可证。

## 官方参考

- [Fabric 开发版本](https://fabricmc.net/develop/)
- [Fabric 官方示例](https://github.com/FabricMC/fabric-example-mod/tree/26.3)
- [IDEA 设置](https://docs.fabricmc.net/develop/getting-started/intellij-idea/setting-up)
- [生成与附加 Minecraft 源码](https://docs.fabricmc.net/develop/getting-started/intellij-idea/generating-sources)

Gradle Wrapper 文件取自官方示例提交 `91f19010a4e764f193fe50acfe2c8ee68f1f15cd`。
