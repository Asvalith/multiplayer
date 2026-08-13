# GAS / Co-op 网络问题调查与优化记录模板

> 用途：M6 以后每个核心网络、GAS、生命周期或性能问题都按本模板记录。
> 目标不是增加篇幅，而是让代码、工具、操作、证据与结论能够被复现。

## 0. 真实性分类

开始前必须选择一种，不能混写：

- **问题复盘**：玩家、日志或运行测试中真实发生并可复现。
- **代码审查风险**：源码中发现风险，但尚未在运行中触发。
- **场景题**：面试条件变化后的推演，不属于项目经历。
- **优化候选**：尚未实施，或没有前后数据支撑。

没有运行过的步骤统一写“待验证”；编译通过不能写成玩法通过；单次样本不能
写成稳定性或错误率结论。

---

## 1. 编号与标题

```text
编号：GAS/NET/LIFE/PERF-XXX
类型：问题复盘 / 代码审查风险 / 场景题 / 优化候选
首次发现：YYYY-MM-DD
影响版本与分支：
负责人：
当前状态：定位中 / 已修复待验 / 部分通过 / 已关闭
```

## 2. 需求与正确性规则

- 玩家想完成什么。
- 哪一端拥有最终决定权。
- 哪些反馈允许预测，哪些结果只能由服务器确认。
- 必须保持的幂等、团队、资源、死亡和生命周期规则。

## 3. 现象

- 发生在什么输入或网络事件后。
- 正常预期与实际结果分别是什么。
- 是否稳定复现，频率是多少。
- 影响正确性、网络一致性、性能、体验还是维护性。

禁止只写“有 Bug”“网络不稳定”或“UE 有问题”。

## 4. 最小复现操作

逐步写出实际操作，不省略环境：

1. 使用的地图、GameMode、角色、能力和目标。
2. Listen Server / Client / Dedicated Server 数量。
3. 输入顺序及等待条件。
4. `PktLag`、`PktLoss`、帧率限制等网络/性能参数。
5. 预期结果。
6. 实际结果和 RunId。

示例：

```powershell
.\Scripts\StartGASM5TwoPlayers.ps1 `
  -Stage M6Intent -AutoSequence -Headless `
  -PktLagMs 150 -PktLossPercent 0 -Port 17782
```

注意：两端都设置 `PktLag=150` 表示每方向约 150ms、RTT 约 300ms。

## 5. 为什么难

只记录真正的技术难点，例如：

- Activation PredictionKey、TargetData 依赖 Key 与业务 ShotId 是不同身份。
- Client 本地预测与 Server 权威结算位于两个进程、两条时序链。
- GE、Cue、属性复制与 Result RPC 可能按不同机制收敛。
- ASC 在 PlayerState，Avatar 在 Character，死亡重生会更换 Avatar。
- 客户端查询结果正确不等于服务器可以信任其 Actor、HitResult 或伤害值。
- 平均帧率掩盖 P95/P99 尖峰或少量网络错误。

## 6. 初始假设与排除过程

| 假设 | 为什么怀疑 | 使用工具 | 具体操作 | 证据/结果 | 结论 |
|---|---|---|---|---|---|
| 例：服务器重复应用 GE | 生命下降两次 | 结构化 `UE_LOG` | 按 ShotId 统计 `Committed` 和 `GAS_DAMAGE_EXEC` | 同一事务各一条 | 排除 |

至少保留一个被证据排除的假设；不能把最终答案包装成一开始就知道。

## 7. 工具与具体操作记录

每个工具必须说明“它回答了什么问题”。

### 7.1 Visual Studio / Rider

- 断点文件、函数与条件：
- 条件断点表达式：
- 调用堆栈中关键帧：
- Watch 的变量：`SpecHandle`、`PredictionKey`、`ShotId`、Role、TagCount、GE Handle。
- 工具回答的问题：

### 7.2 Unreal 日志与命令

```text
-LogCmds="LogMultiplayerGAS VeryVerbose,LogGameplayCues VeryVerbose,LogAbilitySystem Verbose,LogNet Log"
```

- 新增的日志分类与字段：
- 用于关联的事务身份：
- `rg` / `Select-String` 的精确命令：
- 工具回答的问题：

### 7.3 Blueprint 调试

- Blueprint Breakpoint 节点：
- Watch Value：
- Execution Trace 路径：
- 蓝图编译结果：
- 工具回答的问题：

### 7.4 碰撞与导航

- `show collision`、Debug Draw、Collision Analyzer 的打开方式和过滤条件。
- Trace Channel、Object Type、Response、Generate Overlap Events 的实际配置。
- `show Navigation` 与 NavMesh 覆盖结果（仅 AI/移动相关）。
- 工具回答的问题：命中查询错、过滤规则错，还是权威结算错。

