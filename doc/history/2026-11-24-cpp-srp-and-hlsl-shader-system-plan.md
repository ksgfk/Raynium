> - 适用: 执行或评审 Raynium C++ 可编程渲染管线（SRP）基座、纯 HLSL/DXC `.rnsp` v2 着色器系统及首期原版管线（`VanillaRenderer`）重构计划
> - 权威: 经用户要求保存的架构设计与实施计划，不构成当前已实现契约；当前实现见 [架构地图](../architecture/overview.md)、[Shader pack](../architecture/shader-pack.md)、[Native runtime](../architecture/native-runtime.md)、[互操作与加载](../architecture/interop.md) 与 [Minecraft 接入](../architecture/minecraft-integration.md)
> - 锚点: `native/radray.lock.json`, `native/include/raynium/bridge.h`, `native/include/raynium/modular_frame_pipeline.h`, `native/include/raynium/command_tape.h`, `native/include/raynium/immediate_pass.h`, `native/include/raynium/immediate_draw.h`, `native/include/raynium/terrain_feature.h`, `native/include/raynium/world_features.h`, `native/include/raynium/shader_pack.h`, `native/src/modular_frame_pipeline.cpp`, `native/src/render_program.cpp`, `native/tools/shader_pack.cpp`, `tools/compile_shader_pack.py`, `tools/convert_vanilla_shaders.py`, `tools/validate_visual.py`, `src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java`, `src/client/java/dev/raynium/client/renderpearl/RayniumPrograms.java`, `src/client/java/dev/raynium/client/world/ModelFastTrack.java`, `src/client/java/dev/raynium/client/mixin/LevelTerrainHookMixin.java`, `src/client/java/dev/raynium/client/mixin/TerrainAnchorMixin.java`

# C++ SRP 渲染管线基座、纯 HLSL/DXC 着色器系统与原版管线实施计划

- 原记录日期：2026-11-24
- 状态：经用户确认设计边界、并于同日完成开工前架构审校修订的实施计划；归档时尚未实施本计划对应的源码改动
- 范围：建立运行于 `RadRay::RenderGraphFramePipeline` 之上的 Native C++ 类 Unity SRP 渲染管线基座、纯 HLSL 2021 + DXC 离线编译的 `.rnsp` v2 着色器系统、三层场景输入与开放式标签过滤协议，并在首期完整落地内置原版管线（`VanillaRenderer`）；类 URP 管线（`UniversalRenderer`）在基座与着色器层对齐多 Pass（`LightMode`）与帧黑板契约，视锥外投射物提取与具体 URP Pass 留待二期实现；彻底移除原版 GLSL 自动转换链路且不支持外部 GLSL
- 来源：2026-11-24 会话设计、开工前评审甄别与当前干净工作树源码审计（基于 Minecraft 26.3、ABI 22 与 `native/radray.lock.json` 锁定的上游 `RadRay` 能力）

---

## 0. 背景、用户确认决策与范围边界

### 0.1 转向背景与现状痛点

在完成阶段三及系统热点非 MC 拓展项优化后，Raynium 在双后端（D3D12 / Vulkan）性能上已大幅领先原版 Vulkan，工程重心正式从单点性能优化转向**可扩展渲染框架建设**。审计当前源码，阻碍构建类 Unity SRP / URP 现代渲染管线的核心症结有三处：

