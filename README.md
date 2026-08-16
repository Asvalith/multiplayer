# Multiplayer Co-op GAS Demo

UE5.5 双人合作项目，重点研究 Gameplay Ability System 在真实网络条件下的客户端预测、服务器权威裁决和状态收敛。

当前作品集目标不是堆叠系统，而是完成一个可打包、可复现的聚焦版本：

```text
3 个正式技能（Damage / Ally Heal / Immunity）
+ 1 个正式 Guardian 敌人
+ 1 条双人协作单局流程
+ Prediction Accept / Reject / Target Reject
+ 0 / 150 / 300ms 与 5% loss 证据
+ Dedicated + 2 Client、有限回溯、晚加入和 Insights
```

## 项目亮点

- PlayerState 持有 ASC/AttributeSet，Character 作为 Avatar，复活后重绑而不重复授予能力。
- LocalPredicted 技能、PredictionKey、预测 Cost/Cooldown/持续 GE 与 GameplayCue。
- Development 实验入口触发真实 `ClientActivateAbilityFailed`，验证拒绝后的资源与表现清理。
- DamageIntent 只上传 ShotId、量化 Origin/Direction 和估算服务器时间；服务器重新 Trace 并决定最终结果。
- Ally Heal 只上传队友候选；服务器复验 Team、存活、距离、LOS 和目标 ASC 后才应用治疗 GE。
- 服务器 `ExecutionCalculation` 捕获攻击、护甲、暴击、抗性与 Vulnerability；自定义 EffectContext 实现 `Duplicate` 和 `NetSerialize`。
- Guardian C++ 基座使用独立基础 ASC、服务器计时 AI、GAS 近战、持续护盾 GE 和“一人引导破盾、另一人输出”的合作规则。
- 服务器权威的 Session、钥匙、压力板、门、平台、胜利、死亡与复活流程。
- 已有 0ms、约 300ms RTT 和单次 5% loss 的结构化日志实验；详细边界见面试资料和 Evidence。

## 当前发布状态

GAS M0～M6 技术核心、Ally Heal C++、Guardian C++ 和统一表现契约已经存在，但 Portfolio V1 尚未封板。当前阻塞项是正式蓝图/素材接线、Montage/Cue/VFX、非 Headless 整局、Dedicated/回溯/晚加入/Insights 与打包证据。

任务状态和唯一封板门禁只维护在 [TODO](Docs/GAS_Portfolio_Technical_Route.md)。本 README 不维护逐项完成状态。

## 操作

正式输入：

| 输入 | 能力 |
|---|---|
| 鼠标左键 | Damage |
| Q | Ally Heal；无合法队友候选时可按配置回退自疗 |
| E | Immunity |

常用验证入口：

```powershell
# 双进程技术实验；参数和阶段见脚本帮助
PowerShell -ExecutionPolicy Bypass -File .\Scripts\StartGASM5TwoPlayers.ps1

# 日志行为门禁
PowerShell -ExecutionPolicy Bypass -File .\Scripts\VerifyGASM6Logs.ps1
PowerShell -ExecutionPolicy Bypass -File .\Scripts\VerifyGASM6IntentLogs.ps1
```

构建、Automation、弱网参数及日志字段的具体操作只维护在[面试复习资料](Docs/GAS_Interview_Completed_Snapshot.md#12-工具与具体操作)。历史运行结果位于 [`Docs/Evidence`](Docs/Evidence/)。

## 三份主文档

| 文档 | 唯一职责 | 什么时候更新 |
|---|---|---|
| 本 [README](README.md) | 面向招聘者的项目入口、启动方式、亮点和发布边界 | 形成新可演示版本时 |
| [面试复习资料](Docs/GAS_Interview_Completed_Snapshot.md) | 架构、预测边界、调用链、问题复盘、工具和面试答案 | 设计或事实发生变化时 |
| [TODO / 封板标准](Docs/GAS_Portfolio_Technical_Route.md) | 唯一任务状态、执行顺序、完成门禁和排除项 | 每个任务开始或验收时 |

Evidence 是只读证明，不是第四份状态文档。其他 `Docs/*.md` 是历史研究或旧版长文，只用于追溯，不再维护当前状态。

## 防止重复维护

开始新任务时只按以下顺序读取：

1. 本 README：确认项目边界和文档入口。
2. TODO：确认当前唯一任务、依赖和验收门禁。
3. 只有需要解释设计时才打开面试复习资料。
4. 只有需要核验历史结论时才打开对应 Evidence。

完成任务时：

- 运行结果写入新的 Evidence/RunId，不覆盖历史报告；
- 只在 TODO 更新状态和证据链接；
- 架构没有变化，不修改面试资料；
- 尚未形成新可展示版本，不修改 README；
- 禁止在其他文档复制“当前完成度”或“下一步任务”。

## 准确边界

当前不能声称已经完成 Dedicated Server、晚加入、服务器回溯、Network Insights 优化、正式 Guardian EnemyBP/素材、完整动画特效或打包整局。项目采用服务器权威状态同步与有限客户端预测，不是 Lockstep 帧同步，也不是商业反作弊产品。
