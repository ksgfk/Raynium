> - 适用: 查阅 2026-10-30 的统一 mesh draw pipeline 与 ABI 8历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-30：统一 mesh draw pipeline 与 ABI 8

- 记录日期：2026-10-30
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：统一 mesh draw pipeline 与 ABI 8涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-30：统一 mesh draw pipeline 与 ABI 8”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

本轮使用 Windows x64、Temurin 25.0.4.1、MSVC 19.51.36260.0、RTX 3060 / NVIDIA 591.44、Vulkan SDK 1.4.357.0。
RadRay 为锁定的干净源码；本轮未修改 RHI。以下 Gradle 命令均附 `--console=plain --no-parallel`。

| 命令 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | Java 构建、8 个纯 Java、57 个 Debug native/GPU 用例通过；核心 CTest 通过 |
| `gradlew.bat nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true` | 57 个 Release native/GPU 用例、核心 CTest、Release DLL/stage/JAR 构建通过 |
| `gradlew.bat verifyNativePackage` | 包结构、ABI 8 manifest 与哈希检查通过 |
| `python tools/check_docs.py`、`git diff --check` | 文档引用与差异空白检查通过 |

核心测试覆盖缓存复用、视图/实例不重建、几何/材质/index/纹理版本失效、UV 不受无关纹理重建影响、准备失败不发布、epoch 清除、连续范围合并、严格实例化和 direct/indirect 参数一致。
GPU 测试覆盖 D3D12/Vulkan、完整与分 mip 传输、所有 mip 像素、反向深度、大坐标、连续帧及 epoch。
新增用例在同一帧混合 UV 与 textured program，分别检查 direct/indirect 的完整像素；输入与输出同时重建后，不重新上传场景即可继续绘制。
无输入 signal 时先提交非法可见引用会立即失败；修正后同一序号可正常提交。消费者未 signal 导致输出关闭超时后，target、renderer、runtime、环境仍保持所有权，完成消费后可继续绘制并关闭。
旧 preview/terrain 导出已删除；两配置 stage 的 `/exports` 检查确认十三个 ABI 8 入口。
最终两配置日志未发现 RHI/驱动校验错误或 Vulkan VUID；GPU validation 未开启。硬件覆盖仍仅限本机 GPU/驱动。

本轮启动过 D3D12 native / MC Vulkan 开发客户端，日志确认 ABI 8、早期环境初始化与设备创建。
按用户要求停止 computer-use 后只读取日志：客户端随后进入本地世界，创建 213×120 地形输出，记录连续帧与几何上传统计，最后正常关闭 runtime、环境和 DLL。
日志未出现 RHI/驱动校验错误；启动期间有 OSHI/Perflib 与开发账户 Realms 鉴权错误。未观察游戏画面，移动、区块更新、resize、资源重载、UV 切换及禁用预览的完整交互验收仍未完成。
该客户端启动早于最后的 pass 文件拆分与关闭路径收尾，不能代表最终代码的实机验证。
CPU/GPU 耗时与上传统计保留。上述运行中，约 60 个可见网格的稳定窗口显示 command_builds 保持 82、geometry_upload_bytes 为 0，CPU 帧调用 p50 约 1.8–2.1 ms，GPU 光栅 p50 约 0.026 ms。
这些样本不是固定场景基准，且采自收尾前版本；本轮未完成同场景同配置的迁移前后性能对比，不据此推断帧率变化。
因此本轮自动验证已通过，完整的实机和性能验收尚未完成；旧日期记录不是本轮重测结果。
日志与两配置 XML 位于本机忽略目录 `build/verification/mesh-pipeline-20261030/`，没有新增 history。
