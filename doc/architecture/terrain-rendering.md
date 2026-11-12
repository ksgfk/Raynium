> - 适用: 修改三层地形采集、native 光栅、上传纹理及其生命周期
> - 权威: 本文记录地形采集、绘制与上传协议；设备与交换链见 native-runtime，通用加载见 interop
> - 锚点: `native/src/renderer.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/shaders/terrain.hlsl`, `src/client/java/dev/raynium/client/terrain/*.java`, `native/include/raynium/bridge.h`

# 区块地形渲染

接管 MC 26.3 的 SOLID/CUTOUT/TRANSLUCENT，并在地形之后绘制 GUI，输出为全分辨率 BGRA8 窗口。
透明模式跟随当前帧；普通透明与八系数小波 OIT 可切换。实体、粒子、天空、天气等能力边界见[Minecraft 接入](minecraft-integration.md)。

## 采集与采用

SectionCompilerMixin 在 MC MeshData 释放前复制原始 28 字节 quad 流，不借用编译 arena，不做 Java 分组和 AABB 计算。
SectionPayload 表示同一 compiled section 的三层可选 MeshPayload；只采集 MC 已采用且顶点/索引就绪的层。
RenderSectionIndicesMixin 将透明 uint16/uint32 索引复制并归一为 uint32，在对应原版上传回调确认采用后才发布快照。
透明索引校验使用固定 vertexCount，上传后释放顶点数组不影响后续排序更新。

TerrainCapture 复用扁平数组，采集 section 原点、淡入、层就绪位、顺序键、最终投影、视图及相机整数/小数坐标。
投影包含受伤、摇晃与反胃变换；每帧只传数据，由 Native 进行朝向剔除。
普通透明保持原版反向 section 顺序；其他层使用距离桶及状态排序。
缩减、帧重置、失效和关闭时释放快照引用；关闭的 section 进入延迟回收队列。

## 驻留与顶点释放

Java TerrainResidency 保留 8 MiB 软预算，按整个 section 的三层及初始透明索引原子采用。
超预算时优先近处，同坐标新版本未采用时回退到旧驻留 section 的完整层集合；首个超大 section 可独自推进。
已驻留透明排序索引更新优先，可突破软预算；只更新索引，不重新上传顶点。
当前可见或回退 section 保持活跃，Native 算出的零面组掩码不触发 Java 回收。

RN_STATUS_OK（含跳过呈现、需要重建）后 commit 驻留/索引/移除，并将已消费 MeshPayload.vertices 置空；保留 vertexCount、layer 和索引元数据。
尚未选入上传的顶点继续保留。FAILED 不承诺回滚，关闭 renderer 并向上传播，不在原 renderer 重试。
仅交换链 resize 保留 renderer、驻留几何和纹理。
关卡或图集几何失效沿用 MC 26.3 LevelRenderer.invalidateCompiledGeometry，由注入在新 SectionCompiler 创建前推进 epoch。
显式 renderer 替换也触发同一重新编译流程；旧 epoch payload 被忽略，已释放的顶点不重传。
GUI 帧也同步采集 epoch，因此退出世界后的下一帧会清空 Java 驻留并让 Native 退役旧几何。

## Native 分组与保守剔除

SOLID/CUTOUT 在上传期检查精确轴向平面及两个三角形绕序，按六轴向加 general 重排；组内顺序和全部顶点字节保持。
按实际顶点计算 AABB，允许模型超出 section；非平面、退化、绕序不一致进入 general，非有限数据保守可见。
非空组映射到紧凑 submesh 位。TRANSLUCENT 保持原 quad 顺序与单 submesh。
共享 Quad 索引由 Native 管理；透明索引归对应 mesh，mesh 删除时同时退役。

Native 每帧一次求逆 projection × view，变换 (0,0,1,0) 得到有效视点偏移。
不可逆、非有限或 abs(w)<=1e-8 时关闭面组剔除。相对相机坐标先做整数减法，保留大世界精度。
按实际 AABB 及 0.001 容差计算轴向组；general 始终保留。多实例取可见组并集，再与显式请求掩码和有效元素位求交。
计算得到零掩码正常跳过绘制；显式非法掩码失败。Native 寿命与批次规则见[Native runtime](native-runtime.md)。

