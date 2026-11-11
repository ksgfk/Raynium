> - 适用: 修改三层地形采集、native 光栅、共享图集及其生命周期
> - 权威: 本文记录地形预览的数据、绘制和输入协议；设备与输出 RT 见 native-runtime，通用加载见 interop
> - 锚点: `native/src/renderer.cpp`, `native/src/solid_pipeline.cpp`, `native/src/mesh_pass.cpp`, `native/shaders/terrain.hlsl`, `src/client/java/dev/raynium/client/terrain/*.java`, `native/include/raynium/bridge.h`

# 完整区块地形预览

预览把 MC 26.3 的 SOLID、CUTOUT 和 TRANSLUCENT 地形画到右上角小 RT。原版编译、可见性、世界绘制、手、后处理、GUI、提交和呈现继续运行。
透明模式跟随原版当前帧：关闭改进透明度时使用原版排序混合，开启时使用八系数小波 OIT。
不采集实体、方块实体、粒子、天空、天气、船体水面遮罩或额外物理折射；背景清为雾色、alpha=0。
native 支持 D3D12 与 Vulkan，宿主要求 Windows Vulkan。正常路径不读回纹理像素，显式开发对照会读回截图。

## 网格、快照与驻留

`SectionCompilerMixin` 在编译返回时复制三层 28 字节 quad 顶点：float3 位置、RGBA8 颜色、float2 UV、ushort2 光照。
复制发生在 MC MeshData 释放前，不借用其 arena；worker 只操作 CPU 数据。`SectionPayload` 持有同一 compiled section 的三个可选 `MeshPayload`。
编译器携带创建时的 epoch。只有当前已采用 compiled mesh 能进入可见快照；关闭把整个 section 引用送入延迟回收队列。
顶点在上传后仍保留，以支持同世界预览重建。世界重置、编译器变化与图集重载更新 source epoch，忽略旧 payload；native 拒绝倒退 epoch。

SOLID/CUTOUT 把准确平面、绕序一致的 quad 分为六个轴向组，其余进入 general 组，保留组内顺序和原始精度。
使用实际包围盒及最终投影反算的视点保守剔除；不假设模型位于 section 范围内。TRANSLUCENT 保留全部原始顶点顺序，仅有一个 submesh。
`RenderSectionIndicesMixin` 在透明索引加入 UberGpuBuffer 时复制索引，并把快照绑定到该次上传回调。
只有原版上传采用时才发布快照；失败、被覆盖或取消的上传不发布。源 uint16/uint32 索引统一转换为自有 uint32 数组。

`LevelTerrainMixin` 在原版 extractSectionDrawGroups 释放 dispatcher 锁前冻结 section、三层可见掩码和已采用索引引用。
模式来自该次提取的 respectTranslucentOrder 参数，使用原版实际路径。`GameTerrainMixin` 捕获包含受伤、摇晃和反胃变换的最终世界投影。
采集复用 section、索引引用、原点、淡入、掩码与距离键数组；缩减、beginFrame、失效和关闭时清空引用。
相机保持整数坐标及小数偏移，shader 先做整数减法，避免大世界坐标先转为 float。

`TerrainResidency` 按整个 section 采用或回退，同坐标新版本预算不足时使用旧版本的完整层集合，层删除随新版本一起生效。
8 MiB 软预算包含新 section 的三层顶点及初始透明索引；超预算时按距离选择候选，首个超大 section 可以独自推进。
已驻留透明索引更新优先，可突破软预算；不重新上传顶点。关闭但仍作为当前可见或回退版本的 section 延迟回收，零掩码仍标记活跃。
只有 native 提交成功才提交驻留、索引版本、移除和材质状态。回退驻留表只在更新及回退查询时使用。
跨 FFM 每帧一次批量调用；native 返回前把字节复制到 staging，不保留 Java 指针。

SOLID/CUTOUT 共用按需增长的 quad 索引，至少容纳 65,536 顶点；透明层使用自身索引 ID。
每 section 共享一条 origin/visibility 实例，每个非零层掩码生成一个可见项，材质 mode 区分三层。
普通透明按原版反向 section 顺序生成唯一 sort_key，不使用距离桶；其它 pass 使用距离桶和状态排序。
native 维护 index→mesh、mesh→primitive、material→primitive 的反向引用，更新时刷新受影响的预解析模板、绑定和层缓存；寿命与提交细节见 [Native runtime](native-runtime.md)。

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
小 RT 宽高各取主 RT 四分之一整除值，最小为 1；使用 MC 投影，非整除尺寸存在宽高比取整误差。

## 多 mip 输入与 fallback

