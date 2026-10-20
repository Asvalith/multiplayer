# 网络同步设计与验证边界

## 范围

本项目采用 UE 5.5 Dedicated Server：一个独立服务进程，两名远端客户端。
客户端菜单仅连接服务器地址，不建房、不搜索 LAN Session；服务器通过启动参数选图。
本机已配置 Editor Server 测试；独立 Server 包、Linux 部署和公网验收是单独交付项。
不包含账号、匹配、自研完整移动协议或服务器池。历史 Listen Server 记录不代表当前拓扑。

## 谁决定状态，谁负责显示

| 对象 | 服务器负责 | 客户端获得什么 |
| --- | --- | --- |
| Character / CMC | CharacterMovement 校验和移动处理 | 引擎的移动同步、预测和校正；冲刺根运动仍交给 CMC |
| CombatCharacter / ASC / AttributeSet | 正式技能资格、权威出手、伤害和治疗结算、生命与控制状态 | 所属端预测受支持的自身效果，其他端观察公开属性、标签和动作；能量与成长配装仅发给拥有者 |
| CoopKey / KeySocket | 玩家触碰钥匙，自动归位到预绑定插槽并登记一次进度 | 安装状态、Actor 附着/位置；总进度见 GameState |
| PressurePlate | 不同玩家统计、目标条件、开关 | 开关状态和初始速度，本地播放压下/弹起 |
| CoopGate | 合并压力板状态与不同玩家集合 | 开关状态和初始速度，本地播放开关门 |
| MovingPlatform | 启停条件与固定轨道运动 | Actor 位置与组件速度，默认更新频率上限 30 Hz |
| CoopGameState | 保存 GameMode 提交的目标结果 | 目标进度、目标总数、胜利状态的一份快照 |
| PlayerController | 重开请求交 GameMode 校验，失败回复所属客户端 | 管理重开 / 退出操作状态，通过本地委托向 Widget 发布反馈 |
| BP_CoopHUD / winandquit | 无共享规则 | HUD 订阅 GameState 并补读胜利结果，创建 / 清理界面与管理输入；Widget 展示反馈、转交按钮意图 |

属性复制表达的是当前结果，不保证客户端逐次观察到所有中间变化。
客户端表现必须可以从当前状态恢复，不能依赖“曾收到过一次开门 RPC”。
不同 Actor 的更新不是跨对象原子事务，不能要求宝物、插槽和 GameState 在同一帧全部到达。

原机关玩法的菜单与胜利界面位于 `Content/UI`：两个 Widget 直接继承 `UserWidget`；菜单控制器和机关 HUD 的生命周期由蓝图实现。
C++ GameInstance / PlayerController 只提供请求、状态查询与可绑定委托，GameState 保留共享规则结果。
HUD BeginPlay 尝试绑定已有 GameState；蓝图 GameState BeginPlay 通知已有本地 HUD，覆盖两者先后到达。
绑定后补读胜利快照，重复初始化不重复创建，EndPlay / Destruct 对称解绑；该机关界面不依赖 Tick 轮询。
这套就绪约定针对当前每个 World 一份 GameState、Travel 后重建 World 的流程。

战斗界面另位于 `Content/Combat/UI`，沿用原 HUD 和蓝图布局，接入原生 CombatHUD、事件驱动子控件与世界空间血条。角色替换时解绑旧 Pawn、订阅新 Pawn 后补读当前状态；只有活动冷却、读条和跟随镜头的瞄准几何按需更新。界面不负责技能资格授权或伤害结算。

钥匙只保留关卡实际使用的自动归位流程。每把钥匙在关卡中设置 `DestinationSocket`，
插槽保留 `KeyDisplayPoint` 作为安装位置；缺少绑定或安装失败时钥匙留在原地，不增加进度。
没有角色携带槽、丢弃或走入插槽交付分支，客户端 UI 仍以 GameState 的共享结果为准。

## 静止机关的网络休眠

门和压力板使用 `DORM_DormantAll`，兼容关卡摆放和运行时生成。
服务器改变复制属性之前调用 `FlushNetDormancy`，随后写入状态、更新本机表现并请求网络更新。
初始 JSON 速度也是复制属性，其赋值前同样刷新休眠。

