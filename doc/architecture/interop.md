> - 适用: 修改 C ABI、FFM、native payload 和加载寿命
> - 权威: 本文维护已实现的跨语言、内存所有权与加载契约；操作步骤见构建指南
> - 锚点: `native/include/raynium/bridge.h`, `native/src/bridge.cpp`, `src/client/java/dev/raynium/bridge/*.java`, `src/client/java/dev/raynium/client/RayniumClient.java`, `tools/native.py`

# 互操作与加载

当前 Java 25 通过 FFM 加载 `rayniumruntime.dll`，C ABI 为 6。接口包含版本/构建身份查询、环境初始化/销毁、设备会话和共享 UV 预览；没有通用资源、场景帧或读回入口。
运行命令与验证范围见[构建与测试](../guide/build-test.md)，C++ 设备所有权见[Native runtime](native-runtime.md)。

## 当前 C ABI

| 导出 | 契约 |
|---|---|
| `rn_get_abi_version()` | ABI 版本 `6` |
| `rn_query_abi(info, capacity, error_message, error_capacity)` | 填充调用方的 240 字节结构；失败不改动 info，原因写入诊断字符串 |
| `rn_init_env(desc, error_message, error_capacity)` | 创建 DLL 单例环境，不返回句柄；重复初始化失败且保留原环境；不创建 GPU 设备 |
| `rn_destroy_env(error_message, error_capacity)` | 无环境时成功；存在 runtime 或从其他线程调用时失败并保留环境；成功后释放 factory/instance |
| `rn_create_runtime(desc, out_runtime, error_message, error_capacity)` | 在已有环境中通过 RayniumRuntime::Create 创建唯一设备会话；失败清理局部资源并清零输出槽，保留环境 |
| `rn_destroy_runtime(runtime)` | 预览全部销毁后，等待 Direct 队列并销毁对象，NULL 无操作；非空对象只能销毁一次 |
| `rn_create_preview(runtime, desc, out_preview, info, capacity, error, error_capacity)` | 每 runtime 一个预览；返回不透明句柄和 128 字节导入描述；失败清空输出句柄且保持 info 不变；创建不提交 GPU 工作 |
| `rn_draw_preview(preview, sequence, error, error_capacity)` | 序号从 1 严格递增，提交 UV 绘制；下一次调用前宿主必须提交消费及 consumed signal |
| `rn_destroy_preview(preview, error, error_capacity)` | 宿主先完成消费并释放导入对象；等待 native 完成后销毁，NULL 成功；超时保留活跃对象 |

全部状态入口只返回 OK/FAILED；具体原因写入 UTF-8 诊断字符串，不从返回码区分参数、容量或设备失败。NULL 诊断指针必须配零容量；所有参数缓冲区互不重叠。

`bridge.h` 由 C++ 编译器使用，导出函数显式采用 `extern "C"` 和 `noexcept`，不提供纯 C 编译兼容分支。
状态、后端、runtime/env 标志和共享句柄类型使用非作用域 `enum : uint32_t`；
状态函数返回 `rn_status`，相关描述字段直接使用对应枚举类型。
枚举底层类型、数值、结构尺寸/对齐及导出符号保持原有 ABI 6 契约，Java FFM 继续以 32 位整数读写。
`RN_ABI_VERSION` 保留为版本宏，DLL 导入/导出修饰由 `RAYNIUM_API` 宏选择。

`rn_env_desc` 为 12 字节、4 字节对齐：struct_size/backend/flags。
backend 为 1（D3D12）或 2（Vulkan）；flags 位值 1（`RN_ENV_VALIDATION`）请求 driver validation。
`rn_runtime_desc` 为 20 字节、4 字节对齐：struct_size/flags/node_mask 后跟 8 字节 LUID；
flags 位值 1（`RN_RUNTIME_MATCH_GPU`）请求匹配宿主 GPU，匹配时 node_mask 必须为 1。
后端和校验配置只从环境取得。D3D12 validation 环境初始化必须早于宿主图形初始化。
`rn_preview_desc` 为 16 字节的 struct_size/width/height/reserved，reserved 必须为 0。
参数仅在调用期间借用。诊断容量大于零时截断并 NUL 终止；native 对象只能由同一 DLL 的 destroy 释放。

