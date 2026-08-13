# GAS M2 团队规则与准星目标验收报告

> 分支：`coop-GAS`
>
> 日期：2026-08-13

## 1. 实现边界

- 服务器复制的 `CoopTeamId`：玩家队伍为 1，敌人队伍为 2。
- `ImultiplayerCoopTeamAgentInterface`：PlayerState 和 TargetDummy 提供统一团队身份。
- `UmultiplayerTeamLibrary`：从 Actor、Pawn 的 PlayerState 或 ASC Owner 解析团队。
- GameplayTag 保留为 GAS 查询标记，但不再是唯一团队权威。
- 本地准星 Sphere Sweep 生成 `FGameplayAbilityTargetData_SingleTargetHit`。
- 服务器再次检查目标存在、距离、TeamId 敌对关系、Team Tag、存活和视线。

## 2. 构建与自动化

| 检查 | 结果 |
|---|---|
| Editor Development | 通过 |
| Win64 Development Game | 通过 |
| PlayerState 实现团队接口 | 自动化通过 |
| TargetDummy 实现团队接口 | 自动化通过 |
| 玩家/敌人 TeamId 不同 | 自动化通过 |
| M1 InputConfig/AbilitySet 回归 | 自动化通过 |
| `multiplayer.GAS.Configuration` | 找到 1 项，结果成功 |

## 3. 双窗口运行矩阵

| 编号 | 操作 | 预期 | 状态 |
|---|---|---|---|
| M2-TGT-01 | 按 `7`，准星对准方块，鼠标左键 | 服务器接受 SingleTargetHit，目标扣 25 Health | Run `20260813_013549` 日志记录四次 25 伤害；用户确认通过 |
| M2-TGT-02 | 准星偏离方块，鼠标左键 | TargetData 为空，不扣除目标 Health | 用户确认 M2 运行验收通过 |
| M2-TEAM-01 | 准星对准另一名玩家并攻击 | 本地团队筛选不生成玩家 TargetData；服务器也拒绝同 TeamId | 用户确认 M2 运行验收通过 |
| M2-VAL-01 | 目标超过 650 距离 | 服务器拒绝 | 待验证 |
| M2-VAL-02 | Visibility 墙体遮挡目标 | 采集或服务器视线检查拒绝 | 待验证 |
| M2-NET-01 | Host 和 Client 分别攻击 | 两者均由服务器最终结算，TargetDummy Health 双端一致 | Run `20260813_013549` Host/Client 均记录 `100->75->50->25->0`；通过 |

当前结论：M2 核心目标选择、友军保护和权威复制验收通过。`M2-VAL-01/02` 仍保留为独立边界证据项，不能由正常命中路径代替。

## 4. 技术取舍

| 方案 | 优点 | 问题 | 当前选择 |
|---|---|---|---|
| 只用 GameplayTag 表示队伍 | 与 GAS 查询直接结合 | 松散 Tag 不是清晰的游戏规则权威 | 保留为查询与纵深校验 |
| 复制 TeamId + 团队接口 | 身份清晰，可供技能、AI、UI 共用 | 多一层解析代码 | 作为权威规则来源 |
| 自动选择最近敌人 | 测试方便 | 不是玩家意图，不能复用到射击/SSR | 仅 M0 使用，M2 已移除 |
| ActorArray TargetData | 实现简单 | 不携带命中位置和轨迹证据 | 替换为 SingleTargetHit |
| SingleTargetHit | 能保留命中位置，便于后续服务器验证/回溯 | 网络数据略多 | 当前正式目标格式 |

## 5. 当前不足

- 当前仍是单次准星 Sphere Sweep，不是完整武器/投射物系统。
- 服务器尚未验证客户端视角时间戳、射击方向夹角或 ShotId；这些属于 M6/M7。
- TeamId 目前固定为玩家 1、敌人 2，尚未接 Lobby 分队或动态阵营。
- 治疗目前仍是自疗；合法队友治疗目标将在合作技能扩展阶段补充。
