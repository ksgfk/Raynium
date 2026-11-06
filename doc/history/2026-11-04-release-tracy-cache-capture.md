> - 适用: 查阅 2026-11-04 的Release Tracy 缓存路径采集历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-04：Release Tracy 缓存路径采集

- 记录日期：2026-11-04
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：Release Tracy 缓存路径采集涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-04：Release Tracy 缓存路径采集”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在同日 ABI 9 重写后的源码上补充分阶段 Tracy zones 与缓存/驻留 plots。Release `/MT`、静态 Tracy 0.14.1，
native D3D12、MC Vulkan；RHI wrapper、native 驱动与 MC Vulkan 校验关闭，开启 terrain timestamp 统计。
本配置的 `nativeTest` 61 项通过，无失败或跳过；客户端由用户按上述配置自行启动并操作，采集器先行等待连接。
游戏日志确认 ABI 9、Release/MT、2048×2048 五级 mip 图集直接共享及 213×120 输出 RT。

客户端正常退出后，采集器自动保存 63.3 秒、5,658 个帧标记、207,284 个 CPU zones 的记录，约 1.34 MiB；
Tracy CSV 工具成功读回，游戏日志确认 runtime、环境及 bridge DLL 依次正常关闭。
其中 5,655 次 native Render 调用，包含区块加载和场景增长，全段平均 3.92 ms、p95 10.86 ms。

选择 trace 时间 49.306–51.831 秒的连续 240 次调用：驻留 mesh/primitive 均为 3,155，可见命令 3,361，
缓存命令 17,345；mesh 更新与脏 batch 数均为 0，期间没有 BuildBatch zone。
该区间由资源/可见数量固定条件选择，未记录相机是否固定。

| 项目 | 平均 ms | p95 ms |
| --- | --- | --- |
| native Render | 6.849 | 8.619 |
| Scene Prepare | 1.514 | 2.109 |
| Scene 备份 | 1.381 | 1.835 |
| Scene 增量应用 | 1.293 | 1.912 |
| draw 命令录制 | 0.950 | 1.553 |
| 可见命令排序 | 0.076 | 0.094 |
| 帧槽复用等待 | 0.0011 | 0.0015 |
| 已完成 native GPU timestamp 区间 | 0.458 | 0.550 |

CPU zones 为包含子 zone 的墙钟耗时，不能相加；GPU 样本在回收帧槽时发布，不对应发布时这帧的即时 GPU 耗时。
本轮没有采集 JFR 或 MC 全帧 GPU 数据，场景与此前采样不同，不作为同视点的版本性能对比。
证据位于本机忽略目录 `build/verification/tracy-session-20261104/`：`session.tracy`、游戏/构建测试日志、
manifest、测试 XML、CSV、采集设置及分析脚本/JSON。文档与 diff 检查通过。
