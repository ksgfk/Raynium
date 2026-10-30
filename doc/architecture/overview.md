> - 适用: 定位已实现子系统、当前能力边界和文档
> - 权威: 本文维护当前实现地图与文档索引；详细契约由所属页面维护
> - 锚点: `AGENTS.md`, `native/CMakeLists.txt`, `native/radray.lock.json`, `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/resources/raynium.client.mixins.json`

# Raynium 架构地图

Raynium 是面向 Minecraft 26.3 的 Fabric 客户端模组，当前 native 产物为 Windows x86_64 DLL。
Java 25 通过 FFM 和 C ABI 加载 DLL；DLL 消费上游 `RadRay::runtime` 静态库，再由该库使用 RadRay RHI。

## 当前实现与边界

构建与分发、payload 定位和内容缓存、ABI 9 检查、设备创建/销毁及客户端生命周期已实现。
Windows MC Vulkan 通过 Mixin 在帧末复制 native 共享 SOLID 地形小 RT 到右上角，原版世界绘制和呈现继续运行。
SOLID 使用持久 RadRay Scene、增量 MeshDrawCommand 缓存和双槽 FrameScheduler。Raynium 的 SolidFramePipeline 固定录制上传、输入装配、光栅与共享交接；没有通用 RenderSystem、View/Pass 框架、Render Graph 或窗口接管。
构建与运行验证的日期、环境、证据及限制见[构建与测试](../guide/build-test.md#当前验证范围)。

## 职责与源码地图

| 已有部分 | 当前职责 | 源码入口 |
| --- | --- | --- |
| 客户端入口 | 注册 CLIENT_STARTED/CLIENT_STOPPING，选择 payload 和后端，处理启动失败 | `src/client/java/dev/raynium/client/RayniumClient.java` |
| Java bridge | payload 定位、内容缓存、FFM 布局和绑定、DLL/环境/runtime 所有权 | `src/client/java/dev/raynium/bridge/` |
| C bridge | ABI 查询、参数校验、诊断字符串和不透明 runtime 句柄 | `native/include/raynium/bridge.h`、`native/src/bridge.cpp` |
| MC native 管线 | ABI 转换、SolidPassProcessor、SolidFramePipeline、shader 与单队列提交 | `native/src/renderer.cpp`、`native/src/solid_pipeline.cpp`、`native/src/mesh_pass.cpp` |
| 地形驻留 | 场景增量就地更新、独立几何缓冲、持久材质、可见批次与跨槽资源退役 | `native/src/terrain_scene.cpp` |
| 共享资源 | DLL 共享输入/输出协议、显式 acquire/release 和 split mip 装配 | `native/src/render_resources.cpp` |
| MC 场景适配 | SOLID 网格/面组转换为通用场景增量，上传预算、纹理变化 | `src/client/java/dev/raynium/client/terrain/` |
| MC 预览适配 | 导入原始 Vulkan 对象、帧末复制、resize/退出排空 | `src/client/java/dev/raynium/client/preview/`、`src/client/java/dev/raynium/client/mixin/` |
| RadRay runtime | 环境/设备、Scene 容器、Mesh Drawing Pipeline、FrameScheduler/FrameSlot/ObjectArena；DLL 从锁定源码编译并链接 `RadRay::runtime` | `native/radray.lock.json`、`native/CMakeLists.txt` |
| 工程工具 | 显式依赖准备、CMake 构建、stage、分发和验证 | `tools/`、`gradle/native.gradle` |

```mermaid
flowchart LR
    Lifecycle[Fabric 客户端生命周期] --> Java[NativeEnvironment / NativeRuntime]
    Java --> FFM[NativeBindings / FFM]
    FFM --> ABI[C ABI 9]
    ABI --> Env[RayniumEnvironment 单例环境]
    Env --> Runtime[RayniumRuntime 设备会话]
    Runtime --> Scheduler[FrameScheduler / 双 FrameSlot]
    Scheduler --> Adapter[Raynium SolidFramePipeline]
    Adapter --> Mesh[Scene / SolidPassProcessor / MeshDrawCommand]
    Mesh --> RHI[RadRay Device / Direct Queue]
    RHI --> RT[共享 SOLID 地形小 RT]
    RT --> Copy[MC Vulkan 帧末复制]
    MC[原版渲染] --> Copy
    Copy --> Surface[原版窗口呈现]
```

## 文档索引

| 主题 | 权威入口 |
| --- | --- |
| native 设备会话、所有权、关闭及能力边界 | [Native runtime](native-runtime.md) |
| 当前 C ABI、FFM、payload 与加载 | [互操作与加载](interop.md) |
| SOLID 地形数据、纹理传输与绘制 | [地形预览](terrain-preview.md) |
| Fabric 生命周期和当前 MC 接入范围 | [Minecraft 接入](minecraft-integration.md) |
| MC 26.3 原版渲染、纹理 API 与源码节点 | [Minecraft 渲染流程](minecraft-rendering.md) |
| 已有模组注明日期和版本的源码调查 | [接入参考](integration-references.md) |
| 依赖、构建、测试、启动、stage 和分发 | [构建与测试](../guide/build-test.md) |
| 工具链、IDEA 与 MC 源码生成 | [开发环境](../guide/dev-environment.md) |
| 文档归属、事实边界、引用和历史规则 | [文档维护](../guide/documentation.md) |
| 项目文档维护流程 | [raynium-doc](../../.agents/skills/raynium-doc/SKILL.md) |

## 思考历史索引

| 日期 | 记录内容 |
| --- | --- |
| 2026-10-26 | [渲染方向与实现方案思考记录](../history/2026-10-26-rendering-design-notes.md)：窗口接管到原版保留的方向变化，runtime/互操作/MC 接入草案及 RT 显示候选 |
| 2026-10-30 | [可编程渲染 Runtime](../history/2026-10-30-programmable-runtime.md)：确认的独立静态库、最小 RDG、SOLID 管线与程序化验收范围 |
| 2026-11-04 | [RadRay 新版 Runtime 接入与重写计划](../history/2026-11-04-radray-runtime-rewrite-plan.md)：升级 ABI 9、持久 Scene 增量命令缓存、Group 0/1 绑定拆分、延迟实例流与 RetireResource 资源退役 |
| 2026-11-04 | [Tracy 实机性能瓶颈分析与分项优化计划](../history/2026-11-04-tracy-bottleneck-optimization-plan.md)：实机 CPU/GPU 耗时归因、移除每帧事务回滚深拷贝的确认决策及后续分项优化候选 |

历史记录保存当时的目标和推演，不构成当前实现或接口契约。
