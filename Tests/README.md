# 网络验证与证据

旧构建的回归与复制开销对照见 [2026-09-27 验证报告](Evidence/2026-09-27/README.md)；后续新增的动态载人实验见 [移动平台与角色弱网同步报告](../Docs/Reports/2026-09-27/PlatformRideSync.md)。两批构建不同，不能合并统计通过数或优化收益，也不要把下方的测试计划和期望断言直接当作通过结果。

入口是 `Scripts/RunMultiplayerNetworkTests.ps1`。它启动独立的本机 Listen Server 和客户端进程，读取开发测试驱动输出的 `COOP_TEST` JSON 事件，保存命令、日志、配置和版本指纹。它不修改玩法 JSON，不修改系统网卡／防火墙，只结束本轮自己启动的进程。

默认使用 UE 5.5 Editor 的 `-game -NullRHI`，隐藏窗口。`-Visible` 才开启可见窗口；可见运行依然不自动证明视觉正确。`-GameExecutablePath` 可指定已构建的 Development 游戏程序。Shipping 禁用测试启动入口和回执行为，不能拿它运行这套协议；UHT 生成的测试类型仍可能存在，不应描述为彻底移除了所有测试符号。

## 先验证入口，再执行网络测试

在项目根目录的 PowerShell 中：

```powershell
# 只验证日志解析与缺失断言不会误通过；不启动 UE。
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest

# 生成计划和完整命令，不启动进程；所有案例保持 planned。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Regression -PlanOnly

# 单次快速回归，只用于排查入口或单项问题，不是完整验收。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario SessionRetry -Profiles Normal -Repeat 1

# 正常／中等／较差网络，各重复三次。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Regression -Repeat 3

# 可单独重跑一个模式或使用不同 UE 安装路径。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario Flow -Profiles Normal -Repeat 3 -EditorPath 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe'

# 静止系列：只改变休眠，新增运动平台为 0；每组重复三次。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Static -StaticCounts 0,50,200,500 -WarmupSeconds 5 -SampleSeconds 30 -Repeat 3

# 运动系列：只改变更新频率，新增静止板为 0；每组重复三次。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Moving -MovingCounts 1,5,20 -WarmupSeconds 5 -SampleSeconds 30 -Repeat 3

# 最小采样冒烟：两种休眠设置各一次，不作正式优化结论。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Static -StaticCounts 0 -WarmupSeconds 2 -SampleSeconds 8 -Repeat 1
```

完整矩阵会启动很多次 UE，耗时明显长于单次冒烟检查；先用 `-PlanOnly` 确认范围。客户端从主菜单启动，由测试驱动调用真实会话 API；主机从合作地图启动。本项目建房不会自动从菜单切换合作地图。

网络条件为双端施加的模拟参数，不是测得的 RTT：Normal=0/0/0；Moderate=`PktLag=100, PktLagVariance=20, PktLoss=2`；Harsh=`200/50/5`。Reconnect 在测试进程中将连接超时缩短为 5 秒，临时施加 100% 入站和出站丢包，再恢复原模拟参数，等待引擎真实报错和项目自动重连。它不是主动广播 `ConnectionLost`，也不是物理拔网线；该模式是否通过以完整断言为准。

SessionRetry 与 Flow 的首次重开使用开发故障注入，分别检查建房回滚和“旧 World 仍存活”的重开失败恢复。它们不能证明所有 OnlineSubsystem 服务错误、损坏地图或引擎加载失败都已覆盖。

## 模式和通过条件

| 模式 | 期望的断言，不是预先宣布的结果 |
| --- | --- |
| Flow | 两端 Join；真实钥匙重叠与一次计数；不同玩家数及门规则；客户端 GateOpen/GateClosed；DestroyedPawnCleanup；PlatformEndpoint/ClientRide；ClientVictoryState；RestartFailureRecovery；服务器 Restart 和 ClientRestart；ClientLeave/HostLeave |
| LateJoin | LateJoinState：新客户端读取已完成的权威状态 |
| Reconnect | OutageApplied、ConnectionLostDetected、HostSessionRetained、ReconnectAfterOutage、ReconnectStateRestored；客户端断开不能移除主机房间 |
| SessionRetry | SessionFailureObserved、SessionRetrySucceeded |
| Ride | ClientRide：客户端实际载人结果，不仅是服务器平台到达终点 |
| RideMotion | 双端 Join；客户端 MotionStand/MotionWalk/MotionReverse/MotionJump；主机 MotionServerObserved；双端四阶段均有有效 platformRide 指标 |
| Scale | ScaleCount、客户端 ScaleClientLoad、SampleCompleted，以及服务器真实计数器和 CPU CSV 样本 |

