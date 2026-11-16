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

`runClientReference` 在 Mixin 应用前设置 `raynium.reference=true`；RayniumMixinPlugin 保留测试驱动的 MinecraftBenchmarkMixin，显式视觉配置还启用动画时钟、光照闪烁与 GUI 鼠标位置的测试 Mixin。benchmark 参考模式通过原版 TimerQuery 采集已完成 GPU 样本。
参考模式不定位或加载 native payload，不应用地形采集及接管 Mixin，使用原版设备、资源和渲染流程；启动参数优先选择原版 Vulkan。
原版可能按自身策略回退后端，实际后端须以启动日志为准。此入口保留 Fabric 与测试驱动，不代表已有完整场景视觉或性能验收。

## 接管路径

`raynium.client.mixins.json` 登记接管与公共 Mixin，插件按启动模式选择是否应用。关卡失效并入 LevelRendererTakeoverMixin，编译结果及透明索引快照由公共组提供。
接管路径的 Fabric 入口只准备环境，首次设备信息查询创建 runtime；SDL 窗口不带原版图形 API 标志。
`TakeoverRuntime` 从模组容器提前读取内置 shader pack，以 HWND 创建 renderer 和 BGRA8 交换链。
MC 的 `RenderSystem` 没有设备；使用 RenderPearl API 接口的纹理、视图、采样器和 Buffer 身份句柄。普通身份 Buffer 不保存临时创建数据；天空及世界边界的 SceneGeometry 持有 native 持久几何。两类 `map()` 都明确抛错。

`Minecraft.renderFrame` 保留更新、拾取、抽取、限帧和统计，替换绘制与提交调用点。表面查询由接管状态提供；
VSync 的 FIFO/Immediate 请求交给 native 重建。准备完上传和所有可恢复工作后才获取后备缓冲，已获取帧必调用 Present；跳过窗口绘制时仍提交上传。
关闭位于 `Minecraft.close` 原表面关闭处，在 Java 渲染对象之后，按交换链、renderer、runtime、环境、DLL 的顺序释放。

`TakeoverHandles` 在原版关闭位置之前复制 NativeImage 像素。独立纹理、mipmap、动态贴图、调色板和四类字形上传进入 native 纹理注册表；
Bitmap 字形直接读取 CPU 图像的子矩形。`TakeoverAtlas` 按原布局拼接全部 mip，边距取最近边缘像素，另存 CPU 动画帧、子帧和千分位插值进度。
CubeMapTexture 保留原版堆叠、翻转及面顺序，将六个面同步复制至 native 上传队列，场景管线按原版立方体采样类型绑定。
图集 `upload` 的返回钩子继续执行。GUI 依次准备文字、排序、遍历模糊前后节点，由自有 VertexConsumer 写入复用的堆外帧缓冲，最后重置 GUI 状态。

仅 `gameRenderState.shouldRenderLevel` 为真时执行相机重定位、gizmo 排空、临时方块移除和过期清理、区块编译、持锁排空、遮挡更新、编译回调、世界投影和采集。
CPU staging 保留线程模型与原接纳回调，不创建原版 GPU heap；Uploader 关闭后重置写入位置。采集继续检查 uploaded 标记与透明索引快照。
世界投影保留受伤、视角摇晃和反胃变换；CPU 光照图逐像素使用 MC 26.3 `core/lightmap` 公式，复用 renderer 的像素数组，更新时不分配逐像素临时容器。
驻留增量仅在 native 成功消费后提交；强制 GUI 帧不带地形可见项或驻留增量。

