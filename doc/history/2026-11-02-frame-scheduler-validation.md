> - 适用: 查阅 2026-11-02 的FrameScheduler 与 Raynium 专用 FramePipeline历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-02：FrameScheduler 与 Raynium 专用 FramePipeline

- 记录日期：2026-11-02
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：FrameScheduler 与 Raynium 专用 FramePipeline涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-02：FrameScheduler 与 Raynium 专用 FramePipeline”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

环境为 Windows x64、Temurin 25.0.5.8、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01、Vulkan SDK 1.4.363.0。
先恢复迁移前锁文件对应的 RadRay 源码并重建基线，再更新锁文件、显式 restore 新版；最终使用受管 `external/RadRay`，没有本地源码覆盖。
Debug/Release 的受影响 core/render/runtime 静态库、bridge DLL 与 stage 均重新编译/生成；两份元数据均匹配当前锁文件、`radrayDirty=false`、C++20、`/MT`、ABI 8。
本轮没有修改 RadRay 源码。通用 RenderSystem、SceneRenderer、View/Pass 与 Render Graph 未接入。

以下 Gradle 命令均附 `--console=plain --no-parallel`；图片目录参数使用本机绝对路径。

| 命令/检查 | 实际结果 |
| --- | --- |
| 迁移前 `gradlew.bat nativeTest -PrhiValidation=true -PterrainProfile=true` | 57 个 native/GPU 用例通过，无跳过 |
| 迁移前 `nativeTest --tests '*.NativeTerrainTest' -PrhiValidation=true -PtestImages=<baseline-pixels>` | 保存 48 张固定场景 PNG，完整/split 共享路径使用同一逐像素预期 |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true` | Java 构建、8 个纯 Java 用例与 61 个 Debug native/GPU 用例通过 |
| 最终 `nativeTest -PrhiValidation=true -PterrainProfile=true -PtestImages=<final-debug-pixels> -PtestBaseline=<baseline-pixels>` | 61 个 Debug 用例通过，0 失败、0 跳过；48 张 PNG 与基线逐像素一致，PNG 文件哈希也一致 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true -PtestImages=<final-release-pixels> -PtestBaseline=<baseline-pixels>` | 61 个 Release 用例通过，无跳过；48 张 PNG 与基线一致；生成正式 native JAR |
| `gradlew.bat verifyNativePackage` | 包结构、manifest、ABI 和哈希检查通过 |
| 锁定 RadRay standalone Debug `/MT` 构建 `test_radray_runtime`；`ctest -R 'FrameLoop\|MeshDrawing\|Material'` | 35 项通过，无跳过；FrameLoop 实际运行 D3D12/Vulkan，包括失败恢复、部分提交、arena 回收与退出顺序 |

新增双槽测试先提交两帧，再提供宿主输入 signal，确保两槽同时在途。连续 8 对帧替换同 ID 网格、索引和材质，
交替相机小数偏移、顶点颜色、direct/indirect 和纹理变化掩码；同一可见项展开两个不同原点的实例。
逐像素验证无参数串帧、无旧版本提前释放；非法索引更新在输入 signal 尚不存在时立即拒绝，随后相同序号可继续。
最后验证移除全部场景资源、缺失引用拒绝、epoch 切换和正常 owner 关闭。既有回归继续覆盖 mip、反向深度、大坐标、混合 UV/SOLID、resize、输入重建与消费者超时保活。
两配置日志未发现 RHI/驱动校验错误或 Vulkan VUID。Release 上游 D3D12/Vulkan 各有一处 C4702 不可达代码警告，未导致失败。
GPU validation 未开启；未主动注入 device lost 或 GPU 分配失败。

迁移前实际运行了 D3D12 native / MC Vulkan：进入已有“新的世界”，观察 SOLID 预览，保存截图后返回标题并正常退出，日志确认 runtime、环境和 DLL 顺序关闭。
基线为 854×480 主画面、213×120 小 RT、FOV 配置 0、renderDistance=16、mipmapLevels=4、gamma=0.5、窗口模式，使用 `/time set noon` 固定光照时段。
截图、设置和退出后的世界副本均保存在本机忽略目录，可用同一世界存档中的玩家视点复现。
迁移后的开发客户端已启动并创建设备，但用户按 Escape 停止 computer-use，因此没有继续进入世界、截图比较或交互退出。
这次固定 GPU 场景像素相同不等于最终游戏画面已确认相同，也没有本轮实机性能结论。

本机证据目录为 `build/verification/frame-runtime-20261102/`：`baseline-game.png`、`baseline-options.txt`、`baseline-world/`、
三组像素目录、`final-debug-junit/`、`final-release-junit/`、`runtime-tests.xml` 和相应日志。standalone 的实际 configure/build 脚本也保存在该忽略目录。
未新增开发历史，既有 history 保持原样。
