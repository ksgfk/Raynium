> - 适用: 查阅 2026-11-04 的FrameSlot 资源退役接口升级历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-04：FrameSlot 资源退役接口升级

- 记录日期：2026-11-04
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：FrameSlot 资源退役接口升级涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-04：FrameSlot 资源退役接口升级”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

更新锁定 RadRay，并适配返回 void 的 Extract/Record/RunFrame 及 FrameSlot::RetireResource。
资源转交与失败后的保活边界见 [Native runtime](../architecture/native-runtime.md#上传帧槽与资源退役)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01，Release `/MT`，启用 Tracy 0.14.1。

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel`：65 项现有 native/GPU 用例通过。
- 随后同配置运行 `nativeTest --tests '*.NativeTerrainTest.failedPreparationRetainsResourcesForPendingFrame'`：新增 4 项双后端/direct-split 用例通过；更新中途失败后不重试，完成在途帧像素检查并正常关闭。
- 连接采集器的短测保存了 0.607 秒、81 个 CPU zones 的 Tracy；采集器及测试 JVM 均正常退出。

两轮测试均无失败、错误或跳过，日志无 D3D12 校验错误、Vulkan VUID 或同步错误。
Tracy 重复初始化时仍出现 Windows SymInitialize 诊断，已插桩 zones 可采集；未验证调用栈符号解析。
自动测试、短采集及 stage 清单位于本机忽略目录 `build/verification/tracy-session-20261104-231831/`。

随后用户以 Release、native D3D12 / MC Vulkan 进入开发世界并操作，RHI wrapper、native 驱动与 MC Vulkan 校验关闭，开启 terrain timestamp 统计。
实机日志确认 ABI 9、2048×2048 五级 mip 图集直接共享和 213×120 预览 RT；游戏正常退出后保存 74.8 秒、25,324 个帧标记、544,251 个 CPU zones 的 Tracy，约 3.84 MiB。
同目录 `session.tracy` 已由 CSV 工具读回，runtime、环境和 DLL 按顺序正常关闭，采集器退出。
本轮未验证 Debug 或打包；两次实机操作场景未对齐，不作为同视点的版本性能比较。
