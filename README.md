# UE5 双人 Co-op 网络协作 Demo

UE 5.5 / C++ 双人合作原型。正式入口为 **一个独立 DS + 两个远端客户端**，不再提供 Listen Server 建房或 LAN 搜房。服务器维护钥匙、机关、共享进度和胜利规则。

当前使用安装版 UE 的 Editor Server 进行本机开发验证。它实际运行于 NM_DedicatedServer，但不是已经打包好的 multiplayerServer.exe。独立 Server 构建及 Linux 部署尚需另行验收。

## 启动

项目根目录中运行：

~~~powershell
# DS + 两个可操作客户端；或双击 LaunchTwoPlayers.bat
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777

# 无画面连接冒烟，结束后自动退出
.\Scripts\RunLocalDedicatedServer.ps1
~~~

先编译 multiplayerEditor；-EditorPath 可覆盖引擎路径，批处理支持 UE_EDITOR 环境变量。默认客户端菜单是 /Game/UI/DSMenu，输入 127.0.0.1:7777 连接。跨机器填写服务器的局域网地址；公网另需路由、防火墙和 UDP 可达性，不提供匹配、鉴权或 NAT 穿透服务。

手动启动：

~~~powershell
$editor = 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe'
# 一个终端运行服务器；没有主机玩家，不渲染画面。
& $editor "$PWD/multiplayer.uproject" /Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo -server -game -log -port=7777
# 另开终端运行客户端；未指定地址时进入连接菜单。
& $editor "$PWD/multiplayer.uproject" -game -windowed
~~~

玩家退出不关闭 DS。两人都退出后服务器仍保留本局，直到管理员结束进程或玩家胜利后请求重开；未实现空房自动重置或服务器池。手动启动脚本关闭两个窗口后会清理它自己启动的 DS。

## 同步与连接设计

| 对象 | 实现 |
| --- | --- |
| 角色 | 原生 CharacterMovement 预测、服务器校正与重放，不是自研完整移动协议 |
| 钥匙/插槽 | 玩家触碰钥匙后，服务器自动安装到预绑定插槽并登记一次进度；客户端恢复安装状态和原生附着，不提供携带或丢弃玩法 |
| 压力板/门 | 服务器统计不同玩家；复制开关和初始速度，客户端更新表现；静止时休眠 |
| 移动平台 | 服务器决定启停和轨道位移；Actor Movement Replication，频率上限默认 30 Hz |
| GameMode / GameState | 前者判断规则；后者复制目标快照，支持晚绑定补读当前结果 |
| PlayerController | 所属连接提交重开 RPC；各客户端创建自己的 UI，DS 不创建界面 |
| GameInstance | 地址校验、连接、取消、失败恢复和有限重连；不再管理 OnlineSession |

连接状态：Idle → Connecting → Connected；短暂断线进入 ReconnectWaiting → Reconnecting，次数耗尽返回 Idle；主动退出进入 Leaving，菜单加载完成后回 Idle。成功以本地控制器进入 PlayingState 为准，不等于所有 Actor 已就绪。拒绝请求不覆盖旧地址；同步取消会使操作编号失效，防止旧流程继续 Travel。

原有占用变化、Pawn 销毁/失去控制清理、钥匙提交失败回滚、重复广播保护、胜利晚绑定和重开失败恢复继续复用。换成 DS 不会自动解决平台弱网自由跳跃的参考系误差。

## 目录

~~~text
Source/
  multiplayer.Target.cs                 游戏目标
  multiplayerEditor.Target.cs           本机开发目标
  multiplayerServer.Target.cs           独立 DS 目标（需支持 Server 的引擎构建）
  multiplayer/
    Core/                               规则、共享状态、JSON、日志
    Network/multiplayerGameInstance      DS 连接状态机
    Player/                             输入、角色、所属玩家请求
    Mechanisms/                         钥匙、插槽、压力板、门、平台、终点、占用组件
    UI/                                 原生连接菜单、胜利界面、状态监听
    Testing/                            三进程回归、连接单测、平台诊断
Content/UI/DSMenu.umap                   无旧建房蓝图的连接菜单关卡
Content/Config/Gameplay.json             外置玩法表
Scripts/RunLocalDedicatedServer.ps1       手动启动/转发连接冒烟
Scripts/RunMultiplayerNetworkTests.ps1    统一自动测试入口：DS 冒烟、回归、复制规模实验
Scripts/PlotRideMotionTimeline.py        实测时间线（兼容历史 Host 与当前 Server）
Scripts/ExportNetworkEvidence.ps1       已结束报告脱敏导出
Tests/README.md                          测试命令与证据边界
Docs/NetworkDesign.md                    当前设计
Docs/Reports/                           按日期保留历史与新实验
~~~

## JSON 参数

Content/Config/Gameplay.json 启动时读取一次，失败整表回退默认值；不在 Tick 读磁盘，不支持热更新。

| 字段 | 默认值 | 当前含义 |
| --- | ---: | --- |
| SchemaVersion | 1 | 配置版本 |
| SessionMaxPlayers | 2 | 保留旧字段名；现在表示 DS 远端玩家总数，不含服务器 |
| WinRequiredPlayers | 2 | 终点不同玩家数 |
| PlatformRequiredPlayers | 1 | 自身占用模式的平台人数要求 |
| PlatformMoveSpeed | 150 | 平台速度 cm/s |
| DoorMoveSpeed | 250 | 门速度 cm/s |
| PlateMoveSpeed | 80 | 压力板速度 cm/s |
| ReconnectDelaysSeconds | [1,2,4] | 重试等待秒数；空数组关闭自动重连 |

模型、碰撞、端点、机关关联仍在蓝图/关卡设置；目标数来自实际插槽。打包须携带外置配置。修改人数不自动增加出生点或改造双人关卡。

## 验证与边界

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Normal,Moderate,Harsh -Repeat 1
~~~

All 包含 Flow、Keys、LateJoin、Reconnect、ConnectionRetry、Ride、RideMotion。三端必需断言、DONE、正常退出与致命日志检查共同决定结果。单轮是迁移回归，不是稳定性能统计。

历史 Listen Server 报告保留其原拓扑与结果，不重命名成 DS 成绩。旧建房失败用例由 DS 连接失败恢复替代，两者不是同一个实验。当前测试见 Tests/README.md，迁移实测见 Docs/Reports/2026-09-29/DSMigration.md。

- 不承诺几十/上百人容量、匹配、账号鉴权、跨服恢复或服务器池。
- 地址校验不是身份认证，不是完整安全防护。
- NullRHI 自动断言不能代替画面、输入焦点和平滑度验收。
- 独立 Server 目标已配置；源码构建引擎、Server 包、Linux 部署与公网验收仍是独立交付项。
