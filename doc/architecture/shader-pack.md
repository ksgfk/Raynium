> - 适用: 修改 shader 资产、离线编译、容器解析与内置资源加载
> - 权威: 本文维护 Raynium shader pack 格式、来源与寿命；C ABI 布局归互操作文档
> - 锚点: `native/include/raynium/shader_pack.h`, `native/src/shader_pack.cpp`, `native/tools/shader_pack.cpp`, `native/shaders/terrain.hlsl`, `tools/compile_shader_pack.py`, `tools/shader_pack.py`, `src/client/java/dev/raynium/bridge/NativeShaderPack.java`, `src/client/java/dev/raynium/client/renderpearl/BuiltinShaderPack.java`, `src/client/resources/assets/raynium/shaderpacks/default.json`, `src/client/resources/assets/raynium/shaderpacks/default.rnsp`

# Shader pack

## 资产与编译

仓库提交的 `default.rnsp` 含 170 个程序，分别提供 DXIL 与 SPIR-V，共 340 个目录条目。其中有 6 个专用地形程序、1 个上屏程序、132 个原版即时与后处理程序，以及 31 个实例化伴生程序（27 个实体模型、4 个粒子）。离线转换器也支持生成 `core/rendertype_entity_shadow` 的实例化变体。随包的 `default.json` 和二进制包均缺少这组变体，游戏使用原版即时阴影管线。

`default.json` 按展开后的顶点与片元源码及宏的摘要索引活动资源、顶点输入和 Push Constant 字节数。带 `instanced` 的条目还记录伴生程序名、模式、双流输入与资源表。`terrain-programs.json` 将地形宏组合的摘要映射到内置程序键。

`exportVanillaPipelines` 导出当前 MC 的管线身份，离线转换器展开 include，分别编译、反射和链接两阶段。转换保留 std140 偏移，按实际纹素格式声明 Buffer，区分 DXIL 和 SPIR-V 的裁剪 Y 与片元 W。构建工具调用锁定的 RadRay shader compiler 生成双目标包，写入后校验目录、CRC 与 artifact。开发命令见[构建与测试](../guide/build-test.md)。

## 容器格式

所有整数为小端定宽值。包上限 64 MiB，目录条目为 1..4096 个；artifact 起点按 8 字节对齐。

| 记录 | 字段与字节布局 |
| --- | --- |
| 包头，24 字节 | `uint32` magic=`0x50534e52`（RNSP）、version=1、总字节数、条目数、目录偏移=24、reserved=0 |
| 目录条目，96 字节 | 程序名 `[64]`；偏移 64/68/72/76 为 `uint32` target、artifact 偏移、大小、CRC32；偏移 80 为 16 字节预期 GpuArtifactHash |

程序名为 1..63 个小写 ASCII 字母、数字或 `_./-`，以 NUL 结束并补零；不用于文件系统路径。
target=0 为 DXIL，1 为 SPIR-V。同一程序/target 不得重复；artifact 不得覆盖目录、越界或相互重叠。
CRC32 使用标准 IEEE 多项式，覆盖整份 artifact。包头版本独立于 RadRay artifact schema。

创建 renderer 时，地形、上屏和即时程序库各自读取包目录并校验各条目的 CRC32。解码具体程序时，native 按名称和后端选取条目，由上游 decoder 校验 schema、target、预期 artifact identity 和锁定的 toolchain identity。
专用地形程序的 Frame 常量占 224 B，复用 256 B 的常量切片。偏移 208 的 `uint4 TerrainFade` 保存当前毫秒、时长、以位表示的浮点时长倒数和零填充。SOLID、CUTOUT、普通透明与 OIT 累积阶段从实例的无符号 `TEXCOORD3` 读取淡入起点。OIT 深度范围与透射率阶段只使用原有的 alpha 输入。

普通资源构建和 JAR 分发检查包结构、CRC32、artifact envelope 与必需程序双后端条目是否存在；完整 metadata 安全性由 native decoder 检查。

## 加载与所有权

设备创建时，`BuiltinShaderPack.load()` 从 Raynium 模组容器读取内置资源。`rn_create_renderer` 在调用期间借用 Java 传入的字节，native 持有包字节及目录，供后续按程序解码、创建着色器和布局。MC 负责资源包搜索顺序和重新加载。

`RayniumPrograms` 按活动着色器引用校验当前管线的 uniform 类型、顶点格式、采样维度及 Push Constant 声明。后台任务只准备 CPU 数据，所属线程的 `finishCompile()` 注册管线，并在命中内置 `instanced` 元数据时一并注册对应的伴生实例化 `RayniumPipeline`（不修改原版 `RenderSystem` 管线查询结果）。共享同一程序的管线各自保存顶点布局、深度、剔除及混合状态。

## 资源包着色器

即时程序优先查内置目录，再查游戏目录的 `.raynium/shader-cache/v2/`。缓存文件同时保存反射信息、包字节和摘要。内存中按程序包字节数计权，单项权重至少 256，总权重上限为 16 MiB。同键并发请求共享加载结果，待注册管线持有自身需要的准备数据。

通过游戏 JVM 属性 `-Draynium.shader.tools-dir=<目录>` 指定编译工具后，后台任务可为缺失或损坏的缓存调用独立编译进程，再原子发布结果。程序缺失且无法编译，或接口不兼容时，游戏收到 `CompiledRenderPipeline.Pending.NULL`。原版地形程序被覆盖时，保留专用内置地形程序，每个设备提示一次。从外部包取得的即时程序只注册普通管线，模型与粒子按原版路径绘制。

后处理 JSON、输入和输出连接由 MC 解释，相应光栅 Pass 经同一命令流执行。
