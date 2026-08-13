# M6 Damage Intent 可提交证据索引

本目录归档 `M6Intent` 双进程验证摘要。`20260813_163052` 与
`20260813_163248` 是最终审查修复重新编译后的正式回归证据；较早的
`20260813_155519` 与 `20260813_155730` 保留为历史诊断运行，不删除，
但已经被两组最终回归取代。

原始 `Host.log`、`Client.log` 与 `RunInfo.txt` 保留在本机
`Saved/GASBaseline/<RunId>/`，不进入 Git。最终两组归档的 Markdown 与
JSON Summary 都是对应 Saved 生成物的逐字节副本；因此克隆仓库后仍可从
JSON 机器复核完整 52 项断言。历史两组维持仅归档 Markdown。

## Run 索引

| RunId | 网络条件 | 结果 | 检查数 | 证据状态 | 归档摘要 |
|---|---|---:|---:|---|---|
| `20260813_163052` | 双向 0ms / 0% loss | PASS | 52/52 | 正式最终证据 | [Markdown](20260813_163052/M6IntentSummary.md) / [JSON](20260813_163052/M6IntentSummary.json) |
| `20260813_163248` | 双向各 150ms / 0% loss，约 300ms RTT | PASS | 52/52 | 正式最终证据 | [Markdown](20260813_163248/M6IntentSummary.md) / [JSON](20260813_163248/M6IntentSummary.json) |
| `20260813_155519` | 双向 0ms / 0% loss | PASS | 52/52 | 历史诊断；已被 `163052` 取代 | [M6IntentSummary.md](20260813_155519/M6IntentSummary.md) |
| `20260813_155730` | 双向各 150ms / 0% loss，约 300ms RTT | PASS | 52/52 | 历史诊断；已被 `163248` 取代 | [M6IntentSummary.md](20260813_155730/M6IntentSummary.md) |

## 复现操作

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\VerifyGASM6IntentLogs.ps1 -RunId 20260813_163052
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\VerifyGASM6IntentLogs.ps1 -RunId 20260813_163248
```

脚本会重新读取本机原始日志、覆盖该 Run 下的 Summary，并在任一断言失败
时返回退出码 `1`。归档 Summary 不能替代原始日志；它使仓库克隆者能够
检查当时采用了哪些断言、得到什么结构化结果和证据边界。

## 最终归档完整性

| RunId | 归档 JSON 解析 | JSON 结果 | 归档 Markdown/JSON 与 Saved 生成物 |
|---|---|---|---|
| `20260813_163052` | PASS | `Passed=true`，`52/52`，`FailedCheckCount=0` | 两者 SHA256 分别相同，均逐字节一致 |
| `20260813_163248` | PASS | `Passed=true`，`52/52`，`FailedCheckCount=0` | 两者 SHA256 分别相同，均逐字节一致 |

历史运行目录中的归档 Markdown 是当时提交的精简可读摘要，不主张与本机
Saved 下的完整 52 项 Markdown 逐字节一致；历史状态和原始生成物哈希仍
在下表保留。

## 正式最终证据 SHA256

| RunId | 文件 | SHA256 |
|---|---|---|
| `20260813_163052` | `RunInfo.txt` | `8802C66C9481DCCC19C14395252E1EE59C59A8EF0E927E144B9DC7CA188495E2` |
| `20260813_163052` | `Host.log` | `C06C86A48E1C4F171E922BC8230EF542C99806880A0D44C71A196B02ACBA57D2` |
| `20260813_163052` | `Client.log` | `17B7B85EAADB682F19D0C6E9D49017CA148759573B5527F0E1442E7208BFEE26` |
| `20260813_163052` | generated and archived `M6IntentSummary.md` | `4425565675E4EB9603CC9C3783E15313AC9DB24ED2C0427D6D04D473A42E8B32` |
| `20260813_163052` | generated and archived `M6IntentSummary.json` | `FA501A7A913B5DC8581F9DA1DD633EAAFDF15F996E7DA7F953C275F4AF10871D` |
| `20260813_163248` | `RunInfo.txt` | `F044CE27ABFF488CC0345970D4BF48AEBD91CC6DA5B9F0373B093B4CC13BD4F8` |
| `20260813_163248` | `Host.log` | `5F4931799CC2DF5ABAC3902435729DE6E219A5322B9EB6F5563450C0C7A61700` |
| `20260813_163248` | `Client.log` | `1DEBE3326C1559E43154F2F4BFD3B111BD6183663DD7FB981B10D51527435B03` |
| `20260813_163248` | generated and archived `M6IntentSummary.md` | `10A60291C68623AA0D37620D18B3DE5DFDAEE8DF4482E81216A69A9564485F39` |
| `20260813_163248` | generated and archived `M6IntentSummary.json` | `393B2308952E41C82229B2838230C0E06899F8092E914A15E5960EA68F341351` |

## 被取代的历史诊断证据 SHA256

| RunId | 文件 | SHA256 |
|---|---|---|
| `20260813_155519` | `RunInfo.txt` | `8C18668F8E8177F0298A5F195194B2A21A08591AD9F177CC48E133E37BE05A7E` |
| `20260813_155519` | `Host.log` | `4B7194ED3DA9B2FD8C5A86614C811580FF7CA99981BEB38E6EC540D51370C8C6` |
| `20260813_155519` | `Client.log` | `A871EB085E7995F64718E97399C679D2BF4B1504352E34FF417D817249B3B9A2` |
| `20260813_155519` | generated `M6IntentSummary.md` | `48FD06204899C2B5A099CD21A77141F817DE7CD2FB8CB08685DB63DFC447791E` |
| `20260813_155519` | generated `M6IntentSummary.json` | `CC7C6F73AF6634CA2B77CF86E3442C01168FCE8D4B4164807DD83474E0E2B588` |
| `20260813_155730` | `RunInfo.txt` | `1E5F958DBB4ED643F1F9F3A0FE9B0B533A59B250225596FC436E41D7138FCBB8` |
| `20260813_155730` | `Host.log` | `B12D7DA542C1372DDBEFED603D065FC113957B1FAEDCB3EBD354D42033BAD717` |
| `20260813_155730` | `Client.log` | `F236927790F38ABA5B1E89D28CE1C1A0E90B0197395043E0AA7C98C839819AC2` |
| `20260813_155730` | generated `M6IntentSummary.md` | `836DA26E6319B67A18977F70EC3A87AFE15E2F7C2A5B76A53CF2F1A8B4DAA3C1` |
| `20260813_155730` | generated `M6IntentSummary.json` | `B5FB3DEA781BBCDB80D2FA5627034C641C405B9AC5A4BE45F42994557552D073` |

## HEAD 与 dirty-worktree 边界

归档时的 Git HEAD 是：

```text
dc3969f457cfd52318442c78355fadf727d5ef1c
```

归档时工作树不是 clean。Damage Intent 相关边界如下；`M` 表示相对 HEAD
已有未提交修改，`??` 表示相对 HEAD 尚未跟踪：

```text
 M Source/multiplayer/AbilitySystem/Abilities/multiplayerGameplayAbility.cpp
 M Source/multiplayer/AbilitySystem/Abilities/multiplayerGameplayAbility.h
 M Source/multiplayer/AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.cpp
 M Source/multiplayer/AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h
 M Source/multiplayer/AbilitySystem/multiplayerAbilitySystemComponent.cpp
 M Source/multiplayer/AbilitySystem/multiplayerAbilitySystemComponent.h
 M Source/multiplayer/Tests/multiplayerGASAutomationTests.cpp
 M Source/multiplayer/multiplayerCharacter.cpp
 M Source/multiplayer/multiplayerCharacter.h
