> - 适用: 查阅 2026-11-05 的帧内 GPU 资源持久化历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-05：帧内 GPU 资源持久化

- 记录日期：2026-11-05
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：帧内 GPU 资源持久化涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-05：帧内 GPU 资源持久化”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

每槽帧资源、每槽 staging 与几何 arena 落地后的验证。资源归属与退役边界见 [Native runtime](../architecture/native-runtime.md#上传帧槽与资源退役)。
环境为 Windows x64、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44；RadRay 基于锁文件提交，但工作树含本机改动（`radrayDirty=true`）。
Debug `/MTd` 依赖按当前 RadRay 提交重建：`python tools/build_debug_dependencies.py --clean --parallel 8`。

| 实际检查 | 结果 |
| --- | --- |
| `gradlew.bat nativeTest -PnativeConfig=Debug -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 81 项 Debug native/GPU 用例通过，无失败、错误或跳过；含 RadRay RHI wrapper 检查 |
| `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=true -PterrainProfile=true --console=plain --no-parallel` | 81 项 Release native/GPU 用例通过，无失败、错误或跳过 |
| 两配置日志 | 双后端 driver validation 开启；无 RHI 校验拒绝、D3D12 警告/错误、Vulkan VUID 或同步验证报告，无 `[warn]`/`[error]` 行 |
| 两配置 stage manifest | ABI 9、Debug MTd / Release MT、`radrayDirty=true`；Release 另含 profiler |

新增三组双后端 × 完整/split 用例：驻留顶点跨两个 arena 块且第二块含非零 base vertex、
单帧上传超过初始 8 MiB staging 并在替换后由两槽复用、删除 mesh 后区间在槽复用后才被同尺寸新 mesh 重新分配（同块邻居全程可见）。
既有 78 项继续覆盖双槽在途参数/资源隔离、准备失败保活、输出按 32/36/40 逐代重建（槽 0 遇尺寸变化重建深度）、增量驻留、材质/mesh 更新、反向深度与大坐标。

本轮未启动游戏客户端、未重采 Tracy，也没有实机性能结论；`packageNativeMod` / `verifyNativePackage` 未执行，
因为受管 RadRay 工作树处于 dirty 状态，打包会按要求拒绝。
本机日志位于忽略目录 `build/_dtest_full2.log`、`build/_rtest_final.log`，JUnit XML 在 `build/test-results/nativeTest/`。
