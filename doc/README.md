# Raynium 设计与源码导航

仓库关键约束见 [AGENTS.md](../AGENTS.md)；文档归属、状态标注、引用与验证记录规范见 [文档维护](documentation.md)。

Raynium 面向 Minecraft 26.3，通过 Fabric 接入通用的 native 渲染 runtime。当前确定的边界是：Java 负责 MC 场景和数据准备，native 负责渲染管线与 GPU 执行，底层使用 RadRay RHI。

当前已有开发环境、静态 CRT native bridge、RadRay 拉取脚本、Gradle native 任务、FFM loader 和 Release JAR 打包代码。本机编译、开发客户端启动与 Release payload 加载检查已通过，范围见 [Native 工程操作](native-development.md#本机验证范围)。Mixin 配置为空，渲染 runtime 尚未实现。整理基线为 2026-10-26。

| 文档 | 内容 |
| --- | --- |
| [文档维护](documentation.md) | 文档归属、组织原则、状态标注、源码引用、验证记录与变更同步 |
| [Native 工程操作](native-development.md) | 依赖准备、静态 CRT、Gradle/IDEA 启动、FFM 加载、缓存、Release 打包和 bridge 导出 |
| [渲染接入设计](rendering-design.md) | 已确认的职责划分、候选数据接口、生命周期约束和待定事项 |
| [实现设计](implementation-design.md) | Windows + RadRay RHI 路线、模块与 ABI 提案、资源和帧寿命、26.3 接入节点、native 管线及实施阶段 |
| [Native runtime 功能设计](native-runtime-design.md) | 通用资源服务、完整帧入口、pass 执行、barrier、GPU 寿命、管线支持、输出及功能分级 |
| [RadRay 构建与 native 加载](native-integration.md) | 现有模组做法、CMake 源码引入、bridge DLL、Gradle 任务、FFM、开发联调与 JAR 分发 |
| [Minecraft 26.3 渲染流程](minecraft-26.3-rendering.md) | 本机生成源码中的帧流程、关键类、地形与动态对象通路、接入注意点 |
| [已有模组接入调研](native-mod-research.md) | Remix、Radiance、wgpu-mc、VulkanMod、C2ME 的实现路线、关键 Mixin 和固定提交源码入口 |
| [开发环境](../README.md) | 固定工具链、代理、IDEA 源码阅读和开发命令 |

已确认平台为 Windows，渲染侧面向 RadRay RHI，不在 Raynium 内分别实现后端。FFM 加载已接入；完整帧提交、native GUI 绘制和统一呈现仍是实施提案。第三方项目的实现只作为参考。

MC 源码位于本机忽略目录 `.gradle/mc-source-26.3/`，第三方检出位于 `.gradle/research/`。这些缓存不随工程分发。第三方源码链接使用固定 Git commit，MC 源码以版本号和类/方法定位。

