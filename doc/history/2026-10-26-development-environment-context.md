> - 适用: 查阅开发环境中的调查缓存与工程来源说明的历史来源与调查上下文
> - 权威: 本文保存原调查或说明，不作为当前实现、外部项目现状或本次重新验证的保证
> - 锚点: `gradle.properties`, `build.gradle`, `doc/guide/dev-environment.md`

# 开发环境中的调查缓存与工程来源说明

- 原记录日期：2026-10-26（来源页首次保存日期；原段未单列日期）
- 归档日期：2026-11-09
- 状态：经用户授权归档；原文按原日期理解，本次未重跑原调查、第三方模组或游戏验证
- 范围：IDEA 可选插件建议、调查用解包目录、忽略缓存说明与 Wrapper 来源表述
- 来源：归档前工作树 `doc/guide/dev-environment.md 的“在 IntelliJ IDEA 中打开”“本机源码缓存”和“工程来源”节`

当前项目事实见[架构地图](../architecture/overview.md)，其他历史见[历史索引](README.md)。
下列原文的“当前”、行号、缓存路径和版本只表示来源页的调查上下文；浮动外部链接不代表归档时重新核实。
本次仅调整迁移后的文档链接，保留其余原文。

## 本机源码缓存


历史调查使用 `.gradle/mc-source-26.3/client/` 和 `common/` 作为解包阅读目录；它们不是 Loom 保证生成的路径。
先运行上面的 `genSources`，按需要将对应 clientOnly/common sources JAR 解压到阅读目录，或直接在 IDEA 中导航类与方法。
文档中的类名、包名与 MC 版本用于重新定位，旧行号只作辅助；忽略缓存不存在不影响仓库文档检查。

`.gradle/`、`.idea/`、`build/`、`run/` 是本机缓存和产物，不提交配置、日志或测试世界。
构建、开发客户端和 native 运行命令见[构建与测试](../guide/build-test.md)。

## 工程来源

Gradle Wrapper 文件取自 Fabric 官方示例；当前版本和校验值以 Wrapper 配置为准。
官方参考：[IDEA 设置](https://docs.fabricmc.net/develop/getting-started/intellij-idea/setting-up)、[生成与附加 Minecraft 源码](https://docs.fabricmc.net/develop/getting-started/intellij-idea/generating-sources)。

## 原页的可选 IDE 插件建议

5. 可选安装 **Minecraft Development** 插件，便于编辑 Mixin；阅读 MC 源码不依赖此插件。
