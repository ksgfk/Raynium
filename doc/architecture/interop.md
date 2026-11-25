> - 适用: 修改 C ABI、FFM、native payload 和加载寿命
> - 权威: 本文维护跨语言、内存所有权与加载契约；操作步骤见构建指南
> - 锚点: `native/include/raynium/bridge.h`, `native/src/bridge.cpp`, `src/client/java/dev/raynium/bridge/*.java`, `src/client/java/dev/raynium/client/RayniumClient.java`, `tools/native.py`

# 互操作与加载

Java 25 通过普通 FFM downcall 加载 `rayniumruntime.dll`，当前 ABI 为 25，共有 29 个导出入口，提供窗口交换链输出、常驻模型网格注册及按请求的异步像素读回。
设备、帧槽与完成序号见 [Native runtime](native-runtime.md)，操作入口见[构建与测试](../guide/build-test.md)。

## 当前 C ABI

`rn_get_abi_version` 在解析其余导出前校验版本。`rn_query_abi` 核对 build ID、指针宽度和编译配置。入口签名以 `bridge.h` 为准，FFM 按固定宽度和 8 字节对齐构建命令记录。设备与资源句柄只在所属 renderer 和所属线程使用。运行时 downcall 按具体签名使用 `invokeExact`。renderer 复用提交、等待、截图查询信息槽和缓冲创建的描述符及输出槽，先取出返回值，再发布完成回调。地形视图和映射脏区间的暂存按需扩缩。
`rn_upsert_model_mesh` 在调用期间复制 24 B/顶点的四边形网格，向 `out_page_id` 写入所在顶点页号。Java 分配从 16 开始的用户句柄，使用 `rn_remove_model_mesh` 退役网格。1 和 2 分别标识内建粒子和阴影单位四边形，3 至 15 保留。

## 提交布局

| 结构 | Windows x64 大小 | 内容 |
| --- | ---: | --- |
| `rn_abi_info` / `rn_device_info` | 240 / 600 B | 构建身份与真实设备的限制、特性和时间戳标定。末尾偏移 592 的 64 位字段 `host_coherent_atom_size` 给出主机缓存同步的原子大小。Java 对超出有符号范围的无符号容量做饱和转换。 |
| `rn_terrain_view_desc` | 312 B | epoch、网格变更、可见项、实例、可见集/实例原点/完整实例三组版本号、相机、变换和雾；支持 `RN_FRAME_REUSE_VISIBLE`/`RN_FRAME_REUSE_INSTANCES` 视图复用标志。批次中可以有多个视图。 |
| `rn_mesh_update` / `rn_terrain_mesh_layout` | 32 / 72 B | 网格更新句柄、顶点指针与必填的预分析布局头，包含每组 quad 计数、包围盒及多面组 3 位线框组号。 |
| `rn_batch_desc` | 128 B | 命令、传输字节、实例字节、脏映射区间、视图数组、surface、测量开关，以及按命令顺序登记的截图编号与时间戳写入。描述符、视图头、脏区间和两组登记项在返回前复制。默认复制负载。设置 `RN_BATCH_BORROW_POOLED` 时，调用方须保持命令、上传数据及视图引用的数组和字节地址有效、内容不变，直至 `consumed_sequence` 达到该批序号。 |
| `rn_submit_result` | 56 B | 偏移 0 为本批入队序号，偏移 8 为完整进度快照。 |
| `rn_wait_result` / `rn_query_sample` | 48 / 16 B | 进度快照保存消费、提交、完成水位，累计呈现帧数，交换链代数、恢复态、最近呈现结果与终态位。查询样本保存提交序号与设备 ticks。 |
| `rn_batch_telemetry` | 152 B | 单个测量批次的序号、GPU 样本有效位与耗时、四项 CPU 时间和 96 B 的帧指标。`rn_drain_telemetry` 按序弹出记录。 |
| `rn_mapped_range` / `rn_capture_info` | 24 / 32 B | 待刷新缓冲区间、颜色截图的尺寸与紧凑行距。对标记 `RN_BUFFER_FENCE_ROTATED` 的缓冲，调用方须等到最后一次使用该区间的 GPU 批次达到 `completed_sequence`，才能重写同一区间。 |

