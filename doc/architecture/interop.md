> - 适用: 修改 C ABI、FFM、native payload 和加载寿命
> - 权威: 本文维护跨语言、内存所有权与加载契约；操作步骤见构建指南
> - 锚点: `native/include/raynium/bridge.h`, `native/src/bridge.cpp`, `src/client/java/dev/raynium/bridge/*.java`, `src/client/java/dev/raynium/client/RayniumClient.java`, `tools/native.py`

# 互操作与加载

Java 25 通过普通 FFM downcall 加载 `rayniumruntime.dll`，当前 ABI 为 8。
旧 preview/terrain 专用导出和 Java owner 已删除，无兼容层。UV 与 SOLID 都提交通用场景帧。
设备、场景及完成序号见 [Native runtime](native-runtime.md)，操作与验证见[构建与测试](../guide/build-test.md)。

## 当前 C ABI

| 导出 | 契约 |
| --- | --- |
| `rn_get_abi_version` | 返回 8 |
| `rn_query_abi` | 240 字节身份与布局；失败不改 info |
| `rn_init_env` / `rn_destroy_env` | DLL 单例环境；重复初始化或有活跃 runtime 时销毁失败并保留所有者 |
| `rn_create_runtime` / `rn_destroy_runtime` | 唯一设备会话；销毁前必须先关闭 renderer |
| `rn_create_renderer` / `rn_destroy_renderer` | 唯一 renderer；持有场景、帧上下文与完成 fence；有共享子对象时拒绝关闭 |
| `rn_create_render_target` / `rn_destroy_render_target` | 每 renderer 一个输出；返回句柄及 128 字节图像信息；创建不提交 GPU 工作 |
| `rn_create_shared_textures` / `rn_destroy_shared_textures` | 按描述创建逻辑输入纹理与物理导出；创建提交初始 release，序号为 1 |
| `rn_render_frame` | 一次提交资源/场景增量、删除项、可见项、实例、视图与输入输出序号；成功提交才发布增量 |

三类子 owner 的 destroy 返回状态，NULL 成功；超时保留 owner。runtime destroy 保持 void 契约，NULL 无操作。
所有状态入口仅返回 `RN_STATUS_OK=0` / `RN_STATUS_FAILED=1`，具体原因写入调用方 UTF-8 字符串缓冲区。
诊断容量大于零时截断并 NUL 终止；NULL 指针要求零容量。参数缓冲区不得重叠，调用方负责真实容量。
创建失败清空有效输出句柄槽；信息输出在创建成功前不改写。

`bridge.h` 使用 C++ 固定宽度非作用域枚举与 `extern "C"` / `noexcept` 导出，不提供纯 C 编译分支。
新渲染入口通过统一异常转换层报告 FAILED；底层 RHI 的不可恢复终止不等于可恢复错误。
没有 upcall、全局 last-error、跨语言共享队列或 GC/Cleaner 回收。

## ABI 8 布局

所有指针仅在本次同步调用期间借用；保留的 submesh、索引、材质、primitive 被复制，顶点写入 native staging。
ID 非零，属于 frame epoch；同 ID 更新替换资源，epoch 非零且不能倒退。
材质 program 为 UV=1 / TEXTURED=2，layout 为 PROCEDURAL=0 / POSITION_COLOR_UV_LIGHT=1。
材质中的纹理编号指向当前共享集合的逻辑纹理；共享 quad 索引也通过普通索引 ID 上传。

