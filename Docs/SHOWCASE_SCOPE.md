# 展示范围与来源

main 用于审阅源码、取舍与证据，不包含完整运行资源。

## 当前来源

Source 与开发提交 2efa683616770e05d915276bb074fbcccff6bc0a 一致，Source 树 704ea2b98c0a35f78f2539d4e6f5be80f77b5218。没有为展示再实现一套技能或取消系统。本次同步玩法／资源边界、既有激活身份复用、动画拒绝收口、连接计时与真实回归代码。

保留 UE 配置、uproject、构建目标、原有公开启动／网络／只读审计脚本及 Gameplay JSON 示例。Source 中的编辑器资产生成入口说明蓝图契约，不能替代资源或素材授权。个人复习笔记、原素材导入与一次性修复包装器不分发。

## 当前快照排除

Content、第三方原素材、缓存与打包产物、本机原始日志／Trace／截图／录像、测试与恢复票据、机器与账号标识。现有本地 Content 保留，没有递归删除或重写历史；旧提交及其他分支仍可能含资源。

## 验证结果与摘录

[当前证据](../Tests/README.md)记录 184 项回归与两种构建；Normal 带渲染双客户端通过 148 项门禁，Harsh 的拥有者 R 结果表现未观察到，保持失败。不会从服务器结算正确推导客户端表现通过。

公开 JSON 保留原报告相对路径与 SHA-256、汇总、逐项名称／状态、必需门禁及显式 ASSERT／DONE／FAIL 的非身份字段，不上传 token、detail、命令、日志或机器路径。原报告在提交前产生，实测 Git HEAD/dirty 单独保留；Source 清单与随后提交的源码相符。main 缺少资源，未独立重新运行该快照。

推荐阅读：[架构](ARCHITECTURE.md) → [实现链](INTERVIEW_GUIDE.md) → CombatAbility／AbilitySystem／Attributes → GameMode／GameInstance → [测试](../Tests/README.md)。旧性能与网络证据按各自构建理解。