## 绘制与 OIT

renderer 创建时由 `raynium.terrain.submission=auto|direct|indirect` 选择专用提交模式，默认 auto。设备条件、错误和帧槽契约见 [Native runtime](native-runtime.md#提交模式与参数)。

[Shader pack](shader-pack.md) 提供离线 DXIL/SPIR-V 程序。Group 0 保存每槽 Frame，Group 1 保存图集、lightmap 和 sampler；Group 2 保存每槽 OIT 阶段输入。
驻留绘制模板不保存瞬态 view。每槽命令 epoch 回收后重置 OIT 参数集，再在 RDG 执行时绑定当前 view。
模式切换选择已有 pass，不重新加载包、创建材质或上传顶点。仅复用 MC 可见列表，没有额外 GPU 可见性或异步计算队列。

先绘制 SOLID，再绘制 CUTOUT。二者使用反向 Z（clear=0、GreaterEqual）、背面剔除和深度写入；CUTOUT 丢弃 alpha<0.5 的片元。
图集过滤、RGSS、各向异性、lightmap、雾与淡入按原版 terrain shader 执行。
普通透明丢弃 alpha<0.1，RGB 使用 SrcAlpha/OneMinusSrcAlpha，alpha 使用 One/OneMinusSrcAlpha，GreaterEqual 且写深度。
此 pass 使用 DepthFirst 和唯一顺序键，禁用元素合并；其它地形 pass 使用专用参数构建器的连续面组合并。

改进透明度使用 MC 26.3 LevelRenderer、RenderPipelines 与内置 oit shader 的八系数小波规则：

| 阶段 | 附件与行为 |
| --- | --- |
| 深度范围 | RGBA32_FLOAT，清为 (-Float.MAX_VALUE,0,0,0)，MAX 混合记录范围、最近与近不透明深度 |
| 范围裁剪 | 复制到另一张 RGBA32_FLOAT，再按近不透明深度裁剪范围并更新深度附件 |
| 透射率 | 两张清零 RGBA16_FLOAT，加法累积八个小波系数 |
| 颜色累积 | 清零 RGBA16_FLOAT，根据透射率累积预乘颜色，在预乘空间施雾 |
| 合成 | 覆盖率归一化后以预乘 alpha 混合到输出，并写最近深度 |

三次透明几何阶段共用一次 StateFirst 稳定排序生成的参数数组和批次表，各自绑定管线及 OIT 参数；变换、采样及 alpha 阈值相同，前两次省略 lightmap 和颜色计算。
无可见透明地形时跳过整条透明路径。OIT 中间附件每槽约 56 字节/像素，由 RDG 瞬态池按尺寸复用。
世界帧设置 Y 反射标志；native 对裁剪坐标和正面绕序一起反射，保持各层的剔除语义。

## 纹理、ABI 与测试

TakeoverAtlas 在 CPU 拼接图集与 mip/padding，动画只上传变化区域；CPU lightmap 使用 MC 26.3 规则计算。
ABI 17 在一次 present_frame 传递网格、透明索引、可见项、全局纹理参数、相机和 GUI。结构及指针借用见[互操作与加载](interop.md#abi-17-布局)。

benchmark 活跃或 JVM 属性 raynium.terrain.profile=true 时启用 GPU timestamp；native 每 120 样本输出阶段统计，并向 Tracy 发布 GPU 毫秒样本。
JFR TerrainFrame/ClientFrame 记录 Java 准备、FFM 包络及客户端帧；操作入口见[构建与测试](../guide/build-test.md)。
C++ 测试覆盖网格重排、AABB、非有限值、视点/多实例剔除及排序参数；Java 测试覆盖 FFM、预算、回退和释放。
nativeTest 覆盖双后端交换链提交、纹理上传、OIT/普通透明切换、GUI 与窗口状态，不进行像素读回比对。
