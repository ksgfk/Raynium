> - 适用: 修改三层地形采集、native 光栅、上传纹理及其生命周期
> - 权威: 本文记录地形采集、绘制与上传协议；设备与交换链见 native-runtime，通用加载见 interop
> - 锚点: `native/src/renderer.cpp`, `native/src/terrain_feature.cpp`, `native/src/mesh_pass.cpp`, `native/shaders/terrain.hlsl`, `src/client/java/dev/raynium/client/terrain/*.java`, `native/include/raynium/bridge.h`

# 区块地形渲染

原版渲染图决定区块地形的附件、阶段和绘制顺序。Raynium 保留 SOLID、CUTOUT、TRANSLUCENT 的区块几何，复用同一套模型、排序和重载输入。

## 采集与采用

SectionCompilerMixin 在 MC MeshData 释放前复制原始 28 字节 quad 流，不借用编译 arena，不做 Java 分组和 AABB 计算。
SectionPayload 表示同一 compiled section 的三层可选 MeshPayload；只采集 MC 已采用且顶点/索引就绪的层。
RenderSectionIndicesMixin 将透明 uint16/uint32 索引复制并归一为 uint32，在对应原版上传回调确认采用后才发布快照。
透明索引校验使用固定 vertexCount，上传后释放顶点数组不影响后续排序更新。

TerrainCapture 复用扁平数组，采集 section 原点、淡入、层就绪位、顺序键、最终投影、视图及相机整数/小数坐标。
投影包含受伤、摇晃与反胃变换；每帧只传数据，由 Native 进行朝向剔除。
普通透明保持原版反向 section 顺序；其他层使用距离桶及状态排序。
缩减、帧重置、失效和关闭时释放快照引用；关闭的 section 进入延迟回收队列。

同批的每个视图独占一份 `NativeFrameBuffer`。`rn_submit_frame` 返回后，原生帧槽已接管本次提交所需的数据，编码器回收 Java 堆外暂存供后续视图复用。关闭编码器会释放这些暂存。

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
下次地形准备遇到新 epoch 时重建驻留状态。

## Native 分组与保守剔除

SOLID/CUTOUT 在准备期检查精确轴向平面及两个三角形绕序，记录六轴向加 general 的分组与偏移；RDG Prepare 直接写入最终上传区间，保持组内顺序和全部顶点字节。
按实际顶点计算 AABB，允许模型超出 section；非平面、退化、绕序不一致进入 general，非有限数据保守可见。
非空组映射到紧凑 submesh 位。TRANSLUCENT 保持原 quad 顺序与单 submesh。
共享 Quad 索引由 Native 管理；透明索引归对应 mesh，mesh 删除时同时退役。

多面组的 SOLID/CUTOUT 另外保留每个原始 quad 的 3 位面组号；单组及透明网格不保留此映射。
线框首次绘制时按显式面掩码生成独立索引，在已分组顶点上恢复原始 quad 顺序，保持共享边的覆盖顺序；索引按网格和掩码缓存，不随相机朝向重复上传。
线框使用完整请求面组交给 GPU 背面剔除；CPU 仍可跳过全部面组均不可见的网格。
关闭线框、替换或删除网格及 epoch 切换都通过 FrameSlot 退役索引子分配，材质变化时重绑对应状态。

Native 按视图求逆 projection × view，变换 (0,0,1,0) 得到有效视点偏移。
不可逆、非有限或 abs(w)<=1e-8 时关闭面组剔除。相对相机坐标先做整数减法，保留大世界精度。
按实际 AABB 及 0.001 容差计算轴向组；general 始终保留。多实例取可见组并集，再与显式请求掩码和有效元素位求交。
计算得到零掩码正常跳过绘制；显式非法掩码失败。Native 寿命与批次规则见[Native runtime](native-runtime.md)。

## 绘制与 OIT

`TerrainAnchorMixin` 从原版 `renderLayers` 调用捕获当前三层或 OIT 阶段以及该次绑定的 atlas、lightmap、mip 范围、采样器和 OIT 视图。命令引用本批地形视图的索引。native 在当前 raster pass 声明对应缓冲与纹理访问，按原版附件与 scissor 内联录制。光栅 Pass 只按原版指定的 load/clear 操作开始一次。

SOLID 与 CUTOUT 使用反向 Z，普通透明按原版可见顺序并使用透明索引。启用改进透明度后，原版控制范围、裁剪、透射率、颜色累积和合成的顺序与附件。专用地形程序只承担带区块几何的阶段；全屏裁剪与合成经即时绘制路径执行。绘制完地形后，下一条即时 draw 依自己的快照恢复管线和资源状态。

## 纹理、ABI 与测试

原版维护并更新图集、光照和采样器。每次准备产生独立的视图描述，上传数据随批次保活直至 `rn_submit_frame` 返回。非零提交序号触发驻留提交回调。资源重载后新 epoch 重新采集编译产物。[互操作与加载](interop.md#提交布局)

CPU 用例覆盖网格、面组和索引排序；双后端原生测试检查 direct/indirect 线框、显式面掩码、缓存与跨页扩容。基准与视觉工具分别通过世界帧计数及实际截图检验游戏路径。[构建与测试](../guide/build-test.md)
