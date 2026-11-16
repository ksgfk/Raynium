# Raynium

基于 Fabric 的 Minecraft 客户端模组，包含 Java FFM/C ABI 桥接与使用 RadRay RHI 的 native 设备会话。

当前在 Windows 以 D3D12/Vulkan 独立设备接管地形、实体、环境、第一人称和 GUI 渲染，不创建原版 GPU 设备。
Java 25 保留原版模型、动画、文字和节点排序，通过 FFM 与 ABI 18 提交数据；native 使用 RadRay 帧调度和 Render Graph 组织 direct/indirect 地形、动态场景、离屏 GUI、透明及后处理合成。
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
