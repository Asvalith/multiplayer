# GAS M0 双窗口基线验收报告

> 分支：`coop-GAS`
>
> 基线提交：`dc3969f`
>
> 测试地图：`/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`
>
> 测试脚本：`TestTwoPlayers.bat`
>
> 当前状态：2026-08-13 Editor/Game 编译和配置自动化预检通过；`Team.Enemy` 训练目标的生成、服务器伤害结算、双端属性复制及 M0 可见反馈均已通过双窗口验收。其余冷却、消耗、超距和遮挡边界仍按矩阵逐项验证。

本报告只记录真实执行结果。没有执行的条目保持“待验证”，失败项保留日志和复现步骤后再修改代码。

---

## 1. 固定环境

| 项目 | 值 |
|---|---|
| Unreal Engine | 5.5 |
| 网络模式 | Player 1 Listen Server + Player 2 Client |
| 窗口 | 900×650，左右排列 |
| 端口 | 17777 |
| 初始网络条件 | 0ms，不注入丢包 |
| 正式网络条件 | M6 阶段再测试 150/300ms 和丢包；M0 不混入弱网变量 |
| 默认 PlayerState | `AmultiplayerGASPlayerState`，仍需从运行日志/调试界面确认地图蓝图没有覆盖 |
| 测试输入 | `1` 输出网络/GAS 状态、`4` 对敌伤害、`5` 自我治疗、`6` 状态免疫、`7` 生成/重置敌对训练目标 |

每次运行由脚本创建：

```text
Saved/GASBaseline/<RunId>/
├─ RunInfo.txt
├─ Host.log
└─ Client.log
```

在下面填写实际 `RunId`：

```text
RunId：20260813_004026（当前 Target Enemy 验收）；20260813_001858 为修改前初始化复测
测试人：Codex 启动与日志预检；玩法输入待用户执行
测试日期：2026-08-13
对应提交：dc3969f + 当前 M0 工作树修改
```

---

## 2. 发布前预检

| 编号 | 检查 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| PRE-01 | Editor Development 编译 | 成功，无新增错误 | 2026-08-13 重新编译成功 | 通过 |
| PRE-02 | Win64 Development Game 编译 | 成功，无新增错误 | 2026-08-13 重新编译成功 | 通过 |
| PRE-03 | `multiplayer.GAS.Configuration` | 找到 1 项并成功 | 找到 1 项；`Result={成功}` | 通过 |
| PRE-04 | Host/Client 启动 | 两个窗口进入同一地图 | Host `Join succeeded`；Client `TravelCompleted`；两个窗口仍在运行 | 通过 |
| PRE-05 | PlayerState 类型 | 两端按 `1` 后日志均显示 `multiplayerGASPlayerState` | Host/Client `GAS_INIT` 均显示正确 PlayerState、Owner 和 Avatar | 通过 |
| PRE-06 | 能力授予 | 两端按 `1` 后日志均显示 `Abilities=3` | Server 为两个 PlayerState 各授予 3 个；Client 按 `1` 的复制确认待执行 | 部分通过 |

---

## 3. 0ms 双窗口功能矩阵

### 3.1 连接和所有权

| 编号 | 操作 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| NET-01 | 启动脚本 | Host 创建 Listen Server，Client 连接 127.0.0.1:17777 | Run `20260813_001858` 连接成功 | 通过 |
| NET-02 | 分别移动两个角色 | 每个窗口只控制自己的 Pawn | 待填写 | 待验证 |
| NET-03 | 检查初始化日志并在两端按 `1` | Host/Client 的 Authority、AutonomousProxy、SimulatedProxy 符合预期 | 初始化日志已显示 Authority/AutonomousProxy/SimulatedProxy；按键输出待执行 | 部分通过 |
| NET-04 | 检查 `GAS_INIT/GAS_BASELINE` 日志 | OwnerActor=PlayerState，AvatarActor=当前 Character，Abilities=3 | Owner/Avatar 已通过初始化日志确认；Client Abilities 数量待按 `1` | 部分通过 |

### 3.2 友军保护与伤害能力

