# 自动化测试

测试运行一个 DS 与两个独立客户端：Server 执行权威规则，Partner 是先入场的远端协作玩家，Client 是主要受测玩家。每端独立输出断言与完成记录。

## 环境和运行

构建 `multiplayerEditor / Development Editor / Win64`，在项目根目录配置引擎位置：

~~~powershell
$env:UE_EDITOR = 'C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe'

# 解析器契约测试，不启动 UE
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest

# DS 与两客户端连接冒烟
.\Scripts\RunLocalDedicatedServer.ps1

# 三档功能矩阵
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Normal,Moderate,Harsh -Repeat 1

# 载人移动与时间线
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -Profiles Harsh -Repeat 3

# 单变量规模对照
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Static -StaticCounts 0,50,200,500 -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Moving -MovingCounts 1,5,20 -Repeat 3

# 可操作的双客户端
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777
~~~

支持 `-EditorPath` 显式覆盖环境变量。默认 NullRHI；`-Offscreen` 让客户端走 DX11 离屏渲染；`-Visible` 显示测试窗口。DS 始终无画面。打包程序测试使用 `-GameExecutablePath` 与 `-ServerExecutablePath` 分别指定客户端和服务器。

`RunLocalDedicatedServer` 默认转发到统一运行器的 DedicatedSmoke 场景，指定档位运行一次。`-Play` 为交互式会话，日志放在 Saved/DedicatedServer。两种入口都支持 `-PlanOnly`，只输出计划、不启动进程。

## 场景

| 场景 | 检查内容 |
| --- | --- |
| DedicatedSmoke | DS 无 LocalPlayer、两个真实远端、网络角色与独立回执 |
| Flow | 重叠归位、协作门、占用清理、平台、双端胜利、失败重开恢复、客户端退出后 DS 保持 |
| Keys | 持有、安装、消费、失败保留、重入保护、销毁与重新拾取 |
| LateJoin | 已有目标、休眠机关与钥匙附着的状态恢复 |
| Reconnect | 单连接真实超时、有限重连、另一连接保持、共享进度恢复 |
| ConnectionRetry | 不可达地址失败后恢复 Idle，再连接有效地址 |
| Ride | 客户端 CMC 基座、位移、相对偏移及服务端到达 |
| RideMotion | 站立、行走、反向、自由跳跃，两端四阶段指标及历史 Move |
| Scale | 实际对象数、客户端观察、服务器发送计数与复制 CPU |

`All` 包含 Flow、Keys、LateJoin、Reconnect、ConnectionRetry、Ride、RideMotion。DedicatedSmoke 单独调用。Flow/Keys 由测试驱动放置角色触发真实 Overlap；胜利 UI 检查客户端 Widget 加入 Viewport。可视运行用于进一步观察操作和画面。

## 网络模拟

| 档位 | PktLag | PktLagVariance | PktLoss |
| --- | ---: | ---: | ---: |
| Normal | 0 ms | 0 ms | 0% |
| Moderate | 100 ms | 20 ms | 2% |
| Harsh | 200 ms | 50 ms | 5% |

参数施加到三个进程，是模拟设置而非实测 RTT。Reconnect 额外阻断主测连接的服务器出站包，被测客户端超时设为 5 秒，等待真实断开后恢复连接。

## 报告与通过条件

`Saved/NetworkValidation/<run>/` 保存 report.json、逐端日志、samples.csv 和 summary.md；RideMotion 额外输出 SVG 时间线、逐 Move 配对和校正 CSV。

通过需要同时满足：

1. 同一运行 token、模式与正确角色的必需断言通过。
2. 三端各自收到成功 DONE。
3. 三个进程正常退出，日志无致命错误。

旧日志、缺失客户端回执、超时或强制退出都不能替代通过。规模测试要求有效的复制 CPU 列；总 GameThreadTime 缺失或全零时保留不可用标记。报告最终状态以 `report.status` 为准。

运行器只清理本次启动的进程。使用 `ExportNetworkEvidence.ps1` 可从已结束报告生成新的脱敏摘录，保留源报告指纹。

## 连接状态单测

`Coop.Connection.State` 覆盖地址格式、忙碌时旧地址保留、通知同步取消与重连次数上限。

~~~powershell
$editorCmd = Join-Path (Split-Path $env:UE_EDITOR) 'UnrealEditor-Cmd.exe'
& $editorCmd "$PWD/multiplayer.uproject" /Game/UI/DSMenu -unattended -nop4 -nosplash -NullRHI -DDC=InstalledNoZenLocalFallback '-ExecCmds=Automation RunTests Coop.Connection' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$PWD/Saved/TestReports/DSConnection"
~~~

## 已记录结果

- [DS 回归与规模复核](Evidence/2026-09-29/DSMigration.json)：21 项功能矩阵、2 项收尾 Flow、4 项规模复核及连接冒烟；批次、构建指纹及首次解析失败记录分别保留。
- [历史参数对照](Evidence/2026-09-27/performance.json)：Listen Server 拓扑，42 轮、每组 3 次。
- [性能报告](../Docs/Performance.md)：方法、汇总表及测量口径。

证据描述对应的已记录构建，重新运行会生成新的版本指纹；不同拓扑与采样批次分别统计。