### 7.5 自动化与双进程测试

```powershell
& 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'E:\ueprojrct\multiplayer\multiplayer.uproject' `
  -unattended -nop4 -nosplash -NullRHI `
  '-ExecCmds=Automation RunTests <TestName>;Quit' `
  '-TestExit=Automation Test Queue Empty' -log
```

- 测试名、退出码、通过数：
- 双进程 RunId：
- 后处理器命令、断言数、退出码：
- 工具回答的问题：静态配置、纯函数边界，还是实际网络时序。

### 7.6 性能工具（仅性能问题填写）

- `stat unit` / `stat game` / `stat physics`：确定瓶颈线程。
- CSV Profiler：固定窗口的帧时间分布。
- Unreal Insights：Game/Render/Network trace、事件书签和 RPC/复制成本。
- Network Insights：连接、包、RPC、属性复制的前后对照。
- `ProfileGPU` / RenderDoc：GPU pass 和 draw/texture 问题。
- MemReport / Object List：ASC、GE、Cue、Task、Widget 是否增长。
- 固定场景、机器、对象数、时长和版本：

没有同条件前后数据时，只能写“预计收益”，不能写“性能提升”。

## 8. 根因

根因必须落到具体所有权、时序、配置或成本变量：

```text
所有权：谁能修改最终状态？
事务身份：PredictionKey / ShotId / GE Handle 分别负责什么？
时序：哪个回调先到、哪个延迟回调仍存活？
配置：哪个 Collision Profile、Tag、GameMode 或资产配置参与？
成本：调用次数 × 候选数 × 单次成本，瓶颈在哪个线程？
```

## 9. 候选方案与取舍

| 方案 | 能解决什么 | 新增成本/风险 | 是否采用 | 原因 |
|---|---|---|---|---|

必须说明为什么适合当前双人 Co-op Demo，不声称方案在所有项目中最优。

## 10. 最终修改与调用链

分别记录修改前/后的调用链，并链接源码位置：

```text
修改前：Input -> Client HitResult -> Server 直接消费 -> Damage
修改后：Input -> Client Intent -> TargetData RPC -> Server Validate
       -> Server Trace -> Commit -> ExecCalc -> Attribute -> Cue/Replication
```

列出所有边界保护：状态门禁、ShotId、TSet/序列、Timer 清理、Delegate
解绑、弱引用、死亡/EndPlay 收口、服务器二次校验。

## 11. 验证结果

| 验证层级 | 命令/操作 | 结果 | 证据位置 |
|---|---|---|---|
| C++ Editor Build |  | 待验证/通过/失败 |  |
| Game Build |  |  |  |
| Automation |  |  |  |
| 蓝图编译 |  |  |  |
| 0ms 双进程 |  |  |  |
| 弱网 |  |  |  |
| 丢包/低帧率 |  |  |  |
| 人工视觉/HUD |  |  |  |
| 性能证据 |  |  |  |

记录源文件时间、DLL/EXE 时间和 RunId，避免用旧二进制证明新源码。

## 12. 最终效果

- 正确性：
- 网络一致性：
- 安全边界：
- 架构与可维护性：
- 玩家体验：
- 性能：有数据 / 无数据，仅预期。

## 13. 遗留问题与优化路线

按优先级写清：

| 优先级 | 尚未解决 | 为什么现在不做 | 下一步操作 | 完成证据 |
|---|---|---|---|---|

对本项目通常包括：Dedicated Server + 2 Clients、晚加入、断线/Travel、
多轮 loss、快速移动/急转向容差、专用 Trace Channel、token bucket、异常
请求遥测、服务器历史快照/rewind，以及 Network Insights 前后对照。

## 14. 条件变化场景题

- RTT 从 100ms 改为 400ms，哪些阈值和证据要变？
- 从 Listen Server 改 Dedicated，哪些本地权威分支消失？
- 从静止目标改高速移动目标，当前世界 Trace 为什么会 miss？
- 同时存在多武器/多 Spec，ShotId guard 如何分区？
- Cue 从瞬时反馈改持续状态，拒绝/追平如何清理？

场景题必须写成推演，不能冒充项目实测。

## 15. 可复用原则

用一句话提炼，例如：

- PredictionKey 是预测事务身份，不是反重放业务序号。
- 客户端可以提交意图，不能提交最终命中、伤害和胜负事实。
- Result RPC 只传达判定，不手工篡改权威属性来“修正”预测。
- 优化前先测次数、字节、候选和瓶颈线程；优化后必须同条件复测。
