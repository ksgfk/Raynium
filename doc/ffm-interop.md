# Java 与 C++ 的 FFM 互操作

基线：JDK 25、Windows x64、`rayniumruntime.dll`。本文将 JDK 官方约束与 Raynium 的实施建议分开说明。当前已实现 DLL 加载、ABI/构建身份检查、runtime 创建/销毁和 Java 生命周期封装；资源、帧提交仍是提案。ABI 当前版本为 `3`，临时探针已删除。

## 接口边界

建议保持 `Java 场景适配 → FFM → 窄 C ABI → C++ runtime/管线 → RadRay RHI`。库名表示最终 native 产物；`bridge.h/.cpp` 继续承担跨语言入口，C++ runtime 可以在库内使用自己的类型与模块。改名不表示 runtime 功能已经实现，也不要求 Java 直接调用 C++ 类。

JDK native linker 依据平台 ABI 和调用方提供的 `FunctionDescriptor` 建立调用。它不会从导出符号推导参数类型。Raynium 因此建议只导出 `extern "C"` 函数，不暴露 C++ 类、虚表、STL 容器、引用、重载或可变参数。类型宽度不能照抄其他平台：Windows x64 的 C `long` 是 32 位。见 [JDK 25 Linker](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.html)。

| C ABI 表达 | Java 表达 | Raynium 建议 |
| --- | --- | --- |
| `uint32_t` / `int32_t` | `JAVA_INT` / `int` | 状态、枚举和计数明确范围；需要时使用无符号转换 |
| `uint64_t` | `JAVA_LONG` / `long` | 资源句柄和版本号按位传递，不依赖有符号大小关系 |
| `float` / `double` | `JAVA_FLOAT` / `JAVA_DOUBLE` | 坐标和矩阵另行明确约定 |
| `rn_runtime*` | `ADDRESS` / `MemorySegment` | 不透明拥有对象，Java 不解引用其内部 |
| `const void*` + 字节数 | native segment + 明确长度 | 同时约定可访问范围和借用时长 |
| UTF-8 字符串 | 字节段 + 长度 | 不使用平台相关的 `wchar_t` 作为通用文本格式 |

固定宽度类型是本项目的接口选择，不是 FFM 只支持这些类型。必须使用 `size_t` 等平台类型时，从 `Linker.canonicalLayouts()` 取得对应布局；不要把所有 C 整数映射为 Java `long`。

## 布局与版本

`MemoryLayout.structLayout` 不替 C 编译器插入 padding。Java 必须明确内部和尾部填充，并使用有名字的字段、`byteOffset` 或 VarHandle；native 用 `sizeof`、`alignof`、`offsetof` 断言对应布局。见 [JDK 25 MemoryLayout](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/MemoryLayout.html)。

建议将小型控制结构与批量数据分开：控制结构使用匹配 C ABI 的自然对齐布局；ResourceBatch/FramePacket 使用明确定义的字节协议，规定小端、字段宽度、offset、count 和 stride。两者都不能直接等同于任意 C++ 内部结构。native 校验 offset/length、乘加溢出、对齐、枚举和句柄 generation。

公共描述结构可带 `struct_size` 与版本，兼容扩展需要定义旧字段含义、缺省值和可选特性。删除函数或改变参数语义属于不兼容修改，应升级 ABI。当前 ABI 为 3：`rn_query_abi` 增加诊断缓冲区参数，状态统一为 OK/FAILED；`rn_abi_info` 仍为 240 字节；schema 版本与 ABI 版本是不同概念。

