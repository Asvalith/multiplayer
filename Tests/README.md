# 网络验证与证据

当前套件固定为一个 DS 和两个独立客户端：Server 执行权威判定，Partner 是先入场的远端协作玩家，Client 是主要受测玩家。Partner 不是服务器本地玩家，也不是假 Pawn。

## 命令

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest
.\Scripts\RunLocalDedicatedServer.ps1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Normal -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Moderate,Harsh -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -Profiles Harsh -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Static -StaticCounts 0,50,200,500 -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Moving -MovingCounts 1,5,20 -Repeat 3
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777
~~~

运行前编译 multiplayerEditor。默认安装路径可用 -EditorPath 覆盖。默认 NullRHI；-Offscreen 使用客户端 DX11 离屏渲染，DS 始终无画面。打包测试须同时提供 GameExecutablePath 与独立 ServerExecutablePath；当前安装版引擎先验收 Editor Server。

RunLocalDedicatedServer 不带 -Play 时，转发到统一入口的 DedicatedSmoke 场景（指定档位、一次运行），报告同样保存在 Saved/NetworkValidation。-Play 只启动交互式会话，在 Saved/DedicatedServer 保存日志，不生成自动通过结论。两种入口均支持 -PlanOnly；自动测试的 -ListenPort（启动包装器中为 -Port）为 0 时自动选端口。

## 覆盖

| 场景 | 检查内容 |
| --- | --- |
| 通用 | DS 无本地玩家、Partner 收到角色/共享状态；三端分别完成且正常退出 |
| DedicatedSmoke | 直接连接 DS；检查两个真实远端的网络角色、共享状态与独立回执。不等待其他场景的 Partner 预置步骤，不含在 All 中 |
| Flow | 真实钥匙重叠、不同玩家计数、触发体重绑定清空/恢复/同人数换成员、门开关、控制权/Pawn 清理、载人平台、双端胜利、失败重开恢复、双端重开、Client 退出后 DS 与 Partner 保持 |
| Keys | 触碰预绑定钥匙自动归位、缺绑定与安装失败保留原地、重试与重复触碰去重、目标满后拒绝、提交重入保护、远端附着收敛 |
| LateJoin | Partner 先改变目标和休眠机关；Client 后加入读取状态及归位附着 |
| Reconnect | 仅阻断 Client 连接的 DS 出站包直到真实超时；DS 和 Partner 保留；Client 有限重连并恢复进度 |
| ConnectionRetry | 真实连接不可达端口，项目期限到达后恢复 Idle，再连接正确地址；不是历史 CreateSession 注入 |
| Ride | 客户端实际 CMC 基座、位移和偏移；服务器检查到达 |
| RideMotion | 原生 CMC 站立、行走、反向、自由跳跃；保存 Server/Client 四阶段指标和 Move 时间戳 |
| Scale | 实际对象计数、客户端收齐与运动证据、服务器出站计数和 CPU CSV |

Flow/Keys 由服务器放置角色触发真实 Overlap，不模拟系统键鼠。胜利 UI 检查真实 Widget 加入视口，不检查像素。使用网络身份跨重开区分两人，不依赖登录顺序。历史 Keys 报告保留当时的携带路径结果，不替代当前自动归位流程的重测。

Normal=0/0/0；Moderate=PktLag 100、Variance 20、Loss 2%；Harsh=200、50、5%。参数施加到三个进程，不是测得的 RTT。Reconnect 额外阻断单连接出站包，并将被测客户端 ConnectionTimeout 缩短为 5 秒；不是全服务器断网或公网 NAT 验收。

## 结果与判定

Saved/NetworkValidation/<run>/ 保存 report.json、每端日志、samples.csv、summary.md；平台测试额外生成 SVG 和逐 Move CSV。报告包含源码/模块指纹、命令及各端断言。缺事件、超时、强制结束、非零退出或致命日志不能通过。

只认最终 report.status。运行中的单项 passed 不代表整轮成功。脚本只清理自己启动的进程。

DS 带宽是两条连接的总出站量，不是单客户端量。本机三个进程存在资源竞争，不与历史 Listen 数据混算。性能对照至少重复三次；单轮不能承诺稳定提升或玩家容量。GameThreadTime 缺失或全零都标为不可用，而非没有 CPU 消耗；服务器复制耗时仍要求有效实测值，不能缺列过关。

2026-09-29 的 DS 迁移实测：21 项功能矩阵、2 项收尾 Flow、4 项短规模复核及连接冒烟通过。首次规模解析失败记录也保留。完整解释见 Docs/Reports/2026-09-29/DSMigration.md，精简证据见 Tests/Evidence/2026-09-29/DSMigration.json；单次规模复核不替代三次全档位压测。

## 连接状态单测

Coop.Connection.State 检查地址格式、忙碌时保留旧地址、通知中的同步取消、重连上限。这是受控逻辑测试，不替代三进程运行。

~~~powershell
& 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' "$PWD/multiplayer.uproject" /Game/UI/DSMenu -unattended -nop4 -nosplash -NullRHI -DDC=InstalledNoZenLocalFallback '-ExecCmds=Automation RunTests Coop.Connection' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$PWD/Saved/TestReports/DSConnection"
~~~

旧 Coop.Session.* 对应已移除的 LAN 实现，源码在迁移前已本地备份，历史报告仍保留，不宣称新套件重测了已移除的 OnlineSession 回调。
