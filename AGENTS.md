# Raynium

Minecraft Fabric 客户端模组。当前 Java 通过 FFM/C ABI 加载 native runtime，后者通过 RadRay RHI 创建设备和 Direct 队列；游戏仍使用原版渲染。

## 文档入口

本文件也是长期文档，负责仓库级约束与导航。按任务读取，不预加载整个 `doc/`。

| 任务 | 入口 |
| --- | --- |
| 定位已有子系统、当前状态与文档 | [架构地图](doc/architecture/overview.md)，再读所属文档 |
| 准备依赖、构建、测试、启动与打包 | [构建与测试](doc/guide/build-test.md) |
| 配置 IDEA、阅读 Minecraft 源码 | [开发环境](doc/guide/dev-environment.md) |
| 修改 C ABI、FFM、加载与跨语言寿命 | [互操作与加载](doc/architecture/interop.md) |
| 修改 native 设备会话和所有权 | [Native runtime](doc/architecture/native-runtime.md) |
| 查看 Fabric 生命周期与 MC 接入边界 | [Minecraft 接入](doc/architecture/minecraft-integration.md) |
| 查阅渲染目标变化与未实施方案 | [思考历史索引](doc/architecture/overview.md#思考历史索引) |
| 整理或同步文档 | 项目技能 [raynium-doc](.agents/skills/raynium-doc/SKILL.md) |

除 `doc/history/` 外，仓库文档只记录事实：当前源码/配置/锁文件支撑的实现、已执行的验证、注明版本的源码调查，以及已生效的仓库约束和可执行操作步骤。
建议、设想、推断、候选方案、未来职责/接口、实施路线和未实施验收计划只保存在 `doc/history/`，作为当时的思考过程；已经确认但尚未落地的目标或设计同样归入历史。
不得仅删去“建议”等措辞，把未实现方案改写成当前契约；缺失能力可以写为现状，不在当前页面附实现计划。
当前事实归属本文件、`doc/architecture/` 与 `doc/guide/`；README 只保留项目现状和入口。同一事实只维护一份权威说明，行为、接口或流程变化时同步所属页面。
保存思考历史或迁移已有方案须有用户授权；已有授权直接执行，不重复确认。普通实现、测试通过和日常文档同步不自动生成历史。
迁移时先保存带日期、来源和确认状态的记录，再清理当前页面；已有历史记录与附件保持原样，不修改、不改名、不删除。
除非用户明确要求，文档（含 history）不记录 Git hash，包括完整值、缩写和链接 URL 中的值。使用日期、版本、来源路径和状态说明背景；依赖精确版本以锁文件为准。
具体归属、页面格式和历史规则见[文档维护](doc/guide/documentation.md)。

## 关键约束

- 实现事实以源码、配置和锁文件为准，不把未执行的检查写成通过；第三方源码调查不等于本项目已实现。
- 代码注释写 API 契约与局部不变量，设计说明放文档；代码文件至多保留一处指向所属文档的文件头入口。
- C++ 层禁止默认构造后再调用 `Init` / `Initialize` 的二段式构造。可失败的创建使用 `static Create` 返回成功/失败，准备完全部资源后才构造并交付有效对象；失败由局部 RAII 清理。
- 当前 C ABI 状态只使用 `RN_STATUS_OK` / `RN_STATUS_FAILED`，具体失败原因写入调用方提供的字符串缓冲区。