`commands` 中的 Opcode 16（`InstancedGeometry`）携带 64 B 命令头、48 B/项的绑定、16 B/run 的网格与实例区间，以及按 8 字节对齐的 Push Constant。实例偏移从独立的 `instance_uploads` 起算。地形之外的纹理与缓冲传输使用 `uploads`。原生解码核对槽 1 的实例步长和实例区间。

## Java 包装与所有权

创建纹理、缓冲、查询池与管线时取得所属 renderer 的不透明指针。Java 将地址写入 64 位字段。调用方须持有命令和视图引用的资源直至批次消费，无论输入字节采用复制还是借用。Java `CommandTape` 为每个录制批次分配单调递增的 `tapeId`，通过资源对象上的 `lastRetainedTapeId` 戳记以 $O(1)$ 去重保活引用，并按各命令定长协议显式写入全部字段（重置命令流时不整段清零），允许先录制、再关闭、最后提交。超过 16 MiB 的命令、传输和实例流在连续 120 次重置前使用量低于容量四分之一时释放，地形帧存储按每次预留量执行相同的回收策略。native 保存帧槽使用中的对象。Java 在入队时将正序号绑定到 Fence。池化的 `InFlightBatch` 按消费水位回收借用存储与资源引用，按提交水位派发提交回调，按完成水位派发读回回调。未入队批次逆序撤销地形驻留变更。

`rn_wait_submit` 返回真实完成值，支持轮询、限时和无限等待。可读映射与回调按已完成的序号对外发布。提交、等待、查询同步与排空在返回前将已发布的读回字节复制到前端映射。非设备丢失的终态保留最后完整完成批次的查询、截图与只读映射，设备丢失态保留最后完整完成批次的查询与已复制到前端的只读映射（颜色截图随设备丢失失效），关闭仍可释放句柄。`rn_flush_renderer` 排空呈现并返回最近呈现结果。直接关闭健康的 Java renderer 时，它先排空尚未观察到完成的在途批次，在映射仍可读时派发完成回调。

## 构建身份与 payload

`native/CMakeLists.txt` 用独立 binary 目录将锁定 RadRay 作为源码子工程引入，DLL 链接上游 `RadRay::runtime` 静态库，另静态链接 `RadRay::shader` decoder；依赖传递到 render/core，导出清单由 tools/native.py 校验。
Windows x86_64 的 Debug 固定 `/MTd`，Release 固定 `/MT`，bridge、RadRay 和所有链接的依赖必须匹配对应配置的 CRT。
Debug 使用 Raynium 显式准备的独立依赖前缀，Release 使用 RadRay 的 `windows-x64-msvc-staticcrt` 三元组。默认受管源码的版本由锁文件约束，本地覆盖和 dirty 源码可用于开发；分发要求锁定版本的干净源码。
CMake 拒绝不匹配的 CRT/依赖配置，bridge 编译期拒绝动态 CRT并断言 Debug/Release 的 STL iterator 等级分别为 2/0。
两配置都通过 whole-archive 链接静态 mimalloc，payload 只有 `rayniumruntime.dll`。Debug 的 mimalloc 使用 `MI_OVERRIDE=OFF`，
保留显式 `mi_*` 分配，普通 CRT/new/delete 使用 Debug CRT；Release 保留 `MI_OVERRIDE=ON` 静态覆盖。
bridge 不包含重复的 new/delete 替换头。
Native 运行库、工具和测试直接使用锁定 RadRay 的 `radray/types.h` 导出的容器及相关类型。
`radray::string`、`radray::vector`、map/set 及哈希容器通过 `radray::allocator` 使用 `mi_stl_allocator`。
`radray::array`、智能指针及 `make_shared` / `make_unique` 是标准库的重导出，不额外指定 allocator；其动态分配遵循当前 CRT 与静态覆盖配置。
没有对应导出的 `std::span`、`std::string_view`、`std::pair`、`std::tuple` 等类型及标准算法继续使用标准库。
配置阶段不恢复依赖，发布要求锁定提交的干净 Release 源码。当前未实现 RadRay 安装包消费模式。