1. **世界 Pass 控制流倒置**：[modular_frame_pipeline.cpp](../../native/src/modular_frame_pipeline.cpp) 的 `SetupGraph` 仅调用 `_tape->Replay(...)` 原样回放 MC 26.3 `LevelRenderer.render`（源码获取方式见 [阅读 Minecraft 源码](../guide/dev-environment.md#阅读-minecraft-源码)）硬编码构建的 `RenderPass` 序列。C++ 侧无权决定帧内 Pass 拓扑、瞬态渲染目标（如 HDR 颜色、阴影图、法线深度预通道或 RDG 托管的 OIT 附件）及多趟重绘。
2. **着色器停留在“单变体 GLSL 转换”模式**：[convert_vanilla_shaders.py](../../tools/convert_vanilla_shaders.py) 依赖 `glslangValidator` 与 `spirv-cross` 将展开后的原版 GLSL 转为 `Raw_ubo` 形式的单变体 HLSL 并用正则改写生成 `*_inst` 伴生变体；[shader_pack.cpp](../../native/tools/shader_pack.cpp) 禁用了上游已具备的 `#pragma radray_keyword_group` 变体发现，[render_program.cpp](../../native/src/render_program.cpp) 手工拼凑模仿 OpenGL 的槽位布局，未使用 `RadRay` 现成的 `ReflectShaderFamily`、`ShaderAsset`、`PipelineLayoutRegistry` 与 `PsoCache`。
3. **即时绘制路径（`ImmediateDraw`）耦合主相机旋转且丢失语义信息**：原版 `LevelRenderer.render` 向 `ModelViewStack` 预乘主相机 `viewRotationMatrix` 并写入 `DynamicTransforms`，且 [ImmediateDraw](../../native/include/raynium/immediate_draw.h) 将解析后的 `_pso` 与 `_sets` 直接写回绘制对象自身，既无法在同一帧内跨多个不同 `LightMode` / 附件格式的 Pass 安全重放，也无法让自定义 `RendererFeature` 识别或接管特定世界要素（如天空、云层、雨雪）。

### 0.2 用户明确拍板的架构决策

以下核心取舍已由用户在会话中逐项确认：

1. **分期边界**：首期**仅实现 C++ SRP 基座、纯 HLSL/DXC 着色器系统与原版管线（`VanillaRenderer`）**；类 URP 管线（`UniversalRenderer`）留待下一阶段平行建设，不在首期引入平行光阴影视锥外实体提取等非原版负担。
2. **SRP 主场位置**：采用 **Native C++ 主导 SRP**。管线、Pass 与 `RendererFeature` 基于 `RadRay::RenderGraphFramePipeline`、`RDGBuilder`、`ShaderAsset` 与 `PsoCache` 在 C++ 侧实现，**不推翻重写现有 `ModularFramePipeline`，并将现有三大绘制器（`TerrainFeature`、`InstancedGeometryFeature`、`ImmediateDraw`）全量收编接入**；Java 侧仅负责状态提取、几何/实例采集与管线配置下发。
3. **Shader 资产形式**：采用 **`.shader.json` 声明清单 + 100% 纯标准 HLSL 2021（`.hlsl` / `.hlsli`）**。根 `.hlsl` 直接使用 `RadRay` 原生 `[shader(...)]` 阶段标注、`#pragma radray_keyword_group` 与 `<core/platform.hlsli>`，旁配轻量 JSON 声明 `LightMode` Pass、默认光栅状态及变体裁剪规则，专门使用锁定的 `RadRay` DXC 编译封包。
4. **外部 GLSL 策略**：**不支持外部 GLSL**（不兼容资源包 GLSL 覆盖或第三方模组自定义 GLSL 着色器），彻底移除 `glslangValidator`、`spirv-cross`、[convert_vanilla_shaders.py](../../tools/convert_vanilla_shaders.py)、`exportVanillaPipelines` 与运行时 `.raynium/shader-cache/`。
5. **环境模块首期节奏（节奏 A）**：首期即建立完整的 `rn_world_state` 原始语义状态传递、世界绘制主相机 `ViewRotation` 剥离（统一至相机相对世界空间）及独立开放标签（天空、云、天气、世界边界各自可独立筛选或屏蔽）；首期 `VanillaRenderer` 中这四项的网格消费 MC 剥离视角后的现成几何以保障原版视觉一致，后续 URP 或自定义 Feature 可直接在 C++ 读取 `rn_world_state` 屏蔽并替换对应标签槽。
6. **面向附属 Mod 的开放标签与扩展机制**：基于 `RadRay` 现成的 `MaterialTagTable` / `MaterialTagSet` 建立**驻留字符串标签（`namespace:name -> uint32_t`）+ 连续数值 `RenderQueue` + `RenderingLayerMask`** 开放过滤协议；并在 C++ SRP 基座内置参数化的通用标准 Pass 原语（绘制、全屏 Blit、Compute），允许纯 Java 附属 Mod 配合预编译扩展 `.rnsp` 挂载自定义 `RendererFeature`。

> 下文第 1～5 节为助手依据上述确认边界与源码核查修订后的详细技术方案。所有新增 C++ 类严守仓库根 `AGENTS.md` 硬约束：禁止默认构造加 `Init` 二段式初始化（可失败创建统一使用 `static bool Create` 准备完全部资源后交付有效对象），显式使用 `radray::` 容器/智能指针，C ABI 仅返回 `RN_STATUS_OK` / `RN_STATUS_FAILED`。

---

## 1. 支柱一（`P1`）：纯 HLSL + DXC 着色器系统与 `.rnsp` v2 封包

### 1.1 源码分层与 `.shader.json` 多 Pass 清单

着色器源码统一收拢至仓库 `native/shaders/`，共享数学、色彩与 BSDF 库直接 `#include` 上游 `external/RadRay/shaderlib`（如 `<core/platform.hlsli>`、`<core/math.hlsli>`、`<core/color.hlsli>`、`<bsdf/principled.hlsli>`）：

```text
native/shaders/
  common/
    per_view.hlsli          # Group 0 (space0 / set 0) 视图常量与全局采样器契约
    geometry_domain.hlsli   # 统一顶点/实例输入解码适配器（即时 / 区块 / 刚体 / 粒子 / 阴影片）
    fog.hlsli               # 原版双距离雾与淡入公共函数
  vanilla/
    terrain.shader.json     # 声明 VanillaForward / OITBounds / OITTransmittance / OITAccumulate 等 LightMode Pass
    terrain_forward.hlsl
    terrain_oit.hlsl
    entity.shader.json      # 合并原 core/entity, item, armor, glint, energy_swirl, beacon 等材质变体
    entity_forward.hlsl
    particle.shader.json    # 合并即时粒子与 56B 四元数实例化粒子
    particle_forward.hlsl
    shadow.shader.json      # 实体圆形阴影片（即时 + 56B 实例化）
    sky_environment.shader.json # 天空、日月星辰、霞光、云层、雨雪、世界边界、末地传送门
    primitive.shader.json   # 通用 PositionColor / PositionTexColor / Lines / Outline / SeeThrough / DepthBlit
    oit_fullscreen.shader.json  # OIT 深度裁剪与最终合成
  post/
    post_effects.shader.json # 覆盖 PostChain 使用的 blit / box_blur / entity_sobel / color_convolve / invert / spiderclip / transparency
  ui/
    gui.shader.json         # GUI 元素、字体字形（普通/阴影/透视）、光照图生成、图集动画插值与画中画
    output.shader.json      # 交换链最终上屏（像素行翻转与色彩输出）
```

**统一几何域解码（严格遵守锁定 DXC 关键词语法）**：
按 `RadRay` DXC 编译器契约（空格分隔的带引号字符串字面量，仅出现在根 `.hlsl`），声明几何域与材质开关关键词组：

```hlsl
#pragma radray_keyword_group GEOMETRY_DOMAIN "IMMEDIATE" "INSTANCED_MODEL" "INSTANCED_PARTICLE" "INSTANCED_SHADOW"
#pragma radray_keyword_group ALPHA_MODE "OPAQUE" "CUTOUT" "BLEND"
#pragma radray_keyword_group LIGHTING_MODE "LIT_CARDINAL" "LIT_PER_FACE" "EMISSIVE" "UNLIT"
```

`geometry_domain.hlsli` 根据 `GEOMETRY_DOMAIN` 将即时单流顶点、24B 局部网格 + 80B 刚体实例（`Stream 0 + Stream 1`）、或单位四边形 + 56B 粒子/阴影实例统一解码为处于**主相机相对世界空间**的标准 `DecodedSurfaceVertex`，顶点阶段最终裁剪坐标统一使用 `precise float4 positionCS = mul(ViewProj, float4(worldRelPos, 1.0f));` 保证跨 Pass（如 OIT 多阶段及未来预通道）深度比特级一致，其余计算保持 DXC 默认 `-O3` Fast Math。

**`.shader.json` 声明契约**：
- `id`：规范化命名空间标识，如 `"raynium:vanilla/entity"`；
- `tags`：默认材质语义标签字典（如 `{"raynium:domain": "raynium:entity", "raynium:surface": "raynium:opaque"}`）；
- `passes`：有序 Pass 数组，每项声明 `lightMode`（如 `"raynium:vanilla_forward"`、`"raynium:oit_bounds"`、`"raynium:oit_transmittance"`、`"raynium:oit_accumulate"`、`"raynium:outline_mask"`、`"raynium:immediate"`）、相对 `native/shaders/` 的根 `.hlsl` 路径 `source`、默认 `renderState` 与非法关键词组合裁剪规则 `stripRules`。

### 1.2 四级频率资源绑定 ABI 与布局匹配规则

所有 HLSL 源码与 `ReflectShaderFamily` 统一按职责固定 `GroupSlot`：

| `GroupSlot` | HLSL 空间 (`space` / `VK_BINDING set`) | 绑定频率与职责 | 典型成员与布局匹配规则 |
| --- | --- | --- | --- |
| **`0` (`PerView`)** | `space0` / `set 0` | 视图级参数（世界视图由 C++ `ViewContext` 提供；世界外 `ImmediatePass` 绑定 MC 帧级 `Projection` / `Globals` / `Fog` / `Lighting` UBO） | 含 `cbuffer PerViewConstants` 及可选 `Lightmap` / `LightSampler`。由于 `PipelineLayoutRegistry` 按当前后端完整活跃绑定列表归并 `BindingGroupLayout`（如仅含 `b0` 的天空着色器与含 `b0+t0+s0` 的实体着色器会生成不同组布局对象），C++ `ViewContext` **按目标 PSO 的 `Group 0` `BindingGroupLayout*` 为键缓存对应的 `ShaderParameterSet*`**（每视图至多 2～3 个组实例，经 `FrameSlot::AcquireParameterSet` 复用），严守 RHI 布局精确匹配契约，不强迫无关着色器声明冗余纹理。 |
| **`1` (`PerPass`)** | `space1` / `set 1` | RDG Pass 级瞬态输入，每 Pass 构建并按目标 `Group 1` `BindingGroupLayout*` 绑定 | 当前 Pass 消费的 RDG 纹理/缓冲，如 OIT 的 `DepthBounds` / `Coeff0` / `Coeff1` / `Accumulation`，以及未来 URP 的阴影图、深度与法线纹理。未声明 `space1` 资源的家族不创建 `Group 1`。 |
| **`2` (`PerMaterial`)** | `space2` / `set 2` | 材质与绘制级资源绑定（经 `FrameSlot::AcquireParameterSet` 平坦哈希表去重） | 纹理与动态采样器（`Sampler0`、`Sampler1`、`InSampler` 等）、纹素缓冲（如 `CloudFaces`），以及 MC 逐绘制写入的缓冲切片（如 `DynamicTransforms`、`CloudInfo`、`PostChain` 的 `SamplerInfo` 与模糊配置 UBO）。 |
| **`PushConstants` + `Stream 1` (`PerDraw`)** | `VK_PUSH_CONSTANT` + D3D12 RootConstants（`PromoteDxilCBufferToRootConstants = true`） | 逐绘制内联根常量或实例化顶点流 | 小尺寸内联参数走根常量；刚体/粒子/阴影走 `Stream 1` 实例缓冲流。 |

### 1.3 `.rnsp` v2 定长包头与纯 DXC 打包工具

升级 [shader_pack.h](../../native/include/raynium/shader_pack.h) 与离线编译器 [shader_pack.cpp](../../native/tools/shader_pack.cpp) 至 **`ShaderPackVersion = 2`**：

1. **80 字节定长小端包头（`ShaderPackHeaderV2`）与整包校验规则**：
   包上限维持 64 MiB，所有段按 8 字节对齐，任意两段区间不得重叠或越界：

   | 字节偏移 | 类型与字段 | 契约说明 |
   | ---: | --- | --- |
   | `0..3` | `uint32 magic` | 固定 `0x50534e52`（`"RNSP"`） |
   | `4..7` | `uint32 version` | 固定 `2` |
   | `8..11` | `uint32 totalBytes` | 整包精确字节长度 |
   | `12..15` | `uint32 wholePackCrc32` | 标准 IEEE CRC32，计算时将偏移 `12..15` 视作 4 个零字节后覆盖整份文件 `0..totalBytes` |
   | `16..23` | `uint64 toolchainIdentity` | 必须等于 `radray::shader::kShaderToolchainIdentity` |
   | `24..31` | `uint32 flags`, `uint32 reserved` | 当前均固定为 `0` |
   | `32..79` | 6 组 `(uint32 offset, uint32 countOrBytes)` | 依次对应：`StringPool`（字节数）、`ShaderRecords`（条目数）、`PassRecords`（条目数）、`FamilyRecords`（条目数）、`BytecodePool`（条目数）、`StageBindingRecords`（条目数），共 `6 × 8 = 48 B` |

2. **家族预反射与 `BytecodePool` 体积去重**：
   - `raynium_shader_pack` 对每个 `.shader.json` Pass 调用 `DiscoverSourceContract` 与 `CompileVariant`（`ShaderTargetMask::All`），再调用 `ReflectShaderFamily` 生成 `ShaderFamilyLayoutDesc` 并通过 `WriteShaderFamilyLayoutBinary` 写入包内。
   - `BytecodePool` 按 `(target, BytecodeHash)` 对单阶段原始字节码做包内去重以**缩减 `.rnsp` 磁盘体积与加载内存**；运行期 `Renderer::Create` 仅提取当前设备后端的阶段记录构造 `radray::ShaderAssetDesc` 并调用上游 `radray::ShaderAsset::Create`（上游对每条变体阶段正常创建独立 `gpu::Shader` 对象，不修改 `RadRay::ShaderAsset` 所有权语义）。

### 1.4 `MaterialArchetype` 与按 `LightMode` 延迟 PSO 求解

- **注册模型**：Java 在管线编译期向 Native 注册轻量的 **`MaterialArchetype`**（包含目标 `shaderId`、静态 `keywordMask`、基础光栅状态 `MaterialRenderState`、顶点流布局 `radray::VertexInput`、绑定槽位路由表以及开放标签集 `radray::MaterialTagSet`），不再在注册时绑定固定附件格式生成单一 PSO。
- **Pass 期求解**：封装 `AssetManager`、`PipelineLayoutRegistry` 与 `PsoCache` 为 **`SrpShaderLibrary`**。无论在世界外 `ImmediatePass`（使用 `LightMode = "raynium:immediate"`）还是世界内 `ScriptableRenderPass`（使用该 Pass 指定的 `LightMode`），均在 RDG `Prepare` 阶段以 `(archetypeId, lightModeTagId, passKeywordMask, attachmentFormats, stateOverride)` 向 `PsoCache` 查出匹配的 `GraphicsPipelineState*` 与 `GpuPipelineLayout*`；若该材质未声明请求的 `LightMode`，返回空并由当前 Pass 跳过绘制。

---

## 2. 支柱二（`P2`）：三层场景输入、相机相对世界空间与开放标签协议

### 2.1 相机相对世界空间核实与三层输入边界

核对 MC 26.3 渲染源码确认：
- `LevelRenderer.submitFeatures` 使用全新 `new PoseStack()`，刚体 `ModelFastTrack`、粒子/阴影快轨以及 `FeatureRenderDispatcher.prepareFrame` 内 `BufferBuilder` 产出的顶点本就处于**纯相机相对世界空间**（$P_{\text{rel}} = (P_{\text{block}} - C_{\text{block}}) + (P_{\text{frac}} - C_{\text{frac}})$）；
- `Lighting.Entry.LEVEL` 写入的是固定世界空间漫反射方向（`DIFFUSE_LIGHT_0/1`），而透视模式下的 `VIEW_OFFSET_Z_LAYERING` 仅为各向同性缩放 `matrix.scale(1.0f - bias / 4096.0f)`，与绕原点纯旋转矩阵 `cameraState.viewRotationMatrix` 严格可交换；
- 原版唯一混入主相机旋转的位置是 `LevelRenderer.render` 向 `RenderSystem.getModelViewStack()` 执行的 `modelViewStack.mul(cameraState.viewRotationMatrix)`（随后被 `writeTransform` 拷入 `DynamicTransforms.ModelViewMat`）。

因此在世界采集区间**不向 `ModelViewStack` 预乘 `cameraState.viewRotationMatrix`**，改由 C++ `PerView`（`Group 0`）统一提供 `ViewProj = Proj * ViewRotation`，即可在不改变主相机数学结果（含天空盒天体旋转与 Z-layering 缩放）的前提下，使全部世界几何保持视角无关。

世界输入按语义密度分为三层：
1. **第 1 层：结构化世界状态 `rn_world_state`**：从 `LevelRenderState` 提取相机块坐标与小数坐标、主相机 `ViewRotation` 与含晃动/反胃形变的 `Projection`、雾参数、世界光照方向、日月星辰/天空参数、云层与世界边界参数，以及 `use_oit`、`has_water_mask`、`should_render_clouds`、`render_wireframe_terrain`、`has_entity_outline`、`consistent_depth_gizmos` 等帧控制标志。
2. **第 2 层：常驻几何与实例化图元**：驻留地形（`TerrainResidency` 多视图）与常驻刚体/粒子/阴影实例批次（`StaticGeometryRegistry` + 相机相对世界实例流 + `MaterialArchetype` + `layer_mask`）。
3. **第 3 层：受控世界动态绘制（`WorldDynamicDraw`）与世界外 `ImmediatePass`**：世界外（`Lightmap`、图集动画、3D 手持物、`blitEntityOutline`、`applyPostEffects`、`GuiRenderer`）继续走既有 `CommandTape` 的 `ImmediatePass`；世界内动态回退网格与过渡期环境网格经 `WorldCollectorRenderPass` 录为携带开放标签的 `WorldDynamicDraw`。

### 2.2 面向附属 Mod 开放的三维标签与过滤协议

复用 `radray::MaterialTagTable`、`radray::MaterialTagId` 与 `radray::MaterialTagSet`：
1. **动态驻留入口**：新增 `rn_intern_tag` 将任意 `"namespace:name"` 字符串驻留为 32 位 `MaterialTagId`（初始化时预驻留内置 `raynium:domain`、`raynium:surface` 各取值与标准 `LightMode` 名）；渲染帧内仅传递和比对 `uint32_t`。
2. **`FilteringSettings` 四元组**：
   - `RenderQueueRange [minQueue, maxQueue]`（`uint16_t`）；
   - `uint32_t renderingLayerMask`（逐批次位掩码过滤）；
   - `requiredTags`（材质默认标签与批次动态标签合并后须全部匹配）；
   - `excludedTags`（命中任一即排除，供自定义 `ScriptableRendererFeature` 声明接管如 `raynium:domain = raynium:clouds` 后由默认 Pass 自动屏蔽原版绘制）。

### 2.3 原版管线静态语义路由（覆盖 `RenderPipelines` 与动态 `PostChain`）

沿用 [RayniumGpuDevice.java](../../src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java) 现有的 `compilePipeline(RenderPipeline, ShaderSource, Executor)` 异步后台准备、所属线程 `finishCompile` 注册生命周期，将内部 `RayniumPrograms` 替换为 **`VanillaPipelineRouter`**：
- **路由键设计**：由于 MC 26.3 的 `PostChain.createPass` 会在运行期动态拼装 location 形如 `<effect>/<index>` 的 `RenderPipeline`（如实体轮廓、模糊与旁观者生物滤镜），路由器**不以静态字段枚举作为唯一依据，而是按 `(vertexShaderId, fragmentShaderId, ShaderDefines, VertexFormat)` 查表**，同时覆盖 `RenderPipelines` 全部内置管线与 `PostChain` 动态构造管线（映射到 `vanilla/`、`post/`、`ui/` 对应 HLSL 家族及绑定槽位映射表）。
- 遇到不在内置 HLSL 路由表中的未知外部 GLSL 管线时，记录单次日志并返回 `CompiledRenderPipeline.Pending.NULL`。

---

## 3. 支柱三（`P3`）：基于 `RadRay::RenderGraphFramePipeline` 的 C++ SRP 基座

### 3.1 `ModularFramePipeline` 三段式同图编排

保留 [ModularFramePipeline](../../native/include/raynium/modular_frame_pipeline.h) 的帧槽、围栏、读回、时间戳与交换链管理。`CommandTape` 新增 `Opcode::ExecuteWorldPipeline`，使单帧 `SetupGraph` 在同一个 `RDGBuilder` 内按实际时序串联：
1. **世界前**：缓冲/纹理初始上传、离屏 `Lightmap` 与图集动画 `ImmediatePass`；
2. **世界锚点（`ExecuteWorldPipeline`）**：构造 `FrameBlackboard`，调用当前激活 `ScriptableRenderer::SetupWorldGraph` 生成世界 RDG 子图；
3. **世界后**：`3D 手持物（render3dHud）`、按需截图拷贝、`blitEntityOutline`（将轮廓纹理混合至 `MainTarget`）、`applyPostEffects`、`GuiRenderer` 与 `OutputPass` 交换链上屏。

### 3.2 不可变保序 `WorldDrawStream` 与每 Pass 独立 `PassDrawState`

针对 RDG **“所有 Pass 先统一执行 `Prepare`，随后才依次执行 `Record`”** 的执行模型及半透明混合保序要求，严格分离“帧级只读绘制输入”与“Pass 级可变准备输出”：

1. **跨绘制类型严格保序的不可变 `WorldDrawStream`**：
   - `WorldDrawStream` 在解码完成后于整帧内 **100% 只读不可变**（其内部元素绝不保存 `_pso` 或 `ShaderParameterSet*`）；
   - 内部除持有 `InstancedGeometryItem` 数组与 `WorldDynamicDrawItem` 数组外，维护一条统一的 **`radray::vector<OrderedWorldDrawRef> orderedItems`**（记录 `{kind: Instanced | Dynamic, itemIndex, archetypeId, queue, layerMask, dynamicTag}`）。当 [ModelFastTrack.java](../../src/client/java/dev/raynium/client/world/ModelFastTrack.java) 在同一 `RenderType` 组内交错发出“实例化批次 A → 即时回退绘制 B → 实例化批次 C”时，`orderedItems` 精确保留提交先后顺序，回放时按原序遍历。
2. **每 Pass 独占的 `PassDrawState`（存放于 `FrameSlot` 线性存储）**：
   - 当某个 `ScriptableRenderPass` 调用 `BuildRendererList(slot, DrawingSettings, FilteringSettings)` 时，在 `slot.Emplace<PassDrawState>()` 中生成**归属该 Pass 自己**的筛选列表 `radray::pmr::vector<PreparedWorldDraw>`；
   - 在该 Pass 的 `RDGPrepareContext` 回调中，将求解出的 `GraphicsPipelineState*`、`Group 0 (PerView)`、`Group 1 (PerPass)`、`Group 2 (PerMaterial)` `ShaderParameterSet*` 写入本 Pass 自己的 `PassDrawState`，多个 Pass 并发或先后 `Prepare` 互不覆写；
   - 在 `RDGRasterPassContext` 回调中，只读遍历本 Pass 的 `PassDrawState` 并共享栈上 `ImmediatePassStateCache` 录制命令。

### 3.3 C++ SRP 组件与附属 Mod 参数化原语

- **`ScriptableRenderer` / `ScriptableRenderPass` / `ScriptableRendererFeature`**：均以无二段式构造方式管理生命周期；Feature 可通过 `ClaimHandledTags` 声明接管标签自动注入默认 Pass 的 `excludedTags`，并通过 `RenderPassEvent` 向管线队列注入自定义 RDG Pass。
- **附属 Mod 免 C++ 扩展口**：提供 `rn_mount_shader_pack` 加载经同款锁定 DXC 打包的扩展 `.rnsp` v2，并内置 `DataDrivenDrawPass`、`DataDrivenBlitPass`、`DataDrivenComputePass` 三类参数化 Feature 模板，配置期一次性注册、帧内纯 C++ RDG 执行。

---

## 4. 支柱四（`P4`）：MC 26.3 世界精确采集、`VanillaRenderer` 与二期边界

### 4.1 `RayniumDrawSink`、`WorldCollectorRenderPass` 与防重采集切点

为解决原版方法强依赖 `RenderPass` 参数、地形捕获跳过及 OIT 阶段别名重复遍历三项宿主耦合问题：

1. **统一绘制接收口 `RayniumDrawSink`**：
   - 定义内部接口 `RayniumDrawSink`（接收管线绑定、Uniform 切片、顶点/索引缓冲、普通 `draw/drawIndexed` 以及 `instancedGeometry`）；
   - `RayniumRenderPassBackend.Lease`（用于世界外 `ImmediatePass`）与新增的 **`WorldCollectorRenderPass`**（实现 Mojang `RenderPass` + `RayniumDrawSink`，不绑定物理 RT，专门附带当前队列与动态 `MaterialTag` 往 `WorldDrawStream` 追加记录）共同实现 `RayniumDrawSink`；
   - [RayniumGpuDevice.java](../../src/client/java/dev/raynium/client/renderpearl/RayniumGpuDevice.java) 的 `activeTape(RenderPass)` 与 `recordInstancedGeometry(RenderPass, ...)` 改为识别 `RayniumDrawSink`（对 `FrontendRenderPass` 取其 `backend`，对 `WorldCollectorRenderPass` 直接调用），使现有 `ModelFastTrack`、`ParticleFastTrack`、`ShadowFastTrack` 与 `BufferBuilder` 即时回退零分叉写入同一有序 `WorldDrawStream`。
2. **保留地形捕获的拦截位置**：
   - 保留 `LevelRenderer.render` 中 `repositionCamera`、`submitFeatures`、`prepareFrame` 及 `prepareChunkRenders[Indirect]`（继续触发 [LevelTerrainHookMixin.java](../../src/client/java/dev/raynium/client/mixin/LevelTerrainHookMixin.java) 完成当帧唯一一次 `TerrainView` 捕获），仅跳过 `modelViewStack.mul(cameraState.viewRotationMatrix)` 并在 `setupFrameGraph` / `frame.execute` 区间改为执行 `RayniumWorldCollector.collect(...)` + 发出 `Opcode::ExecuteWorldPipeline`。
3. **经典半透明与 OIT 分支无重复采集**：
   - **公共采集**：`Sky`（`skyRenderer.render`）、`SolidFeatures`（`featureFrame.executeSolid`）、可选 `Outline`（`featureFrame.executeOutline`）、可选 `SeeThrough`（`featureFrame.executeSeeThrough`）、可选 `AlwaysOnTop`（`featureFrame.executeAlwaysOnTop`）；
   - **当 `use_oit == false`（经典模式）**：依次采集 `TranslucentBeforeTerrain`（`featureFrame.executeTranslucent`）、`TranslucentAfterTerrain`（`featureFrame.executeTranslucentAfterTerrain`）、`Clouds`、`Weather`、`WorldBorder`；
   - **当 `use_oit == true`（OIT 模式）**：由于 MC 26.3 `SubmitNodeCollection` 在 OIT 模式下将 `shadows`、`translucentModels`、`afterTerrain` 等 10 个字段全部别名指向同一个 `oitTranslucent` 实例，采集器**绝不调用 `executeTranslucent` / `executeTranslucentAfterTerrain`**，而是仅调用一次 `featureFrame.executeOit(OitStage.DEPTH_BOUNDS, collector)` 采集单份 `OitTranslucent` 流（材质经 `VanillaPipelineRouter` 归一为多 Pass `MaterialArchetype`，由 C++ 在三阶段分别切换 `LightMode` 重放），并在 `hasAnyWaterMask()` 为真时单独调用一次 `featureFrame.executeWaterMask` 采集船体水面遮罩流，另单独采集一次 `Clouds`、`Weather`、`WorldBorder`。

### 4.2 `VanillaRenderer` 精确 Pass 拓扑与轮廓合成边界

1. **常驻主干 Pass**：
   - `VanillaClearAndSkyPass`：清理 `MainColor` 与 `MainDepth`（`0.0`），绘制未被接管的 `Sky` 流；
   - `VanillaOpaquePass`：绘制驻留地形 `Solid + Cutout`（或线框）与 `SolidFeatures` 流；
   - `VanillaClassicTranslucentPass`（`use_oit == false` 时激活）：按原版顺序绘制 `TranslucentBeforeTerrain` → 驻留地形 `Translucent` → `TranslucentAfterTerrain` → `Clouds` → `Weather` → `WorldBorder`。
2. **严格对齐 MC 26.3 语义的三项内置 `ScriptableRendererFeature`**：
   - **`VanillaOitFeature`（`use_oit == true`）**：
     由 RDG 创建瞬态 `DepthBounds`（`RGBA32F`）、`DepthBoundsCulled`（`RGBA32F`）、双 `Transmittance`（`RGBA16F`）、`Accumulation`（`RGBA16F`），以及按需瞬态 `TerrainWaterPatchDepth`（`D32F`）与 `CloudDepth`（`D32F`），严格复刻 MC 26.3 顺序：
     1. **阶段 0（`DEPTH_BOUNDS`）**：先以 `MainDepth` 绘制地形 `Translucent`、`OitTranslucent`、`WorldBorder`、`Weather` 的深度范围；紧接着执行 `OitDepthBoundsCullPass`（产出 `DepthBoundsCulled`）；若存在 `Clouds`，将 `MainDepth` 拷入瞬态 `CloudDepth` 并基于 `DepthBoundsCulled` 绘制云层 bounds；若 `has_water_mask == true`，将 `MainDepth` 拷入 `TerrainWaterPatchDepth` 并绘制 `WaterMask`（船内挡水深度补丁），后续两阶段地形改绑 `TerrainWaterPatchDepth`；
     2. **阶段 1（`TRANSMITTANCE`）与阶段 2（`ACCUMULATE`）**：对同一份已采集流分别切换 `LightMode = raynium:oit_transmittance / raynium:oit_accumulate` 重放地形、`OitTranslucent`、`WorldBorder`、`Weather`，以及基于 `CloudDepth` 的 `Clouds`；
     3. **合成阶段（`OitCompositePass`）**：采样 `Accumulation`、双 `Transmittance` 与 `DepthBounds` 混合回 `MainColor`。
   - **`VanillaEntityOutlineFeature`（严守手持物与截图时序）**：
     在 `GameRenderer.render()` 中，原版顺序为：`levelRenderer.render()`（生成轮廓纹理）→ `render3dHud()`（绘制 3D 手持物）→ `tryTakeScreenshotIfNeeded()`（存档图标截图）→ `levelRenderer.blitEntityOutline()`（将轮廓叠加至 `MainTarget`）→ `applyPostEffects()`。
     因此 **`VanillaEntityOutlineFeature` 在世界锚点内仅导入并写入外部 `entityOutlineTarget`（清空后绘制 `Outline` 掩码并执行轮廓提取/模糊 Pass 写回 `entityOutlineTarget`），绝不在世界锚点内提前叠加到 `MainColor`**；最终与 `MainTarget` 的唯一一次混合保留在 `GameRenderer.render()` 原有的 `blitEntityOutline()` 时机执行，确保手持物遮挡关系与存档截图结果与原版完全一致。
   - **`VanillaOverlayGizmoFeature`**：处理 `SeeThrough` 与 `AlwaysOnTop` Gizmo（`consistent_depth_gizmos` 为真时使用 RDG 瞬态深度纹理并执行 `IntegrateDepth`）。

### 4.3 二期 `UniversalRenderer`（类 URP）扩展边界的准确界定

- **首期已就绪能力**：C++ `ScriptableRenderer` / `FrameBlackboard`、不可变多趟重放 `RendererList`、按 `LightMode` 求解 PSO、相机相对世界空间坐标系，以及 C++ `TerrainResidency` 已驻留的玩家周边区块几何槽位池。
- **二期仍需补充的工作（不在首期过度设计）**：由于 MC `LevelExtractor.extractVisibleEntities` 与 `extractVisibleBlockEntities` 在首期仍仅按主相机视锥裁剪，二期引入 `UniversalRenderer` 平行光级联阴影（CSM）时，需在 Java 提取侧扩展阴影距离内的光源视锥候选实体/方块实体收集，并在 C++ 补充基于驻留区块槽位的光源视锥裁剪与具体 URP 着色器 Pass。

---

## 5. 自洽里程碑路线（`M1`～`M5`）与可判定验收标准

调整后的 5 个里程碑消除跨阶段前置依赖，每一步均可独立编译、独立运行测试验收：

| 里程碑 | 实施内容 | 独立可判定验收标准 |
| --- | --- | --- |
| **`M1`：`.rnsp` v2 工具链与 C++ `SrpShaderLibrary` 底座** | 1. 落地 80B 包头的 `.rnsp` v2 编解码与 `raynium_shader_pack`（支持 `.shader.json`、带引号 `#pragma radray_keyword_group`、变体裁剪、`ReflectShaderFamily` 二进制布局与 `BytecodePool` 体积去重）；<br>2. 实现 C++ `SrpShaderLibrary`（`AssetManager`、`ShaderAsset`、`PipelineLayoutRegistry`、`PsoCache`、`MaterialTagTable`、`MaterialArchetype` 及按 `BindingGroupLayout*` 匹配的分组参数集辅助）。 | 1. C++ 与 Python 校验器读取同一份 `.rnsp` v2 样例通过，覆盖整包 CRC32、偏移重叠与非法关键词负例；<br>2. 双后端原生单测验证：多关键词变体、`LightMode` 查找、缺失 `LightMode` 跳过，以及含/不含额外纹理的两个家族在同一 Pass 下按各自 `Group 0` 布局正确获取参数集且零 RHI 校验错误。 |
| **`M2`：材质原型 ABI、纯 HLSL 全量着色器族与 GLSL 工具清退** | 1. **前移材质注册 ABI**：新增 `rn_register_material_archetype` / `rn_destroy_material_archetype` 与 `rn_intern_tag` 及对应 Java FFM `invokeExact` 绑定，让现有 `ImmediatePass` / `ImmediateDraw` / `InstancedGeometryFeature` 先切到 `SrpShaderLibrary` 运行；<br>2. 编写 `common/`、`vanilla/`、`post/`、`ui/` 全套纯 HLSL 着色器族与 `.shader.json`；<br>3. 在 `RayniumGpuDevice.compilePipeline` 接入按 `(vsId, fsId, defines, vertexFormat)` 匹配的 `VanillaPipelineRouter`（覆盖 `RenderPipelines` 与 `PostChain` 动态管线），彻底删除 `convert_vanilla_shaders.py`、`compile_shader_override.py`、`exportVanillaPipelines`、`default.json`、`ShaderProgramCache` 与 `glslang`/`spirv-cross` 依赖。 | 1. 整仓构建仅使用锁定的 `RadRay` DXC；<br>2. 在尚未切换世界帧图的前提下，双后端启动客户端进入主菜单、背包 GUI、测试世界，触发发光轮廓与旁观者 `PostChain` 滤镜，验证所有原版静态与动态管线 100% 命中 `VanillaPipelineRouter`，画面与交互正常。 |
| **`M3`：C++ SRP 基座与不可变多趟 `RendererList`** | 1. 实现 `FrameBlackboard`、`RenderPassEvent`、`FilteringSettings`（含 `excludedTags` 接管屏蔽）、不可变保序 `WorldDrawStream`（`orderedItems`）+ 每 Pass 独立 `PassDrawState` 的 `RendererList`、`ScriptableRenderPass`、`ScriptableRendererFeature`、`ScriptableRenderer`；<br>2. 实现参数化 `DataDrivenDrawPass` / `BlitPass` / `ComputePass` 与 `rn_mount_shader_pack`；<br>3. 递增 ABI 加入 `rn_world_state` 与 `Opcode::ExecuteWorldPipeline`。 | 双后端 Headless 原生测试：构造交错序列“实例化批次 A → 动态绘制 B → 实例化批次 C”写入单份 `WorldDrawStream`，在同一帧 RDG 内连续由两个不同 `LightMode`、不同附件格式的 `ScriptableRenderPass` 重放至两张离屏纹理，读回像素验证两张输出均包含正确顺序与各自着色器结果，且覆盖 `excludedTags` 接管屏蔽测试。 |
| **`M4`：`WorldCollectorRenderPass` 与 `VanillaRenderer` 全量切换** | 1. 实现 `RayniumDrawSink` 与 `WorldCollectorRenderPass`，在保留 `prepareChunkRenders[Indirect]` 地形捕获的前提下接入 `LevelRenderer` 世界采集并剥离 `modelViewStack` 主相机旋转；<br>2. 落地 `VanillaRenderer` 常驻 Pass 及 `VanillaOitFeature`（OIT 单次采集、三阶段重放、`WaterMask` 船体深度补丁与 `CloudDepth` 顺序）、`VanillaEntityOutlineFeature`（仅写 `entityOutlineTarget`，保留 `GameRenderer` 原 `blitEntityOutline` 合成时机）、`VanillaOverlayGizmoFeature`；<br>3. 清退旧 `TerrainAnchorMixin` 与 `render_program.cpp`。 | 1. 集成用例核验：同一帧内地形、刚体实例化、动态回退各采集一次；开启 OIT 时单个透明实体在三阶段各恰好绘制 1 次（无 10 倍别名放大），船与云同屏深度遮挡正确；手持物遮挡发光实体时轮廓与截图行为同原版一致；<br>2. D3D12 与 Vulkan 开启 RHI/驱动校验层跑通主世界、下界、末地零报错。 |
| **`M5`：86 场景可判定视觉回归、基准冒烟与文档同步** | 1. 执行 Native 双后端全量测试、Java 单测与 86 场景 `VisualValidation`；<br>2. 执行 6 场景基准冒烟核对帧耗与显存（确认 OIT RDG 瞬态纹理正常复用无泄漏）；<br>3. 按落地源码同步更新 `doc/architecture/` 与 `doc/guide/build-test.md`。 | 1. **可判定视觉通过准则**：`validate_visual.py` 双后端 86 场景全部完成且 `check_capture_log` 零错误；`pixelsAbove2 == 0`（`maximumChannelError <= 2`）的场景自动通过；对因原生 HLSL/`precise` 与原版 `shaderc` 浮点舍入差异产生 `pixelsAbove2 > 0` 的场景，逐一核查差分瓦片确认无几何缺失、半透明/轮廓反序、水面漏穿或雾效偏移，并将复核结论归档至验证记录；<br>2. `python tools/check_docs.py` 与 `git diff --check` 通过。 |
