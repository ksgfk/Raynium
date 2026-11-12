> - 适用: 修改 Fabric 生命周期、接管帧循环、CPU 纹理与 GUI、窗口呈现
> - 权威: 本文记录 Raynium 已有的 MC 接入；原版源码事实归渲染流程，设备和加载契约归各自页面
> - 锚点: `src/client/java/dev/raynium/client/RayniumClient.java`, `src/client/java/dev/raynium/client/takeover/*.java`, `src/client/java/dev/raynium/client/mixin/*.java`, `src/client/java/dev/raynium/client/mixin/takeover/*.java`, `src/client/resources/raynium.client.mixins.json`, `build.gradle`

# Minecraft 接入

## 生命周期与入口

`RayniumClient.onInitializeClient` 定位 native payload、读取后端与校验配置并安装 TakeoverRuntime。
选定 payload 后，在此加载 DLL 并创建 `NativeEnvironment`，准备所选后端的 factory/instance 与诊断；失败报告 Fabric 初始化失败。
MC 26.3 / Fabric Loader 0.20.1 的 `EntrypointPatch` 在 Minecraft 构造函数中、设置 gameThread 的
`Thread.currentThread()` 调用前插入 `Hooks.startClient`，早于 `RenderSystem.initBackendSystem`、设备和窗口创建。
该入口不访问尚未初始化的 MC 图形对象。对应 MC 源码获取见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)。

首次设备信息查询在初始化线程创建 `NativeRuntime`，不指定宿主 GPU；环境与 DLL 所有权由 `NativeEnvironment` 持有。
窗口 surface 创建时建立 renderer 和交换链，renderer 从自身 Fabric 模组容器提前读取内置 [shader pack](shader-pack.md)。
缺少 native payload 时明确失败。资源重载、切换世界保留设备与 renderer；F3+T 不替换内置 shader。
关闭放在 `Minecraft.close()` 的原表面关闭位置，排空后依次关闭交换链、renderer、runtime、环境和 DLL。
等待失败保留仍活跃的 owner，不记录虚假的关闭成功。

显式设置 `raynium.benchmark.config` 时，`BenchmarkSession` 注册客户端 tick 控制并通过
`MinecraftBenchmarkMixin` 在帧提取前驱动旁观玩家路线；正常启动不注册控制。
路线完成使用 `Minecraft.stop()` 进入既有退出流程。独立存档副本、采集与参数说明见
[自动跑图与保存 Tracy](../guide/build-test.md#自动跑图与保存-tracy)。

## 接管路径

`raynium.client.mixins.json` 直接登记接管与公共 Mixin。关卡失效并入 LevelRendererTakeoverMixin，编译结果及透明索引快照由公共组提供。
接管路径的 Fabric 入口只准备环境，首次设备信息查询创建 runtime；SDL 窗口不带原版图形 API 标志。
`TakeoverRuntime` 从模组容器提前读取内置 shader pack，以 HWND 创建 renderer 和 BGRA8 交换链。
MC 的 `RenderSystem` 没有设备；只使用 RenderPearl API 接口的纹理、视图、采样器和 Buffer 身份句柄。Buffer 不保存临时创建数据，`map()` 明确抛错。

`Minecraft.renderFrame` 保留更新、拾取、抽取、限帧和统计，替换绘制与提交调用点。表面查询由接管状态提供；
VSync 的 FIFO/Immediate 请求交给 native 重建。准备完上传和所有可恢复工作后才获取后备缓冲，已获取帧必调用 Present；跳过窗口绘制时仍提交上传。
关闭位于 `Minecraft.close` 原表面关闭处，在 Java 渲染对象之后，按交换链、renderer、runtime、环境、DLL 的顺序释放。

`TakeoverHandles` 在原版关闭位置之前复制 NativeImage 像素。独立纹理、mipmap、动态贴图、调色板和四类字形上传进入 native 纹理注册表；
Bitmap 字形直接读取 CPU 图像的子矩形。`TakeoverAtlas` 按原布局拼接全部 mip，边距取最近边缘像素，另存 CPU 动画帧、子帧和千分位插值进度。
图集 `upload` 的返回钩子继续执行。GUI 依次准备文字、排序、遍历模糊前后节点，由自有 VertexConsumer 写入复用的堆外帧缓冲，最后重置 GUI 状态。

仅 `gameRenderState.shouldRenderLevel` 为真时执行相机重定位、gizmo 排空、临时方块移除和过期清理、区块编译、持锁排空、遮挡更新、编译回调、世界投影和采集。
CPU staging 保留线程模型与原接纳回调，不创建原版 GPU heap；Uploader 关闭后重置写入位置。采集继续检查 uploaded 标记与透明索引快照。
世界投影保留受伤、视角摇晃和反胃变换；CPU 光照图逐像素使用 MC 26.3 `core/lightmap` 公式。
驻留增量仅在 native 成功消费后提交；强制 GUI 帧不带地形可见项或驻留增量。

当前接管范围是三层地形和十二种 GUI 管线；物品和画中画准备、标题全景、模糊、实体、粒子、天空、云、手持物及世界后处理不绘制。
原版世界绘制事件和 GUI mesh 钩子随其绘制路径不执行。截图与全景截图入口记录一次不支持并返回；线框能力为 false。未知 GUI 管线跳过并按 Identifier 只记录一次。

## 地形与帧事件

`SectionCompilerMixin` 在 worker 返回时复制顶点，`CompiledSectionMeshMixin` 绑定同一结果和关闭事件。
`RenderSectionIndicesMixin` 在 CPU staging 被接纳后发布透明索引快照；正常维护和追加失败后的直接排空都执行原回调。
`TakeoverTerrain` 使用当前可见列表与透明模式，重算最终世界投影；世界 Y 反射同时调整 native 正面绕序。
图集重载与世界重置推进资源 epoch，重新创建带当前 epoch 的编译器。已上传顶点数组在成功消费后释放，Native 承担面组与 AABB 处理。驻留选择、回退与上传寿命见[地形渲染](terrain-rendering.md)。

`MinecraftTakeoverMixin` 在真实 renderFrame 入口清理采集状态，以调用局部引用记录 `ClientFrameEvent`，支持递归加载帧。
`TerrainFrameEvent` 记录接管准备及 FFM 包络；benchmark 只在成功消费世界帧后推进 native 帧计数。
统计默认每 5 秒包含呈现、跳过、重建、获取超时、可见区块、编译队列、临时方块、Java 堆和 BufferPool 占用。

游戏路径不读回像素。交换链和上传所有权见[Native runtime](native-runtime.md)，诊断与测试覆盖见[构建与测试](../guide/build-test.md)。