每例必须同时看到 Host 和 Client 的 `DONE passed=true`，所有必需断言都通过，并且没有 FAIL。解析器核对本轮 token、角色和模式，服务器断言不能代替规定的客户端断言。缺少事件、超时、启动失败或不支持都不得记为通过。Scale 还必须有 `outBytes/outPackets/sampleSeconds/actorCount` 和有效 CPU CSV 列；客户端需真正收到全部合成对象，并观察到各平台移动，才开始采样。

Flow 用服务器放置两名角色触发实际 Overlap；门、压力板、载人平台和终点中包含运行时生成的测试机关。它验证状态路径，不覆盖所有关卡蓝图布置或玩家按键操作。ClientRide 检查实际 CMC 基座、位移及偏移；ClientVictoryState 检查客户端胜利状态和界面是否加入 Viewport，不做画面像素、按钮点击或视觉平滑验收。

驱动日志协议示例（示例不是实测结果）：

```text
COOP_TEST {"mode":"Flow","role":"Host","status":"READY","phase":"ReadyForClient"}
COOP_TEST {"mode":"Flow","role":"Client","status":"ASSERT","assertion":"Join","passed":true}
COOP_TEST {"mode":"Scale","role":"Host","status":"METRIC","metrics":{"outBytes":1000,"outPackets":10,"sampleSeconds":30,"actorCount":51}}
COOP_TEST {"mode":"Flow","role":"Host","status":"DONE","passed":true}
```

Host 的 READY 在实际可加入之后发出；LateJoin 先准备已有目标进度和激活压力板，再启动客户端。驱动用唯一 token 区分会话与事件。脚本负责传入角色、模式、矩阵和计数等开发参数，通常不必手写底层 `-Coop*` 参数。

## 移动平台与角色弱网同步：RideMotion

`RideMotion` 必须显式指定，**不在 `-Scenario All` 的默认五场景矩阵中**。它依次执行站立、横向行走、平台反向、起跳落回，客户端通过真实 CMC 输入、预测与服务器校正路径运动；服务器也独立检查运动结果。每段双端各采样约 4 秒，实际时长以指标为准，不使用规模测试默认的 30 秒采样值。

```powershell
# 原行为对照：旧平台更新时序／速度行为，空中制动恢复旧值 1500。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -PlatformSyncMode Baseline -PlatformNetHz 30 -Profiles Normal -Repeat 1

# 中间对照：修复平台更新顺序和速度，但空中制动仍为 1500。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -PlatformSyncMode OrderedVelocity -PlatformNetHz 30 -Profiles Normal -Repeat 1

# 最终方案（默认模式）：保留空中水平惯性，三档网络各运行一次。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -PlatformSyncMode PlatformInertia -PlatformNetHz 30 -Repeat 1
```

`PlatformNetHz` 范围为 1～60，默认 30；它是复制频率上限，不是实测发送次数。前两种模式用于显式测试对照，不改变普通关卡的运行模式。最终方案的测试角色读取实际关卡蓝图角色的关键移动参数，但它仍是带指标记录的专用 CMC 测试 Pawn，**不是完整蓝图角色、动画、摄像机或所有碰撞附件的验收**。

每例 `report.json` 中的 `motionMetrics` 保留 Host/Client 四阶段原始事件，阶段名读取 `metrics.phase`，时长读取 `metrics.elapsedSeconds`。记录基座占用、相对位移、跳跃高度与落回状态，以及 CMC 校正次数、可比较校正误差和基座切换次数。主机不接收客户端位置校正，其零校正计数不参与客户端改善结论。缺阶段、非有限数值或失败断言均不能通过；是否通过与画面是否完全平滑是两件事。本场景不以 CPU 或带宽指标为验收目标，不应把对应空值解释成零成本。

已完成结果：`PlatformInertia` 的 Normal/Moderate/Harsh 各 1 次通过；Normal 下的 `Baseline` 与 `OrderedVelocity` 均在 Jump 阶段失败。Harsh 的 Jump 仍有 13 次校正，最大可比较误差约 72.12 cm，因此不能宣称弱网下完全平滑，也不能用每档一次的数据给出稳定改善百分比。完整条件、失败过程和边界见 [专项报告](../Docs/Reports/2026-09-27/PlatformRideSync.md)，公开证据位于 [Tests/Evidence/2026-09-27/PlatformRide](Evidence/2026-09-27/PlatformRide)。这批结果独立于旧构建的 15 例回归和 42 轮性能对照。

## 单变量性能对照

| 系列 | 新增合成负载 | 基线与对照 | 保持不变 |
| --- | --- | --- | --- |
| Static | 0、50、200、500 个静止压力板 | Baseline：保持唤醒；Dormancy：网络休眠 | 平台新增数为 0，两组压力板频率上限同为 30 Hz |
| Moving | 1、5、20 个持续往返平台 | Baseline：频率上限 100 Hz；Frequency：30 Hz | 压力板新增数为 0，不同时改休眠 |