| 编号 | 操作 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| TEAM-01 | 两名玩家靠近，任一玩家按 `4` | 玩家均为 `Team.Player`；客户端不选择队友，服务器也拒绝玩家目标；双方 Health 不变 | 待重新运行 | 待验证 |
| DMG-00 | 玩家按 `7` | 服务器在前方生成/重置 `Team.Enemy` GAS 方块；双端出现 `GAS_TARGET_READY/RESET` | Run `20260813_004026`：Host 生成 Authority 目标，Client 收到 SimulatedProxy；Health=100，Team=Enemy | 通过 |
| DMG-01 | Client 对 600 内训练目标按 `4` | 服务器对敌对目标结算 25 伤害；最终 Health 一致 | 服务器记录 `100->75->50`；Client 复制到相同数值 | 通过 |
| DMG-02 | 1 秒内快速重复按 `4` | Cooldown 阻止重复有效结算 | 待重新运行 | 待验证 |
| DMG-03 | Energy 不足时按 `4` | `CommitAbility` 失败；无伤害、无额外 Cost/Cooldown | 待重新运行 | 待验证 |
| DMG-04 | 敌对目标距离超过 650 后按 `4` | 服务器拒绝；目标 Health 不变 | 待重新运行 | 待验证 |
| DMG-05 | 玩家与敌对目标之间有阻挡 Visibility 的墙时按 `4` | 服务器拒绝；目标 Health 不变 | 待重新运行 | 待验证 |
| DMG-06 | Host 对敌对目标按 `4` | 与 Client 发起时遵循相同权威结算规则 | 服务器记录 `100->75->50->25->0`，Client 同步收到完整序列；按 `7` 后双端回到 100 | 通过 |

### 3.3 治疗能力

| 编号 | 操作 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| HEAL-01 | 受伤后按 `5` | 服务器治疗自身 30，最多到 MaxHealth | 待填写 | 待验证 |
| HEAL-02 | 满血时按 `5` | Health 不超过 MaxHealth；Cost/Cooldown 行为与当前设计一致 | 待填写 | 待验证 |
| HEAL-03 | Energy 不足时按 `5` | 无治疗结果 | 待填写 | 待验证 |
| HEAL-04 | 3 秒内重复按 `5` | Cooldown 阻止重复有效治疗 | 待填写 | 待验证 |

### 3.4 免疫能力

| 编号 | 操作 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| IMM-01 | 玩家按 `6` 后受到敌人负面伤害 GE | 负面 GE 被免疫规则阻止 | 需要敌对伤害来源 | 阻塞 |
| IMM-02 | 等待 5 秒后再次受到敌人伤害 | 免疫到期，正常扣除 Health | 需要敌对伤害来源 | 阻塞 |
| IMM-03 | 8 秒内重复按 `6` | Cooldown 阻止重复有效施加 | 待填写 | 待验证 |
| IMM-04 | Energy 不足时按 `6` | 不产生有效免疫 | 待填写 | 待验证 |

### 3.5 生命周期和一致性观察

| 编号 | 操作 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| LIFE-01 | 连续施放三个能力 | 没有重复能力授予、重复属性委托或残留 AbilityTask 警告 | 待填写 | 待验证 |
| LIFE-02 | 关闭 Client | Host 不崩溃，不访问失效客户端 Pawn | 待填写 | 待验证 |
| LIFE-03 | 退出两窗口 | Host.log/Client.log 完整落盘 | 待填写 | 待验证 |

死亡/复活尚未实现，因此本阶段不把死亡生命周期列为通过项；相关工作属于 M3。

---

## 4. 日志摘录

只摘录能支持结论的行，并保留原始 Host/Client 日志。

### 4.1 启动与授予

```text
待填写
```

### 4.2 伤害接受

```text
Host.log: GAS_TARGET_HEALTH ... Old=100.0 New=75.0
Host.log: Server applied 25.0 damage: BP_ThirdPersonCharacter_C_0 -> multiplayerGASTargetDummy_0
Host.log: GAS_TARGET_HEALTH ... Old=25.0 New=0.0
Client.log: GAS_TARGET_HEALTH ... Old=100.0 New=75.0
Client.log: GAS_TARGET_HEALTH ... Old=25.0 New=0.0
Host.log: GAS_TARGET_RESET ... Health=100.0
Client.log: GAS_TARGET_HEALTH ... Old=0.0 New=100.0
```

### 4.3 超距或遮挡拒绝

```text
当前代码未输出结构化 RejectReason；若只能从 Health 未变化推断，必须标记证据不足。M6 将补结构化拒绝日志。
```

### 4.4 治疗和免疫

```text
待填写
```

---

## 5. 问题记录