RadRay 锁定版本已支持多 mip 共享纹理。native 创建 RGBA8_UNORM、2D、单层、单采样、device-local、COPY_SRC/COPY_DST/SAMPLED 纹理；
先 QueryTextureSupport，再创建和导出。D3D12 用 D3D12_RESOURCE/D3D12_FENCE，Vulkan 用 OPAQUE_WIN32。
Java 检查单节点 LUID、importable support、内存要求；opaque 另核对 device/driver UUID、format/usage/flags/tiling 和分配元数据。
导入仍检查 GPU、驱动和共享能力；导入失败时清理局部导入对象。

默认 `auto` 先导出一张完整 mip 图集和一张 16×16 lightmap，MC 把原版 GPU 内容复制进来，native 直接采样共享纹理。
共享的是额外输入纹理，MC 原图集和 lightmap 继续由原版持有。lightmap 创建增加 COPY_SRC 用途。
初始化/重载复制完整图集；之后只复制本帧实际动画涉及的矩形（含 padding），同一矩形跨 tick 去重；lightmap 只在更新时复制。
没有变化时仍交接输入所有权，但不复制纹理内容。

完整 mip 纹理创建或导入失败时，`auto` 清理本次资源后尝试 split：每个 mip 单独导出一张单 mip 纹理，外加 lightmap。
native 采样私有完整 mip 图集；图集有变化时，把这些共享单 mip 内容复制到私有图集。此 fallback 的 native 复制覆盖全 mip 链，额外带宽高于直接路径。
`direct` 强制直接路径并在失败时禁用预览；`split` 强制 fallback 用于验证。单 mip 也失败则明确记录原因、禁用预览，继续原版。
失败/超时不能作为成功或 GPU 完成，等待失败保留资源及 DLL 所有者。

```text
创建: native 初始化布局 → release inputs → inputReleased(1)
MC:   lightmap 完成 → wait inputReleased(n) → acquire/write/release inputs → inputAvailable(n)
      继续原版世界/手/GUI
native: wait inputAvailable(n) + outputConsumed(previous)
        几何复制 → acquire inputs → terrain passes → release inputs/output
        signal inputReleased(n+1) + outputReady(frame)
MC:   wait outputReady(frame) → acquire/copy/release output → outputConsumed(frame)
      原有 blit/submit/present
```

每条 timeline 只有一个 signal 方。MC wait/signal 划分同一次 submit 中的多个 batch，不在正常帧中途 CPU 等待尚未提交的 MC 工作。
resize 排空帧槽后重建输出颜色和交接信息，各槽在下一帧从 runtime 池按尺寸取得深度附件，保留 renderer、帧调度器与驻留几何。
图集对象变化先排空再重建共享输入，后续帧从新输入创建材质绑定；MC 的资源 epoch 变化另使旧 payload 失效。
世界离开排空输出和输入后关闭 renderer，清除场景；runtime 设备继续保留。

## 通用 ABI 接入

地形通过 ABI 13 的 `renderer`、`render_target`、`shared_textures` 与一次 `render_frame` 传递数据。
完整布局和借用约束归[互操作与加载](interop.md#abi-13-布局)。
`TerrainPreview` 负责 MC 输入复制、纹理变化和性能采样，`TerrainResidency` 管理上传选择、版本暂用、延迟回收与直写帧缓冲；anisotropy 改变时才发送材质更新。
共享图像依据 export 的 textureIndex/mipLevel/mipCount 导入；图集 direct 导出一张完整 mip 图像，split 导出每级一张，lightmap 另占一个逻辑纹理。

## 诊断与限制

`terrainProfile=true` 每 120 样本记录 Java 采集、驻留与索引准备、输入复制和 native 帧调用 p50/p95/p99，并记录上传量与驻留/可见数。
支持 timestamp query 的设备记录全帧和各实际执行地形阶段的 GPU p50/p95/p99；全帧包含上传、split 图集装配及所有权屏障。
各 pass 仅统计实际执行的样本，帧槽完成后读回计时；这些是小型统计读回，正常路径不读像素。

`terrainCompareDir` 显式开启开发对照，`terrainCompareInterval` 指定采样帧间隔，默认 120。
`TerrainReference` 复用同一帧 ChunkSectionsToRender、原版管线、纹理和 uniform，在同尺寸 RT 绘制纯地形。
native 上传稳定时保存 MC/native 配对 PNG、RGB 差分 PNG，以及尺寸、RGB 误差和参考 GPU 毫秒的 JSON。
输出只有区块地形；与完整主画面中的天空、实体及粒子不同。对照会增加绘制和读回开销，性能采样需记录该开关。
运行原版和预览的整机 FPS 不代表独立地形管线性能。命令和测试覆盖见[构建与测试](../guide/build-test.md)。
