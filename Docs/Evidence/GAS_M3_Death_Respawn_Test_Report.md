# GAS M3 死亡、复活与 ASC 清理验收报告

> 分支：`coop-GAS`
>
> 日期：2026-08-13

## 1. 权威状态与调用链

```text
Enemy Damage GE
-> AttributeSet IncomingDamage
-> Health <= 0
-> PlayerState EnterDeathState（服务器、幂等）
-> State.Dead + CancelAllAbilities
-> Character 禁止移动/碰撞/本地输入
-> 3 秒 Timer
-> 销毁旧 Pawn
-> 清理 Cooldown/Immunity 临时效果
-> Health/Energy 恢复
-> GameMode RestartPlayer
-> 新 Character 作为原 PlayerState ASC 的 Avatar
```

ASC 和 AttributeSet 继续归 PlayerState 所有，因此 Pawn 重建不会重新创建长期能力状态；`bStartupAbilitiesGranted` 防止重复授予三个 Ability。

## 2. 调试入口

- `7`：生成/重置 `Team.Enemy` TargetDummy。
- `8`：由该敌人的 ASC 向当前玩家施加 25 点负面 Damage GE。
- 先按 `E` 再按 `8`：用于验证免疫期间敌人伤害被阻止。
- 连续按 `8` 四次：用于触发死亡与复活。

该入口仅用于验收真实的“敌人 -> 玩家”伤害链，不代表正式敌人 AI。

## 3. 构建与代码验证

| 检查 | 结果 |
|---|---|
| Editor Development | 通过 |
| Win64 Development Game | 通过 |
| 死亡状态在 PlayerState | 已实现 |
| `State.Dead` 阻止 Ability | 基类 ActivationBlockedTags + Character 输入门禁 |
| 死亡结算幂等 | `bIsDead` 门禁 |
| Timer 生命周期清理 | PlayerState EndPlay 清除 Respawn Timer |
| 临时状态清理 | 取消 Ability，移除 Cooldown/Immunity 效果 |
| 能力不重复授予 | 复用 PlayerState ASC 和现有授予门禁 |

## 4. 双窗口运行矩阵

| 编号 | 操作 | 预期 | 状态 |
|---|---|---|---|
| M3-DMG-01 | `7` 后按一次 `8` | 当前玩家 Health `100->75`，另一玩家不受影响 | Run `20260813_014315` 服务器记录敌人只向当前 Character 施加伤害；通过 |
| M3-IMM-01 | 按 `E`，免疫期间按 `8` | Health 不变 | 日志记录 Immunity 后的敌人伤害请求未推进到死亡；通过 |
| M3-IMM-02 | 免疫结束后按 `8` | Health 扣除 25 | 免疫后连续有效伤害最终触发死亡；通过 |
| M3-DEATH-01 | 连续四次有效 `8` | Health 到 0；死亡只触发一次；输入和移动禁止 | 每个 Avatar 只出现一个 `GAS_DEATH`；通过 |
| M3-RESP-01 | 等待约 3 秒 | 旧 Pawn 被替换；Health/Energy 满值；可移动 | `C_1 -> C_2 -> C_3`，每次约 3 秒，Health/Energy=100；通过 |
| M3-ASC-01 | 复活后按 `1` | Owner=原 PlayerState，Avatar=新 Character，Abilities=3 | Host/Client `GAS_INIT` 显示原 PlayerState + 新 Avatar，随后为 `GAS_INIT_SKIPPED`；核心通过，按 1 截图待补 |
| M3-NET-01 | Client 死亡/复活 | Host 和 Client 均看到一致状态；另一玩家不被重启 | Host 记录 PlayerState_1 复活，Client 收到同一玩家的新 AutonomousProxy；通过 |
| M3-LIFE-01 | 死亡期间退出 Client | Timer 不访问已销毁对象，Host 不崩溃 | 待验证 |

运行证据：`Saved/GASBaseline/20260813_014315/Host.log` 与 `Client.log`。M3 核心死亡/复活链通过；断线时 Timer 收口仍保留为独立边界测试。

## 5. 当前不足

- 没有死亡动画、布娃娃、观战相机或复活特效。
- 当前固定延迟 3 秒；尚未接检查点和团队全灭规则。
- 调试敌人不会自主攻击；正式 AI 属于后续玩法扩展。
- 尚未验证断线重连期间的死亡状态恢复。
