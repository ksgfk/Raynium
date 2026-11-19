> - 适用: 修改 shader 资产、离线编译、容器解析与内置资源加载
> - 权威: 本文维护 Raynium shader pack 格式、来源与寿命；C ABI 布局归互操作文档
> - 锚点: `native/include/raynium/shader_pack.h`, `native/src/shader_pack.cpp`, `native/tools/shader_pack.cpp`, `native/shaders/terrain.hlsl`, `tools/compile_shader_pack.py`, `tools/shader_pack.py`, `src/client/java/dev/raynium/bridge/NativeShaderPack.java`, `src/client/java/dev/raynium/client/terrain/BuiltinShaderPack.java`, `src/client/resources/assets/raynium/shaderpacks/default.rnsp`

# Shader pack

## 资产与编译

默认资产为 `assets/raynium/shaderpacks/default.rnsp`，资源标识为 `raynium:shaderpacks/default.rnsp`。
它保存八个地形、三个 GUI、一个输出程序和 133 个原版转换程序的 DXIL 和 SPIR-V artifact，每个程序按后端索引。原版程序覆盖 195 个静态管线身份，渲染状态相同与否不影响 shader 源码/宏组合去重。地形程序为：
`terrain`、`terrain_cutout`、`terrain_translucent`、`terrain_oit_bounds`、`terrain_oit_transmittance`、
`terrain_oit_accumulate`、`oit_bounds_cull` 和 `oit_composite`。
各入口文件共享顶点、采样、雾和 OIT include；alpha-only 程序不采样 lightmap。
GUI 程序为 `gui_color`、`gui_textured`、`gui_text`，共享 `gui_common.hlsli`，由独立的 GuiPassResources 与 ValidateGuiShader 消费。
`output` 使用无顶点缓冲的全屏三角形和像素 Load，将内部 RGBA8 颜色输出至 BGRA8 交换链，由 ValidateOutputShader 校验单纹理接口。
当前没有纹理、材质资产或 keyword variant 选择。

离线工具调用锁定 RadRay 的 `radray::shader::Client`，对带 `[shader]` 阶段标记的 HLSL 执行 source discovery 和双 target 编译。
每份 RadRay metadata artifact 原样包含阶段字节码、入口、绑定、类型树与顶点输入。
当前消费上游 metadata schema 10，资源绑定种类通过 `DecodeBindingKind` 解码。类型记录区分数组步长、元素大小和矩阵步长。精确依赖以 `native/radray.lock.json` 为准，上游契约入口为
`modules/shader/include/radray/shader/shader_compiler_contract.h` 与 `shader_artifact.h`。

工具验证所有 artifact、terrain 与 GUI 接口后才写入临时包；Python 入口在成功后替换目标资源。
条目按程序名和 target 排序，空白填零，不保存时间戳或机器路径。
生成的二进制作为源码资源提交，普通构建只验证并复制，不调用 DXC。
生成、依赖准备与重编命令见[构建与测试](../guide/build-test.md)。

## 容器格式

所有整数为小端定宽值。包上限 64 MiB，目录条目为 1..4096 个；artifact 起点按 8 字节对齐。

| 记录 | 字段与字节布局 |
| --- | --- |
| 包头，24 字节 | `uint32` magic=`0x50534e52`（RNSP）、version=1、总字节数、条目数、目录偏移=24、reserved=0 |
| 目录条目，96 字节 | 程序名 `[64]`；偏移 64/68/72/76 为 `uint32` target、artifact 偏移、大小、CRC32；偏移 80 为 16 字节预期 GpuArtifactHash |

程序名为 1..63 个小写 ASCII 字母、数字或 `_./-`，以 NUL 结束并补零；不用于文件系统路径。
target=0 为 DXIL，1 为 SPIR-V。同一程序/target 不得重复；artifact 不得覆盖目录、越界或相互重叠。
CRC32 使用标准 IEEE 多项式，覆盖整份 artifact。包头版本独立于 RadRay artifact schema。

Native 在创建 renderer 时一次校验整个目录与 CRC32，再按程序/后端选择条目，调用上游 decoder 校验 schema、target、预期 artifact identity 和锁定 toolchain identity。
普通资源构建和 JAR 分发检查包结构、CRC32、artifact envelope 与十二程序双后端条目是否存在；完整 metadata 安全性由 native decoder 检查。

## 加载与所有权