当前接管范围包括三层地形、十二种平面 GUI 管线、GUI 末地传送门/折跃门、标题全景、动态世界 feature 提交、天空、雨雪、世界边界以及第一人称手臂、手持物与屏幕覆盖效果；云、GUI 物品 atlas 和画中画也已接入；GUI 模糊、原版内置世界后处理及实体发光轮廓由 native RDG 合成。
GuiCaptureMixin 将原版全景旋转状态交给 ScenePanorama，使用原版立方体顶点、投影和采样输入，经场景 ABI 在 GUI 前绘制。
SkySceneMixin 保留原版 SkyRenderer 的几何生成、抽取状态与重载寿命，接入天空圆盘、日月、星辰、日出日落、末地天空与闪光；原版顶点及索引为持久缓冲。天空在清屏后、地形之前绘制。WeatherSceneMixin 调用原版雨雪列生成，复用 CPU 容量并借用 MeshData 至同步消费完成；OIT 三次绘制共享一次几何上传。WorldBorderSceneMixin 保留原版几何重建条件、边界距离顺序、颜色与纹理滚动；普通透明为天气后边界，OIT 为边界后天气。
LevelRenderer 保留原版 render 方法入口及 Fabric context 初始化，随后调用原版 submitFeatures，覆盖实体、方块实体、破坏动画、临时方块、粒子、选中框与 gizmo；对应状态由原版提交方法消费后清理。
SceneFeatures 调用原版 feature builder 和阶段排序，保留模型动画、材质选择及文字几何。连续批次只在原版允许的组内合并；MeshData 借用至同步 present 返回，成功、失败、未提交帧替换和关闭都解除借用。参数与几何进入场景 ABI，原版 GPU 准备和执行不再调用。
透明 quad 索引排序保留原版的全局 improvedTransparency 条件；关闭时，世界与第一人称使用透视距离排序，GUI 物品及画中画使用正交 Z 排序。开启时，这些视图均保留提交索引顺序。
FeatureRenderDispatcher 复用 SceneUniforms 的常量写入区、固定槽表与视图矩阵 scratch；各视图的常量写入复用的堆外帧存储，后续视图不会覆盖已提交的字节。阶段输出回调及 OIT 阶段数组按对象复用。
CloudSceneMixin 复用原版云纹理解析、面生成、重建判断和常量。面以 R8_SINT 持久缓冲保存，索引按容量增长；普通帧不重传面数据。OIT 三次云绘制复用几何，云深度在深度范围裁剪后从主深度复制。LevelRenderer.endFrame 保留，云环形缓冲的轮换由 native 槽寿命替代。

GuiCaptureMixin 复用 TakeoverGui 及特殊 GUI 的 SceneMesh 存储；特殊 GUI 保留原版顶点格式、投影、全局常量和三个纹理槽，按原版排序位置提交。
GUI 保留原版 GuiItemAtlas 分配器、动画失效、GUI 缩放失效与 OversizedItemRenderer 复用逻辑。待更新的 atlas 槽位和 PictureInPictureRenderer 内容使用原版 CPU 提交、矩阵及光照，统一提交离屏 tile。物品 atlas 与画中画颜色缓存保留原版纹理身份，临时颜色和深度由 RDG 池提供；GUI blit 保留原版 UV 方向。人物、皮肤、书、旗帜和 profiler 图表沿用各自 renderToTexture 的 CPU 行为。

普通透明沿用各 collection 的阶段与稳定顺序；OIT 每个批次只生成一次几何，三组管线引用相同字节片段，水体遮罩单独提交。实体 outline 独立绘制到 RGBA8 目标，经原版 sobel、双向模糊和 blit 后，在第一人称之后混合到主场景。
第一人称沿用原版手持物和 ScreenEffectRenderer 的 CPU 提交，独立 HUD 投影及深度保留原版手臂摇晃和屏幕覆盖顺序，物品激活动画使用原版物品光照方向。
原版 submitFeatures 内的 Fabric 收集钩子继续执行；依赖原版 GPU execute 的世界绘制事件及 GUI mesh 钩子不执行。调试三维准星保留原版构造的持久线条几何和两次深度偏置绘制；线框能力由 native 可选 PSO 的实际创建结果提供，开关进入原版抽取状态及帧标志。
现有 GUI 管线按原版管线对象匹配，未知或同名替代变体明确抛错，不按 Identifier 复用状态或静默跳过。