ABI 结构布局以 `bridge.h` 为准；查询结构包含四个 uint32 和固定容量 UTF-8 字符串。Java 检查尺寸、指针宽度、flags、build ID、RadRay revision、配置、平台和工具链与 manifest 一致。结构和临时内存由 Java arena 管理，不跨边界 free/delete。`NativeBindings` 使用普通 FFM downcall，不使用 critical 调用。

ABI 6 的状态仍仅为 `RN_STATUS_OK=0` 和 `RN_STATUS_FAILED=1`，240 字节查询结构保持不变。
Java 在调用 query 前核对版本，manifest 也只接受 ABI 6；旧 payload 必须重新构建/stage。
stage 和 JAR 只收集 `rayniumruntime.dll`，不自动删除旧构建目录或内容缓存。

## 接口边界

当前调用链是 `RayniumClient → NativeEnvironment/NativeRuntime → NativeBindings/FFM → C ABI → RayniumEnvironment/RayniumRuntime → RadRay RHI`。
`bridge.h/.cpp` 承担跨语言入口，DLL 导出两个查询、环境 init/destroy、runtime create/destroy 和三个预览函数。没有 Java 场景数据或完整帧输入进入 native。

`NativeBindings` 用 `FunctionDescriptor` 描述签名，在库加载时查找符号并缓存 MethodHandle，通过 `invokeExact` 调用；全部使用普通 downcall，没有 critical 选项或 upcall。

| 当前 C ABI 数据 | Java 表达 | 用途 |
| --- | --- | --- |
| `uint32_t`、底层为 `uint32_t` 的枚举 | `ValueLayout.JAVA_INT` / `int` | 状态、版本、尺寸、容量、flags 和后端枚举 |
| `rn_runtime*` 及指针参数 | `ValueLayout.ADDRESS` / `MemorySegment` | 不透明 runtime、结构与诊断缓冲区地址 |
| `rn_preview*` | `ValueLayout.ADDRESS` / `MemorySegment` | 不透明预览对象 |
| `uint64_t` | `ValueLayout.JAVA_LONG` / `long` | Win32 句柄值、分配大小/偏移和交接序号 |
| 固定容量 `char[]` | `JAVA_BYTE` sequence | 查询结构中的 UTF-8、NUL 结尾字符串 |

JDK linker 依据平台 ABI 和调用方提供的 `FunctionDescriptor` 建立调用，不从符号名称推导参数类型。
当前 C 入口不暴露 C++ 对象布局、容器、引用或 Java 对象。见 [JDK 25 Linker](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.html)。

## 布局与版本

`NativeBindings` 手写查询/runtime 的 `StructLayout`，环境描述按固定字段偏移写入。
native 侧用 `static_assert` 检查五种枚举底层类型均为 `uint32_t`，以及查询结构 240 字节、环境描述 12 字节与 runtime 描述 20 字节（均为 4 字节对齐）、预览描述 16 字节、导入描述 128 字节及关键偏移。
Java `NativePreview.Info.read` 显式读取导入描述，实际 GPU 共享测试覆盖两种后端的字段传递。

| rn_preview_info 字段组 | 字节偏移 |
| --- | --- |
| struct_size、handle_type、width、height | 0、4、8、12 |
| memory_handle、ready_handle、consumed_handle、allocation_size | 16、24、32、40 |
| memory_type_index、node_mask、luid[8] | 48、52、56 |
| device_uuid[16]、driver_uuid[16] | 64、80 |
| native_format、native_usage、native_flags、native_tiling | 96、100、104、108 |
| allocation_offset、dedicated、reserved | 112、120、124 |

handle_type 为 1 时表示 D3D12Resource/D3D12Fence，为 2 时表示 OpaqueWin32。
opaque 路径必须使用导出端的完整 Vulkan 元数据；D3D12 路径由导入设备查询内存类型。
当前没有 ResourceBatch/FramePacket 布局、schema 协商或自动绑定生成器。

