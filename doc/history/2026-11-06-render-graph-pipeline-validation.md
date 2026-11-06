> - 适用: 查阅 2026-11-06 的RenderGraphFramePipeline 接入历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-06：RenderGraphFramePipeline 接入

- 记录日期：2026-11-06
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：RenderGraphFramePipeline 接入涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-06：RenderGraphFramePipeline 接入”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到锁文件版本，受管源码保持干净（`radrayDirty=false`），原有本机插桩未回放。
SolidFramePipeline 改用 runtime RDG，消费边界和共享交接见 [Native runtime](../architecture/native-runtime.md)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44；显式执行
`python tools/build_debug_dependencies.py --parallel 8` 更新 Debug 依赖身份并完成增量准备。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat test nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 19 项纯 Java 测试、85 项 native/GPU 用例通过，无失败、错误或跳过 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 85 项 native/GPU 用例通过，无失败、错误或跳过 |
| `gradlew.bat packageNativeMod verifyNativePackage --console=plain --no-parallel` | 干净 Release/MT、未启用 profiler 的分发包构建及结构、哈希校验通过 |
| `python tools/check_docs.py`、`git diff --check` | 通过 |

双配置覆盖 D3D12/Vulkan、direct/split 纹理、双槽在途资源、几何 arena、深度与大坐标。
新增 4 项参数化用例：在无可见 draw 时上传几何及纹理，两槽空帧后再绘制，随后输出尺寸按 32/36/40/32 往返，
每次均在空帧后复用驻留资产并逐像素验证，覆盖上传输出保活、槽池复用与资源交接。
两配置均启用 native 驱动校验；Debug 启用 RHI wrapper，Release 按上游默认关闭 wrapper。
日志无 RHI 拒绝、D3D12 警告/错误、Vulkan VUID 或同步验证错误。
非阻塞诊断包括 LWJGL 的 Unsafe 弃用提示、Vulkan loader 的包映射调试消息、上游 D3D12 的 C4702 不可达代码警告，
以及启用 Tracy 时已有的 SymInitialize 错误；本轮未采集 Tracy、未验证符号解析，也未启动游戏客户端。

本机证据在忽略目录 `build/verification/radray-upgrade-20261106/`：`debug-final.log`、`release.log`、`package.log`，
以及分别保存的 `debug-results/`、`release-results/`、`java-results/` JUnit XML。
分发包为 `build/libs/raynium-0.1.0-SNAPSHOT-windows-x86_64.jar`。

随后用户以 Release/MT、native D3D12 / MC Vulkan 启动开发客户端并进入本地世界操作，启用 Tracy 和 terrain timestamp，
关闭 RHI wrapper、native 驱动及 MC Vulkan 校验。游戏日志确认干净 RadRay、ABI 9、2048×2048 五级 mip 图集直接共享及 213×120 预览 RT。
游戏正常退出后，runtime、环境和 DLL 依次关闭，采集器自动结束并保存约 73.9 秒、21,524 个帧标记、456,455 个 CPU zones，文件 5,001,314 字节。
`tracy-csvexport` 成功读回，确认 `Raynium.Frame.SetupGraph` 和 `Raynium.Frame.RecordDraws` 等 zone 存在。
证据在本机忽略目录 `build/verification/tracy-session-20261106-151006/`，含 `session.tracy`、`zones.csv`、`game.log`、构建清单、采集配置及摘要。
本次未录制 JFR，场景未与旧采样对齐，不作为同视点版本性能比较。