| 结构 | 字节数及关键布局 |
| --- | --- |
| `rn_env_desc` | 12，size/backend/flags；backend=1 D3D12 或 2 Vulkan，flags=1 driver validation |
| `rn_runtime_desc` | 20，size/flags/node_mask/luid[8]，LUID 偏移 12；flags=1 匹配宿主 GPU |
| `rn_renderer_desc` | 8，size/flags；flags 当前必须为 0 |
| `rn_render_target_desc` | 16，size/width/height/reserved |
| `rn_texture_desc` | 16，width/height/mip_levels/split_mips |
| `rn_shared_textures_desc` | 16，size/count/textures 指针；最多 16 个逻辑纹理 |
| `rn_shared_image_export` | 144，前 128 字节图像信息，随后 texture_index/mip_level/mip_count/reserved；capacity 按记录数 |
| `rn_index_update` | 24，ID/indices 指针/count/reserved |
| `rn_submesh` | 16，first/count/base_vertex/reserved |
| `rn_mesh_update` | 48，ID/vertices 指针/index ID/submeshes 指针，随后 vertex_count/stride/layout/submesh_count |
| `rn_material_update` | 24，ID/program/albedo_texture/light_texture/anisotropy |
| `rn_primitive_update` | 24，ID/mesh ID/material ID |
| `rn_instance` | 16，int3 origin/float visibility |
| `rn_visible_item` | 24，primitive ID/submesh/depth_key/first_instance/instance_count |
| `rn_render_frame_desc` | 344，布局见下表 |

| frame 字段组 | 字节偏移 |
| --- | --- |
| struct_size / flags / epoch / output_sequence / input_sequence | 0 / 4 / 8 / 16 / 24 |
| indices / meshes / materials / primitives 指针 | 32 / 40 / 48 / 56 |
| removed_meshes / removed_primitives / removed_materials / removed_indices 指针 | 64 / 72 / 80 / 88 |
| visible / instances 指针 | 96 / 104 |
| 上述十个数组的 count | 112..148，每项 4 字节 |
| changed_textures / reserved | 152 / 156 |
| projection[16] / view[16] | 160 / 224 |
| camera_block[3] / camera_fraction[3] | 288 / 300 |
| fog_color[4] / fog_distances[4] | 312 / 328 |

frame flags 位值 1/4/8 为 direct/RGSS/profile；changed_textures 的第 i 位表示第 i 个逻辑纹理已更新。
首次输入使用须初始化全部纹理。每帧资源更新各最多 65,536 项，可见项与实例各最多 1,048,576 项。
mesh 最多 1,048,576 顶点，submesh 最多 65,536 项；索引资源最多 16,777,216 个 uint32。
范围必须在几何内且 count 是 3 的倍数；当前 textured 布局 stride=28 并使用索引，procedural 无顶点/索引 buffer。
删除不能留下悬空 primitive 或 mesh/index 引用；帧中同类更新或删除 ID 不能重复。

| `rn_shared_image_info` 字段组（共 128 字节） | 字节偏移 |
| --- | --- |
| struct_size、handle_type、width、height | 0、4、8、12 |
| memory_handle、ready_handle、consumed_handle、allocation_size | 16、24、32、40 |
| memory_type_index、node_mask、luid[8] | 48、52、56 |
| device_uuid[16]、driver_uuid[16] | 64、80 |
| native_format、native_usage、native_flags、native_tiling | 96、100、104、108 |
| allocation_offset、dedicated、reserved | 112、120、124 |

handle_type=1 表示 D3D12Resource/D3D12Fence，2 表示 OpaqueWin32。输入图像的 ready/consumed 分别表示 native 释放与 MC 写入完成。
输出信息 capacity 按字节；输入导出 capacity 按 144 字节记录数量。native 静态断言校验尺寸和关键偏移，Java `SharedImageInfo.read` 显式读取。
查询结构仍为 240 字节；Java、构建 manifest、stage 和分发校验都只接受 ABI 8，旧 payload 须重建。

## Java 包装与所有权

