# 阶段 3～4：胜利 UI 与 GAS 数值链证据报告

> 分支：`coop-GAS`
> 日期：2026-08-15
> 证据边界：本报告记录代码、资产配置、编译、自动化和 Headless 双进程回归。尚未执行正式双窗口点击与肉眼 UI 验收，不能把 Headless 结果描述为视觉验收通过。

## 1. 本轮交付

### 1.1 阶段 3：胜利 UI 生命周期闭环

- `UmultiplayerVictoryPresenterComponent` 只在本地控制角色上监听 `GameState.OnGameWon`。
- Presenter 不创建 Widget、不按字符串查找 Designer 按钮；它只调用 Character 的 `BlueprintImplementableEvent ReceiveCoopGameWon`（节点显示名 `On Coop Game Won`）。
- `bVictoryNotified` 以 GameState 为生命周期门禁；同一局 Controller/PlayerState 重绑不会重复触发，新 GameState 可重新开始本地表现生命周期。
- Character Blueprint 负责创建/保存 `winandquit`、显示鼠标、设置 Game and UI 输入模式；Widget 中文按钮名保持不变。
- `重新开始.OnClicked` 通过 `Get Owning Player Pawn -> Request Restart Coop Game` 提交意图；蓝图可禁用按钮防止重复点击，但不得直接 `OpenLevel`。
- 重开请求仍沿用拥有者 RPC：`Character -> ServerRestartCoopGame -> GameMode::RestartCoopGame -> ServerTravel`。GameMode 用 `bRestartTravelRequested` 合并两个客户端可能同时到达的请求；ServerTravel 启动失败会解除门禁并记录 `COOP_RESTART`，允许后续重试。Widget 不直接决定 Travel。
- 旧的 `ConfigureVictoryUI.py`、`VictoryWidgetClass` 和按名称查找按钮方案已删除，避免本地化 Designer 名称与 C++ 运行时耦合。
- 结构化日志使用 `COOP_VICTORY_UI Phase=Bound/BlueprintEvent/BlueprintEventIgnored`，用于区分绑定、首次转发和重复抑制。

### 1.2 阶段 4：复制属性、捕获策略与权威 ExecCalc

`UmultiplayerAttributeSet` 新增并复制五项战斗属性：

| 属性 | 默认值 | Clamp | 捕获语义 |
|---|---:|---:|---|
| `AttackPower` | 0 | `>= 0` | Source，Snapshot |
| `Armor` | 0 | `>= 0` | Target，Live |
| `CriticalChance` | 0 | `[0, 1]` | Source，Snapshot |
| `CriticalMultiplier` | 1.5 | `>= 1` | Source，Snapshot |
| `Resistance` | 0 | `[0, 0.8]` | Target，Live |
| `Health/MaxHealth` | 100/100 | 原有规则 | Target，Live |

所有五项属性使用 `ReplicatedUsing`、`REPNOTIFY_Always` 和对应 `GAMEPLAYATTRIBUTE_REPNOTIFY`。战斗属性 Clamp 对 NaN/Inf 使用显式安全默认值，避免非有限输入进入聚合器；`PreAttributeChange` 有专项单测。`UmultiplayerInitStatsEffect` 从 4 个 Override Modifier 扩展为 9 个，默认值保持旧伤害行为不变。

服务器 `UmultiplayerDamageExecution` 的计算链为：

```text
Raw = max(BaseDamage + AttackPower, 0)
ArmorMultiplier = 100 / (100 + max(Armor, 0))
ResistanceMultiplier = 1 - clamp(Resistance, 0, 0.8)
VulnerabilityMultiplier = 1 + 0.1 * clamp(Stacks, 0, 3)
Critical = LivingTarget AND (Health <= 50% MaxHealth OR ServerRoll < clamp(CriticalChance, 0, 1))
Final = Raw * ArmorMultiplier * ResistanceMultiplier
        * VulnerabilityMultiplier * (Critical ? max(CriticalMultiplier, 1) : 1)
```

客户端不提交暴击结果或 Roll。真实 Execution 只在服务器结算链中调用 `FMath::FRand()`；纯函数 `CalculateDamage(...)` 接收确定性 Roll，专供公式自动化。Execution 将 Critical、HitType、ImpactImpulse 写入已有自定义 `FmultiplayerGameplayEffectContext`，最终只输出 `IncomingDamage`。

