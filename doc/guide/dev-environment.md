> - 适用: 配置 Java/IDEA、生成和阅读 MC 源码
> - 权威: 本文维护开发环境和源码获取方法；native 依赖、构建与测试见构建指南
> - 锚点: `gradle.properties`, `gradle/wrapper/gradle-wrapper.properties`, `build.gradle`, `settings.gradle`

# 开发环境

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

安装本机 JDK 25，并为 Gradle JVM 与项目 SDK 选择它。IDEA 的 JDK 路径与运行配置是本机状态，不在公共文档写死。

## 在 IntelliJ IDEA 中打开

1. 使用 **File → Open** 打开本仓库根目录，按 Gradle 工程导入，等待同步和索引结束。
2. **Settings → Build, Execution, Deployment → Build Tools → Gradle** 中使用项目的 **Wrapper**，Gradle JVM 选择 **JDK 25**。若本机路径未自动解析，选择本机 JDK 25 目录。
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

MC 26.3 的渲染入口与职责如下：

| 类 / 包 | 源码职责 |
| --- | --- |
| `net.minecraft.client.Minecraft` | 客户端生命周期、主循环、窗口与渲染调度 |
| `net.minecraft.client.renderer.GameRenderer` | 帧级渲染组织、相机与画面输出 |
| `net.minecraft.client.renderer.LevelRenderer` | 世界渲染与场景数据组织 |
| `com.mojang.blaze3d.systems.RenderSystem` | 渲染系统入口与设备访问 |
| `com.mojang.renderpearl` | 26.3 的 GPU API、前端、资源、命令与图形后端 |
| `com.mojang.blaze3d.platform.Window` | SDL 窗口；旧版模组的 GLFW 接入不能直接沿用 |

## 本机源码缓存

历史调查使用 `.gradle/mc-source-26.3/client/` 和 `common/` 作为解包阅读目录；它们不是 Loom 保证生成的路径。
先运行上面的 `genSources`，按需要将对应 clientOnly/common sources JAR 解压到阅读目录，或直接在 IDEA 中导航类与方法。
文档中的类名、包名与 MC 版本用于重新定位，旧行号只作辅助；忽略缓存不存在不影响仓库文档检查。

`.gradle/`、`.idea/`、`build/`、`run/` 是本机缓存和产物，不提交配置、日志或测试世界。
构建、开发客户端和 native 运行命令见[构建与测试](build-test.md)。

## 工程来源

Gradle Wrapper 文件取自 Fabric 官方示例；当前版本和校验值以 Wrapper 配置为准。
官方参考：[IDEA 设置](https://docs.fabricmc.net/develop/getting-started/intellij-idea/setting-up)、[生成与附加 Minecraft 源码](https://docs.fabricmc.net/develop/getting-started/intellij-idea/generating-sources)。