VanillaPipelineCatalog 从 MC 26.3 的静态管线字段、OIT 集合和注册表枚举变体，PipelineIdentity 包含 shader、宏、顶点格式、拓扑、混合、深度及附件格式。
该目录保留同名物品的不同宏变体，未知对象明确拒绝；离线清单与动态绘制使用相同身份编号。

## 截图与纹理导出

ScreenshotGuardMixin 接管原版 `Screenshot.takeScreenshot`，保留原版 grab 的命名、消息及异步文件写入。
普通截图立即登记并保活最后提交的主颜色来源；世界预览沿用 GameRenderer 的原版触发条件和缩图保存回调，在当前世界/第一人称之后、轮廓/后处理/GUI 之前采集。
全景截图保留原版六个相机方向、4096 方形渲染、四倍下采样和最终恢复逻辑，renderLevel 改为仅世界内容的 native 离屏帧，交换链尺寸不变。
TakeoverScreenshots 在 fence 完成后轮询，将 CPU 图像转换和回调交给 IO 池。截图顶行在前、alpha 固定 255，下采样沿用方块平均。
TextureDumpMixin 接管原版 TextureUtil.writeAsPNG，按请求读回 RGBA8 mip，保留像素变换器和文件命名；纹理层/mip 由 native 读回协议验证，原版导出入口使用第零层。
关闭时提交待请求读回，随后等待并轮询；提交失败清空 Java 请求后进入资源关闭。

ScenePost 保留内置效果请求顺序；GUI 在原版模糊分界拆为两段，几何只上传一次。原版可选 end_of_frame 钩子在缺省资源中不产生绘制，资源包自定义钩子明确拒绝。GUI 准备期间切换到原版 UI 光照图，之后恢复世界光照图。
ShaderReloadMixin 在原版资源加载阶段拒绝由非 vanilla 包提供的 minecraft 命名空间 shader 和 post_effect，错误包含资源位置及包名；纹理、模型、字体资源不受该检查影响。

## 地形与帧事件

`SectionCompilerMixin` 在 worker 返回时复制顶点，`CompiledSectionMeshMixin` 绑定同一结果和关闭事件。
`RenderSectionIndicesMixin` 在 CPU staging 被接纳后发布透明索引快照；正常维护和追加失败后的直接排空都执行原回调。
`TakeoverTerrain` 使用当前可见列表与透明模式，重算最终世界投影；世界 Y 反射同时调整 native 正面绕序。
图集重载与世界重置推进资源 epoch，重新创建带当前 epoch 的编译器。已上传顶点数组在成功消费后释放，Native 承担面组与 AABB 处理。驻留选择、回退与上传寿命见[地形渲染](terrain-rendering.md)。

`MinecraftTakeoverMixin` 在真实 renderFrame 入口清理采集状态，以调用局部引用记录 `ClientFrameEvent`，支持递归加载帧。
`TerrainFrameEvent` 记录接管准备及 FFM 包络；benchmark 只在成功消费世界帧后推进 native 帧计数。
统计默认每 5 秒包含呈现、跳过、重建、获取超时、可见区块、编译队列、临时方块、Java 堆和 BufferPool 占用。

原版显式开启 Tracy 图像时，TakeoverTracyImages 保留单个在途请求及帧延迟计数；native 使用原版 TRACY_BLIT 离线程序缩放最终画面，完成后在复用的堆外字节区交给 Tracy。缩放目标由 RDG 池分配，管线和参数集按槽复用，图内绑定随槽清理。未启用该诊断时不分配缩略图或请求读回。

游戏正常帧不读回像素，仅显式截图或 Tracy 图像请求触发读回。交换链和上传所有权见[Native runtime](native-runtime.md)，诊断与测试覆盖见[构建与测试](../guide/build-test.md)。
