> - 适用: 查阅 2026-10-28 的C++ bridge 与固定底层枚举历史记录
> - 权威: 本文保留原记录的过程、结果与限制，不作为当前实现或验证保证；当前操作见构建与测试指南
> - 锚点: `doc/guide/build-test.md`, `build.gradle`, `gradle/native.gradle`

# 2026-10-28：C++ bridge 与固定底层枚举

- 记录日期：2026-10-28
- 归档日期：2026-11-09
- 状态：原有开发与验证记录归档；本次未重新执行记录中的检查
- 范围：C++ bridge 与固定底层枚举涉及的构建、测试、运行或采样
- 来源：归档前工作树 `doc/guide/build-test.md` 的“2026-10-28：C++ bridge 与固定底层枚举”节，包含此前未提交的文档内容

当前执行入口见[构建与测试](../guide/build-test.md)，相邻历史记录见[历史索引](README.md)。
下列内容按原文保留；“当前”“本节”“下节”“下文”等指代以归档前的来源页及记录日期为准，链接不表示对当前版本重新验证。

## 原记录

在下节单例环境的同一机器和工具链上，将 bridge 改为 C++ 头文件后，重新执行
`build nativeTest -PrhiValidation=true`、
`nativeTest packageNativeMod -PnativeConfig=Release -PrhiValidation=true` 和 `verifyNativePackage`，
均附 `--console=plain --no-parallel`。Java 构建成功，Debug/Release 各 39 个 native 用例通过，
Release 打包与包校验通过；纯 Java 测试复用本轮先前的通过结果。
两配置的 `dumpbin /exports` 均确认九个 C 导出名称保持不变，日志无驱动校验错误。
已拉取 RadRay 远端 develop，其版本与锁文件和干净工作树一致，无需更新依赖。
本次仅在后台构建和测试，未启动 MC；证据位于 `build/verification/bridge-fixed-enums-20261028/`。
