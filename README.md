# Raynium

基于 Fabric 的 Minecraft 客户端模组，包含 Java FFM/C ABI 桥接与使用 RadRay RHI 的 native 设备会话。

当前已接通 native 构建与分发、Java FFM、ABI 检查和 D3D12/Vulkan 设备会话。
Windows MC Vulkan 可在右上角显示 native 共享小 RT 的 SOLID 地形预览（保留 UV 诊断模式）；原版世界渲染和呈现继续运行。
CUTOUT、TRANSLUCENT、通用资源服务和窗口接管尚未实现。具体状态与文档索引见[架构地图](doc/architecture/overview.md)。

## 开发入口

| 任务 | 文档 |
| --- | --- |
| 工具链、IDEA、阅读 Minecraft 源码 | [开发环境](doc/guide/dev-environment.md) |
| 依赖、构建、全部测试、开发启动与打包 | [构建与测试](doc/guide/build-test.md) |
| 架构、当前进度与完整文档索引 | [架构地图](doc/architecture/overview.md) |
| 仓库约束与文档工作流程 | [AGENTS](AGENTS.md)、[文档维护](doc/guide/documentation.md) |

使用仓库 Gradle Wrapper；native 依赖需要显式准备，普通 Java build 不触发 native 构建。
分发包包含 Release DLL 与上游许可证，产物路径和已验证范围见构建指南。

Raynium 自身尚未选择分发许可证；收集的上游许可证不替代项目自身授权。