`-Matrix All` 依次运行这两条曲线，不做静止数量与运动数量的笛卡尔积。系列只在 Normal 网络下比较。数量表示在原地图背景上**新增**的对象；0 档仍有角色、探针及地图自身机关，不是空世界。合成对象保持网络相关、关闭交互碰撞，平台持续往返，目的是隔离复制成本，不代表正式关卡交互负载。

测试进程限制为 60 FPS，因此 100 Hz 只是属性设置的上限，不代表实际每秒发出了 100 次更新。正式每组至少三次有效样本；`-Repeat 1` 与短采样只作快速排错。不同构建、人数、地图或采样时长的数据不能直接混算优化百分比。

这里是参数对照，不是旧版本与新版本的全项目 A/B：平台在本轮之前的代码中已经设置为 30 Hz，100 Hz 是合成压力基线；正式压力板原有频率为 5 Hz，静止实验把两组都设为 30 Hz 以隔离休眠变量。不能把合成对照百分比改写成真实关卡或历史版本的整体提升。

## 报告与解释范围

结果默认在 `Saved/NetworkValidation/<UTC时间-随机ID>/`：

- `report.json`：环境、源文件清单及 SHA-256、commit/dirty、完整配置文本与 hash、启动命令、逐角色日志、每条断言、失败原因和原始计数。Editor-game 还记录项目 DLL 的 SHA-256，而不只记录引擎 exe；这属于磁盘文件指纹，不等于已独立核验进程实际加载的模块。RideMotion 的阶段数据位于 `motionMetrics`，通用 `warmupSeconds/sampleSeconds` 留空。
- `samples.csv`：每次带宽采样、服务器复制 CPU 与 GameThread 计数的均值/P95，以及休眠对象等可用辅助计数，不把失败／不支持／仅计划案例计为成功。
- `summary.md`：同组带宽的中位数、最小值与最大值；CPU 列是每轮均值/P95 的中位数，不是把所有帧合并后再算 P95。每轮 P95 使用 nearest-rank。至少三次有效样本才满足对照要求；B/s 是字节每秒，不是 bit/s。
- 报告结构见 `Tests/NetworkReport.schema.json`。大日志保留在 Saved；展示时可把脱敏摘要另存到 `Tests/Evidence`，禁止手填 PASS 冒充脚本生成。

发送量来自采样期间 `UNetDriver.OutTotalBytes/OutTotalPackets` 累计计数之差，是服务器向远端连接的引擎发送计数，含配置的包头开销、原地图背景和测试通信，并非新增机关独占流量。它不是网卡抓包：后续 PacketHandler 加工和模拟丢弃可能使实际链路字节或成功到达量与它不同。CPU 关注 CSV 的 `Exclusive/GameThread/ServerReplicateActors`（或等价无线程前缀列）；GameThreadTime 还包括其他游戏工作，不能当作网络专属成本。缺列保留 null；原始全零计数需标为测量限制，不能写成 CPU 成本为零。

同机双进程会竞争 CPU，合成机关负载不代表真实玩家容量；NullRHI 的逻辑／网络测量不代表画面流畅或渲染帧率。实际玩家输入、跨物理机、画面、打包外置 JSON 的交付验收需单独记录，不能仅凭本报告认定通过。设计边界见 [网络设计](../Docs/NetworkDesign.md)。

## 导出可公开证据

原始报告结束后，再生成脱敏摘录；导出不会修改状态或补造通过结果：

```powershell
.\Scripts\ExportNetworkEvidence.ps1 -ReportPath 'Saved/NetworkValidation/<运行目录>/report.json' -OutputPath 'Tests/Evidence/<日期>/regression.json'
```

输出必须是新文件。工具记录原报告 SHA-256 和被测程序指纹，移除本机路径、机器名及原始命令；它是摘录，不是 `NetworkReport.schema.json` 描述的原始报告。性能案例会先核对服务器 CSV 的指纹，再从 `FrameTime` 派生均值、P95 和估算运行帧率；CSV 缺失或不匹配时保留 null 和警告。这个帧率不代表渲染 FPS，也不把 `GameThreadTime` 全零解释成无成本。

旧的 2026-09-10 证据可追溯到 `70007ee` 的 `Tests/Evidence`，当时版本指纹基于 `5533bb5` 的 dirty 源码；`05b7cf1` 删除了旧测试入口与驱动。旧数据不得当作本次结果，旧钥匙测试直接调服务器登记接口、平台仅验服务器终点、重连为事件注入。迁移旧工具不等于补齐这些覆盖。
