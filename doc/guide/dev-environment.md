> - 适用: 配置 Java/IDEA、本机配置、格式化工具与阅读 MC 源码
> - 权威: 本文维护开发环境和源码获取方法；native 依赖、构建与测试见构建指南，代码规范见编码与工程规范
> - 锚点: `gradle.properties`, `local.properties.example`, `.clang-format`, `gradle/wrapper/gradle-wrapper.properties`, `build.gradle`, `settings.gradle`, `.gitignore`

# 开发环境

## 工具链

| 组件 | 固定版本 |
| --- | --- |
| Minecraft Java Edition | 26.3 |
| Java | 25 |
| Fabric Loader | 0.20.1 |
| Fabric API | 0.166.2+26.3 |
| Fabric Loom | 1.19.4 |
| Gradle Wrapper | 9.9.2 |

MC 和 Fabric 组件版本见 `gradle.properties`，Java toolchain 和编译目标见 `build.gradle`，Gradle 版本及分发包 SHA-256 见 `gradle/wrapper/gradle-wrapper.properties`。使用仓库内的 Wrapper 即可。

安装本机 JDK 25，并为 Gradle JVM 与项目 SDK 选择它。IDEA 的 JDK 路径与运行配置是本机状态，不在公共文档写死。

## 本机配置 `local.properties`

仓库不提交本机代理、Python 路径或游戏目录配置。按需将根目录 [local.properties.example](../../local.properties.example) 复制为被 Git 忽略的 `local.properties`：

- `proxy` 可填带主机与端口的 HTTP(S) 代理 URL。`settings.gradle` 在插件解析阶段读取它，在 JVM 尚未设置 `http.proxyHost` 时为 HTTP 和 HTTPS 写入主机与端口。留空时沿用 Gradle/JVM 原有设置。`fetch_radray.py` 另有命令行和环境变量回退顺序，见[构建与测试](build-test.md#显式准备依赖)。
- `python`：Python 可执行文件路径，默认 `python`。
- `parallel`：CMake/Ninja 并行度，默认 `8`。
- `radraySourceDir`：可选本地 RadRay 源码覆盖路径；留空时使用锁定检出 `external/RadRay`。
- `gameRunDir`：开发客户端游戏目录，默认 `run`。

对于 `python`、`parallel`、`radraySourceDir` 和 `gameRunDir`，Gradle 优先读取同名的非空项目属性（例如 `-Pparallel=4`），其次读取本机文件，最后使用默认值。

根目录 [.clang-format](../../.clang-format) 同时覆盖 C++ 与 Java 格式化规则，使用方式见[编码与工程规范](conventions.md#代码格式化)。

## 在 IntelliJ IDEA 中打开

1. 使用 **File → Open** 打开本仓库根目录，按 Gradle 工程导入，等待同步和索引结束。
2. **Settings → Build, Execution, Deployment → Build Tools → Gradle** 中使用项目的 **Wrapper**，Gradle JVM 选择 **JDK 25**。若本机路径未自动解析，选择本机 JDK 25 目录。
3. **File → Project Structure → Project SDK** 选择 **JDK 25**。项目使用 Java 25 toolchain。
4. Loom 会为 `client` 与 `clientNative` 生成运行/调试配置（如 **Minecraft Client (Raynium)**）。若首次导入后暂未显示，可执行 `ideaSyncTask` 或重新加载 Gradle 工程。

## 阅读 Minecraft 源码

Minecraft 26.3 使用未混淆的官方名称，本工程使用 `net.fabricmc.fabric-loom`，不配置 Yarn 或旧版 remap 插件。源码由 Loom 在本机反编译生成。

```powershell
.\gradlew.bat genSources ideaSyncTask
```

生成后重新加载 IDEA 的 Gradle 工程。使用 **Ctrl+N** 查类，必要时开启搜索库类；也可从 **External Libraries** 展开 Minecraft 库。**Ctrl+B / Ctrl+单击** 跳转，**Alt+F7** 查找引用。

若打开类时顶部仍出现 **Choose Sources / Attach Sources**，选择本工程 `.gradle/loom-cache/minecraftMaven/net/minecraft/` 下对应库的 `*-sources.jar`：`minecraft-clientOnly-*` 包含客户端渲染代码，`minecraft-common-*` 包含公共代码。源码附件是本机生成物，不放入模组源目录。

MC 26.3 的渲染入口与职责如下：

| 类 / 方法 / 包 | 源码职责 |
| --- | --- |
| `net.minecraft.client.Minecraft` | 客户端生命周期、主循环、窗口与渲染调度 |
| `net.minecraft.client.renderer.GameRenderer` | 帧级渲染组织、相机与画面输出 |
| `net.minecraft.client.renderer.LevelRenderer` | 世界渲染与场景数据组织 |
| `com.mojang.blaze3d.systems.RenderSystem` | 渲染系统入口与设备访问 |
| `com.mojang.renderpearl` | 26.3 的 GPU API、前端、资源、命令与图形后端 |
| `com.mojang.blaze3d.platform.Window` | SDL 窗口；旧版模组的 GLFW 接入不能直接沿用 |

## 缓存与产物

`.gradle/`、`.idea/`、`build/`、`run/` 与 `local.properties` 是本机缓存、配置和产物，不提交到仓库。
构建、开发客户端和 native 运行命令见[构建与测试](build-test.md)。

## 工具参考

官方参考：[IDEA 设置](https://docs.fabricmc.net/develop/getting-started/intellij-idea/setting-up)、[生成与附加 Minecraft 源码](https://docs.fabricmc.net/develop/getting-started/intellij-idea/generating-sources)。