?? Scripts/VerifyGASM6IntentLogs.ps1
?? Source/multiplayer/AbilitySystem/multiplayerGameplayAbilityTargetData.cpp
?? Source/multiplayer/AbilitySystem/multiplayerGameplayAbilityTargetData.h
```

本机最终回归使用的 `UnrealEditor-multiplayer.dll` 在两次 Run 前生成；归档
时其 SHA256 为
`1B5858B77287E80423B1C42CAB1B6692B15C42BCC2609AA0F26C787130980D52`。
但是 RunInfo 没有在进程启动时记录 HEAD、dirty diff/source manifest 或
DLL hash。因此以上 SHA256 能证明现存证据文件未变，不能把运行结果
密码学地归因给 clean HEAD，也不能把当前 dirty 内容误写成 HEAD 已包含。

## 证据边界

- 两组正式最终证据都是 Listen Server + 一个 Client，不是 Dedicated Server。
- 150ms 同时施加到两个方向，因此近似 RTT 为 300ms，不是 150ms RTT。
- 没有注入丢包，也没有形成长期错误率、P95/P99 或 Network Insights 结论。
- 52 项断言证明既定八步矩阵，不覆盖所有 Schema、限流、序号环绕、
  SourceDead、Task 超时/取消、断线或 Travel 分支。
- 语义拒绝是业务 Result，不是 `ClientActivateAbilityFailed`；PredictionKey
  CatchUp 和瞬时预测表现的边界不能由 52 项结果外推成完整视觉回滚。
- `GAS_DAMAGE_EXEC` 与 `GAS_DAMAGE_CONTEXT` 当前不直接携带 ShotId；脚本以
  全局精确次数和 `AuthorityTrace -> Damage -> Committed` 事务区间归因，
  没有伪造直接关联。
- 当前实现只做服务器当前世界 Trace；客户端时间戳用于 freshness 校验，
  不构成 M7 历史碰撞回溯。
