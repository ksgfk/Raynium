> - 适用: 查阅 2026-11-06 的批量绘制命令升级与采样准备历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-06：批量绘制命令升级与采样准备

- 记录日期：2026-11-06
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：批量绘制命令升级与采样准备涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-06：批量绘制命令升级与采样准备”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

RadRay 更新到锁文件版本，受管源码保持干净。Raynium 将 ABI submesh 可见项转换成 ElementMask，
按同一掩码声明几何访问，并对齐上游每 batch 最多 32 个 submesh 的限制；缓存及绑定池边界见 [Native runtime](../architecture/native-runtime.md#命令缓存与参数绑定)。
Windows x64、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01、Release `/MT`、Tracy 0.14.1：

- `gradlew.bat nativeTest -PnativeConfig=Release -PnativeProfiler=true -PrhiValidation=false -PvulkanValidation=false -PterrainProfile=true --console=plain --no-parallel`：89 项 native/GPU 用例通过，无失败、错误或跳过；新增双后端 × direct/split 用例逐像素验证第 0/31 个 submesh、全部元素掩码与中间空元素。
- 同配置连接采集器运行 `NativeTerrainTest.twoPendingSlotsKeepParametersAndReplacedGeometry`：4 项参数化用例通过；采集器自动保存并退出，约 809 ms、21 个帧标记、1,793 个 CPU zones，CSV 成功读回 106 种区间，含 `RadRay.MeshDraw.Submit`。
- `python tools/check_docs.py`、`git diff --check`：通过。

日志无 RHI 拒绝、D3D12 警告/错误或 Vulkan VUID；Tracy 重复初始化仍有 SymInitialize 诊断，本轮未验证调用栈符号解析。
测试消费者和显式诊断用例的校验设置与游戏采样不同。本轮未重跑纯 Java 测试、Debug 或发布打包。
证据在本机忽略目录 `build/verification/tracy-session-20261106-222653/`，含测试日志、两轮测试 XML、短采集及构建清单。

随后用户以同一 Release/MT 构建启动游戏并操作本地世界，native D3D12 / MC Vulkan，关闭 RHI wrapper、native 驱动与 MC Vulkan 校验，启用 terrain timestamp。
日志确认干净 RadRay、2048×2048 五级 mip 图集直接共享及 213×120 预览 RT；退出时 runtime、环境和 DLL 依次正常关闭。
Tracy 自动保存约 101.4 秒、19,342 个帧标记、1,602,135 个 CPU zones，`session.tracy` 为 10,902,033 字节，采集器和游戏均已退出。
CPU 总耗时、CPU self time 与 GPU 事件 CSV 成功读回，同目录保存 `game.log` 和 `capture-summary.json`。本次未录制 JFR，也未与旧采样对齐场景或视点。
游戏日志有开发账号认证/Realms 诊断及缺少 post effect 的警告，未见 native 渲染失败。
