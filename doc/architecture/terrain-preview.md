> - 适用: 修改 SOLID 地形采集、native 光栅、共享图集及其生命周期
> - 权威: 本文记录地形预览的数据、绘制和输入协议；设备与输出 RT 见 native-runtime，通用加载见 interop
> - 锚点: `native/include/raynium/terrain.h`, `native/src/terrain.cpp`, `native/shaders/terrain.hlsl`, `src/client/java/dev/raynium/client/terrain/*.java`, `native/include/raynium/bridge.h`

# SOLID 地形预览

预览只把 MC 26.3 的 SOLID 地形画到已有右上角小 RT。原版编译、可见性、世界绘制、手、后处理、GUI、提交和呈现继续运行。
不采集 CUTOUT 或 TRANSLUCENT，没有透明排序、OIT、实体、方块实体或天空绘制；背景清为当前雾色。
native 支持 D3D12 与 Vulkan，宿主要求 Windows Vulkan。运行路径不读回纹理像素。

## 网格与帧

`SectionCompilerMixin` 在编译返回时复制 SOLID 的 28 字节 quad 顶点：float3 位置、RGBA8 颜色、float2 UV、ushort2 光照。
复制发生在 MC MeshData 释放前，不借用其 arena；worker 只操作 CPU 数据。编译器携带创建时的 epoch，结果和 compiled mesh 共享对应 payload。
原版取消、上传、采用回调保持原样。只有 `RenderSection.getSectionMesh()` 当前返回的 compiled mesh 能进入可见快照。
compiled mesh 关闭把 ID 放入回收队列；没有独立积压的编译结果历史队列。

`LevelTerrainMixin` 在原版准备地形 draw 时冻结可见 mesh、原点和淡入值，早于绘制后的上传维护。
`GameTerrainMixin` 捕获 `renderLevel` 最终投影，包含受伤、摇晃和反胃变换，不读取帧末已覆盖成手或 GUI 的投影。
相机保持整数位置与小数偏移，shader 先做整数减法；不会把远处世界坐标先转为 float。
编译器失效、世界重置与方块图集重载更新 epoch；旧 epoch 的 payload 被忽略，native 拒绝倒退的 epoch。

只上传尚未驻留的可见 mesh，按距相机顺序消费每帧 8 MiB 预算；首个超预算 mesh 可单独前进。
相同世界/资源 epoch 中，新版本受预算限制时暂用同坐标上次就绪版本；卸载、换世界或资源重载后不能继续使用旧版本。
跨 FFM 每帧一次批量调用，native 在调用返回前把顶点写入自己的 staging，不保留 Java 指针。

## 几何与绘制

顶点驻留 device-local 分页缓冲，普通页约 4 MiB，超大 mesh 单独扩页；按四顶点对齐分配，支持空闲区合并复用。
页在会话内保留，世界离开或资源重建销毁会话。mesh 退役等待独立输入释放 fence 完成，不能仅按 CPU 帧数回收。
共用 32 位 quad 索引，更新只复制变化的顶点；同一页的复制集中在 draw 前录制，随后一次转为顶点读取状态。
两组输出命令槽复用时只等该槽 n−2 完成；对应 staging、instance、indirect 和常量数据也在此后重用。

CPU 把准确平面、两三角绕序一致的 quad 分成六个轴向组，其余进入 general 组；保留组内顺序及原始顶点精度。
使用 mesh 实际包围范围和最终投影反算的视点保守剔除面组；不假定模型都位于 16³ section 内，退化/非轴向面不剔除。
仅复用 MC 可见列表，没有 GPU 可见性、mesh shader、量化或额外异步队列。
CPU 按 64 格深度桶和几何页组织 indexed indirect；相邻可见面组合并，origin/visibility 走 instance 流。
硬件 indirect 特性不足或显式 direct 开关走逐 draw 路径；后者消费相同列表。

同一 HLSL 离线编译 DXIL/SPIR-V。shader 包含图集采样修正、RGSS、光照、雾和淡入；sampler 随 MC anisotropy 设置更新。
SOLID 使用不混合颜色写入、反向 Z（clear=0、GreaterEqual、深度写入）、私有 D32，D3D12 补偿 clip Y，绕序与 MC Vulkan 一致。
小 RT 的宽高各取主 RT 的四分之一整除值，使用 MC 投影；非整除尺寸的宽高比存在取整误差。

## 多 mip 输入与 fallback

RadRay 锁定版本已支持多 mip 共享纹理。native 创建 RGBA8_UNORM、2D、单层、单采样、device-local、COPY_SRC/COPY_DST/SAMPLED 纹理；
先 QueryTextureSupport，再创建和导出。D3D12 用 D3D12_RESOURCE/D3D12_FENCE，Vulkan 用 OPAQUE_WIN32。
Java 检查单节点 LUID、importable support、内存要求；opaque 另核对 device/driver UUID、format/usage/flags/tiling 和分配元数据。
接口支持扩大不等于所有 GPU/驱动组合都可用，导入失败必须清理局部导入对象。

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
        几何复制 → acquire inputs → SOLID → release inputs/output
        signal inputReleased(n+1) + outputReady(frame)
MC:   wait outputReady(frame) → acquire/copy/release output → outputConsumed(frame)
      原有 blit/submit/present
```

每条 timeline 只有一个 signal 方。MC wait/signal 划分同一次 submit 中的多个 batch，不在正常帧中途 CPU 等待尚未提交的 MC 工作。
resize 只重建输出颜色/深度及命令槽，保留地形；图集对象变化先排空再重建输入会话。世界离开关闭地形会话。

## ABI 7 地形布局

`rn_create_terrain` / `rn_draw_terrain` / `rn_destroy_terrain` 与原有入口共十二个 C 导出。
创建描述 24 字节，输入图像沿用 128 字节描述，direct 返回 2 个，split 返回 mip 数加 1 个（lightmap 最后）。
创建会提交初始化工作；这区别于不提交的 `rn_create_preview`。输入图像的 ready/consumed 分别表示 native 释放与 MC 写入完成。

| 结构 | 布局 |
| --- | --- |
| `rn_terrain_mesh`，56 字节 | ID、顶点指针、顶点数、reserved、7 个组的顶点数、尾部对齐 |
| `rn_terrain_draw`，32 字节 | ID、int3 原点、visibility、7 位 mask、reserved |
| `rn_terrain_frame`，248 字节 | 前 64 字节为 size/flags、epoch、输入序号、三个数组指针、数量和 anisotropy；projection/view 位于 64/128；camera block/fraction 位于 192/204；fog color/distances 位于 216/232 |

frame flags 的 1/2/4/8 分别为 direct draw、图集变化、RGSS、性能采样。所有输入只借用本次同步调用。
每 mesh 最多 1,048,576 顶点，要求完整 quad 和组计数一致；每帧至多 65,536 个更新/draw。
epoch 不倒退；ID 在同一 epoch 中不可代表不同几何。运行时、预览、地形必须由同一初始化线程串行操作。

## 诊断与限制

`terrainProfile=true` 每 120 样本记录 CPU 输入录制/帧调用 p50/p95/p99、上传字节和可见/驻留数。
支持 timestamp query 的设备同时记录 native GPU 准备/光栅 p50/p95/p99、页大小和退役字节；这是小型计时结果读回，不是像素读回。
CPU 帧调用包含命令槽等待、FFM 和 native 录制，不包含 worker 编译或之前的可见快照；GPU 准备包含 fallback 的图集复制。
当前原版仍同时绘制，整机 FPS 不能代表独立地形渲染性能。实际验证、命令和剩余覆盖限制见[构建与测试](../guide/build-test.md)。
