> - 适用: 修改 C ABI、FFM、native payload 和加载寿命
> - 权威: 本文维护跨语言、内存所有权与加载契约；操作步骤见构建指南
> - 锚点: `native/include/raynium/bridge.h`, `native/src/bridge.cpp`, `src/client/java/dev/raynium/bridge/*.java`, `src/client/java/dev/raynium/client/RayniumClient.java`, `tools/native.py`

# 互操作与加载

Java 25 通过普通 FFM downcall 加载 `rayniumruntime.dll`，当前 ABI 为 17，仅提供窗口交换链输出。
设备、帧槽与完成序号见 [Native runtime](native-runtime.md)，操作入口见[构建与测试](../guide/build-test.md)。

## 当前 C ABI

| 导出 | 契约 |
| --- | --- |
| `rn_get_abi_version` / `rn_query_abi` | 版本 17；240 字节身份结构，查询失败不改 info |
| `rn_init_env` / `rn_destroy_env` | DLL 单例环境；重复初始化、有活跃 runtime 时拒绝；环境不创建设备 |
| `rn_create_runtime` / `rn_destroy_runtime` | 唯一设备会话；销毁前先关闭 renderer；不接收宿主 GPU 身份 |
| `rn_create_renderer` / `rn_destroy_renderer` | 唯一 renderer，持有驻留地形、上传纹理与帧调度；有交换链时拒绝关闭 |
| `rn_query_device_info` | 当前 RHI 设备详情及能力；无需外部共享扩展，失败不改输出 |
| `rn_create_swapchain` / `rn_resize_swapchain` / `rn_destroy_swapchain` | 每 renderer 一个 BGRA8 HWND 交换链；重建与销毁先回收槽再等待呈现队列 |
| `rn_present_frame` | 一次同步传递网格、透明索引、移除、可见项、实例、相机及 GUI；OK 表示增量已消费 |
| `rn_create_texture` / `rn_upload_texture` / `rn_destroy_texture` | 编号 >=16 的 RGBA8/R8 上传纹理；同步复制源字节，帧内提交 GPU 拷贝 |

