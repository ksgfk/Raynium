> - 适用: 查阅Remix 与 Radiance 场景输入源码调查的历史来源与调查上下文
> - 权威: 本文保存原调查或说明，不作为当前实现、外部项目现状或本次重新验证的保证
> - 锚点: `gradle.properties`, `build.gradle`, `doc/guide/dev-environment.md`

# Remix 与 Radiance 场景输入源码调查

- 原记录日期：2026-10-28
- 归档日期：2026-11-09
- 状态：经用户授权归档；原文按原日期理解，本次未重跑原调查、第三方模组或游戏验证
- 范围：地形、动态对象、材质、灯光、可见范围、数据寿命及源码定位表
- 来源：归档前工作树 `doc/architecture/integration-references.md 的“场景数据选择：2026-10-28 深入调查”节`

当前项目事实见[架构地图](../architecture/overview.md)，其他历史见[历史索引](README.md)。
下列原文的“当前”、行号、缓存路径和版本只表示来源页的调查上下文；浮动外部链接不代表归档时重新核实。
本次仅调整迁移后的文档链接，保留其余原文。

## 场景数据选择：2026-10-28 深入调查

本节回答两个项目怎样从 MC 客户端状态选出渲染输入：数据从哪里来、在哪里过滤、保留什么语义、何时重新提交、何时撤销。只追到数据进入 native 场景及其用途分类，不展开光追积分、降噪、GPU 同步和呈现算法。

