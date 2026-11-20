> - 适用: 修改后端选择、窗口与表面生命周期、地形切点和验证入口
> - 权威: 本文记录 MC 26.3 与 Raynium 的接入点
> - 锚点: `src/client/resources/raynium.client.mixins.json`, `src/client/java/dev/raynium/client/mixin/RayniumBackendMixin.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuBackend.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuSurfaceBackend.java`

# Minecraft 接入

## 生命周期与入口

`Minecraft` 构造时，`RayniumBackendMixin` 用单个 `RayniumGpuBackend` 替换待尝试的图形后端。后端加载阶段定位并验证 DLL，按 `raynium.rhi.backend` 初始化环境。游戏通过 SDL 创建窗口，设备创建阶段建立 runtime、renderer 和 `RayniumGpuDevice`。窗口表面使用该 SDL 窗口的 HWND。关闭顺序是表面、设备、后端库。

`runClientReference` 通过 `raynium.reference` 选择原版后端。视觉驱动与基准驱动在两种启动入口使用相同的游戏逻辑。[构建与测试](../guide/build-test.md)

## 录制与呈现

MC 的实体、天空、GUI、画中画、光照图、动画图集与后处理继续使用自己的 RenderPass、管线、附件和视图。`RayniumGpuDevice` 提供 `FrontendCommandEncoder`，后端在绘制点保存当前管线、窗口区域、绑定、动态偏移与 Push Constant。时间戳、跨资源复制和最终上屏随实际调用位置进入批次。

`acquireNextTexture` 的前端获取状态可以跨多个离屏提交。只有记录上屏 blit 的批次携带 surface。可恢复的交换链结果由表面保存，后续 configure 重建。终态通过共用路径释放前端获取状态。截图和图集导出使用 MC 自身的纹理复制、只读映射与 Fence 路径。

## 地形与帧事件

编译、索引采用和区块移除切点维护驻留数据。地形准备为每个视图保存相机、可见项与上传输入。原版 `renderLayers` 切点在当前打开的 Pass 插入地形命令，记录实际图集、lightmap 和 OIT 输入。随后游戏可继续在同一组附件绘制即时几何。[区块地形渲染](terrain-rendering.md)

`MinecraftBenchmarkMixin` 在世界帧开始时推进采集与基准状态。测试模式可冻结时间、相机与动画并采集实际选用的管线。管线身份、视觉证据与离线产物通过覆盖率脚本关联。[Minecraft 渲染流程](minecraft-rendering.md)
