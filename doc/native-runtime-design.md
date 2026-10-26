# Native runtime 功能设计

native runtime 接收通用渲染资源和完整帧输入，调用 native 渲染管线，通过 RadRay RHI 执行并输出画面。它负责让数据在正确的时间可用、让 pass 在正确的状态下执行，并让资源在最后一次使用结束后释放。

状态：`RayniumRuntime` 的设备会话已实现；资源服务、管线、完整帧与输出仍是详细实施提案。Windows + RadRay RHI 和 Java/native 职责边界已确认。基线为 2026-10-26 的工作区。MC 接入及跨语言封装见 [实现设计](implementation-design.md)。

## 当前实现：设备会话

`native/include/raynium/runtime.h` 与 `native/src/runtime.cpp` 定义不可复制、不可移动的 `raynium::RayniumRuntime`，编译进 `rayniumruntime.dll`，通过 RadRay RHI 创建一个设备及一条 Direct 队列。`static bool Create(backend, output, error)` 是唯一创建入口：先在局部 RAII 对象中准备 Factory/环境、Device 和 Queue，全部成功后才调用私有构造函数。没有默认构造或 Initialize；成功返回 true 并交付有效对象，失败返回 false 和诊断字符串，不产生半初始化 runtime。output 必须为空，失败不改动它。本阶段没有 swapchain、资源表、渲染管线或帧提交，MC 继续原版绘制。

Java `NativeRuntime` 拥有不透明的 `rn_runtime*` 和 DLL。`RayniumClient` 在 Fabric `CLIENT_STARTED` 创建它，在 `CLIENT_STOPPING` 关闭它。两处均在 MC 渲染线程；创建不依赖世界加载，资源重载、进出世界不重建 runtime。`CLIENT_STARTED` 位于 `Minecraft.run()` 的异常捕获之前，所以 Java 初始化失败时显式构造 CrashReport 并调用 `emergencySaveAndCrash`，不依赖主循环捕获。