`rn_query_abi` 返回布局、ABI 和构建身份。Java 在调用 query 前先检查版本，再将查询内容与 manifest 对照；不兼容版本会被拒绝。
JDK `MemoryLayout.structLayout` 不自动插入 C 编译器 padding。见 [JDK 25 MemoryLayout](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/MemoryLayout.html)。

## 内存所有权与借用

| 当前数据 | 所有者 | 寿命 |
| --- | --- | --- |
| 创建参数、输出指针槽、查询结构和诊断缓冲区 | Java confined scratch arena | 本次同步调用期间；native 不保留输入地址 |
| 单例环境、DXGI factory/Vulkan instance | DLL 环境槽位 | init 成功至 destroy_env 成功；必须覆盖全部 runtime 寿命 |
| `rn_runtime*` 指向的对象 | DLL | 成功创建后存活至对应 destroy |
| `rn_preview*`、导出 Win32 句柄、共享纹理/fence | native preview | 宿主完成消费、销毁导入对象且 native 工作完成后销毁 |
| 导入 VkImage/VkDeviceMemory/semaphore | Java VulkanPreviewImage | 显式排空后关闭；不拥有导出 Win32 句柄 |
| RHI 设备与队列引用 | native runtime/设备 | 由 runtime 的创建和关闭路径管理 |
| DLL 符号及绑定 | `NativeLibrary` | library arena 关闭前有效 |

输出槽的 arena 关闭不释放槽中返回地址指向的 runtime。Java 不解引用该对象地址；native 对象由同一 DLL 的 destroy 释放，Java arena 内存由 Java 关闭。
`MemorySegment` 的 Java 边界检查不约束 C++ 裸指针访问；`reinterpret` 也不验证地址真实容量或转移所有权。见 [JDK 25 MemorySegment](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/MemorySegment.html)。

## 线程与调用粒度

生命周期和逐帧调用来自 MC 渲染线程。`NativeEnvironment` 保存初始化线程，环境、runtime、预览操作沿用该线程检查。
native 创建 runtime 和销毁环境同样检查环境所属线程；所有调用由调用方串行执行，ABI 查询不依赖环境。
library 使用 shared arena，参数和返回缓冲区使用 confined scratch arena；shared arena 不使 native runtime 自动线程安全。
每帧通过一次 draw downcall 提交预览；没有资源批次、native 工作线程或跨语言共享命令队列。

## 错误与回调

状态返回入口只返回 `RN_STATUS_OK` 或 `RN_STATUS_FAILED`，原因写入调用方提供的诊断缓冲区；没有全局 last-error 或 Java 日志回调。
Java 将失败状态转换为 `IllegalStateException`；预览创建/导入失败由 MC 适配层降级，等待失败保留资源。
当前 C ABI 函数声明为 `noexcept`，bridge 没有异常捕获/转换层；异常逃逸不等于返回 FAILED。
底层 RHI 日志直接输出，当前没有 upcall stub。

## 会话和库的寿命

`SymbolLookup.libraryLookup(Path, Arena)` 将库加载寿命关联到 arena。库不能先于其对象、函数指针或回调关闭。见 [JDK 25 SymbolLookup](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/SymbolLookup.html)。

`NativeEnvironment implements AutoCloseable` 拥有成功初始化的单例环境和 `NativeLibrary`，
后者保存绑定和 DLL arena。环境通过 `createRuntime(hostGpu)` 创建至多一个 `NativeRuntime`，
runtime 持有父环境引用、设备句柄和至多一个 `NativePreview`，关闭 runtime 不卸载 DLL。

游戏在客户端初始化入口加载 DLL 并初始化所选后端的环境；CLIENT_STARTED 再创建设备和预览。
初始化失败只关闭本次加载的 library；重复初始化失败不销毁原有环境。
设备创建失败保留环境，客户端启动失败处理按预览 → runtime → 环境 → DLL 顺序清理。

Java close 幂等，有活跃预览时拒绝关闭 runtime，有活跃 runtime 时拒绝关闭环境。
预览关闭成功后才排空 Direct 队列并销毁 runtime；环境销毁成功后才关闭 library arena。
预览创建成功但 Java 无法解析返回描述时，立即销毁尚未提交的 native 预览。
清理失败保留未关闭的 Java 所有者与 DLL，不继续向上释放；没有引用计数或隐式级联销毁。

