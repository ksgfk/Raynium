> - 适用: 修改 shader 资产、离线编译、容器解析与内置资源加载
> - 权威: 本文维护 Raynium shader pack 格式、来源与寿命；C ABI 布局归互操作文档
> - 锚点: `native/include/raynium/shader_pack.h`, `native/src/shader_pack.cpp`, `native/tools/shader_pack.cpp`, `native/shaders/terrain.hlsl`, `tools/compile_shader_pack.py`, `tools/shader_pack.py`, `src/client/java/dev/raynium/bridge/NativeShaderPack.java`, `src/client/java/dev/raynium/client/terrain/BuiltinShaderPack.java`, `src/client/resources/assets/raynium/shaderpacks/default.rnsp`

# Shader pack

## 资产与编译

内置 `default.rnsp` 共包含 174 个程序，含六个专用地形程序、一个上屏程序、原版即时管线与后处理使用的 132 个去重程序，以及 35 个内置世界实例化伴生程序（27 个 `core/entity`、4 个 `core/particle` 和 4 个 `core/rendertype_entity_shadow` 的 `*_inst` 变体）。每个程序各含 DXIL 与 SPIR-V。随包的 `default.json` 按展开源码和宏的 SHA-256 记录活动资源、顶点输入、Push Constant 字节数，以及对应伴生实例化程序的 `instanced` 元数据（程序名、模式、双流输入与资源表）。`terrain-programs.json` 记录内置地形宏组合。

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

Native 在创建 renderer 时一次校验整个目录与 CRC32，再按程序/后端选择条目，调用上游 decoder 校验 schema、target、预期 artifact identity 和锁定 toolchain identity。
普通资源构建和 JAR 分发检查包结构、CRC32、artifact envelope 与必需程序双后端条目是否存在；完整 metadata 安全性由 native decoder 检查。

## 加载与所有权

设备创建时，`BuiltinShaderPack.load()` 从 Raynium 模组容器读取内置资源。`rn_create_renderer` 借用该次调用传入的字节，native 保有已解码的 artifact、着色器和布局。MC 仍负责资源包搜索顺序和重新加载。

`RayniumPrograms` 按活动着色器引用校验当前管线的 uniform 类型、顶点格式、采样维度及 Push Constant 声明。后台任务只准备 CPU 数据，所属线程的 `finishCompile()` 注册管线，并在命中内置 `instanced` 元数据时一并注册对应的伴生实例化 `RayniumPipeline`（不修改原版 `RenderSystem` 管线查询结果）。共享同一程序的管线各自保存顶点布局、深度、剔除及混合状态。

## 资源包着色器

即时程序优先查内置目录，再查游戏目录的 `.raynium/shader-cache/v2/`。缓存文件同时保存反射信息、包字节和摘要。设置 `raynium.shaderToolsDir` 后，可由后台进程调用独立编译工具生成并原子发布缓存。缺少可用程序或接口不兼容时，编译失败并向游戏返回空的待完成管线。原版地形程序被覆盖时，保留专用内置地形程序，每个设备提示一次。资源包覆盖实体、粒子或阴影着色器时不含内置 `instanced` 伴生条目，对应管线不挂载实例化变体，绘制自动走原版即时路径。

后处理 JSON、输入和输出连接由 MC 解释，相应光栅 Pass 经同一命令流执行。
