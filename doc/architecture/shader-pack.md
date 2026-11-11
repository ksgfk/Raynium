> - 适用: 修改 shader 资产、离线编译、容器解析与内置资源加载
> - 权威: 本文维护 Raynium shader pack 格式、来源与寿命；C ABI 布局归互操作文档
> - 锚点: `native/include/raynium/shader_pack.h`, `native/src/shader_pack.cpp`, `native/tools/shader_pack.cpp`, `native/shaders/terrain.hlsl`, `tools/compile_shader_pack.py`, `tools/shader_pack.py`, `src/client/java/dev/raynium/bridge/NativeShaderPack.java`, `src/client/java/dev/raynium/client/terrain/BuiltinShaderPack.java`, `src/client/resources/assets/raynium/shaderpacks/default.rnsp`

# Shader pack

## 资产与编译

默认资产为 `assets/raynium/shaderpacks/default.rnsp`，资源标识为 `raynium:shaderpacks/default.rnsp`。
它保存八个命名程序的 DXIL 和 SPIR-V artifact，每个程序按后端索引：
`terrain`、`terrain_cutout`、`terrain_translucent`、`terrain_oit_bounds`、`terrain_oit_transmittance`、
`terrain_oit_accumulate`、`oit_bounds_cull` 和 `oit_composite`。
各入口文件共享顶点、采样、雾和 OIT include；alpha-only 程序不采样 lightmap。
当前没有纹理、材质资产或 keyword variant 选择。

离线工具调用锁定 RadRay 的 `shader_compiler::Client`，对带 `[shader]` 阶段标记的 HLSL 执行 source discovery 和双 target 编译。
每份 RadRay metadata artifact 原样包含阶段字节码、入口、绑定、类型树与顶点输入。
当前消费上游 metadata schema 8；精确依赖以 `native/radray.lock.json` 为准，上游契约入口为
`modules/shader/include/radray/shader/shader_compiler_contract.h` 与 `shader_artifact.h`。

工具验证所有 artifact 和 terrain 接口后才写入临时包；Python 入口在成功后替换目标资源。
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
普通资源构建和 JAR 分发检查包结构、CRC32、artifact envelope 与八程序双后端条目是否存在；完整 metadata 安全性由 native decoder 检查。

## 加载与所有权

`PreviewOverlay` 首次创建 renderer 时，从 MC 26.3 `ResourceManager.listPacks()` 找到实现 Fabric
`ModPackResources` 且提供者 ID 为 `raynium` 的包，再读取默认资产；不按用户资源包优先级替换。
资源流和 pack 枚举流在读取后关闭，pack owner 仍由 MC 管理。
Java `NativeShaderPack` 拥有不可变 CPU 字节快照，跨世界 renderer 重建时复用；F3+T 不替换这份快照。
MC/Fabric 版本来自 `gradle.properties`，资源 API 源码获取见[开发环境](../guide/dev-environment.md#阅读-minecraft-源码)。

`NativeRuntime.createRenderer(NativeShaderPack)` 将字节复制到本次调用的 confined arena。
`rn_create_renderer` 只在调用期间借用包；MeshPassResources 拥有 decoder 复制的选中 artifact 和 GPU shader。
阶段入口从 native 自持有的 artifact 读取；Java arena 关闭后 native 不再读取调用方内存。
完整 ABI 和失败诊断约束见[互操作与加载](interop.md)。

各程序的 RHI 布局与渲染状态由适配层显式提供。创建前按程序校验资源绑定、vertex/pixel 阶段、顶点输入及 Frame 的成员布局、矩阵方向和标量类型。
完整地形使用五绑定和六输入，alpha-only 使用 Frame/Atlas/AtlasSampler 及四输入；全屏程序没有顶点输入，各 OIT 程序另校验所需的 Group 2 纹理。
Frame 使用 192 字节有效布局和 256 字节上传缓冲。pipeline layout 只声明该程序使用的 group；全部程序和 PSO 成功后才构造 renderer。
布局及渲染状态不会从 metadata 自动生成，缺失任何必需程序也会拒绝创建。
失败清空创建输出，经诊断缓冲返回原因。客户端沿用预览禁用路径，原版渲染继续执行。
