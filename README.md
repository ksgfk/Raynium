# Raynium

基于 Fabric 的 Minecraft 客户端模组，包含 Java FFM/C ABI 桥接与使用 RadRay RHI 的 native 设备会话。

当前在 Windows 以 D3D12/Vulkan 独立设备接管窗口呈现，绘制全分辨率三层地形与十二种 GUI 管线，不创建原版 GPU 设备。
Java 25 通过 FFM 与 ABI 17 调用 native；RadRay runtime 提供帧调度和 Render Graph，Raynium 使用专用 direct/indirect 地形数据、上传纹理与 CPU GUI 采集。
普通透明和小波 OIT 已实现。物品图标、实体、天空、标题全景、模糊与截图等能力的边界见[架构地图](doc/architecture/overview.md)和[Minecraft 接入](doc/architecture/minecraft-integration.md)。

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
