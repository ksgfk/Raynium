> - 适用: 查阅 2026-11-02 的动态 CRT 与 mimalloc DLL历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-11-02：动态 CRT 与 mimalloc DLL

- 记录日期：2026-11-02
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：动态 CRT 与 mimalloc DLL涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-11-02：动态 CRT 与 mimalloc DLL”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

本节记录当时实际执行的 MD 验证；该构建路线现已由固定 MT 路线替换，当前操作步骤见下文。

使用更新后的锁文件和干净受管 RadRay 源码，按 `windows-x64-msvc` 三元组显式 `--clean` 重建当前所需的 16 个依赖包（含 GTest），
分别生成 Debug `/MDd` 与 Release `/MD` SDK；core/render/runtime 和 bridge 使用新的独立 CMake 目录重新构建。
环境为 Windows x64、Temurin 25.0.5.8、MSVC 19.51.36260.0、RTX 4080 / NVIDIA 621.01。

| 检查 | 实际结果 |
| --- | --- |
| `gradlew.bat build nativeTest -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 17 个纯 Java、64 个 Debug native/GPU 用例通过，无失败或跳过 |
| `nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true -PterrainProfile=true`，附图片/基线目录 | 64 个 Release native/GPU 用例通过；三 DLL 分发 JAR 生成并通过结构/哈希检查 |
| 两配置固定 GPU 场景图像 | 各 48 张 PNG 与此前 FramePipeline 迁移基线逐像素相同，PNG 哈希相同 |
| 分配器探针 | 两配置 `mi_malloc` 分配属于 mimalloc；CRT `malloc` 不属于 mimalloc，`mi_is_redirected() == false`；bridge 关闭后分配器仍有效 |
| 缓存与配置拒绝 | 旧 schema、MT/MD 配置不符、缺失文件、非法路径/哈希、损坏 allocator 缓存及同进程切换 allocator 身份均被拒绝 |
| Release `runClientNative`，关闭 RHI/MC Vulkan 校验和 terrain profile | 标题画面显示并通过“退出游戏”正常结束；日志确认 runtime → environment → bridge 关闭 |

真实客户端 23:39:32 在 Fabric `preLaunch` 加载 mimalloc/redirect，23:39:39 加载 bridge，随后创建 MC Vulkan 和 native D3D12 设备。
实际状态仍为 `redirected=false`：redirect 诊断表明 JVM 的 CRT 已先初始化。这是进程级重定向未生效，不能据此声称全部 CRT/C++ 分配已切换到 mimalloc。
没有修改 Java 可执行文件或安装 JVM 启动前注入。DLL 搜索和寿命契约见[互操作与加载](../architecture/interop.md#dll-的依赖搜索)。
本轮没有进入世界或测量 FPS；地形回归来自自动 GPU 图像用例。GPU 回归日志无 RHI/驱动校验错误或 Vulkan VUID；redirect 自身的未接管诊断保留。
证据保存在本机忽略目录 `build/verification/md-runtime-20261102/`，包含两配置日志、JUnit XML、PNG 和 `game-release.log`。