发现问题时使用新编号，不在此处提前编造根因。

| 编号 | 现象 | 最小复现 | Host/Client | 日志位置 | 当前结论 |
|---|---|---|---|---|---|
| GAS-M0-001 | 同一服务器角色连续输出两次 `GAS_INIT` | 启动 Listen Server；观察首次 Run `20260813_001547` Host.log | Host | `Saved/GASBaseline/20260813_001547/Host.log` | 已定位并修复 |
| GAS-DESIGN-001 | Damage Ability 自动选择另一名玩家，与双人合作定位冲突 | 两名玩家进入地图后按 `4`；检查 AbilityTask `FindNearestPlayerTarget` 和服务器验证 | 双端/代码 | 原实现源码与用户评审 | 已修正代码；待双端运行验收 |
| GAS-M0-002 | 按 `4` 后画面没有反应，但日志显示伤害已成功结算和复制 | Run `20260813_004026` 先按 `7`，再按 `4` | 双端 | 原始复现：`20260813_004026`；修复验收：`20260813_005114` | 已修复并通过双窗口人工验收 |

`GAS-M0-001` 根因与修改：`PossessedBy` 和 `NotifyControllerChanged` 都可能调用 `InitializeAbilitySystem()`；能力授予已有 `bStartupAbilitiesGranted`，但 ActorInfo 初始化和 `OnAbilitySystemInitialized` 广播没有同等级幂等门禁。当前修改比较 ASC、AttributeSet、OwnerActor 和 AvatarActor；相同组合只输出 `GAS_INIT_SKIPPED`，不重复广播。Run `20260813_001858` 已在 Host 上验证每个角色只有一次 `GAS_INIT`，后续重复入口为 `GAS_INIT_SKIPPED`。完整双端 UI 绑定回归属于 M1。

`GAS-DESIGN-001` 根因与修改：第一版为了最小化 TargetData 网络验证，AbilityTask 和服务器验证都把 `AmultiplayerCharacter` 当作目标类型，技术测试边界错误地进入了玩法规则。当前加入 `Team.Player`/`Team.Enemy` 标签：玩家 PlayerState 持有 `Team.Player`，本地选择和服务器验证都只接受 `Team.Enemy` 且拒绝 `Team.Player`。按 `7` 可由服务器生成/重置一个零资产敌对 GAS 方块，用于 M0 伤害验证；不能再用玩家互伤做验收替代。

`GAS-M0-002` 根因与修改：伤害 GameplayEffect、服务器权威结算和 Health 属性复制均已生效，但 M0 训练目标只有静态网格和日志，没有 HUD、GameplayCue 或受击材质，因此“代码正确”没有转化为玩家可观察反馈。当前给训练目标加入轻量调试表现：Health 变化时双端显示血量文本，方块高度随剩余血量缩小，归零后隐藏并关闭碰撞，按 `7` 重置后恢复。该实现用于 M0 可测试性，不替代 M1 正式 HUD 和 M5 GameplayCue。Editor/Game 编译和配置自动化测试均通过；Run `20260813_005114` 的服务器与客户端日志记录了 `100->75->50->25->0` 的一致序列，用户已确认可见反馈符合预期。

本轮开发记录：新增状态输出第一次编译时把 UE5.5 的 `GetActivatableAbilities()` 返回值当成旧容器访问 `.Items`，编译器在 `multiplayerCharacter.cpp` 明确报错。根据本地 UE5.5 声明改为直接 `.Num()` 后，Editor/Game 两目标均通过。该问题只发生在本轮诊断代码编写过程中，不是原 GAS 玩法运行问题，因此不扩写为核心问题复盘。

每个真实问题后续按以下顺序补充：

```text
现象 -> 复现条件 -> 初始假设 -> 工具与证据
-> 根因 -> 候选方案 -> 修改 -> 分层验证 -> 遗留问题
```

---

## 6. M0 退出结论

只有同时满足以下条件才能把 M0 标为完成：

- PRE-01～PRE-06 有实际结果。
- NET-01～NET-04 通过。
- 玩家友军保护通过；DMG 使用 `Team.Enemy` 目标，HEAL/IMM 使用合作语义，正常路径和服务器拒绝路径均有结果。
- Host/Client 日志按 RunId 保存。
- 所有失败项有最小复现步骤，不使用猜测根因。

当前结论：**M0 尚未完成，等待双窗口人工执行。**