这里的三个开关职责不同：

- 停止 Tick：不再逐帧计算本地运动。
- 降低 NetUpdateFrequency：降低常规复制更新频率目标，不承诺固定收包间隔。
- 网络休眠：减少未变化 Actor 的复制检查；不应以每帧唤醒抵消收益。

`ForceNetUpdate` 是调度请求，不是所有客户端已经收到状态的确认。
休眠变更必须回归初次加入、晚加入、反复开关和两端配置不同的场景。
压力板的占用变化事件和激活变化事件仍各自保留：人数变动即使没有改变开关，也可能改变合作门条件。

## 移动与碰撞边界

平台仍由服务器 Transporter 沿固定轨道运动，普通 Actor 位置复制不等于 CharacterMovement 的网络平滑。
平台不做轨道 Sweep，轨道避障由关卡布置保证；不能把 BlockAll 描述成自动避障或防挤压。
客户端站立、行走、跳上跳下、反向和停止必须在有画面的客户端补查。

门和压力板当前按收到的状态开始本地过渡，没有共同的服务器过渡起始时间。
门的碰撞跟随本机网格，两端过渡进度在弱网下可能暂时不同。
若测试证明影响通行，再补服务器起始时间和起始进度；不能只为了简历提前加入预测回滚。

## DS 连接、失败和重连

GameInstance 只维护客户端连接状态，不再依赖 OnlineSession 的 Create/Find/Join/Destroy 回调。
菜单校验 hostname/IPv4:port 后调用 ClientTravel；20 秒项目期限覆盖“请求发出却没有进入游戏”的情况。
Connecting 期间不接受重复连接，主动取消会清理 PendingNetGame、计时器和旧地址，再回到 DSMenu。

状态通知可能同步调用退出，所以提交内部状态后广播，再检查操作编号才继续 Travel。
全局 NetworkFailure 按 World/GameInstance/NetDriver 归属过滤；不处理其他实例的失败。
服务器单个远端断开不会触发整个 DS 返回菜单。客户端断线按 JSON 的有限等待重试，次数耗尽可手动连接。
菜单会显示当前状态，并提供取消连接/重连入口。

本地控制器进入 PlayingState 后，还需通过所属 Controller 的 `ServerEstablishCombatSession` 完成战斗握手。GameMode 接纳新访客或校验恢复票据，恢复成功后才设置战斗就绪；客户端收到同次操作的确认后进入 Connected。进入地图不等于已经允许释放技能，其他复制对象仍可能更晚到达。

共享进度由仍运行的服务器保留，晚加入通过当前复制状态恢复。个人短时恢复另使用服务器签发的票据和 120 秒快照：保存位置、血量、能量、护盾、成长配装、倒地期限及明确支持的剩余冷却、防御、控制和持续效果。离线时间正常扣除，灼烧结算离线伤害并保留下一跳节奏；不续接半次施法、冲刺或救援，也不重放旧结算。票据不能同时被两条在线会话领取，成功恢复后轮换票据。

该恢复仅覆盖同一服务器进程、同一局及仍保留票据的客户端进程，不是账号认证、任意 GE 存档、客户端重启或服务器重启后的恢复。
主动退出取消重连，不对其他玩家广播“主机退出”，也不关闭 DS。
两名玩家都离开时暂保留当前局；空房重置、鉴权和实例回收未实现。

重开仍由所属 PlayerController 的可靠 RPC 请求、GameMode 复核，并对首个有效请求去重。
DS 由进程决定运行模式，不再补写 listen URL；旧 World 中的失败恢复仍保留，既有验证见对应版本报告。

## GAS 请求预测与权威结算

玩家的定向技能把方向随 GameplayEvent 一起提交，治疗走普通尝试激活；二者进入同一 GAS 正式资格检查，不把本地预览当授权。`CheckSkillAvailable` 只查询真实能力的 `CanActivateAbility`，角色补会话、生命与学习等业务条件，成本、冷却和标签关系分别接入 GAS 原生检查。互斥关系借鉴 Lyra 的集中映射思路，不代表接入完整 Lyra 架构。