状态仅为 `RN_STATUS_OK=0` / `RN_STATUS_FAILED=1`。C ABI 不接收错误缓冲区，native 在发现失败的位置记录日志；Java 只报告操作名与状态码。
`rn_query_abi(info, capacity)` 和 `rn_query_device_info(runtime, info, capacity)` 中的容量仅指信息结构的字节容量。
创建失败清空有效输出句柄槽；信息查询和 present 结果失败时不改输出。`rn_destroy_runtime` 为 void，NULL 无操作；错误 owner、错误线程或仍有 renderer 时记录 fatal 日志并 abort。
所有生命周期和渲染调用在环境初始化线程串行执行；父 owner 拒绝在子 owner 存活时关闭。
准备失败可能已应用部分增量，不回滚；调用方关闭 renderer。进入 RadRay 帧回调后的不可恢复失败不转换为可重试错误。异常保护和终止边界见[Native runtime](native-runtime.md#失败处理)。
隐藏、最小化、零客户区、获取超时仍执行上传；OK 的 PRESENTED/SKIPPED/RECREATE 都可提交 Java 驻留事务。

## ABI 17 布局

指针按 8 字节对齐，固定宽度枚举按 uint32 传递；无旧 ABI 兼容层。Java 与 native 均显式核对版本及布局。

| 结构 | 字节数及关键布局 |
| --- | --- |
| `rn_abi_info` | 240；身份 flags 的 bit 1/2 为静态 CRT/mimalloc |
| `rn_env_desc` | 12；size/backend/flags，backend=1 D3D12、2 Vulkan；flags=1 driver validation |
| `rn_runtime_desc` | 8；size/reserved，reserved 必须为 0 |
| `rn_renderer_desc` | 24；size/flags、偏移 8 的 shader pack 指针、偏移 16 的 uint64 长度；flags=0 auto、1 direct、2 indirect |
| `rn_swapchain_desc` | 24；size/width/height/present_mode，偏移 16 的 HWND；mode=0 Immediate、1 FIFO |
| `rn_present_result` | 16；size/outcome、偏移 8 的累计 acquire_timeouts；outcome=0 Presented、1 Skipped、2 Recreate |
| `rn_device_info` | 488；名称/厂商/驱动字符串偏移 40/296/360；驱动版本格式由后端决定 |
| `rn_uploaded_texture_desc` | 24；size/id/width/height/mip_levels/format，format=0 RGBA8、1 R8 |
| `rn_texture_upload_desc` | 48；像素指针偏移 32、uint64 byte_size 偏移 40；row_pitch=0 为紧密行 |
| `rn_mesh_update` | 24；ID/vertices 指针、偏移 16 的 vertex_count、偏移 20 的 layer |
| `rn_index_update` | 24；透明 mesh ID/uint32 索引指针/count/reserved |
| `rn_instance` | 16；int3 origin/float visibility |
| `rn_visible_item` | 24；mesh ID、element_mask/sort_key/first_instance/instance_count，偏移 0/8/12/16/20 |
| `rn_gui_vertex` | 24；float3 position/float2 UV/RGBA8 color |
| `rn_gui_draw` | 40；pipeline/texture/sampler_bits/first_vertex/vertex_count、偏移 20 的 int4 scissor、偏移 36 的 reserved |
| `rn_render_frame_desc` | 328；见下表 |

| frame 字段组 | 字节偏移 |
| --- | --- |
| struct_size / flags / epoch / output_sequence | 0 / 4 / 8 / 16 |
| indices / meshes / removed_meshes / visible / instances 指针 | 24 / 32 / 40 / 48 / 56 |
| 上述五个数组的 count | 64 / 68 / 72 / 76 / 80 |
| terrain_albedo_texture / terrain_light_texture / terrain_anisotropy | 84 / 88 / 92 |
| projection[16] / view[16] | 96 / 160 |
| camera_block[3] / camera_fraction[3] | 224 / 236 |
| fog_color[4] / fog_distances[4] | 248 / 264 |
| gui_vertices / gui_draws 指针 | 280 / 288 |
| gui_vertex_count / gui_draw_count | 296 / 300 |
| gui_width / gui_height / clear_color[4] | 304 / 308 / 312 |

IDs 非零且属于 epoch；epoch 非零且不可倒退。顶点固定 28 字节，完整 quad、最多 1,048,576 顶点。
layer=0/1/2 为 SOLID/CUTOUT/TRANSLUCENT。前两层使用 Native 共享 Quad 索引，透明索引按 mesh ID 更新并随 mesh 删除。
数组指针只借用到同步调用返回；字节已复制到 staging，native 保存的帧标量清除全部借用数组指针。
每帧 mesh/index 更新最多 65,536 项，可见项及实例最多 1,048,576 项；透明索引最多 16,777,216 个 uint32。
Java 检查透明索引值；native 检查描述、层、数量及范围，不逐值扫描索引。调用方保证同类 ID 不重复、更新/删除不相交。

可见项直接引用 mesh；instance_count 非零且范围完整。element_mask=UINT32_MAX 请求全部紧凑面组，显式非零掩码不得含越界位。
掩码与 Native 保守朝向剔除求交，多实例使用各实例可见组的并集；计算结果为零正常跳过。
全局地形纹理只在有可见地形时解析；新帧引用已销毁编号失败，在途资源仍由 owner 保活。

frame flags 的 1/2/4/8 为 RGSS/profile/improved transparency/Y 反射。GUI 管线编号 0–11 及采样器位值保持现有 shader 契约。
GUI 顶点和绘制范围须为完整 quad，裁剪使用逻辑尺寸；clearColor 为 null 时沿用雾色 RGB、alpha=0。
纹理编号 0–15 保留，所需区域和 mip 必须由调用方初始化。

## Java 包装与所有权

`NativeRenderer` 通过 `NativeFrameBuffer` 持有 328 字节描述及可扩容的堆外数组/上传暂存，不分配诊断缓冲。
容量增长先创建新 confined arena，再释放旧 arena；空数组显式写 NULL/0。`presentPrepared` 使用直接写入的帧缓冲，`present` 负责列表编排。
Java 上传预算与 FFM 实际暂存容量相互独立。FFM 使用 ADDRESS 表示指针，JAVA_LONG 表示 ID/序号，JAVA_INT 表示 uint32/枚举。
设备查询、创建与帧调用缓存 MethodHandle；不使用 critical downcall、upcall、GC/Cleaner 或跨语言异步队列。
`MemorySegment` 边界检查不验证 C++ 裸指针真实容量；native owner 只能由同一 DLL 的 destroy 释放。

关闭顺序为 swapchain → renderer → runtime → environment → library；纹理编号和在途资源由 renderer 管理。
交换链借用 HWND，必须早于宿主窗口关闭。失败清理保留仍存活的 owner，不在 GPU 尚未完成时卸载 DLL。
游戏 JVM 使用 `--enable-native-access=ALL-UNNAMED`；Gradle daemon 的选项不能代替游戏 JVM 配置。

## 构建身份与 payload

`native/CMakeLists.txt` 用独立 binary 目录将锁定 RadRay 作为源码子工程引入，DLL 链接上游 `RadRay::runtime` 静态库，另静态链接 `RadRay::shader` decoder；依赖传递到 render/core，只导出 bridge 的十六个 C 入口。
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

stage 使用 `dumpbin /exports` 核对十六个 ABI 17 入口，再用 `/dependents` 拒绝动态 CRT 和未分类的私有 DLL，收集上游许可证，并先验证完整快照再发布 `current.json`。
schema 1 manifest 包含平台、ABI/布局、构建配置、CRT、工具链、build ID、RadRay commit/dirty、DLL/许可证大小与 SHA-256、系统依赖分类。
ABI 17 的身份 flags 中 bit 1（static CRT）和 bit 2（mimalloc）均为 1。loader 只接受 Debug/MTd 或 Release/MT 的单 DLL 清单，拒绝错配和 MD 的 schema 2 payload。
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
4. 客户端初始化入口检查游戏 JVM native access 权限，以绝对路径调用 `SymbolLookup.libraryLookup` 加载 bridge。
5. 先调用版本入口核对 ABI 17，再用 `rn_query_abi` 检查布局与构建身份。
6. Fabric 入口调用 `rn_init_env` 创建环境；首次设备信息查询通过 `rn_create_runtime` 创建 RHI 设备/Direct 队列，窗口 surface 创建时建立 renderer 与交换链。
   缺少 payload 或早期准备失败使 Fabric 初始化失败；帧准备失败关闭 renderer 并向上传播。

所有内容路径落在 loader 自己的根目录内，只接受固定 DLL 文件名并验证大小和哈希。已有损坏缓存直接报错，不覆盖、不删除；暂未实现旧缓存自动清理。

开发也加载内容副本，而不是直接加载 linker 输出目录中的 DLL。Windows 占用已加载 DLL 时，这样仍能重编译新的 bridge；重启客户端后加载新 build。Java 热更新不能替代 native ABI 或 DLL 的重新加载。

### DLL 的依赖搜索

主 DLL 使用绝对路径，并不保证其间接依赖会自动从同目录找到。Windows 对依赖 DLL 的搜索和进程已加载模块仍有独立规则。[Windows DLL 搜索顺序](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)

当前静态链接 RadRay、CRT 与 mimalloc，保留 Windows/驱动系统依赖；无需为 Raynium 额外安装动态 VC++ CRT。
没有 mimalloc/redirect DLL、Fabric preLaunch 分配器加载器或进程级 CRT 重定向。静态分配器随 bridge 加载与卸载；所有 native owner 必须先关闭。

不通过修改进程全局 PATH 或 SetDllDirectory 来影响其他 Minecraft 模组。当前没有为不同模组的同名 DLL 提供版本隔离机制。

驱动和系统图形 DLL 不从开发机复制进 mod；shader 离线编译为内置资源 `.rnsp`，运行时不加载或打包 `dxcompiler.dll`。资产格式与寿命见[Shader pack](shader-pack.md)。
