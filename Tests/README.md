# 网络实验与战斗回归

网络入口支持 RideMotion 和 Combat：一个 DS 和两个独立客户端。Partner 先连接并确认角色与共享状态就绪，Client 执行受测动作；服务器和两个客户端分别记录结果。

## 运行

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Normal,Moderate,Harsh -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -PlatformMotion Static -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario Combat -Profiles Normal,Harsh -Repeat 1 -Map /Game/Combat/Maps/CombatTest
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Map /Game/Combat/Maps/CombatTest -Port 7777
~~~

运行前编译 multiplayerEditor。默认采用 UE 5.5.4 Editor Server；-EditorPath 可覆盖引擎路径。默认 NullRHI；-Offscreen 使用客户端 DX11 离屏渲染，-Visible 显示客户端窗口，DS 始终无画面。打包测试须同时提供 GameExecutablePath 与独立 ServerExecutablePath。

RunLocalDedicatedServer 不带 -Play 时执行一次指定网络档位的平台实验；-Play 只启动可操作的 DS 和两个客户端，不生成自动通过结论。两种入口均支持 -PlanOnly；端口为 0 时自动选择。

## 保留的检查

- DS 没有本地玩家，两名玩家由独立远端进程连接，各端必须独立完成并正常退出。
- 原生 CMC 站立、行走、反向、自由跳跃和落回；服务器与客户端都必须观察到对应动作。
- 采集平台/角色位置、速度、基座、移动模式和校正；使用同一 Move 时间戳关联两端记录，不直接比较 World 时钟。
- 测试角色使用实际蓝图角色的关键移动参数，观测组件不改变原生预测、校正或移动容差。
- 缺少阶段指标、非法数值、配置不匹配、超时、强制结束或致命日志均不能通过。

-PlatformSyncMode 支持 OrderedVelocity 和 PlatformInertia（默认）；两者均使用当前平台修复，前者仅在实验中恢复空中制动参数作对照。旧 Baseline 需使用历史代码版本复现。
-PlatformMotion Static 只关闭平台运动，其余角色配置与输入不变，用于静止平台对照。

Normal=0/0/0；Moderate=PktLag 100、Variance 20、Loss 2%；Harsh=200、50、5%。参数施加到三个进程，不是测得的 RTT。

## 输出与边界

Saved/NetworkValidation/<run>/ 保存 report.json、三端日志和 summary.md；平台实验额外生成时间线 SVG、采样、校正与 Move 配对 CSV。报告记录源码/模块指纹、命令、参数及各端断言。

只认最终 report.status。自动动作和数值断言不等于视觉平滑、UI 排版或人工完整通关验收；同机三进程不等于跨机器公网验收。当前入口不采集带宽或复制 CPU，不用动作通过推导玩家容量。

## 战斗检查

`Multiplayer.Combat.CoreRules` 自动化用例覆盖消耗、冷却、防御到期、治疗选目标、真实投射物、友军穿过、死亡禁用及机关占用清理。

`Combat` 三进程场景由真实客户端申请 GAS 技能，两端独立检查攻击结果、防御标签与数值、队友治疗、死亡状态和碰撞清理。防御额外检查调用返回前的本地预测、真实 PredictionKey 的确认，以及服务器资源不足或死亡时的拒绝与恢复。为隔离规则，场景冻结角色并停止靶敌 AI，因此不能用这份报告证明导航、视觉表现或重连后的个人战斗状态恢复。

`Multiplayer.Combat.MapAI` 在 `/Game/Combat/Maps/CombatTest` 验证真实导航追击和攻击，属于单进程地图验收；仍需另做人工双客户端游玩验收。

## 历史实验

既有玩法、连接、UMG、DS 冒烟和规模压测报告仍保存在 Docs/Reports、Tests/Evidence 和本机 Saved/NetworkValidation；对应测试源码已从当前工程移除。复现须使用报告对应的历史代码版本，不能拿旧报告宣称当前构建已通过这些回归。