阅读入口：[总体对照](#总体结论导出的是解释后的渲染场景)、[Remix 地形](#remix地形取数与逐面选择)、[Radiance 地形](#radiancesection-的重建对象与输入范围)、[对象拆分示例](#同一个-mc-对象如何被拆开)、[源码定位表](#本次调查的源码定位表)。

### 调查范围与基线

| 项目 | 本次读取 | 可核实的版本信息 |
| --- | --- | --- |
| NVIDIA RTX Remix Minecraft | `NVIDIAGameWorks/rtx-remix-minecraft` 的 `main`，2026-10-28 获取 | `gradle.properties` 为 0.1.0；`build.gradle` 目标 MC 1.21.1、Yarn 1.21.1+build.3；检出顶端提交日期为 2026-10-28 |
| Radiance | `Minecraft-Radiance/Radiance` 的 `main`，2026-10-28 获取 | `gradle.properties` 为 0.1.6-alpha、MC 1.21.4、Yarn 1.21.4+build.8，默认 `radianceUiBackend=opengl`；检出顶端提交日期为 2026-09-29 |
| MCVR | `Minecraft-Radiance/MCVR` 的 `main`，2026-10-28 获取 | Radiance 对应的 C++ renderer 源码；检出顶端提交日期为 2026-09-29 |

这里读取的是当日主分支，不把 Remix 的版本字符串 0.1.0 当成“与发布标签内容完全相同”的证明，也不宣称分别取得的 Radiance/MCVR 主分支已在本机完成配套构建。链接使用上游分支和文件路径，内容会随分支变化；日期、方法名和本文描述共同限定调查范围。本机阅读缓存为 `.gradle/research/remix`、`radiance`、`mcvr`，不参与分发。

源码主要依据见本节末尾的[定位表](#本次调查的源码定位表)。以下比较均指这些源码路径，不代表全部模组兼容性或实际性能。

### 总体结论：导出的是解释后的渲染场景

两个项目的世界主通路都没有把 MC 的完整世界对象、区块 NBT 或全部方块状态复制给 native，再让 native 重做 MC 模型系统。它们保留 Java 侧对模型、资源包、实体 renderer、动画和颜色的解释，向 native 交付几何、材质关联、对象位置/姿态、用途标记以及相机和环境状态。

区别在于解释发生的位置：

| 问题 | Remix | Radiance / MCVR |
| --- | --- | --- |
| 普通方块几何从哪里来 | 在原版 section 构建遍历中读取 `BakedModel` / `BakedQuad`；非 vanilla adapter 有 FRAPI 分支 | 接管 `SectionBuilder.build`，调用 MC 的方块/流体渲染器，让它们输出到 `PBRVertexConsumer` |
| Java 侧主要产物 | Remix 格式的 section 几何、灯光条目、遮挡摘要，随后合并成区块列 mesh pages | 按 `RenderLayer` 收集的 `BuiltBuffer`，附对象/区块标识、几何类别和格式描述 |
| 普通实体从哪里来 | 原版实体绘制现场换 capture provider；另外补采视锥外实体 | 自己组织选中的实体 renderer 调用，结果进入自定义 provider |
| 动态模型表达 | cuboid/刚性 part 尽量拆为共享局部 mesh + 每个 occurrence 的 transform/tint；另有瞬态几何 | 普通路径提交本帧已求值的顶点和对象位置；native 按稳定 key 管理本帧对象，另有预构建几何接口 |
| 材质语义 | 将 layer、sprite、透明度、玻璃/水例外及 LabPBR 转成 Remix 材质和实例分类 | 顶点标志、几何类别、纹理 ID、辅助贴图映射分别保留，由 native/shader 解释 |
| 光源来源 | 方块状态规则生成解析光；贴图发光另行处理；手持物、天空有独立路径 | `_s` 发光纹理区域和实际几何相交，形成面光源数据；天空参数另行输入 |
| 静态几何寿命 | mesh/instance 保留，变化时 add/update/remove；普通帧不全量重交地形 | section 资源保留，dirty 后重建；槽位、位置和 generation 校验异步结果 |
| 是否只取相机当前可见内容 | 否；已安装地形保留，普通实体有视锥外补采，但各种对象的边界不同 | 否；地形有额外近场补建，近实体放宽筛选，方块实体从整个 storage 的非空 section 索引取得 |

两条主链路可以概括为：

```text
Remix 地形：
MC section 重建遍历
  → BlockState / 邻居 / 模型随机种子 / BakedModel / 生物群系颜色
  → BlockQuadEmitter：选择表面 + 求值语义
  → DirectQuadWriter：分类、材质桶、标准顶点
  → SectionBlockData：几何 + 灯光 + 遮挡摘要
  → 队列 → 区块列分类 mesh pages → Remix mesh / retained instance

Radiance 地形：
MC section 脏状态 + 自有重建调度
  → ChunkRendererRegion
  → 接管的 SectionBuilder.build
  → MC 模型/流体渲染逻辑 → PBRVertexConsumer
  → RenderLayer → BuiltBuffer
  → ChunkProxy：描述数组 + 原生内存地址 + buffer lease
  → MCVR：持久 section 几何、材质关联、光源与场景记录
```

### Remix：地形取数与逐面选择

`SectionBuilderMixin` 没有取消整个原版 `build`。它在 HEAD 固定本次是否捕获、取得世界/区块有效性 token，并初始化线程局部 emitter；在原版逐方块调用 `isOpaqueFullCube` 的位置执行 `processBlock`；遇到 `MODEL` 类型再调用 `emitBlock`。捕获期间将原版后续 `getRenderType` 查询改成 `INVISIBLE`，使已经输出过的方块不再重复生成 GL 几何；流体调用另行重定向；RETURN 把完成的 section 数据入队。[R1]

这里的类型判断产生明确边界：空气不生成普通模型；`ENTITYBLOCK_ANIMATED` 交给方块实体 renderer；普通方块模型与 `FluidState` 分别处理。因此含水方块不是只能选“方块或水”之一，方块形状和流体形状有各自路径。

普通模型的选择步骤如下：[R2]

1. 用 `BlockState.getRenderingSeed(pos)` 得到位置相关随机种子；每次 `getQuads` 前重新设种子，保留加权/随机变体的一致性。
2. 将 `getModelOffset` 的偏移加到 section 局部原点，保留草、花、滴水石等位置扰动。
3. `BlockRenderManager.getModel(state)` 取得已烘焙模型，不能用方块外形一律替换成单位立方体。
4. 对六个方向先使用 `Block.shouldDrawSide` 判断相邻方块是否遮住该面；对无方向的 quad 集合也读取。筛选依据包含邻接状态，不是相机是否看到该面。
5. 读取 quad 的位置和 atlas UV；读取打包法线，接近零时由边叉积重建；通过 tint index 求 `BlockColors` 颜色。
6. 保留 sprite 标识和 sprite 在 atlas 中的 UV 边界，交给后续材质查找和采样边界处理。

`shouldDrawBlockSide` 的结果缓存按本方块状态、邻居状态和方向组织。它仍调用 MC 的侧面可见性规则；这不是把方块碰撞箱作为渲染几何，也不是导出所有内部接触面。[R2]

非 vanilla adapter 的 `FabricBakedModel` 调用 `emitBlockQuads`，以 `RemixRenderContext` 实现自己的 quad 收集，再写入同一 writer。源码证明存在 FRAPI 入口；这不等于已经证明所有 FRAPI 材质扩展或其他 renderer 修改都能无损保留。[R2][R3]

### Remix：流体不是普通方块网格的一个标记

`emitFluidQuads` 查询 Fabric `FluidRenderHandlerRegistry` 的 sprite 和颜色，但由自身代码构建流体面。它读取邻域流体状态、四角高度、上方同类流体、顶面覆盖情况及流速；熔岩使用 still/flow sprite 和流向 UV，水使用专门的水材质及世界平铺 UV。[R2]

几个具体选择说明它携带了比原版三角形更多的含义：

- 水顶面根据上方流体和覆盖测试决定是否输出，静止水还走局部顶面暴露判断；相关方块变化会扩大重建邻域。
- 水面的正反方向有分别写出的顶面 quad，不能把它概括为所有流体都只输出单面。
- 炼药锅这类由方块模型提供、但 sprite 是 `water_still` / `water_flow` 的面也转入专门水通路。
- 当前通路把 `isLava` 的反面当作 `isWater`，这是源码中的二分，不是对任意模组流体物性的完整描述。

上述规则属于该模组自己的内容解释；不能据此认为只要获得 MC 的最终顶点流，就自然保留了“这是一片水”的信息。[R2]

### Remix：真正输出哪些字段，怎样分组

`SectionBlockData` 包含的不只是网格：[R4]

| 字段 | 内容 | 后续用途 |
| --- | --- | --- |
| `sectionPos` | section 整数坐标 | 归属区块列、重建/卸载定位 |
| `meshData` | 按 category 和 material bucket 分组的表面 | 合并并创建 native mesh |
| `lights` | 规则解析出的 `LightEntry` 列表 | 在 render thread 建立、更新、销毁灯光 |
| 六个 face mask | 每个面一个 64 位开放掩码；8×8 格，每格覆盖 2×2 方块，存在非不透明方块即标记开放 | section 间连通传播 |
| `opacityBitmap` | 4096 个方块的不透明完整立方体信息，64 个 long | 从玩家所在位置作 section 内连通求解，为灯光选择提供种子 |

几何顶点采用 `HardcodedVertex` 的 64 字节 stride。地形 writer 实际写入位置、法线、UV、颜色，以及两组半精度打包的 sprite UV 最小/最大值；剩余位置保留布局填充。每四个顶点对应六个标准三角形索引。初始位置是 section 局部坐标，合并为列时加入 section 的纵向位置，列的 X/Z 再由 instance transform 表达。[R5][R6]

`DirectQuadWriter` 在单个方块内暂存 quad，检测共面覆盖关系及透明 sprite，再决定类别。类别为 solid、cutout、translucent、overlay。它会把实际不含透明像素的 cutout sprite 提升到 solid；会去除符合身份、颜色、平面和顶点条件的重复 cutout 背面；会把共面贴花与底面分开。类别因此不是对 MC `RenderLayer` 名称的简单原样拷贝。[R5]

透明材质还包含明确例外：原版清玻璃/玻璃板走 cutout 语义，Remix 为它们选择可折射的 `glass` 分类；水另有 bucket；存在 SSS 的 sprite 可进入不同材质桶。实体中的普通物品常通过 translucent layer 绘制，但不能据此把苹果等物品都解释为玻璃，`RemixEntitySubmitter.atlasSurfaceMode` 为物品和真实透明方块作了进一步区分。[R2][R5][R12]

section 并非最终唯一 native mesh 单位。`BlockMeshBuilder` 收集同一 X/Z 区块列的 section，按类别和材质桶合并成 pages，默认页面估计数据上限为 8 MiB，并允许配置调整。这个数字是该实现的页面拆分阈值，不是单 section 大小、GPU 总预算或性能测量值。[R6]

### Remix：几何集合、更新集合与灯光集合不同

地形采集的前提是客户端已经取得数据，并进入 MC 的 section 重建路径。模组保留 `setupTerrain` 等原版维护流程，也有重新捕获已有 section 的路径。它没有在本次主通路中读取服务端全部世界，也不能把“客户端加载”直接当成“已经完成 native 场景构建”。[R1][R7]

完成采集后，`ChunkManager` 的分类内容指纹决定哪些 mesh pages 需要替换；队列和异步列构建分别有预算、有效性检查、epoch/generation。创建或改变实例时向 Remix add/update，之后保留在 native 场景；`draw` 不因当前相机视锥而逐帧删掉所有未见地形，也不逐帧重交所有未变实例。`SectionLeftRenderSetQueue` 虽然名字有 render set，实际存放的是“原版发布空数据”的候选，移除前还要验证当前世界 section；不能把它当成视锥离开通知。[R8][R9]

灯光采用另一套集合。`ChunkManager.draw` 每帧依次判断：区块列是否在有限 X/Z 范围内、section 是否在连通集合内、灯光按功率和距离计算的权重是否达到阈值。`TerrainLight` 使用 `power / (4π × max(1, d²))`，对上一帧绘制过的灯降低退出阈值，形成滞回。上述开放掩码和 opacity bitmap 在这里服务于灯光剔除，不能据它们的存在就认定地形网格也按同一个 BFS 删除。[R8][R10]

这产生三种独立事实：几何可能已被捕获但仍在等待安装；网格可能保留而对应解析灯本帧被剔除；原版当前没有画某个 mesh，不表示它已经从 Remix 场景消失。

### Remix：实体、方块实体与第一人称

普通实体首先在原版 `WorldRenderer.render` 调用 entity dispatcher 的现场捕获，替换成 `RemixVertexConsumerProvider`。这样 MC 自身的 renderer 仍负责姿态、装备、模型特性和绘制层选择，结果被交给 Remix。捕获失败或未接管时还有原版调用分支。[R7][R11]

普通实体之外还有一轮 `captureOffFrustum`：遍历 `ClientWorld.getEntities()`，跳过已捕获 ID，在配置距离内挑最近的候选。补采使用 `256 - 已捕获数量` 的剩余预算；因此 **256 不能写成整个实体场景无条件的硬上限**，原版现场捕获已经超过该值时补采直接停止。当前帧拥有 Remix 灯光的实体可以越过距离测试，并优先于非灯光所有者进入候选；它仍受剩余容量和候选竞争约束，不能宣称任何数量的灯光所有者都必定被捕获。[R11]

远处视锥外实体可以复用姿态快照，位置仍每帧重算。复用只适用于可重建的 cuboid-backed parts；携带不能快照的烘焙物品/地图等几何时不强行复用部分姿态。这个机制区分了“对象位置更新”和“重新求值全部模型动画”。[R11]

实体几何进一步区分以下形式：[R12][R13]

| 形式 | 捕获内容 | 保留方式 |
| --- | --- | --- |
| `ModelPart.Cuboid` | cuboid 的 sides 身份、sprite、局部姿态矩阵、折合 tint/overlay | 拦截逐顶点展开；共享局部 mesh，按 owner/category/layer/part 保留 occurrence |
| 其他刚性 quad part | 已捕获顶点、拓扑、几何指纹及所在层 | 通过内容身份池化 mesh，实例表达姿态 |
| 绳索等 triangle strip、变化的 beam | 本帧几何 | 瞬态 mesh，跨帧退役 |
| 名牌/文字的 depth-tested 部分 | 字形纹理和可捕获 quad | 处理共面顺序；always-depth 的 see-through 副本不重复进入场景 |

`RenderLayerUtils` 的普通实体捕获接受 QUADS 和 TRIANGLE_STRIP；无纹理层有专门限制，例如保留带顶点色的 strip，不把所有无纹理名牌背景、线框或 water mask 一律变成会投影的世界表面。选中方块轮廓等还有独立 pass，这个限制不能外推为整个模组完全没有线框支持。[R14]

方块实体的边界与普通实体不同。`RemixBlockEntityPass` 捕获原版 dispatcher 调用；`WorldRendererMixin` 在原版传入的可见 chunk 列表内筛出含 block entity 的 section，dispatcher 自己的距离测试仍会运行。不能把普通实体的视锥外补采结论套到箱子、旗帜等方块实体上。[R7][R15]

第一人称由单独的 `RemixFirstPersonPass` 收集，用 view-model 分类、手部 FOV 修正及 camera-to-world 放置；另外存在本地玩家隐藏的投影/反射模型路径。世界相机和 view-model 相机各有提交，二者不是把一份手部顶点无条件放在世界原点。[R11][R16]

### Remix：纹理、材质与灯光的来源

`BlockTextureManager.initialize` 读取 MC 已拼接 atlas 的 sprite 清单和 `SpriteContents` 像素，在 CPU 重新组成 albedo atlas，同时建立按 sprite ID 的记录。它既产生 atlas 级材质/纹理，也产生每 sprite 的资源；不是从场景中逐块打开 PNG，更不是读取最终屏幕颜色去恢复材质。[R17]

它通过 MC `ResourceManager` 找资源包中的 `_n`、`_s`、`_sss` 附属图片，遵循资源系统所解析的资源来源。`LabPBRConverter` 将其转成 Remix 所需的数据，包含 normal、height、roughness、metallic、emissive、SSS 等；读取 `_n` 的 AO 并乘入 albedo。这里的贴图 AO 与原版逐顶点环境遮蔽不是同一个来源。当前转换中 metallic 对 `_s.g` 使用阈值二分，不能称为完整保留所有原始 LabPBR 参数。[R17][R18]

生命周期不止首次上传：

- atlas 动画上传被收集为局部 patch，`AtlasAnimationForwarder` 在统一提交点批量转发；对有附属贴图的动画保持 albedo 与相关通道的帧对应。
- `DynamicTextureCache` 记录 `NativeImageBackedTexture`、皮肤等动态像素来源，补足资源管理器找不到的运行时纹理。
- 字形页有独立 mirror；只扫描静态资源包不足以覆盖聊天名牌、地图、皮肤和动态文字。
- 资源重载需要重新解释 atlas、sprite 和材质关系，旧 UV/材质身份不能视为永久有效。[R17][R19]

解析灯光的决定来自 `BlockLightRuleManager`，不是“所有 `luminance > 0` 方块都生成一个一样的点光源”。规则按 canonical block state 建索引；支持资源包路径 `rtx_remix/block_lights`，包含方块、标签、状态属性等匹配。`processBlock` 只对命中规则的非空气方块解析灯条目；模型采样使用与几何一致的位置随机种子。[R2][R20]

内建例子包括普通/灵魂火把、带朝向的墙上火把、必须满足 `LIT` 的红石火把和营火、按蜡烛状态变化的灯，以及带等级的不可见 `LIGHT` 方块。规则携带颜色、偏移、形状和强度，可在渲染几何之外独立存在。灯光位置目前按未施加 model offset 的方块坐标求值，几何则会施加 offset；源码明确没有在这条通路中替任意带 offset 的自定义灯规则补上偏移。[R2][R20]

表面发光由 LabPBR/内建发光纹理规则控制；方块 atlas 的 LabPBR 转换不会仅因为原版 luminance 非零，就自动把全部表面当成 emissive。解析灯、发光材质和 MC lightmap 应分开理解。太阳、月亮、天空环境和手持灯也各有独立数据来源。[R17][R18][R20]

### Remix：相机、粒子、天空与天气

`RemixRenderer.updateCamera` 从 MC camera 取位置、yaw、pitch，构建基向量，读取实际世界 FOV 和 framebuffer 宽高比，提交 WORLD 与 VIEW_MODEL camera；还更新相机所在介质。当前这里将 MC double 位置转成 float 传入相机参数。这是该版本边界的事实，不是已经测得的大坐标精度结论。[R21]

`RemixParticleRenderer` 不复用每种粒子的任意自定义 renderer。它读取 particle manager 各 sheet 队列，跳过 `NO_RENDER`、`CUSTOM`、非 `BillboardParticle`、dead 或缺少可用材质的对象；获取粒子位置/插值、大小、旋转、颜色、alpha 和 UV，按材质/类别组成动态 quad bucket。terrain 粒子和普通粒子分类不同；煤烟类颜色还有专门处理。准备的 CPU payload 与 render-thread 提交分开，renderer 的双缓冲安排使该路径使用前一帧准备结果。[R21][R22]

天空不是从世界方块模型推出来的。`SkyRenderer` 使用维度天空类型、天体角度、星光与天气状态，生成/更新天空环境和天体资源；End 和无天空维度有不同处理。云既有程序生成路径，也有保留原版云周期/相位的 classic 路径。天气则捕获原版天气生成阶段的纹理和几何，单独交给 weather renderer。它们取用 MC 的时间和环境语义，并不要求所有内容都来自地形 atlas。[R7][R21][R23]

### Radiance：section 的重建对象与输入范围

`ChunkProxy` 管理需要重建的 `BuiltChunk`，使用自己的普通/重要任务池与提交预算，调用 MC `ChunkRendererRegionBuilder` 取得读取区域，再调用被接管的 `SectionBuilder.build`。[D1]

`SectionBuilderMixins.redirectBuild` 在 HEAD 替换整个实现，逐一处理 16×16×16 section：[D2]

1. 读取 `BlockState`；不透明完整方块写入 `ChunkOcclusionDataBuilder`。
2. 遇到 block entity，按原版分类加入当前 section 的 block-entity 或 no-culling 列表。
3. 非空 `FluidState` 选择流体层，调用 `BlockRenderManager.renderFluid`。
4. `MODEL` 方块选择方块层，用 section 局部平移的矩阵栈调用 `renderBlock`，保留 MC 模型求值和侧面检查。
5. 每层使用 `PBRVertexConsumer`，结束后得到 `BuiltBuffer`，返回各层 buffer 和遮挡连通结果。

所以它也不是复制方块 palette 给 C++，也不是从已提交到 GPU 的普通 GL VBO 读回几何；核心拦截点在 Java 生成顶点时。与 Remix 相比，地形更依赖 MC renderer 产出的顶点，而不是由独立 writer 自行解释所有 vanilla quad。

`BlockModelRendererMixins` 补充 tint/emission 输出；存在 `BlockColorEmissionProvider` 扩展入口，但不能仅凭接口存在就认为所有发光方块都使用它。默认 OpenGL UI 构建的 `OpenGLUiMinecraftClientMixins` 将原版 ambient occlusion 开关查询设为 false；同时顶点格式仍保留 light UV，world uniform 仍传 lightmap 纹理 ID。因此“关闭原版 AO”和“完全删除所有 MC 光照输入”是两件事。[D3][D4]

流体渲染器的替换实现保留邻域流体/方块、面覆盖、高度、流动纹理和生物群系水色计算，并显式写法线。水的物性还可以由辅助 flag 贴图标识；native Advanced 材质路径能读取该标记，不能只用普通透明 layer 代替水语义。[D5][D13]

### Radiance：选择哪些 section 来构建

默认非 Vista 完整列分支同时考虑三个来源：原版可见 section 列表、原版 nearby 列表、额外的光追近场 section 集合。近场取相机所在 section 的 X/Z ±4、Y ±2，受世界高度及 storage 实际覆盖限制；这是最多 9×9×5 的候选格子，不是“恒定保证半径 64 米内所有东西当帧齐全”。[D1]

候选还要满足 `needsRebuild`、构建就绪、没有同槽在途任务等条件，按距离排序，并受每帧提交量和在途数量限制。源码为近场候选预留一部分提交额度，有覆盖缺失后的 boost、进入世界的初始 boost，以及背景重建队列；重要候选包括非常近的 section 和原版标记的重要重建。此处选择的是“本帧值得做重建的工作”，不等于“本帧全部可渲染几何”。[D1]

Vista 距离启用后走完整列的候选逻辑，优先依据水平距离组织，并处理空 section 完成状态。Java 侧还会从 section 的 X/Z 边界采集 `VistaTerrain` 摘要；它是额外远景通路的输入，不能把整个 Radiance 简化成唯有一种普通三角形输入。[D1]

已经安装到 MCVR 的 section 在资源集合中保留。`chunk_render_state.comp` 遍历 native 提供的 chunk records，构造相机相对 transform 和 TLAS instance；本次所读代码没有把这个过程变成原版当前 frustum 列表的逐项回放。Vista 覆盖可以把普通 chunk 的 instance mask 置零，因此“资源仍在”与“当前参与光追”也必须分开。[N1]

### Radiance：传到 native 的具体数据

`ChunkProxy.rebuildSingleNative` 的输入包含：[D1][N2]

| 层级 | 字段 |
| --- | --- |
| section 身份 | origin X/Y/Z、`BuiltChunk.index`、generation、重要任务标记 |
| 几何批次 | 数量；每批 geometry type、group name（layer 名）、纹理 ID |
| 几何描述 | vertex format、index type、vertex count、顶点和可选索引地址 |
| 异步寿命 | `TransferredBuildBuffer[]` |
| 可选摘要 | Vista terrain count/address |

section 的地形顶点是 40 字节 `ChunkPBRVertex`；实体顶点是 64 字节 `EntityPBRVertex`。Java writer 和 C++ `shared.hpp` 的布局/断言相互对应：[D6][N3]

| 字段 | 地形 | 实体 |
| --- | --- | --- |
| Position | 3×float，section 局部 | 3×float；所属坐标系单独标记 |
| Normal | 32 位八面体编码 | 同左 |
| Texture UV | 2×float | 同左 |
| Color | RGBA8 | 同左 |
| Light UV | 两个 16 位量打包 | 同左 |
| Texture ID | 有 | 有 |
| Packed data | 是否有色/纹理/法线/light、alpha mode、坐标系、附加 emission 等位域 | 同类位域，并含实体所需语义 |
| Overlay UV | 无 | 有 |
| Glint UV / texture | 无 | 有 |
| Post base | 无 | 3×float，供后续绘制路径使用 |

`GeometryTypes` 区分 world solid、world transparent、world no-reflect、cloud、boat water mask、End portal/gateway 等。`PBRVertexConsumer` 另有 opaque/cutout/transparent 的 alpha mode；不能把枚举 `WORLD_TRANSPARENT` 直接解释成每个像素都半透明，它和 alpha-test 位域是不同层次的信息。[D6][D7]

native 对满足格式与 lease 条件的输入保留外部 PBR 顶点，其他路径转换/复制为内部格式；标准 quad 索引可以生成，而不必从 Java 全量复制。`JavaBuildBufferLease` 用 JNI global reference 维持 transferred buffer，最后释放时回调 `close`。这里是跨语言 CPU 输入的保活；GPU staging/upload 仍存在，不能把整条路径称为端到端零复制。[N2][N4]

### Radiance：实体与特殊对象的选择

`WorldRendererMixins.redirectRender` 自己编排世界阶段，同时显式继续 queued chunk update、light update、`setupTerrain`、dispatcher 配置和 Fabric world render events 等维护。它通过原版 `getEntitiesToRender` 得到实体列表，但重定向两个判断：将第三人称检查视为 true，使本地玩家身体有机会进入集合；距离相机小于 24 的实体放宽 `shouldRender`，更远的继续交给 dispatcher 的原判断。这不是取消所有实体的所有筛选。[D8]

`EntityProxy.queueEntitiesBuild` 用 tick delta 插值得到对象位置，给 renderer 提供局部原点和自定义 provider，收集模型顶点。对象 key 来自实体 ID 的混合；block entity、方块破坏、轮廓等有不同 key 前缀，防止类别混同。批量提交还包括 ray-tracing mask、post-render flag、geometry type、draw mode、纹理和顶点格式等。[D9]

`StorageVertexConsumerProvider` 对 QUADS 使用 PBR writer，对其他模式保留普通 `BufferBuilder`。常规处理显式接纳 QUADS、TRIANGLE_STRIP、LINE_STRIP、LINES；其他 draw mode 不能仅因 enum 中有定义就认定这条通路已支持。[D9][D10]

各类对象实际去向如下：[D9]

| 内容 | 数据来源与选择 | 输出用途 |
| --- | --- | --- |
| 普通实体/装备/物品 | 入选实体的 MC renderer 和 layer | 世界几何；本地 player 与 fishing bobber 有独立 mask |
| 方块实体 | `BlockEntitySectionCache` 从整个 `BuiltChunkStorage.chunks` 索引含实体的 section；加 no-culling 集合并按 key 去重；dispatcher 仍会检查自身适用条件 | 世界几何；独立于普通方块模型 |
| 第一人称手和物品 | 首人称、非睡眠、HUD 未隐藏、非旁观等条件；held-item renderer | CAMERA 坐标、HAND mask，带手部投影比例 |
| TextDisplay、实体名称文字 | 独立 provider 收集 | TEXT / NAME_TAG post-render 通路，不与实体身体共用普通世界用途 |
| 普通 sheet 粒子 | particle manager 队列；调用粒子自身 `render`；按 content name 分组 | CAMERA_SHIFT 坐标、PARTICLE post-render |
| CUSTOM 粒子 | 调用 `renderCustom` | 另一份世界几何输入，可带 PARTICLE ray-tracing mask |
| 方块破坏裂纹 | breaking progression + 对应模型/方块实体 renderer | 独立数据，可施加法线方向偏移 |
| 目标方块轮廓 | crosshair target 和方块形状 | 独立轮廓输入 |
| 天气和 world border | 各自 renderer | 单独收集和用途分类 |

这张表中的“已导出”不等于“每一类都进入同一个 TLAS、都投影、都参与反射”。例如普通粒子和文本选择 post-render，CUSTOM 粒子另走世界路径；mask 则允许不同射线查询包含不同对象。MCVR Advanced 的直射光阴影路径就会依据当前表面是否为手部来决定是否包含 PLAYER mask。[N5]

普通动态顶点路径每帧重新收集并提交，native 按 `entityKey` 查找实体、记录当前帧活跃状态并安排几何资源；没有在本帧出现的缓存对象会退出当前集合。`prebuiltBLAS`、共享几何和 shader-program 实例有另外的接口/分支，不能把这些优化直接算成所有原版 mob 已采用的输入模式，也不能说 Radiance 完全没有实例化能力。[N6]

### Radiance：纹理不是只有一张 albedo atlas

默认 OpenGL UI 变体仍在 `NativeImage.uploadInternal` 附近取得上传源指针、图像尺寸、源矩形、目标矩形、mip level、像素通道和 texture generation，然后向 native 排队。原版 GL UI 使用自己的图像，native 世界取得镜像数据。atlas 上传和 tick 包在 batch 中，`SpriteContents` 负责把目标纹理身份传到对应 image；关闭 image 之前要 flush 未消费上传。[D11]

`TextureTracker` 维护资源 Identifier→纹理 ID、ID→纹理信息，以及基础纹理到 specular/normal/flag 辅助纹理的绑定。绑定带 base/auxiliary generation，防止释放后复用同一数字 ID 时继续引用旧资源。纹理 ID 在这里是 renderer 的资源关联键，不能理解为把一个 GL 纹理对象直接当 Vulkan 图像使用。[D12]

`AuxiliaryTextures` 读取 `_s`、`_n`、`_f`：前两类为 specular 与 normal，后一类保存额外材质标记。辅助图片和基础 sprite 对齐上传，基础图片存在时避免把辅助图片当普通 atlas sprite 重复纳入。没有辅助数据时使用各自默认清除值。MCVR 的 Advanced 路径会读取 flag 纹理 red 通道的水标志；实际材质因此不只取决于 layer 名和 alpha。[D13]

`BufferProxy.updateMapping` 把这些 ID 关联传给 native。动画更新、资源重载、动态纹理、字体等分别有资源 tracker/hook，数据选择同时包含“哪个资源”与“哪个资源版本”。[D4][D11][D12]

### Radiance：从纹理发光区域得到几何光源

这条通路分成 Java 的二维摘要与 native 的三维关联，避免仅用方块中心代替发光表面。[D14][N7]

Java `EmissionRecorder.buildTileUpdate` 在基础图像和 specular 图像的上传区域内：

1. 读取 albedo alpha，透明像素不贡献覆盖。
2. 解码 `_s` 的 alpha：255 表示不在此编码中发光，其他值按 alpha/254 取强度。
3. 在细分格中统计发光权重、加权颜色及有效像素包围矩形。
4. 合并稠密/相邻格，得到一个或多个 `EmissionCell`：UV 矩形、平均发光、平均 RGB。
5. 使用 texture ID 与上传区域的 tile key 管理替换，动画/局部更新可以更新区域摘要。

没有 specular 图像时，该函数返回空发光 cells；这不排除其他材质/天空路径，但不能宣称它从任何原版亮度值自动恢复全部物理光源。

native `ChunkBuildData.buildLightInfos` 再逐个实际 quad：

1. 确认顶点有纹理，取得 texture ID 和 quad UV 范围。
2. 用纹理发光占用摘要快速排除无交集的面。
3. 取得相交 `EmissionCell`，把 quad 的两个 UV 三角形裁剪到 cell 的矩形。
4. 将裁剪多边形映回实际三维三角形，插值顶点 tint。
5. 生成光源几何点、法线、面积、颜色/辐射强度和稳定 ID；辐射量包含 cell 平均颜色、发光强度和顶点 tint。

因此贴图上一小块发光区域可以只让对应的模型区域成为采样光源，而不是让整个方块六面、整个 quad 或整个 atlas 都变成灯。这里采用区域平均和合并摘要，并不保存每一个发光 texel 为独立灯。实体也有相应 `EntityBuildInput::buildLightInfos`，并与实际实例位置/几何关联。功能受 `collectChunkEmission` 等选项控制；本次源码中该选项默认开启。[D14][N7]

### Radiance：相机、环境和少量世界语义数据

`WorldRendererMixins` / `BufferProxy` 提交当前 view、受效果影响的 view、projection、游戏时间、附魔纹理矩阵、第一人称状态、雾范围/颜色/形状、维度天空类型、世界高度范围、lightmap、末地天空/传送门纹理以及渲染距离等。相机位置另经 `PlayerProxy` 交接；native 维护所需历史信息。[D4][D8]

sky uniform 包含天空/地平线颜色、太阳方向、天空类型、日出日落、黑暗/失明、相机浸没介质、月相、降雨和太阳/月亮纹理 ID。它们直接来自世界/相机/维度状态，不是从当前可见 mesh 猜测。[D4][D8]

`RainExposure` 另外建立以区块为 tile 的 512×512 图，保存 `MOTION_BLOCKING` 高度、当前位置生物群系是否下雨和有效标记；以脏 tile 和每帧预算更新，并保持雨后湿润状态的时间变化。这个例子说明即使主输入为 mesh，renderer 仍会选择少量原始世界语义摘要；它不需要为此完整镜像 MC 世界。[D15]

MCVR 的 chunk instance 变换使用整数相机 base 加小数 fraction，并减去区块整数原点；既有本帧也有上一帧数据。实体对象位置由 Java 的 double 数组传入，而顶点保持局部 float。坐标精度与历史状态属于实际输入契约的一部分，不能只记录一份 float 世界顶点数组就概括其数据模型。[N1][N3]

### 数据失效与内容边界对照

| 事件或边界 | Remix | Radiance / MCVR |
| --- | --- | --- |
| 方块/邻域变化 | MC section 重建触发重新采集；分类指纹用于减少无关页面替换 | 标脏→候选调度→section buffers→native 更新 |
| 单纯对象移动 | 保留 part mesh，更新实例/位置；可复用部分远距离姿态 | 普通路径本帧重新求值顶点并提交位置；预构建路径另行复用 |
| 区块卸载/槽位复用 | capture token、world generation、chunk incarnation、列构建 epoch/generation 拒绝旧结果并撤销资源 | slot index 不独自代表身份；同时核对 origin、generation、world/source chunk |
| 旧任务完成 | 不得替代新世界/新 incarnation；丢弃其拥有的 CPU 数据 | Java 发布前及 C++ 接收/安装时有当前性检查 |
| 纹理动画 | atlas 局部 patch 与相关 PBR 更新 | upload batch + auxiliary patch；发光摘要按区域更新 |
| 资源包重载 | atlas、sprite、材质与场景需要重新建立对应 | texture generations、辅助绑定、相关 rebuild 路径 |
| 当前帧未见对象 | 地形 retained instance 不等于每帧重交；动态 part 按 frame stamp 管理 | 动态对象每帧形成 active set，未再次 queue 的缓存退出；地形持久 |
| 从未加载/从未完成构建的区域 | 主场景通路没有完整服务器世界副本 | 主 section 通路同样受客户端数据与构建完成状态限制；Vista 等另有输入 |

上述机制分别见 `BlockDataQueue` / `ChunkManager`、`ChunkProxy.isCurrentBuild` / `isBuildSourceLoaded`、MCVR `isBuildDataCurrentLocked` / `relocateChunk`、实体 frame 管理和纹理 generation 代码。[R8][R9][R12][D1][D12][N2][N6]

### 同一个 MC 对象如何被拆开

| MC 内容 | Remix 的实际拆分 | Radiance 的实际拆分 |
| --- | --- | --- |
| 一块草方块 | 邻接判断后的模型面 + 生物群系 tint + sprite/atlas + 可能的共面 overlay 分类 | 模型 renderer 输出的 layer 顶点 + tint + 纹理/辅助映射 |
| 一支亮着的墙上火把 | 有朝向的模型 + 命中状态规则的偏移解析灯 + 贴图自身的发光数据 | 模型面；存在对应 `_s` 发光数据时生成区域摘要，native 关联 UV 生成对应面光源 |
| 一扇玻璃/玻璃板 | 模型几何 + 额外 glass 分类，重新解释原版 cutout | 模型几何 + alpha mode、几何类别及材质数据；不能只凭 layer 名断定最终光学行为 |
| 一片水 | 邻域/高度决定的流体表面 + 专门水材质和世界 UV；相机介质另行更新 | 带法线的流体顶点 + 辅助水标记；浸没状态在环境输入中 |
| 一个正在开盖的箱子 | 方块实体 renderer + cuboid sides/sprite + part pose；受方块实体捕获范围约束 | block-entity renderer 当帧输出 + 位置/key；从 storage 的含实体 section 索引选取 |
| 玩家手持火把 | 第一人称 view model、玩家投影模型、手持灯及效果分别处理 | 手部 CAMERA/HAND 数据与玩家 PLAYER 数据分开，相关发光依赖材质/几何路径 |
| 一个粒子 | 可接受的 billboard 数据构造 quad；CUSTOM 不在此通路 | 普通 particle render 到 post；CUSTOM 另走世界捕获 |
| 一块告示牌/名称文字 | 字形纹理、刚性/文字 quad、共面顺序及 see-through 过滤 | 文本和身体可分离，名称/TextDisplay 进入专用 post 路径 |

这几种拆分直接展示了数据选择的粒度：方块或实体是源对象，native 场景对象可以是它的某一组表面、某个动态部件、一盏灯或一项覆盖效果。一个源对象不必对应一个 mesh，也不必对应一种渲染用途。

### 与 Raynium 当前状态的关系

该次调查没有加入场景采集接口。Raynium 后续实现的 SOLID 地形预览、共享输入及其边界见[地形预览](../architecture/terrain-preview.md)；第三方源码调查不代表本项目已实现相同接口。

版本边界已有实际源码差异：Raynium 的 MC 26.3 `SectionCompiler.compile` 使用 `BlockStateModelSet`、模型 quad output 和流体输出；`DefaultVertexFormat.BLOCK` 有 Position、Color、UV0、UV2，没有 Normal。动态对象经状态提取、submit nodes 和 feature dispatcher 组织。两模组旧版的 `SectionBuilder` / `VertexConsumerProvider` 注入点与顶点假设不能当成 26.3 已有接口。具体源码定位与生成方式仍由 [Minecraft 渲染流程](../architecture/minecraft-rendering.md)维护。

本节没有运行第三方模组、进行视觉对照、验证所有资源包/模组组合，也没有从源码推算实际帧率。特别没有把源码中的有界队列、预算、缓存或实例复用直接写成性能提升的实测结论。

### 本次调查的源码定位表

以下均为上游公开主分支文件；方法名优先于行号。R 表示 Remix，D 表示 Radiance Java，N 表示 MCVR。同组多文件的其他入口在对应文件所在 package 中，可按正文给出的类名定位。

| 引用 | 文件、方法与核对内容 |
| --- | --- |
| [R1] | `mixin/SectionBuilderMixin.java`：HEAD/逐方块/流体/RETURN 捕获边界 |
| [R2] | `terrain/capture/BlockQuadEmitter.java`：`processBlock`、`emitBlock`、`emitVanillaBlock`、`stageBakedQuad`、`emitFrapiBlock`、`emitFluidQuads`、`shouldDrawBlockSide`、`classifyBlockLayer` |
| [R3] | `terrain/capture/RemixRenderContext.java`：FRAPI quad 收集、transform/cull/tint |
| [R4] | `terrain/capture/SectionBlockData.java`，同目录 `SectionMeshData.java`：section payload 与几何寿命 |
| [R5] | `terrain/capture/DirectQuadWriter.java`：`writeQuad`、`flushBlock`、`classifyStagedQuads`、`discardDuplicateCutoutBackfaces` |
| [R6] | `terrain/BlockMeshBuilder.java`：`buildColumnMesh`、`PageAccumulator`、分类 pages 与局部坐标合并 |
| [R7] | `mixin/WorldRendererMixin.java`：现场实体/方块实体/天气捕获及原版维护边界 |
| [R8] | `terrain/ChunkManager.java`：`updateVisibleChunks`、`submitDirtyBuilds`、`installReadyMeshes`、`draw`；同目录 `SectionLeftRenderSetQueue.java` |
| [R9] | `terrain/capture/BlockDataQueue.java`：`CaptureToken`、队列上限、旧结果拒绝；`asset/RemixInstance.java`：retained add/update/remove |
| [R10] | `terrain/TerrainState.java`：灯光连通集合；同目录 `TerrainLight.java`：功率/距离/滞回 |
| [R11] | `entity/RemixEntityPass.java`：`captureAndSubmit`、`captureOffFrustum`、`submitOffFrustum`、`snapshotPose` |
| [R12] | `entity/RemixEntitySubmitter.java`：刚性 mesh 池、retained part、瞬态 mesh、atlas material 解释 |
| [R13] | `mixin/CuboidMixin.java`：cuboid 身份、sprite、姿态和颜色；`entity/RemixCapturingConsumer.java` |
| [R14] | `entity/RenderLayerUtils.java`：capturable texture、topology、无纹理层、surface mode |
| [R15] | `entity/RemixBlockEntityPass.java`：`capture`、dispatcher 边界 |
| [R16] | `entity/RemixFirstPersonPass.java`：第一人称专门捕获/放置 |
| [R17] | `texture/BlockTextureManager.java`：`initialize`、`captureAtlas`、`buildSprites`、`buildAtlasMaterials`、`stitchAtlasRGBA` |
| [R18] | `texture/LabPBRConverter.java`：通道转换、AO、发光 fallback |
| [R19] | `texture/AtlasAnimationForwarder.java`；同目录 `DynamicTextureCache.java`、`GlyphPageMirror.java` |
| [R20] | `light/BlockLightRuleManager.java`；同目录 `BlockLightDefaults.java`、`LightEntry.java`、`LightManager.java` |
| [R21] | `RemixRenderer.java`：`updateCamera`、`updateCameraMedium`、逐帧编排 |
| [R22] | `particle/RemixParticleRenderer.java`：`prepare`、`submit`、bucket；`particle/ParticleTint.java` |
| [R23] | `sky/SkyRenderer.java`；`cloud/CloudRenderer.java`、`weather/RemixWeatherRenderer.java` |
| [D1] | `client/proxy/world/ChunkProxy.java`：`rebuild`、近场/Vista 候选、`rebuildSingle`、`buildRenderData`、`isCurrentBuild` |
| [D2] | `mixins/vulkan_render_integration/SectionBuilderMixins.java`：完整 section 生成流程 |
| [D3] | `mixins/vulkan_render_integration/BlockModelRendererMixins.java`；`mixins/opengl_ui/OpenGLUiMinecraftClientMixins.java` |
| [D4] | `client/proxy/vulkan/BufferProxy.java`：world/sky uniform、辅助纹理 mapping |
| [D5] | `mixins/vulkan_render_integration/FluidRendererMixins.java`：流体几何与法线 |
| [D6] | `client/vertex/PBRVertexConsumer.java`；同目录 `PBRVertexFormats.java`、`PBRVertexFormatElements.java` |
| [D7] | `client/constant/Constants.java`：geometry、coordinate、ray-tracing、post 分类 |
| [D8] | `mixins/vulkan_render_integration/WorldRendererMixins.java`：实体筛选和世界编排 |
| [D9] | `client/proxy/world/EntityProxy.java`：实体、方块实体、手、粒子、文本、天气和批量提交 |
| [D10] | `client/vertex/StorageVertexConsumerProvider.java`；`client/proxy/world/BlockEntitySectionCache.java` |
| [D11] | `mixins/opengl_ui/OpenGLUiNativeImageMixins.java`；`mixins/vanilla_resource_tracker/SpriteContentsMixins.java`、`SpriteAtlasTextureMixins.java` |
| [D12] | `client/texture/TextureTracker.java`；`client/proxy/vulkan/TextureProxy.java` |
| [D13] | `client/texture/AuxiliaryTextures.java`；MCVR Advanced `scene/materials/any_hit.glsl`、`scene/surface.glsl` |
| [D14] | `client/texture/EmissionRecorder.java`：`buildTileUpdate`、`buildRegionCells`、`decodeLabPbrEmission` |
| [D15] | `client/texture/RainExposure.java`：高度/降雨摘要和湿润时间变化 |
| [N1] | `src/shader/world/ray_tracing/chunk_render_state.comp`；`WorldPrepareContext::prepareRayTracingBuffers` |
| [N2] | `src/core/middleware/com_radiance_client_proxy_world_ChunkProxy.cpp`；`Chunks::queueChunkBuild`、`relocateChunk` |
| [N3] | `src/common/shared.hpp`：`ChunkPBRVertex`、`EntityPBRVertex` 与尺寸断言 |
| [N4] | `src/core/middleware/java_build_buffer_lease.cpp`；`src/core/render/build_geometry.hpp` |
| [N5] | `src/shader/world/ray_tracing/internal/advanced/path/direct.glsl`：阴影查询 mask |
| [N6] | `src/core/render/entities.cpp`：`queueBuild`、`registerPrebuiltGeometry`、当前帧实体集合和资源更新 |
| [N7] | `src/core/render/chunks.cpp`：`ChunkBuildData::buildLightInfos`；`entities.cpp`：`EntityBuildInput::buildLightInfos` |

[R1]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/mixin/SectionBuilderMixin.java
[R2]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/capture/BlockQuadEmitter.java
[R3]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/capture/RemixRenderContext.java
[R4]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/capture/SectionBlockData.java
[R5]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/capture/DirectQuadWriter.java
[R6]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/BlockMeshBuilder.java
[R7]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/mixin/WorldRendererMixin.java
[R8]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/ChunkManager.java
[R9]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/capture/BlockDataQueue.java
[R10]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/terrain/TerrainState.java
[R11]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/entity/RemixEntityPass.java
[R12]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/entity/RemixEntitySubmitter.java
[R13]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/mixin/CuboidMixin.java
[R14]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/entity/RenderLayerUtils.java
[R15]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/entity/RemixBlockEntityPass.java
[R16]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/entity/RemixFirstPersonPass.java
[R17]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/texture/BlockTextureManager.java
[R18]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/texture/LabPBRConverter.java
[R19]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/texture/AtlasAnimationForwarder.java
[R20]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/light/BlockLightRuleManager.java
[R21]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/RemixRenderer.java
[R22]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/particle/RemixParticleRenderer.java
[R23]: https://github.com/NVIDIAGameWorks/rtx-remix-minecraft/blob/main/src/main/java/com/nvidia/rtxremix/minecraft/sky/SkyRenderer.java
[D1]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/proxy/world/ChunkProxy.java
[D2]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/mixins/vulkan_render_integration/SectionBuilderMixins.java
[D3]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/mixins/vulkan_render_integration/BlockModelRendererMixins.java
[D4]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/proxy/vulkan/BufferProxy.java
[D5]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/mixins/vulkan_render_integration/FluidRendererMixins.java
[D6]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/vertex/PBRVertexConsumer.java
[D7]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/constant/Constants.java
[D8]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/mixins/vulkan_render_integration/WorldRendererMixins.java
[D9]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/proxy/world/EntityProxy.java
[D10]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/vertex/StorageVertexConsumerProvider.java
[D11]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/mixins/opengl_ui/OpenGLUiNativeImageMixins.java
[D12]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/texture/TextureTracker.java
[D13]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/texture/AuxiliaryTextures.java
[D14]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/texture/EmissionRecorder.java
[D15]: https://github.com/Minecraft-Radiance/Radiance/blob/main/src/main/java/com/radiance/client/texture/RainExposure.java
[N1]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/shader/world/ray_tracing/chunk_render_state.comp
[N2]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/core/middleware/com_radiance_client_proxy_world_ChunkProxy.cpp
[N3]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/common/shared.hpp
[N4]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/core/middleware/java_build_buffer_lease.cpp
[N5]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/shader/world/ray_tracing/internal/advanced/path/direct.glsl
[N6]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/core/render/entities.cpp
[N7]: https://github.com/Minecraft-Radiance/MCVR/blob/main/src/core/render/chunks.cpp