默认 D3D12，`raynium.rhi.backend=d3d12|vulkan` 显式选择；所选后端失败时清理创建过程的局部资源并终止启动，不自动切换后端。普通 Java 开发运行未选择 native payload 时保持原有可选模式。初始化/销毁 C ABI 与参数规则以 [Native 工程操作](native-development.md#c-abi-与内存边界) 和 `bridge.h` 为准。

| 后端 | 创建顺序 | 清理顺序 |
| --- | --- | --- |
| D3D12 | DXGI Factory → 高性能适配器 → Device → Direct Queue | 队列 Wait → Device → Factory |
| Vulkan | InitEnv → 高性能物理设备 → Device → Direct Queue | 队列 Wait → Device → ShutdownEnv |

队列是设备所有的借用对象，不单独 delete。Create 的局部 guard 只在本次取得 Vulkan 环境所有权后负责 ShutdownEnv；成功构造后将该责任交给 runtime；RadRay 目前只允许一个活跃 Vulkan 环境。宿主契约是每客户端一个活跃 runtime，释放后可以重新创建。新代码不增加全局 registry，也不支持跨线程并发访问 RHI。

Debug 启用 RadRay 默认 RHI 校验，Release 关闭；驱动调试层、同步验证层和 GPU validation 均未开启。日志包含创建阶段、后端、GPU 名称和队列等待，底层具体失败原因仍由 RadRay 输出。不通过 Java upcall 收集日志，不新增 C++ try/catch；RHI 返回的可恢复失败转换为状态码，底层异常或不可恢复驱动错误不承诺生成 Java 崩溃报告。

正常退出严格先销毁 runtime，再关闭 library arena。当前没有 native 工作线程或回调。强杀进程、JVM/native 崩溃及其他模组提前终止生命周期事件不保证执行关闭回调；不通过 shutdown hook 在未知线程操作 RHI。

## 后续功能提案

下文描述未来资源与管线能力；除上述设备会话外，不能据此认为这些功能已经实现。

## 职责与边界

这里的 native 渲染系统由 runtime 基础服务和具体渲染管线组成。二者都在 native，但功能归属不同。

| 功能 | 宿主 Java | 通用 runtime | 具体 native 管线 |
| --- | --- | --- | --- |
| 世界、实体、区块和资源包 | 管理与解析 | 不感知这些概念 | 不直接读取 |
| 模型求值、动画、几何生成 | 生成通用结果 | 接收 mesh 和动态数据 | 使用数据绘制 |
| 应用侧裁剪与 LOD | 选择候选对象 | 校验并保留本帧输入 | 进行特定 pass 所需的筛选 |
| 纹理、网格、材质数据 | 创建/更新/释放请求 | 管理身份、GPU 对象和寿命 | 解释材质模型 |
| 光照、阴影、透明、后处理 | 提供输入和参数 | 提供执行设施 | 决定算法、pass 和 shader |
| GUI | 布局、排版和模型求值 | 接收有序几何及目标引用 | 绘制、模糊和合成 |
| 帧节奏、同步、资源状态 | 提交完整帧 | acquire、上传、barrier、submit、present | 生成本帧执行计划 |

runtime 不建立 MC 场景图或 ECS，也不解析模型文件、图片文件、资源包和 shader 源码。它可以承载一般应用的场景输入，但第一版不提供持久实例数据库；宿主每帧给出完整候选列表。

FFM、JNI、C ABI 的字节解码属于 bridge。runtime 的主要接口是普通 C++ 数据结构和 span，独立 C++ 宿主无需构造跨语言 FramePacket 就能使用它。

## 最小公共对象模型

建议公开少量业务对象，把上传池和 command allocator 等机制封装在内部。

| 对象 | 定义和寿命 |
| --- | --- |
| `RayniumRuntime` | 现已拥有设备会话；后续扩展资源与执行环境，独占设备的 CPU 调用调度 |
| `OutputHandle` | swapchain 输出或离屏输出；销毁不等于销毁宿主窗口 |
| `PipelineHandle` | 一个 native 管线实例，拥有其材质 schema、布局、PSO 和历史状态 |
| `MeshHandle` | 持久网格的公开身份，独立于绘制实例 |
| `TextureHandle` | 持久纹理的公开身份；尺寸、格式和 mip 数创建后固定 |
| `MaterialHandle` | 某个管线 schema 的不可变材质数据及资源依赖 |
| `FrameInput` | 调用期间借用的通用帧数据 |
| `FrameResult` | CPU 接受情况、GPU 提交序号、输出状态及诊断 |
| `ReadbackTicket` | 异步读回请求及其 CPU 可读取状态，后续阶段加入 |

首版一个 runtime 使用一条 direct/graphics queue，允许一个主要窗口输出和管线内部离屏目标。多窗口独立 present、多个执行线程和多队列并行均后置，不在类型名中写死 MC 主窗口。

设备创建已集中在 `RayniumRuntime::Create` 中，通过 RadRay 现有工厂选择适配器并创建设备；后续资源与管线代码面向 RHI，不散布后端分支，也不允许宿主绕过调度器操作同一设备。

```text
RayniumRuntime
  DeviceSession
  ResourceStore
  UploadService
  FrameScheduler
  ResourceStateTracker
  TransientResourcePool
  OutputStore
  PipelineStore
  Diagnostics

PipelineInstance
  MaterialSchema
  Shader/Layout/PSO owners
  Per-material prepared bindings
  History resources
  BuildFrame / RecordPass implementation
```

## 资源服务

### 资源身份与依赖

runtime 必须区分公开句柄和实际对象。建议句柄由槽位与 generation 构成，并按资源类型和 runtime 归属使用。释放公开句柄后立即拒绝新的外部引用；仍有内部依赖的实际对象继续存活。

资源记录至少包含描述、生命周期状态、GPU 对象、内部依赖和最后一次 GPU 提交序号。公开槽位复用必须改变 generation，不能让旧句柄命中新资源。

例如材质创建时将 TextureHandle 解析为内部纹理引用。之后宿主释放该纹理的公开句柄，已有材质仍能合法使用它；当材质、帧和上传任务都不再引用该纹理时，才进入最终销毁。这样引用的有效性不会取决于宿主释放句柄的偶然顺序。

首版资源依赖采用受控的单向关系：material → texture，mesh → 内部 buffer，view → texture，PSO/parameter set → layout 和绑定对象。拒绝任意资源依赖环。内部对象引用由 runtime 或管线实例持有，不新增进程级全局注册表。

### Mesh 功能

`MeshDesc` 包含 vertex streams、布局、拓扑、索引格式、submesh 范围和可选包围盒。宿主传入已求值的几何，runtime 负责：

- 检查 stride、offset、索引范围、字节数和设备限制。
- 创建 GPU buffer，安排上传，记录可被绘制使用的顺序。
- 暴露 submesh 的绘制范围，供管线绑定和组织 draw。
- 管理替换与释放，保留在途帧正在使用的旧版本。

第一版持久 mesh 采用创建后不可变的方式，变化时新建 mesh 并交换宿主句柄。动态几何走帧内顶点/索引区，不为每帧的小粒子和 GUI 元素反复创建持久对象。

初期可每个 mesh 使用独立 buffer；需要减少大量小 buffer 的开销时，再增加按用途分组的 GPU buffer page 和切片分配。该优化不改变 MeshHandle 或 DrawItem 契约，也不要求 runtime 自行实现一套显存 heap 后端。

### Texture 功能

必须支持创建、完整上传、区域更新、多 mip/array layer 描述和显式释放。v1 优先实现普通 2D 采样纹理；深度和渲染目标由管线目标服务创建，复杂格式按需求扩展。

纹理区域更新必须描述源 row pitch、mip、layer、矩形与数据范围。runtime 把源数据整理成 RHI 要求的上传布局，处理 placement/pitch 对齐和非 coherent 映射内存的 flush。压缩纹理若加入支持，还需要块尺寸与块对齐校验，不能照搬普通像素路径。

尺寸或格式变化创建新纹理，不偷偷改变已有句柄的资源描述。普通像素区域更新允许复用同一 GPU 对象，按单队列顺序保证旧读取结束后再写入，新绘制随后读取新内容。

首版要求宿主提交所需 mip 数据。自动 mip 生成属于可选的 native utility pass，不能在缺失 mip 时假设 RHI 会自动补齐。

颜色空间是纹理及管线输入的明确属性。runtime 保留和校验这份信息；色调映射、曝光和最终颜色转换由管线负责。

### Material 功能

runtime 保存 schema 标识、版本、参数数据，以及显式列出的纹理/采样依赖。具体管线负责验证参数意义并生成 shader 变体和绑定。

建议形状为：

```text
MaterialDesc
  pipelineSchemaId, schemaVersion
  typed parameter bytes
  textureBindings[]: semantic slot, TextureHandle, SamplerDescriptor
```

纹理引用必须是结构化字段，不能藏在任意参数 blob 中，否则 runtime 无法跟踪寿命。参数块只携带该 schema 定义的值，不包含 RHI 指针或宿主对象地址。

第一版材质不可变，修改时创建新材质。旧材质由已有帧继续持有；新帧使用新句柄。后续若增加 UpdateMaterial，应产生新内部版本，不能原地覆盖在途 descriptor。

不做一个支持任意 shader 自动反射、任意参数自动绑定的材质编辑系统。管线明确声明 schema、binding layout 和允许的变体；通用性来自可安装多种管线，而不是让一个参数字典承担全部语义。

### 上传与背压

`ApplyResources` 返回前已经复制或消费宿主 CPU 字节。成功表示资源请求被接收，不表示上传已由 GPU 完成。内部状态可采用 `PendingUpload → UsableInQueueOrder → Resident`；逻辑释放单独记录，不与 GPU 就绪混为一谈。

待上传队列拥有 CPU 数据和资源引用。绘制引用待上传资源时，runtime 必须在它第一次使用之前提交所需上传；尚未准备好时明确返回 Deferred，不能让 shader 读取未初始化图像。

设置待上传字节和 staging 内存预算。容量不足返回背压结果，由宿主保留旧 mesh、减少任务生产或稍后重试。v1 对一帧必需的新资源采取“全部排入该帧之前，或整帧暂缓”的规则，不静默显示一半新场景。后台非必需上传可分批推进。

增加 `PumpResources`，让没有可呈现窗口时仍能按预算推进已接受的资源请求。它只执行资源工作，不持有 swapchain token，也不要求构造一份假 FrameInput。

## 完整帧输入与执行入口

### FrameInput 的内容

runtime 接收只读的连续数据视图，不保留宿主对象指针：

| 内容 | 作用 |
| --- | --- |
| frame ID、pipeline、output | 标识本次请求和执行目标 |
| Views | 当前/上一帧矩阵、viewport、投影和历史失效标志 |
| DrawItems | mesh/submesh/material、当前/上一帧变换、稳定 ID 与候选标志 |
| TransientGeometry | 仅本帧有效的顶点和索引 |
| Lights / Environment | 管线声明的通用光源和环境参数 |
| OrderedDraws | GUI、叠加图形等顺序敏感批次及 scissor |
| PipelineInputs | 经该管线 schema 验证的附加输入，例如 preview 或特定效果参数 |

runtime 校验索引和句柄后，生成内部 `ResolvedFrame`：资源引用已经解析并保活，瞬态数据已复制到自有内存，管线不能再通过回调读取 Java。主视图和阴影视图的候选输入由宿主提供，runtime 不重新遍历世界。

### 单次 RenderFrame 负责完整结束一帧

建议公开入口为 `RenderFrame(output, pipeline, input)`。acquire、command recording、submit 和 present 属于 runtime 内部，不向宿主暴露需要配对调用的 swapchain token。这里收紧了先前实现提案中的分离式 begin/render/present 接口。

调用返回前必须处理完已取得的输出 token；GPU 工作可以仍在运行，但宿主 CPU 内存不再被借用。

```text
RenderFrame
  1. 校验输入和句柄，解析并固定本帧资源引用
  2. 检查输出、必需上传、管线状态和预算
  3. 等待或检查可复用帧槽，结束其旧 command epoch
  4. 准备 PSO、参数集、临时目标和 fallback 资源
  5. acquire 输出；拿不到则释放本次临时引用并返回
  6. 由管线生成具体 pass，完成 GPU 命令录制
  7. 单队列提交：上传、绘制、输出转换、完成 fence
  8. present 并消费 token
  9. 返回 submission ID 与输出结果，GPU 异步继续
```

将多数可能失败的校验和资源准备放在 acquire 之前。实际 backbuffer 及其 view 在 acquire 后绑定到已准备的输出计划，避免依赖一张尚未取得的 swapchain image。

`FrameResult` 必须分开报告 CPU 接受、GPU 是否提交和输出状态。Present 返回 RetryLater 或要求重建时，之前的绘制可能已经提交，不能按“整帧未发生”释放资源或回滚历史。

### 帧槽与等待

首版两个在途帧槽，每个槽包含 command allocator/buffer、上传页、动态顶点区、参数集租借、瞬态目标引用、查询区和完成序号。帧槽索引与 swapchain image 索引没有一一对应关系。

正常帧路径支持有界等待：利用 RHI `Fence::WaitFor` 或完成值查询判断可复用性，超时返回 Busy/Deferred。超时不是完成证明，不能继续 Reset。宿主据返回值维持窗口和游戏响应，不必把无限等待写进跨语言调用。

同一帧槽复用顺序为：GPU 完成 → Reset/释放旧录制使 command epoch 结束 → 归还参数集和临时资源 → 开始新录制。仅等待 fence 后就修改参数集还不够，旧录制引用也必须结束。

## Pass 执行与资源状态

### 管线生成执行计划

runtime 需要一个轻量 `FramePlan`，首版按管线给定顺序执行，不实现自动调度到多队列的完整 render graph。

每个 pass 声明名称、graphics/compute/copy 类型、资源读取与写入、subresource range、shader stages、附件及 native 录制回调。录制回调只在当前 render thread 内执行，其数据由本帧持有。

```text
FramePlan
  Pass Upload          : write mesh/texture destinations
  Pass Opaque          : read meshes/materials, write HDR and depth
  Pass ToneMap         : read HDR, write output color
  Pass Overlay         : read GUI textures, read/write output attachment
  Pass Present         : transition output
```

这个列表只是管线示例，runtime 本身不要求所有管线都有这些 pass。

管线通过 `PassContext` 使用 RHI graphics/compute encoder，runtime 控制 pass begin/end、barrier 和提交。管线不自行调用 queue Submit/Wait/Present，也不绕过状态系统访问未声明资源。

### 状态跟踪

RHI 当前不会自动追踪资源状态或补 barrier。因此 runtime 的 `ResourceStateTracker` 是必需功能，负责：

- 记录每个 buffer/texture 的已排队状态及首次使用条件。
- 按 pass 声明生成 before/after barrier，处理 copy、shader read、attachment、depth、UAV 和 present。
- 跟踪 texture 的 mip/layer/aspect；buffer 首版可保守按整资源状态管理。
- 对无状态变化但存在 UAV 写依赖的情况发出必要内存 barrier。
- 检查读取未初始化内容、非法同 pass 读写和缺失用途声明。

资源状态表示 GPU 队列按序执行后将达到的状态，不等于 CPU 此刻观察到的 GPU 进度。命令录制先更新本次候选状态表，实际提交后才合并到已排队状态；未提交录制被取消时不能留下假的状态变化。

GPU 完成状态由 submission fence 单独管理。不能用“已标为 ShaderRead”推断上传已经完成，也不能在重新借出池内纹理时直接把其状态重置成 Undefined。

新建资源及首次取得的输出图像按 RHI 初始状态契约初始化，复用对象沿用实际状态。初始状态的公共约定若不够明确，应补充 RHI 文档或薄接口，不能在各管线内散布后端猜测。

首版禁止在同一个 pass 中对同一子资源进行未明确支持的采样与写入反馈。只读深度与特殊混合权限依 RHI 能力约束校验；不能因为名称同为 DepthRead 就承诺所有 shader 访问方式都合法。

### 瞬态目标池

管线按描述请求临时 color/depth/storage 图像，runtime 负责创建、复用、预算和延迟退役。池 key 包括尺寸、格式、samples、mip/layer、usage 和所需 view。

v1 只复用已结束全部引用的对象，不做不同逻辑图像共享同一显存区间的 aliasing。第一版把临时目标与帧槽关联，可先减少跨帧复用推理；后续再增加更细的存活区间分析。

空闲池使用容量预算和回收策略，GPU 在途对象不受 LRU 驱逐。resize 后旧尺寸对象立即失去新帧匹配资格，但实际销毁仍要等引用结束。

## 管线支持设施

### 管线生命周期

`IRenderPipeline` 通过工厂创建完备实例；实例提供能力与 schema 查询、输入验证、资源准备、构造 FramePlan、输出变化通知和销毁入口，不提供构造后的 Init/Initialize。第一版使用编译进程序的 native 实现，不引入动态 DLL 管线插件 ABI。

runtime 向管线提供受控服务：资源解析、临时目标、上传切片、参数集、历史资源和诊断。管线保持自己的材质模型、draw 排序与 shader 选择，runtime 不为某种 forward/deferred/光追算法添加固定字段。

第一套管线只需纹理网格、深度和基础 overlay。加入阴影、OIT、雾或时序算法时扩展管线及其 schema，不把算法实现搬进 ResourceStore。

### Shader、布局和 PSO

runtime 核心不依赖 DXC。管线加载离线 shader 产物，显式创建并持有 RHI `BindingGroupLayout`、`PipelineLayout`、shader 和 PSO。shader 产物解析可由依赖 `radrayshader` 的管线支持层完成，保持 `runtime → render/core` 的依赖方向。

当前 RHI 不替调用方共享布局，管线实例必须保存和复用同一布局对象。PSO 按 shader 变体、布局、顶点布局、附件格式、sample count 和固定状态复用；key 由完整描述确认，不能只比较 hash。

首次需要的 PSO 尽量在 acquire 前创建，避免在已持有输出帧时做大量昂贵准备。缓存属于管线实例生命周期，有预算且只淘汰无使用者的对象，不建立全局 PSO cache。

### 参数集和常量

`ShaderParameterSet` 第一次绑定后视为不可变。runtime 提供按 layout 分组的借用池，完整写入 → FlushWrites → 绑定 → 等待 GPU 与 command epoch 结束 → ResetBindingsForReuse → 再次借出。

每帧常量使用按 RHI alignment 切分的 buffer 区域；多帧在途不能覆盖同一切片。材质的稳定绑定可以跨帧复用，但其资源依赖和 layout 必须一直存活。sampler 优先复用已有 RHI device sampler cache，不在 runtime 再建一个意义相同的缓存层。

### 历史资源

时序历史作为管线实例下的资源组，以 pipeline instance、稳定 view ID、用途和输出代次标识。runtime 提供分配与寿命服务，管线决定历史内容及何时失效。

resize、projection/camera cut、scene discontinuity 和管线切换需要失效标志。v1 没有时序 pass 时无需建立历史池；接口只保留表达失效的空间。

历史是否推进取决于对应 GPU 写入是否已提交，不能只看 present 成功。宿主提供的 previous transform 也必须与最后一次被接受执行的帧对应，跳帧不能无条件推进“上一帧”。

## 输出、取消和关闭

`OutputStore` 管理宿主窗口引用、swapchain、尺寸和输出代次。窗口创建、消息循环、输入和 DPI/GUI scale 策略仍在宿主。runtime 接收最终像素尺寸和呈现意图，使用 RHI 输出。

resize 在没有对外悬空帧 token 的边界执行：终结当前帧 → 等待使用旧 backbuffer 的提交 → Reset/释放相关录制和 view → Recreate → 重新取得新代对象。Recreate 失败后不能继续使用旧 backbuffer 指针。

最小化或零尺寸时先返回 OutputSuspended，不 acquire。离屏渲染则以自己的尺寸和完成序号执行，不调用 present。

正常输入错误和预算不足尽量在 acquire 前返回。已 acquire 后，如果设备仍健康且主录制可安全放弃，丢弃未提交录制并用预备的最小 clear/present 路径终结；该路径也可能失败，不能宣称可恢复所有设备故障。

当前 `DiscardAcquiredFrame` 并非所有 RHI 后端都支持，因此不把它作为通用 AbortFrame 的基础。RHI 某些执行错误会直接终止进程，也没有统一的设备丢失恢复协议；runtime 保留已有错误契约，不承诺把所有失败转换为可恢复状态，更不通过捕获任意异常制造恢复假象。

正常关闭按依赖逆序进行：停止接受新工作，释放未提交 CPU 请求，排空已提交 GPU 工作，结束录制 epoch，释放帧/参数集/PSO/view，再释放资源、输出和设备会话。故障关闭遵循 RHI 可用能力，不在已知无法完成的 fence 上无限等待。第一版不做自动设备恢复。

## 预算、诊断和读回

runtime 至少需要以下生产级限制：持久资源数量与字节、待上传 CPU 字节、上传页峰值、瞬态目标池预算、每帧动态数据上限和可配置等待时间。达到限制时返回具体资源类别及请求量，不能无界扩容到进程失去响应。

RHI 当前提供的 budget/capability 信息不足以证明完整的实时显存驻留状态。runtime 首版做自身分配记账和预算控制，不承诺自动显存驻留管理或按重要性淘汰宿主仍在使用的纹理。

诊断分成输入错误、资源准备失败、管线错误、输出不可用和 RHI 原生诊断。消息带 frame/resource/pass 标识，native 回调写入由会话持有的线程安全有界接收器，由 Java 主动读取；驱动回调不能重入 RHI 或调用 Java。

运行指标包括上传字节、GPU 对象数量、池占用、队列积压、帧槽等待、CPU 准备/录制耗时和可用的 GPU pass 时间。使用 RadRay profiler，不在生产对象添加仅供测试的计数和轨迹。

截图和离屏导出使用异步 readback：请求 → copy → fence 完成 → invalidate/map → 调用方取走 CPU 结果。ticket 固定资源和输出代次，resize 后仍可读取已经提交的旧结果。它是按需功能，不能成为正常每帧输出链路。

## 接口草案

以下是 C++ 语义接口清单，不是最终头文件；FFM/C ABI 仅对它做数据转换。

| 接口 | 语义 |
| --- | --- |
| `CreateRuntime(deviceSession, config)` | 建立单设备执行环境及预算 |
| `CreateWindowOutput(desc)` / `CreateOffscreenOutput(desc)` | 创建输出，宿主保有窗口所有权 |
| `CreatePipeline(factory, config)` | 构造 native 管线实例及 schema |
| `CreateMesh` / `CreateTexture` / `CreateMaterial` | 同步消费 CPU 输入，返回公开句柄 |
| `UpdateTextureRegions` | 接受有序区域更新 |
| `ReleaseResource(handle)` | 释放外部身份，实际对象按内部引用退役 |
| `ApplyResources(batch)` | 批量执行上述操作，逐项返回结果 |
| `PumpResources(budget)` | 无画面输出时也可推进资源上传 |
| `RenderFrame(output, pipeline, input)` | 内部完成整帧及 token 终结，返回提交/输出结果 |
| `ResizeOutput` / `DestroyOutput` | 在合法边界重建或销毁输出 |
| `RequestReadback` / `PollReadback` / `ReleaseReadback` | 后续加入的按需异步读回 |
| `GetCapabilities` / `ReadDiagnostics` / `GetStatistics` | 能力、错误与生产运行状态 |
| `Shutdown` | 显式结束 GPU 使用并按顺序销毁 |

所有操作都由同一个调度线程串行调用。宿主其他线程可以准备 CPU 数据，但不直接调用这些会进入 RHI 的接口。无后台 native 线程是首版策略，并不限制 GPU 多帧在途。

## RHI 已提供与 runtime 必须补上的功能

| 已有 RHI 能力 | runtime 新增职责 |
| --- | --- |
| Device、资源创建、能力/纹理支持查询 | 会话、资源身份、请求校验、依赖与预算 |
| Buffer map、copy、region upload | 数据保有、行距整理、上传队列、staging 回收 |
| 显式 layout、parameter set、PSO | 由管线共享对象，安全借用和版本寿命 |
| Command allocator/buffer 和 encoder | 帧槽、统一录制、epoch 结束与复用 |
| 显式 resource barrier | 访问声明、状态跟踪与依赖生成 |
| Fence、Submit、有限等待 | 提交序号、CPU/GPU 进度和延迟释放 |
| Swapchain acquire/present/recreate | 输出状态机、唯一 token 所有者及尺寸变更 |
| Query 与 profiler 支持 | 命名 pass、能力不足时的真实可用性与耗时展示 |

设计依据为 [RHI 与后端](../../RadRay/docs/architecture/render-rhi.md) 和 [rhi.h](../../RadRay/modules/render/include/radray/render/rhi.h)。这些功能大部分是 RHI 之上的组织工作，不需要另起一套图形后端。完整跨队列 ownership transfer、通用输出取消和统一可恢复设备丢失等能力不应被 runtime 假定已经存在。

## 实施分级

| 级别 | 功能 | 完成后的能力 |
| --- | --- | --- |
| P0 最小执行基础 | 会话、资源表、mesh/texture 上传、两帧槽、状态跟踪、延迟释放、窗口输出、完整 RenderFrame | 独立宿主通过 native 管线稳定绘制纹理网格 |
| P0 管线基本支持 | 明确 schema、layout/PSO 所有权、参数集安全复用、动态几何、最小 FramePlan | 不依赖 MC draw API，能扩展基础 overlay |
| P1 实际场景支持 | 上传背压/预算、纹理区域更新、多个 view、目标池、完整 material、基础读回和诊断 | 接入地形、动画纹理、实体和 GUI |
| P2 高级管线支持 | 历史资源、preview 缓存、更多格式和 resolve、GPU pass 计时、PSO 预热 | 承载完整效果与复杂界面 |
| P3 测量后扩展 | mesh buffer 分页、实例批处理、自动帧图优化、GPU culling、多队列、光追所需 RHI 扩展 | 按实际瓶颈和算法需求扩展 |

P0 也必须有基本容量上限、错误诊断和正确的资源退役；P1 的预算功能指更完善的调度与统计。首版不实现资产导入器、场景图、ECS、通用任务系统、shader 热编译、动态插件系统或自动设备恢复。

最先编写的模块建议为 `runtime`、`resource_store`、`upload_service`、`frame_scheduler`、`resource_state_tracker`、`output` 和一个最小 pipeline。采用同名 `.h/.cpp`，状态由实例持有。先用一个纹理网格贯穿完整生命周期，再逐步加入 pool、material、overlay 等功能，避免每个子系统都有接口却没有可执行的一帧。