具体能力显式调用 Commit 和 End。提交时再次校验成本与冷却，并用一次局部规则快照固定本次扣费与冷却；起手、出手和收招任务由能力管理。服务器使用自己的发射点、目标、范围和遮挡执行命中，伤害 GE 经执行计算与 AttributeSet 统一处理格挡、护盾和生命。GameplayCue 和界面只表现结果。

LocalPredicted 预测所属端的资源、冷却、部分状态和动作，PredictionKey 关联接受或拒绝；自定义副作用仍需按本次任务、效果句柄及激活版本清理。客户端不预测远端扣血或权威弹体命中；当前没有通用动作日志回滚、目标历史命中回溯或完整世界回滚。投射物按轨迹段初始状态重建客户端表现，不等于客户端有命中裁决权。

完整调用顺序、取消与重入边界、数值捕获时机见 [GAS 架构与调用链](Study/GAS架构与调用链.md)。

## 测试结果应如何解释

当前网络脚本提供 `RideMotion`、`Combat`、`CombatTraffic`、`CombatReconnect`、`CombatLifecycle`、`CombatExpansion` 与 `CoopEscape` 场景，另有 `Multiplayer.Combat` 功能回归。场景存在不等于当前构建已通过，受控战斗夹具也不等于正式地图的完整人工通关；具体执行结果统一见测试说明。

历史 Listen 连接／Session、机关与规模报告继续保留，但不能把已经移除的历史入口描述为当前可重跑功能。以下规模与断包口径分别按报告对应版本解释，不把不同拓扑、关卡与源码版本混算。

- 状态测试：分别检查服务器结果和客户端观察结果，不能只检查服务端变量。
- 压测：固定地图背景、玩家位置、相关性、时长与更新频率，一次只改变一个条件。
- 静止系列仅对比压力板保持唤醒/休眠；运动系列仅对比平台 100/30 Hz 更新频率上限，两类数量不交叉叠加。正式每组至少三次，单次短采样只检查工具链。
- CPU：观察服务器复制工作耗时，服务器无渲染，但不可把整个帧周期都当成复制成本。
- 发送量：历史规模实验采集服务器 NetDriver 引擎发送字节，含配置的包头开销、原地图和测试通信，不是单个机关的独占带宽。该计数不是网卡抓包或成功到达字节，不能用于宣称物理链路流量；B/s 表示字节每秒。
- 时延：模拟参数不等于实测 RTT；服务器与客户端同时加发送延迟会共同影响往返时间。
- 断包：只对一条远端连接设置服务器出站 100% 丢包，被测客户端缩短超时并触发真实重连；另一连接保持，不主动广播断线事件；不声称覆盖网卡、路由器和运营商的全部故障。
- 无画面测试可以检验状态和数值，不能证明视觉平滑、UI 排版和人工操作手感。
- 同机三进程不是三台机器的局域网测试；开发版运行也不是打包交付验收。

历史玩法场景会放置角色触发实际重叠，也会生成合成机关；它们不是完整人工通关录像。
历史胜利界面断言检查客户端状态和 Viewport 中的实例，不检查像素；重开测试会调用真实按钮处理入口，但不模拟系统鼠标。
平台载人断言检查 CMC 基座、位移和偏移，不等于弱网下视觉平滑已经合格。
60 FPS 限制下的 100 Hz 参数不是实测发包频率；全零/缺失 CPU 计数必须注明，不能据此宣称没有成本。

每份报告记录代码版本、未提交状态、源码和配置指纹、命令、运行环境、断言以及日志路径。
失败、未运行和不适用必须分开，历史报告不得直接宣称当前版本通过。
运行入口、对照命令与报告口径统一维护在 [测试说明](../Tests/README.md)，这里不复制结果表。

## 技术依据

- [UE 5.5 Actor 网络休眠](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine?application_version=5.5)
- [UE 5.5 Networking Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/networking-insights-in-unreal-engine?application_version=5.5)
- [UE 5.5 网络模拟](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-network-emulation-in-unreal-engine?application_version=5.5)