Source 进攻属性在 Spec 创建时 Snapshot，保证一次施法事务使用稳定的施法者快照；Target 的生命、防御和抗性在 Execution 时实时读取，避免命中落地前目标状态变化却仍按旧防御结算。Vulnerability 层数继续查询目标当前 Active GE。

## 2. 验证结果

| 层级 | 结果 | 说明 |
|---|---|---|
| Blueprint 资产接线 | 待执行 | 正式 Character 的 `On Coop Game Won` 与 `winandquit.重新开始` 图表需在安全关闭当前 Editor 后接线、Compile/Save |
| UE5.5 Development Editor | 待本轮重编 | 运行中的 Editor 正占用 DLL；未强制关闭，避免丢失另一个项目的未保存内容 |
| UE5.5 Development Game | 通过 | 新蓝图接口、重复门禁、旧 GameState UI 入口删除及自动化代码均完成 UHT、编译和链接 |
| `multiplayer.GAS` 自动化 | 旧阶段 2/2；本轮待运行 | 当前只检查原生胜利/重开接口及两个中文按钮存在；蓝图事件图与 OnClicked 接线仍需 Compile 和双窗口证明 |
| M5 `20260815_002532` | 日志清点正常 | 0ms，技术接受路径；M5 工具不是严格行为断言器 |
| M6 `20260815_002809` | 95/95 PASS | 0ms，真实激活拒绝与预测清理回归 |
| M6Intent `20260815_002959` | 52/52 PASS | 0ms，DamageIntent 权威验证回归 |
| M6Intent `20260815_003155` | 52/52 PASS | 双方各 `PktLag=150ms`，配置约 300ms RTT，0% loss |
| M6Intent `20260815_004559` | 52/52 PASS | 最终二进制，0ms；包含 finite clamp 与 restart gate 后的最终回归 |
| 正式双窗口 UI 点击/肉眼 | 待人工 | 需实际触发胜利、点击重开和退出，并观察两端 UI/输入状态 |
| Dedicated/晚加入/Travel 后对象审计 | 待验证 | 本轮未执行 |
| Network Insights | 待采集 | 本轮没有优化前后性能结论 |

自动化覆盖默认 25 点伤害、AttackPower 加法、100 Armor 减半、0.2 Resistance、三层 Vulnerability、低血量确定性暴击、100%/0% 暴击概率、组合公式、Clamp 与非有限输入；同时检查战斗属性 `PreAttributeChange` 的 NaN/Inf 安全值、7 个 Capture Definition 的 Source/Target 与 Snapshot 策略、9 个初始化 Modifier。当前胜利配置检查只证明原生事件/重开接口存在，以及 `winandquit` 保留两个中文按钮；它不能证明正式 Character 已实现事件图或重开按钮已接线，这两项必须由 Blueprint Compile 和双窗口点击验收关闭。

本轮五组核验器输出、原始日志哈希和证据限制归档在[Phase 3-4 network regression evidence](Runs/Phase34/README.md)；最终回归 `20260815_004559` 的原始 `RunInfo/Host/Client` 证据位于本地 `Saved/GASBaseline/20260815_004559`。

## 3. 使用的工具与操作

```powershell
# Editor / Game 编译
& 'E:\program\ue554\UE_5.5\Engine\Build\BatchFiles\Build.bat' `
  multiplayerEditor Win64 Development 'E:\ueprojrct\multiplayer\multiplayer.uproject' -WaitMutex
& 'E:\program\ue554\UE_5.5\Engine\Build\BatchFiles\Build.bat' `
  multiplayer Win64 Development 'E:\ueprojrct\multiplayer\multiplayer.uproject' -WaitMutex

# GAS 自动化
& 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'E:\ueprojrct\multiplayer\multiplayer.uproject' -Unattended -NullRHI -NoSplash `
  '-DDC=InstalledNoZenLocalFallback' '-ExecCmds=Automation RunTests multiplayer.GAS;Quit'