现在有两个查询函数和 create/destroy，手写绑定规模可控。`NativeBindings` 已集中声明 ABI 与 runtime 描述结构布局，按字段名获取偏移；接口增多后再评估从公共 C 头生成绑定。OpenJDK 的 [jextract](https://github.com/openjdk/jextract) 可以从 native 头生成 FFM 绑定，但不会替项目定义所有权、线程和错误契约。若引入，固定工具版本及与 JDK 25 匹配的分支/构建，不默认跟随最新主线；不要将整个 RadRay C++ 头树作为输入。

## 内存所有权与借用

`Arena` 同时描述 Java 访问线程限制和释放时机；confined arena 由创建线程访问/关闭，shared arena 支持多线程访问/关闭，自动 arena 的回收时间由 GC 决定。对于需要及时释放的 runtime，优先显式关闭。见 [JDK 25 Arena](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Arena.html)。

建议首版采用以下契约：

| 数据 | 所有者 | native 可使用到何时 |
| --- | --- | --- |
| 创建参数、资源批次、FramePacket | Java 调用方 | 本次 downcall 返回前 |
| 已接受上传的 staging 数据 | native runtime | native 自己管理，保留至 GPU 不再使用 |
| mesh/texture/material | native runtime | Java 请求逻辑释放后，按 GPU 完成进度延迟销毁 |
| 诊断输出缓冲区 | Java 调用方 | native 只在当前调用中写入 |
| runtime | native runtime | 通过对应销毁入口释放 |

核心要求是：资源上传或帧调用返回前，native 已完成对 Java 输入的读取，或已复制到 native 拥有的存储。调用返回不代表 GPU 完成，但此时 Java 可以重用 CPU 输入；native 不保留其裸指针。未来若实测需要跨调用借用，再增加显式 lease/完成通知，不静默延长借用期。

`MemorySegment` 的 Java 边界检查不会约束 C++ 对裸指针的使用。`MemorySegment.ofArray` 产生的 heap segment 也不是可长期保存的 native 地址。跨调用返回的地址没有自动附带真实容量和寿命；`reinterpret` 只建立访问视图，不能证明地址有效或转移所有权。见 [JDK 25 MemorySegment](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/MemorySegment.html)。

Java arena 分配的输入由 Java 释放；runtime/mimalloc 分配的对象由同一个 DLL 的销毁 API 释放。不能用 JVM 所见的默认 `free` 释放 mimalloc 指针，也不能把 Java 输入交给 C++ `delete`。这在当前 `/MT` 配置下尤其需要坚持；参见 [Microsoft：跨 DLL 传递 CRT 对象](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)。

## 线程与调用粒度

首版建议由 MC 渲染线程串行调用 runtime 的创建、资源提交、渲染、resize 和销毁。区块工作线程先发布不可变结果，渲染线程再写入自己的 native 缓冲区。需要直接跨线程传递 native 数据时，显式定义 shared arena、发布协议和回收方；不能转交一个 confined segment 后让其他 Java 线程访问。

shared arena 仅解除访问线程限制，不会让 C++ runtime 或共享字段自动线程安全。Java 与 C++ 的无锁共享结构需要单独约定内存顺序和完成通知；首版使用同步调用和有所有者的队列，不引入跨语言共享原子协议。native 内存也不能直接套用普通 Java 对象的全部内存模型保证。见 [JDK 25 foreign 包说明](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/package-summary.html)。

绑定在库初始化时建立并缓存在会话对象中；使用签名准确的 `invokeExact`，不要逐帧查符号或重建 MethodHandle。性能优先优化为批量资源更新和一次完整帧提交，避免逐顶点、逐 draw 调用。先测量 CPU 时间、复制量与提交频次，不预设 FFM 一定比 JNI 更快。

渲染、上传、驱动调用、等待和销毁都使用普通 downcall。`Linker.Option.critical` 只适用于所有情况下都极短且不回调 Java 的函数；允许 heap access 时获得的地址只在调用期间有效。它不是通用的“更快调用”开关。见 [JDK 25 Linker.Option](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.Option.html)。

## 错误与回调

当前 C ABI 只返回 `RN_STATUS_OK=0` 或 `RN_STATUS_FAILED=1`。具体失败原因写入调用方提供的 UTF-8 字符串缓冲区；`rn_query_abi` 与 `rn_create_runtime` 使用相同诊断约定，不维护全局 last-error，不引入日志 upcall。未来资源与批次接口同样遵循当前状态约束，需要更细机器可读分类时再单独设计。

C++ 异常不跨 C ABI；`noexcept` 也不是异常到状态码的转换器，异常逃出会终止进程。可恢复失败使用显式结果，并审核实际调用路径的分配/异常策略；不要用 `catch (...)` 隐藏 OOM 或不变量破坏。Java 层可将可恢复状态转换为类型明确的结果或异常，native 内存错误不能靠 Java `catch` 恢复。

若以后必须 upcall，回调 stub 和它引用的 Java 状态必须活到 native 注销回调且排空在途调用之后。回调只做有界的记录/入队工作，不直接访问 MC 世界或递归调用 runtime。回调目标不能让异常逃出；JDK 明确指出这会使 JVM 突然终止，因此必须在回调边界完整处理异常并触发明确的失败策略。见 [JDK 25 upcallStub](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.html#upcallStub(java.lang.invoke.MethodHandle,java.lang.foreign.FunctionDescriptor,java.lang.foreign.Arena,java.lang.foreign.Linker.Option...))。

## 会话和库的寿命

`SymbolLookup.libraryLookup(Path, Arena)` 将库加载寿命关联到 arena。库不能先于其对象、函数指针或回调关闭。见 [JDK 25 SymbolLookup](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/SymbolLookup.html)。

`NativeRuntime implements AutoCloseable` 已拥有不透明 runtime handle 与 `NativeLibrary`；后者持久保存 `NativeBindings`。创建线程检查阻止跨线程关闭，close 幂等，先通过 destroy 排空 Direct 队列并销毁 runtime，再关闭 library arena。创建失败先释放已成功创建的 native 对象，再关闭 DLL。以后加入提交、工作线程或回调时，须先停止提交并排空调用/回调，再销毁对象。

`NativeLibrary` 的 shared arena 不代表 RHI 支持并发；当前全部生命周期操作在 MC 渲染线程串行执行。创建描述、输出指针槽与诊断缓冲区使用 confined scratch arena，输出槽关闭不释放其指向的 runtime。Java 不解引用或 reinterpret 该对象地址；只有 DLL 的 destroy 负责释放。Library、runtime、frame scratch 是不同寿命，不使用 GC/Cleaner 作为正常 GPU 资源回收机制。

Fabric 当前使用 unnamed module，游戏 JVM 继续显式设置 `--enable-native-access=ALL-UNNAMED`。不能把该参数只写入 Gradle daemon，也不能依靠普通 mod JAR 的 manifest 给宿主 JVM 授权；官方 manifest 入口针对可执行 JAR。见 [JDK 25 Restricted Methods](https://docs.oracle.com/en/java/javase/25/core/restricted-methods.html)。

## 下一步实施顺序

已完成集中 ABI 布局、`rn_create_runtime` / `rn_destroy_runtime` 和 Java `NativeRuntime` 的所有权、线程及失败清理。`RayniumRuntime::Create` 在局部资源准备完成后才构造有效对象。当前 query 诊断参数与状态语义已升级到 ABI 3；manifest 和版本入口都会拒绝旧 ABI，不能用 ABI 2 的调用签名访问新 query。

1. 建立具有 generation 的资源句柄、资源批次和诊断输出，明确成功接收时的数据复制与背压规则。
2. 接入 FramePacket 与完整帧执行，最后按现有 MC 接入设计替换呈现与绘制节点。

资源与帧入口仍是实施建议。功能范围仍以 [runtime 设计](native-runtime-design.md) 和 [实现设计](implementation-design.md) 为准；构建和加载操作见 [Native 工程操作](native-development.md)。