调用链为 `MC adapter → NativeRenderer → NativeBindings/FFM → C ABI → MC native 适配 → RadRay runtime 静态库 → RadRay RHI`。
`NativeRenderer.Frame` 携带通用增量与视图数据，序列化进 confined arena；Java 检查数组长度，native 检查资源引用及绘制范围。
`NativeBindings` 加载符号并缓存 MethodHandle，查询/环境/runtime 使用 invokeExact，新渲染入口通过统一 invokeWithArguments 状态处理；均无 critical 选项。
FFM 使用 ADDRESS 表示不透明指针、JAVA_LONG 表示 ID/句柄/序号、JAVA_INT 表示 uint32/枚举，数组按固定偏移编排。

| 所有者 | 直接持有 | 关闭顺序/条件 |
| --- | --- | --- |
| `NativeEnvironment` | `NativeLibrary`、唯一 runtime | runtime 成功关闭后销毁环境，再关闭 library arena |
| `NativeRuntime` | 唯一 renderer | renderer 成功关闭后销毁设备会话 |
| `NativeRenderer` | 场景、唯一 target 与 shared textures 的 Java owner | 子 owner 关闭后等待自身提交完成 |
| `NativeRenderTarget` | 输出共享颜色、导出与输出 timeline（深度归 RDG） | 宿主完成消费并关闭导入后销毁 |
| `NativeSharedTextures` | 输入纹理、物理导出映射和输入 timeline | 宿主完成写入并关闭导入后销毁 |
| `VulkanPreviewImage` | VkImage/VkDeviceMemory/semaphore | 显式排空后关闭；不拥有导出 Win32 句柄 |
| 调用 scratch arena | 描述、数组、返回槽、诊断缓冲区 | 同步调用结束关闭，不释放返回槽中指向的 native owner |

所有生命周期和渲染调用都在环境初始化线程串行执行，Java 与 native 均检查 owner/thread。
父 owner 拒绝在子 owner 存活时关闭；子对象解析失败尝试清理，清理失败保留在父对象中，禁止继续卸载 DLL。
资源等待失败保留句柄和所有者，不把超时当完成。library 的 shared arena 不使 RHI 自动线程安全。

`MemorySegment` 的边界检查不能验证 C++ 裸指针容量。所有 native 对象只能由同一 DLL 的 destroy 释放。
JDK linker 按 FunctionDescriptor 建立调用，不推断 C++ 类型；MemoryLayout 不自动插入 C padding。
参见 [JDK 25 Linker](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.html) 和
[MemoryLayout](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/MemoryLayout.html)。
Fabric 使用 unnamed module，游戏 JVM 显式设置 `--enable-native-access=ALL-UNNAMED`；只设置 Gradle daemon 不生效。

## 构建身份与 payload

`native/CMakeLists.txt` 用独立 binary 目录将锁定 RadRay 作为源码子工程引入，DLL 链接上游 `RadRay::runtime` 静态库，由该库传递依赖 render/core，只导出 bridge 的十三个 C 入口。
当前只生成一个 Windows x86_64 DLL，Debug/Release 均使用 `/MT` 和静态 mimalloc，Debug 保留调试宏而不启用 `/MTd`；bridge 不再包含重复的 new/delete 替换头。
配置阶段不恢复依赖，发布要求锁定提交的干净 Release 源码。当前未实现 RadRay 安装包消费模式。

stage 使用 `dumpbin /exports` 核对十三个 ABI 8 入口，再用 `/dependents` 检查直接依赖，收集上游许可证，并先验证完整快照再发布 `current.json`。
manifest 包含平台、ABI/布局、构建配置、CRT、工具链、build ID、RadRay commit/dirty、DLL/许可证大小与 SHA-256、系统依赖分类。
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
4. 检查游戏 JVM native access 权限，以绝对路径调用 `SymbolLookup.libraryLookup`。
5. 先调用版本入口核对 ABI 8，再用带诊断缓冲区的 `rn_query_abi` 检查布局与构建身份。
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

早期接口草案保存在[思考历史](../history/2026-10-26-rendering-design-notes.md#当时的跨语言草案)；该记录不是 ABI 8 契约，当前接口以本文与 bridge.h 为准。
