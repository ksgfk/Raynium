# Raynium

基于 Fabric 的 Minecraft 客户端模组，包含 Java FFM/C ABI 桥接与使用 RadRay RHI 的 native 设备会话。

Windows 后端将原版 RenderPearl 命令录入同一个批次，通过 FFM 交给 RadRay 在 D3D12 或 Vulkan 上执行。区块地形使用驻留几何，在原版光栅 Pass 的绘制点内联执行。实体、GUI、光照和后处理沿用游戏的管线及附件。
已实现的适配、资源包支持、截图与生命周期边界见[架构地图](doc/architecture/overview.md)和[Minecraft 接入](doc/architecture/minecraft-integration.md)。

## 开发入口

| 任务 | 文档 |
| --- | --- |
| 工具链、IDEA、阅读 Minecraft 源码 | [开发环境](doc/guide/dev-environment.md) |
| 依赖、构建、全部测试、开发启动与打包 | [构建与测试](doc/guide/build-test.md) |
| 架构、当前进度与完整文档索引 | [架构地图](doc/architecture/overview.md) |
| 历史计划、开发验证与采样记录 | [历史索引](doc/history/README.md) |
| 仓库约束与文档工作流程 | [AGENTS](AGENTS.md)、[文档维护](doc/guide/documentation.md) |

使用仓库 Gradle Wrapper；native 依赖需要显式准备，普通 Java build 不触发 native 构建。
分发包包含固定 `/MT` 的 Release DLL（内含静态 mimalloc）与上游许可证，以及离线生成的内置 shader pack，产物路径和测试入口见构建指南。

Raynium 自身尚未选择分发许可证；收集的上游许可证不替代项目自身授权。