stage 使用 `dumpbin /exports` 核对构建脚本列出的 ABI 入口，再用 `/dependents` 拒绝动态 CRT 和未分类的私有 DLL，收集上游许可证，并先验证完整快照再发布 `current.json`。
schema 1 manifest 包含平台、ABI/布局、构建配置、CRT、工具链、build ID、RadRay commit/dirty、DLL/许可证大小与 SHA-256、系统依赖分类。
ABI 25 的身份 flags 中 bit 1（static CRT）和 bit 2（mimalloc）均为 1。loader 只接受 Debug/MTd 或 Release/MT 的单 DLL 清单，拒绝错配和 MD 的 schema 2 payload。
build ID 来自 native/RadRay（含 `modules/runtime/`）源码及配置/工具链指纹；DLL SHA-256 标识实际二进制，manifest SHA-256 标识内容快照，三者用途不同。
哈希用于一致性检查，不等于来源认证。

```text
build/native/stage/windows-x86_64/<Debug|Release>/
  current.json
  <manifest-sha256>/
    native-manifest.json
    rayniumruntime.dll
    licenses/<dependency>/...
```

JAR 中 DLL/manifest 位于 `natives/windows-x86_64/`，许可证位于 `META-INF/licenses/raynium-native/`。
不打包 PDB、import/static lib、SDK 全目录或本机绝对路径配置；PDB 保存在本地构建目录。
平台匹配依据 JVM 的 OS/架构，当前接受 Windows 的 `amd64/x86_64`。

## 加载流程

loader 的选择顺序为：显式开发目录 `-Draynium.native.dir=...`，否则读取 mod JAR 中的 payload 并检查平台。开发目录无效时直接报告具体路径，不回退到旧内置版本。

两条来源共用后续流程：

1. 读取 manifest，确认 JVM 架构、ABI 和配置。
2. 将实际 payload 发布到游戏目录下 `.raynium/natives/windows-x86_64/<content-hash>/`。
3. 检查已有完整副本，或写入独立 staging 目录，校验后再发布；多进程使用文件锁协调解包。
4. 客户端初始化入口检查游戏 JVM native access 权限，将规范化的绝对路径转换为 Windows 扩展路径（含 UNC），使用 `SymbolLookup.libraryLookup(String, Arena)` 加载 bridge；保留长路径前缀，避免 Path 重解析后触发 Windows 路径长度限制。
5. 先调用版本入口核对 ABI 25，再用 `rn_query_abi` 检查布局与构建身份。
6. 游戏后端执行 loadLibrary 时创建环境。createDevice 创建 runtime 与 renderer，createSurface 将交换链关联到 SDL 窗口。

所有内容路径落在 loader 自己的根目录内，只接受固定 DLL 文件名并验证大小和哈希。已有损坏缓存直接报错，不覆盖、不删除；暂未实现旧缓存自动清理。

开发也加载内容副本，而不是直接加载 linker 输出目录中的 DLL。Windows 占用已加载 DLL 时，这样仍能重编译新的 bridge；重启客户端后加载新 build。Java 热更新不能替代 native ABI 或 DLL 的重新加载。

### DLL 的依赖搜索

主 DLL 使用绝对路径，并不保证其间接依赖会自动从同目录找到。Windows 对依赖 DLL 的搜索和进程已加载模块仍有独立规则。[Windows DLL 搜索顺序](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)

当前静态链接 RadRay、CRT 与 mimalloc，保留 Windows/驱动系统依赖；无需为 Raynium 额外安装动态 VC++ CRT。
没有 mimalloc/redirect DLL、Fabric preLaunch 分配器加载器或进程级 CRT 重定向。静态分配器随 bridge 加载与卸载；所有 native owner 必须先关闭。

不通过修改进程全局 PATH 或 SetDllDirectory 来影响其他 Minecraft 模组。当前没有为不同模组的同名 DLL 提供版本隔离机制。

驱动和系统图形 DLL 不从开发机复制进 mod；shader 离线编译为内置资源 `.rnsp`，运行时不加载或打包 `dxcompiler.dll`。资产格式与寿命见[Shader pack](shader-pack.md)。