`TakeoverRuntime` 在窗口 surface 创建时调用 `BuiltinShaderPack.load()`，从 FabricLoader 提供者 ID 为 `raynium` 的模组容器读取默认资产。
该读取早于 MC ResourceManager 就绪，不按用户或其它模组资源包优先级替换。读取流和来源枚举流在读取后关闭。
Java `NativeShaderPack` 拥有不可变 CPU 字节；世界切换保留 renderer，F3+T 不替换内置 shader。
MC/Fabric 版本来自 `gradle.properties`，源码获取见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)。

`NativeRuntime.createRenderer(NativeShaderPack)` 将字节复制到本次调用的 confined arena。
`rn_create_renderer` 只在调用期间借用包；MeshPassResources 拥有 decoder 复制的选中 artifact 和 GPU shader。
阶段入口从 native 自持有的 artifact 读取；Java arena 关闭后 native 不再读取调用方内存。
完整 ABI 和失败处理约束见[互操作与加载](interop.md)。

地形、现有 GUI 和输出程序的 RHI 布局与渲染状态由适配层显式提供。创建前按程序校验资源绑定、vertex/pixel 阶段、顶点输入及 Frame 的成员布局、矩阵方向和标量类型。
完整地形使用五绑定和六输入，alpha-only 使用 Frame/Atlas/AtlasSampler 及四输入；全屏程序没有顶点输入，各 OIT 程序另校验所需的 Group 2 纹理。
Frame 使用 192 字节有效布局和 256 字节上传缓冲。pipeline layout 只声明该程序使用的 group；地形、GUI 和输出的程序及 PSO 成功后才构造 renderer。
场景程序在 Prepare 按需解码，依 artifact 的实际资源及顶点接口创建布局；声明绑定、栅格状态与顶点步长/偏移来自生成的 `native/src/scene_catalog.inc`，按完整管线编号缓存 PSO。输入必须属于声明接口，仅向编译产物保留的绑定写入参数。所需程序缺失时准备失败。
GUI 使用独立的 16 字节 GuiViewport 常量、24 字节顶点、纹理与采样器组；灰度文字复制 R 通道至 RGBA，文字 alpha 小于 0.1 时丢弃。
GUI 的十二种混合管线固定 RGBA8 输出，无深度，顶点及索引由每槽上传缓冲持有。
失败清空创建输出，详细原因在 native 校验或创建失败处记录，只向客户端返回失败状态；客户端不能继续创建接管 surface。

## 原版离线转换

`exportVanillaPipelines` 从当前 MC 类导出完整管线身份，包括同名不同宏及 OIT 字段。
`tools/convert_vanilla_shaders.py` 接受该目录、原版 shader 目录和显式指定的 glslang/SPIRV-Cross，将有限的原版程序转为 HLSL，输出到构建目录。
生成端保留 std140 字节布局，以 uint4 常量块和访问表达式消除 DXIL/SPIR-V 的 float3 反射差异；不支持的结构或内建输入写入接口报告并以非零状态退出。
转换器先删除片元阶段不消费的顶点输出，再由 DXC 编译两阶段，保持独立签名打包时的接口一致。
片元位置的 `w` 在 Vulkan 中已是裁剪 `w` 的倒数，转换后的 SPIRV-Cross 倒数适配仅编入 DXIL，避免 Vulkan 光束雾距离被二次求逆。
点大小通过六角点实例展开表达；额外 SceneBackend 常量统一裁剪 Y 方向，原版常量布局保持原样。
转换器为两个 native 后端注入 `RENDERPEARL_DEPTH_IS_ZERO_TO_ONE`，与零到一投影和 OIT 深度换算一致。MC 26.3 的 `GlslCompiler.createBaseShaderOptions` 同样为 Vulkan 注入该宏。
`tools/generate_scene_catalog.py` 从同一导出目录和转换接口生成 native 栅格描述及 Java 的 `scene-interfaces.json`；Java 对照当前目录的 ID 和名称拒绝不匹配输入。
转换程序已进入内置包，由 ScenePassResources 消费；普通构建不调用转换工具，不接收资源包 shader 扩展。

转换工具同时读取原版五个内置 post_effect JSON，生成 post-interfaces.json 与 post-chains.json；generate_post_catalog.py 固化其目标连接、采样模式和 std140 常量。compile_shader_pack.py 将对应八个程序与场景程序一起编译为 DXIL/SPIR-V。原版轮廓最终合成复用 ENTITY_OUTLINE_BLIT 的混合状态。
原版 Vulkan 在呈现及截图时翻转内部目标，native 内部目标则保持正向。后处理保留原版顶点 UV 和变换计算，以对应的 clip Y 方向绘制，并反射输入采样位置。nearest 路径先选择整数 texel 再反射行号，避免反射改变边界两侧的选择；模糊保留原版线性采样。Prepare 只向反射结果中实际存在的纹理及采样器绑定写入参数。