# 双进程技术回归；端口对应本轮 RunInfo
& '.\Scripts\StartGASM5TwoPlayers.ps1' -Stage M5 -PktLagMs 0 -PktLossPercent 0 -AutoSequence -Headless -Port 17810
& '.\Scripts\StartGASM5TwoPlayers.ps1' -Stage M6 -PktLagMs 0 -PktLossPercent 0 -AutoSequence -Headless -Port 17811
& '.\Scripts\StartGASM5TwoPlayers.ps1' -Stage M6Intent -PktLagMs 0 -PktLossPercent 0 -AutoSequence -Headless -Port 17812
& '.\Scripts\StartGASM5TwoPlayers.ps1' -Stage M6Intent -PktLagMs 150 -PktLossPercent 0 -AutoSequence -Headless -Port 17813
& '.\Scripts\StartGASM5TwoPlayers.ps1' -Stage M6Intent -PktLagMs 0 -PktLossPercent 0 -AutoSequence -Headless -Port 17814
& '.\Scripts\VerifyGASM5Logs.ps1' -RunId 20260815_002532
& '.\Scripts\VerifyGASM6Logs.ps1' -RunId 20260815_002809
& '.\Scripts\VerifyGASM6IntentLogs.ps1' -RunId 20260815_002959
& '.\Scripts\VerifyGASM6IntentLogs.ps1' -RunId 20260815_003155
& '.\Scripts\VerifyGASM6IntentLogs.ps1' -RunId 20260815_004559
```

工具回答的问题：

- Unreal Python/Editor Asset Library：历史上曾用于确认 CDO 的 Widget Class；最终方案已取消该配置脚本，避免自动保存覆盖运行中 Editor 的资产。
- Automation Framework：确定公式、Clamp、Capture Source/Snapshot、Init GE 和资产引用是否满足配置契约。
- Host/Client 分离日志与核验脚本：确认新 ExecCalc 日志没有破坏 M5/M6/M6Intent 的既有行为和解析协议。
- Headless 双进程：验证网络事务与日志断言；它不能回答按钮是否可见、焦点是否舒服或鼠标是否能被真人正确点击。
- 后续人工验收应使用两个 900×650 可见窗口，观察 `COOP_VICTORY_UI` 日志，并用 `showdebug abilitysystem` 辅助核对 ASC 状态。

## 4. 真实问题复盘

### UI-001：胜利状态成立但正式 UI/重开链未闭合

#### 1. 现象与复现条件

最初代码审查发现胜利状态可以复制，但 `winandquit` 没有接入重开意图。第一版曾把 Widget Class 和中文 Designer 按钮名配置给 C++ Presenter；后续审查确认该方案让 C++ 依赖本地化名称，也让 UI 布局、鼠标和输入表现越过蓝图边界。最终要求是两端本地蓝图各创建一次 UI，Client 重开请求仍由服务器统一 Travel。

#### 2. 为什么难

C++ 胜利状态、复制委托和重开 RPC 已存在，但最后一段依赖二进制 Blueprint 图表；“代码编译通过”不能证明事件、中文按钮、焦点和鼠标链有效。UI 又是本地对象，不能把服务器规则和本地输入恢复混在一起。

#### 3. 初始假设与定位

| 假设 | 为什么怀疑 | 验证 | 结果 |
|---|---|---|---|
| `GameState.OnGameWon` 没有复制到客户端 | 胜利后 UI 不出现 | 静态检查 RepNotify/委托链 | 排除；复制通知路径存在 |
| Presenter 没有挂到 Character | 没有本地消费者 | 检查 Character 默认组件与自动化 CDO | 排除；组件存在 |
| 蓝图胜利事件/按钮意图未配置 | 权威状态已完成，但本地表现无消费者 | 检查正式 Character 与 Widget 事件图 | 确认；待接线 |

使用 `rg` 和 C++ 调用链检查回答“重开权限是否仍经服务器”；使用 UHT/Game Build 回答接口是否可编译；关闭 Editor 后再用 Blueprint Compile、自动化与双窗口回答“图表是否真正保存并可点击”。

#### 4. 根因、候选方案与取舍

根因是表现层资产没有完成最后的本地事件与按钮意图接入；第一版修复又把 Widget 和本地化按钮名称放进了 C++ Presenter，造成不必要的表现层耦合。

| 方案 | 成本 | 取舍 |
|---|---|---|
| Widget 蓝图直接 `OpenLevel` | 接线最短 | 放弃；Client 会绕过服务器统一重开 |
| Presenter 按按钮名绑定 Character 公开意图 | 可集中日志 | 放弃；C++ 与中文 Designer 名称及具体 Widget 耦合 |
| C++ 提供胜利事件与重开意图，蓝图拥有表现 | 需要两处明确接线 | 采用；权威链唯一，中文按钮和 UI 生命周期留在蓝图 |

#### 5. 最终修改与边界保护

```text
GameState replicated win
-> local VictoryPresenter
-> Character.On Coop Game Won once
-> Character Blueprint creates/stores winandquit
-> winandquit.重新开始 OnClicked
-> Character.RequestRestartCoopGame
-> owning Client Server RPC
-> GameMode bRestartTravelRequested idempotency gate
-> ServerTravel; failure clears gate and logs COOP_RESTART
```

保护包括 `IsLocallyControlled`、按 GameState 的 `bVictoryNotified`、`AddUniqueDynamic/RemoveDynamic`、GameMode 的 `bRestartTravelRequested` 以及 Blueprint 保存的 Widget 引用。按钮禁用、`RemoveFromParent` 和输入/鼠标恢复属于蓝图验收项，不能在接线前写成已通过。

#### 6. 验证与遗留

新接口的 Game Target 编译通过；此前 M5/M6/M6Intent 证据仍有效。Editor Target、正式蓝图 Compile/Save、两窗口胜利触发、真人点击重开/退出、焦点和鼠标体验均需在安全关闭 Editor 后重验，不能写成已通过。

可复用经验：网络 UI 必须同时处理“权威结果来源、本地创建幂等、操作意图回到服务器、Travel/EndPlay 对称清理”四条链。

### TEST-001：Resistance Clamp 自动化首次失败

#### 1. 现象与复现条件

新增公式测试首次运行时，`ResistanceMultiplier` 预期 0.2 的断言失败；其他公式测试和编译正常。失败稳定发生在默认近似比较上，实际差异来自二进制浮点表示，不是公式输出偏离设计。

#### 2. 为什么难与初始假设

初始同时考虑了三种可能：Resistance Clamp 未生效、计算次序错误、浮点容差过严。通过单独核对 `1 - clamp(2, 0, 0.8)` 的中间值和其他组合公式，排除 Clamp 与次序问题，确认是断言容差。

#### 3. 工具、根因与候选方案

Automation Framework 的失败详情回答“哪一个数值契约失败”；纯函数中间结果避免为了测公式构造 World/ASC。候选方案是改公式凑成精确 0.2、放宽全局容差、只为该断言设置显式容差。采用第三项，将容差写为 `0.0001f`，既保留公式又限制误差范围。

#### 4. 验证、效果与遗留

修正后 `multiplayer.GAS` 2/2 PASS，随后 Editor/Game 和网络行为核验通过；追加 finite clamp/restart gate 后的最终二进制又以 `20260815_004559` 取得 52/52。该修复只证明公式契约，不能证明具体装备或 UI 配置。可复用经验：浮点业务断言要定义领域容差，不能把表示误差误诊为玩法根因，也不能用过宽容差掩盖回归。

## 5. 代码审查风险

### GAS-RISK-001：扩展 ExecCalc 日志可能破坏旧核验器

这是代码审查发现的兼容风险，不是已发生的线上 Bug。M6Intent 核验器用正则读取：

```text
GAS_DAMAGE_EXEC Base=... Health=... Vulnerability=... Critical=... Final=...
```

若为了新属性重排或重命名前六个字段，即使 Gameplay 行为正确，旧证据工具也会失败。候选方案包括同步重写全部核验器、另加一条 Detail 日志、保留旧前缀后追加字段。本轮采用第三项：保持旧六字段前缀与顺序，后追加 AttackPower、Armor、CriticalChance、CriticalMultiplier、Resistance、Roll 和各中间乘数。旧 0/300ms 证据与最终二进制 `20260815_004559` 均通过，说明兼容契约未破坏。

遗留：文本正则仍比结构化 Trace/Event 脆弱；进入 Network Insights 阶段可把事务字段迁入 Trace Channel，但在证据链迁移完成前必须保留旧日志协议。

## 6. 尚未关闭的门禁

1. 可见双窗口完成“四钥匙 + 两人 WinArea -> 两端 UI -> Client 重开 -> 两端同图 -> 退出”的人工验收并保存日志/录屏。
2. Dedicated Server + 两个客户端验证胜利、属性、GE、Tag 和重开链。
3. 胜利后晚加入、Travel 后旧 Widget/Delegate/Timer/Task/Object List 残留检查。
4. 0/150/300ms 每方向、5% loss 多轮矩阵；本轮只有 0ms 与双方各 150ms 的 M6Intent 回归。
5. Network Insights 同条件前后采样；在采集前不宣称带宽或帧时间已经优化。
