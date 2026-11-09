> - 适用: 定位已实现子系统、当前能力边界和文档
> - 权威: 本文维护当前实现地图与文档索引；详细契约由所属页面维护
> - 锚点: `AGENTS.md`, `native/CMakeLists.txt`, `native/radray.lock.json`, `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/resources/raynium.client.mixins.json`

# Raynium 架构地图

Raynium 是面向 Minecraft 26.3 的 Fabric 客户端模组，当前 native 产物为 Windows x86_64 DLL。
Java 25 通过 FFM 和 C ABI 加载 DLL；DLL 消费上游 `RadRay::runtime` 静态库，再由该库使用 RadRay RHI。

## 当前实现与边界

构建与分发、payload 定位和内容缓存、ABI 10 检查、设备创建/销毁及客户端生命周期已实现。
Windows MC Vulkan 通过 Mixin 在帧末复制 native 共享 SOLID 地形小 RT 到右上角，原版世界绘制和呈现继续运行。
SOLID 使用持久 RadRay Scene、增量 MeshDrawCommand 缓存和双槽 FrameScheduler。SolidFramePipeline 基于上游 RenderGraphFramePipeline，使用 runtime 状态排序/元素合并及 RDG 上传、输入装配、光栅与外部所有权屏障，Raynium 负责跨宿主同步和队列提交；没有通用 RenderSystem、View/Pass 框架或窗口接管。
Java 复用扁平采集与堆外帧缓冲，按事件同步驻留状态，并直接传递 section 元素掩码；上传预算与回退回收契约见[地形预览](terrain-preview.md)。
当前构建、测试和采样方法见[构建与测试](../guide/build-test.md)。

## 职责与源码地图

| 已有部分 | 当前职责 | 源码入口 |
| --- | --- | --- |
| 客户端入口 | 注册 CLIENT_STARTED/CLIENT_STOPPING，选择 payload 和后端，处理启动失败 | `src/client/java/dev/raynium/client/RayniumClient.java` |
| Java bridge | payload 定位、内容缓存、FFM 布局和绑定、DLL/环境/runtime 所有权 | `src/client/java/dev/raynium/bridge/` |
| C bridge | ABI 查询、参数校验、诊断字符串和不透明 runtime 句柄 | `native/include/raynium/bridge.h`、`native/src/bridge.cpp` |
| MC native 管线 | ABI 转换、SolidPassProcessor、SolidFramePipeline、shader 与单队列提交 | `native/src/renderer.cpp`、`native/src/solid_pipeline.cpp`、`native/src/mesh_pass.cpp` |
| 地形驻留 | 场景增量就地更新、消费 runtime 顶点/索引池、持久材质、可见批次与跨槽资源退役 | `native/src/terrain_scene.cpp`、`native/src/renderer.cpp` |
| 共享资源 | DLL 共享输入/输出协议、显式 acquire/release 和 split mip 装配 | `native/src/render_resources.cpp` |
| MC 场景适配 | SOLID 网格/面组转换为通用场景增量，上传预算、纹理变化 | `src/client/java/dev/raynium/client/terrain/` |
| MC 预览适配 | 导入原始 Vulkan 对象、帧末复制、resize/退出排空 | `src/client/java/dev/raynium/client/preview/`、`src/client/java/dev/raynium/client/mixin/` |
| RadRay runtime | 环境/设备、Scene 容器、Mesh Drawing Pipeline、FrameScheduler/FrameSlot/ObjectArena、RenderGraphFramePipeline/RDG；DLL 从锁定源码编译并链接 `RadRay::runtime` | `native/radray.lock.json`、`native/CMakeLists.txt` |
| 工程工具 | 显式依赖准备、CMake 构建、stage、分发和验证 | `tools/`、`gradle/native.gradle` |

```mermaid
flowchart LR
    Lifecycle[Fabric 客户端生命周期] --> Java[NativeEnvironment / NativeRuntime]
    Java --> FFM[NativeBindings / FFM]
    FFM --> ABI[C ABI 10]
    ABI --> Env[RayniumEnvironment 单例环境]
    Env --> Runtime[RayniumRuntime 设备会话]
    Runtime --> Scheduler[FrameScheduler / 双 FrameSlot]
    Scheduler --> Adapter[SolidFramePipeline / RenderGraphFramePipeline]
    Adapter --> Mesh[Scene / SolidPassProcessor / MeshDrawCommand]
    Mesh --> RDG[RDG / 每槽瞬态资源池]
    RDG --> RHI[RadRay Device / Direct Queue]
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
| 依赖、构建、测试、启动、stage 和分发 | [构建与测试](../guide/build-test.md) |
| 工具链、IDEA 与 MC 源码生成 | [开发环境](../guide/dev-environment.md) |
| 文档归属、事实边界、引用和历史规则 | [文档维护](../guide/documentation.md) |
| 历史计划、开发验证与采样记录 | [历史索引](../history/README.md) |
| 项目文档维护流程 | [raynium-doc](../../.agents/skills/raynium-doc/SKILL.md) |