`NativeLibrary` 的 shared arena 不代表 RHI 支持并发；当前全部生命周期操作在 MC 渲染线程串行执行。创建描述、输出指针槽与诊断缓冲区使用 confined scratch arena，输出槽关闭不释放其指向的 runtime。Java 不解引用或 reinterpret 该对象地址；只有 DLL 的 destroy 负责释放。Library、runtime、调用 scratch 是不同寿命，不使用 GC/Cleaner 作为正常 GPU 资源回收机制。

Fabric 当前使用 unnamed module，游戏 JVM 继续显式设置 `--enable-native-access=ALL-UNNAMED`。不能把该参数只写入 Gradle daemon，也不能依靠普通 mod JAR 的 manifest 给宿主 JVM 授权；官方 manifest 入口针对可执行 JAR。见 [JDK 25 Restricted Methods](https://docs.oracle.com/en/java/javase/25/core/restricted-methods.html)。

## 构建身份与 payload

`native/CMakeLists.txt` 用独立 binary 目录将锁定 RadRay 作为源码子工程引入，静态链接 `RadRay::render/core`，只导出 bridge 的九个 C 入口。
当前只生成一个 Windows x86_64 DLL，Debug/Release 均使用 `/MT` 和静态 mimalloc，Debug 保留调试宏而不启用 `/MTd`；bridge 不再包含重复的 new/delete 替换头。
配置阶段不恢复依赖，发布要求锁定提交的干净 Release 源码。当前未实现 RadRay 安装包消费模式。

stage 使用 `dumpbin /dependents` 检查直接依赖，收集上游许可证，并先验证完整快照再发布 `current.json`。
manifest 包含平台、ABI/布局、构建配置、CRT、工具链、build ID、RadRay commit/dirty、DLL/许可证大小与 SHA-256、系统依赖分类。
build ID 来自 native/RadRay 源码及配置/工具链指纹；DLL SHA-256 标识实际二进制，manifest SHA-256 标识内容快照，三者用途不同。
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
4. 检查游戏 JVM native access 权限，以绝对路径调用 `SymbolLookup.libraryLookup`。
5. 先调用版本入口核对 ABI 6，再用带诊断缓冲区的 `rn_query_abi` 检查布局与构建身份。
6. 客户端初始化入口调用 `rn_init_env` 创建环境；CLIENT_STARTED 再通过 `rn_create_runtime` 创建 RHI 设备/Direct 队列。
   早期准备失败由 Fabric 初始化失败路径报告，设备创建失败清理并输出 MC 崩溃报告，仍不接管 MC 绘制。

所有内容路径落在 loader 自己的根目录内，当前仅接受固定 DLL 文件名。已有损坏缓存直接报错，不覆盖、不删除；暂未实现旧缓存自动清理。

开发也加载内容副本，而不是直接加载 linker 输出目录中的 DLL。Windows 占用已加载 DLL 时，这样仍能重编译新的 bridge；重启客户端后加载新 build。Java 热更新不能替代 native ABI 或 DLL 的重新加载。

### DLL 的依赖搜索

主 DLL 使用绝对路径，并不保证其间接依赖会自动从同目录找到。Windows 对依赖 DLL 的搜索和进程已加载模块仍有独立规则。[Windows DLL 搜索顺序](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)

当前静态链接 RadRay、CRT 与第三方实现，保留 Windows/驱动系统依赖。stage 通过 `dumpbin /dependents` 拒绝动态 CRT 和未分类的私有 DLL。当前没有私有 DLL 依赖预加载列表或 `LoadLibraryExW` bootstrap。

不通过修改进程全局 PATH 或 SetDllDirectory 来影响其他 Minecraft 模组。当前没有为不同模组的同名 DLL 提供版本隔离机制。

驱动和系统图形 DLL 不从开发机复制进 mod；shader 离线编译后嵌入 DLL，运行时不加载或打包 `dxcompiler.dll`。

## 讨论记录

通用资源句柄、批量数据、FramePacket 和读回接口的讨论保存在[思考历史](../history/2026-10-26-rendering-design-notes.md#当时的跨语言草案)，不属于当前 ABI。
