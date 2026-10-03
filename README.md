# multiplayer · UE5 C++ 双人 Co-op

基于 Unreal Engine 5.5 和 C++ 的双人合作 Demo，采用 **一个 Dedicated Server + 两个远端客户端**，实现地址连接、钥匙自动归位、压力板与合作门、移动平台、目标进度、双人胜利与重开。

项目重点是服务器权威的玩法规则、可恢复的状态复制、连接生命周期管理，以及通过重复对照和历史 Move 配对分析网络成本与同步误差。

## 项目亮点

- **服务器权威的合作玩法**：服务器处理钥匙触碰与安装、不同玩家计数、机关激活和胜利判定；客户端接收结果并更新表现。GameMode、GameState、PlayerController 与机关 Actor 分工明确，分别管理连接就绪与玩法状态就绪。
- **状态同步与晚加入恢复**：目标进度和胜利组成一份业务快照；门、压力板和钥匙分别复制必要状态。界面在绑定事件后补读当前结果，GameState 尚未到达时等待就绪事件，避免依赖某一次历史广播。
- **交互一致性与生命周期治理**：共用占用组件处理多碰撞体重叠、控制权变化、Pawn 销毁和触发体重绑定；合作门合并不同玩家集合。钥匙安装采用提交保护和失败恢复，目标通知防同步重入，重开请求经过服务器校验和去重。
- **DS 连接与失败恢复**：GameInstance 管理地址校验、连接期限、取消、有限重连和退出；按 World / NetDriver 过滤全局失败事件，用操作编号拒绝失效流程。某个玩家退出不关闭服务器，重连后重新获取仍在运行的 DS 保存的共享状态。
- **复制成本优化与数据验证**：静止机关采用网络休眠，变化前刷新休眠；移动平台采用受控的更新频率上限。历史 Listen 正式对照中，500 个合成压力板的复制作用域耗时降低约 **89.0%**，20 个合成平台的引擎发送量降低约 **36.2%**；两项分别统计复制 CPU 与引擎发送量。
- **移动基座与弱网定位**：角色复用原生 CharacterMovement；平台维护根组件速度和更新阶段，角色保留空中水平惯性。测试按同一 Move 时间戳关联双端记录，输出基座状态与校正时间线，并通过频率对照验证同步策略的取舍。
- **配置与自动化验证**：玩法参数集中到 JSON，启动时校验并完整提交，失败整表回退。统一脚本编排 DS 与两个客户端，覆盖玩法、晚加入、断线恢复、载人动作和复制规模实验，并检查各端断言、退出状态与采样有效性。

## 目录结构

~~~text
Config/                                  引擎、地图、输入与打包配置
Content/Config/Gameplay.json              人数、机关速度与重连等待参数
Content/UI/DSMenu.umap                    地址连接菜单关卡
Content/ThirdPerson/                      角色、输入与机关蓝图
Source/
  multiplayer.Target.cs                  游戏目标
  multiplayerEditor.Target.cs            编辑器开发目标
  multiplayerServer.Target.cs            独立服务器目标
  multiplayer/
    Core/                                权威规则、共享快照、JSON 与日志
    Network/                             GameInstance 连接生命周期
    Player/                              第三人称输入、CMC 与所属玩家请求
    Mechanisms/                          钥匙、插槽、压力板、门、平台与终点
    UI/                                  原生连接菜单与胜利界面
    Testing/                             三进程回归、连接单测与移动诊断
Scripts/
  RunLocalDedicatedServer.ps1             可操作双客户端 / 连接冒烟入口
  RunMultiplayerNetworkTests.ps1          统一回归与规模实验
  PlotRideMotionTimeline.py               平台与角色的实测时间线
  ExportNetworkEvidence.ps1               已完成实验的脱敏证据导出
Tests/                                   测试说明、报告结构与公开证据
Docs/NetworkDesign.md                     当前同步方案与职责
Docs/Performance.md                      实验设计、性能结果与移动诊断
~~~

业务和测试代码位于一个 `multiplayer` Runtime 模块，按职责组织目录。机关规则与共用组件集中在 `Mechanisms/`，复用占用统计和网格过渡逻辑。

## 玩法与同步架构

~~~mermaid
flowchart LR
    subgraph CLIENT["每个远端客户端"]
        CC["Character + CMC：输入、移动预测"]
        PC["PlayerController：请求与本地界面入口"]
        CGS["GameState 副本：共享结果"]
        VIEW["机关表现与本地 UI"]
        CGS -->|状态通知与绑定后补读| VIEW
        PC -->|创建本地界面| VIEW
    end

    subgraph SERVER["Dedicated Server"]
        SC["Character + CMC：移动处理与校验"]
        SPC["所属 PlayerController：接收请求"]
        MECH["机关 Actor：占用、安装、启停与局部规则"]
        GM["GameMode：进度登记、胜利复核与重开"]
        SGS["GameState：保存权威目标快照"]
        SC -->|服务器重叠事件| MECH
        MECH -->|登记目标 / 请求胜利复核| GM
        SPC -->|校验重开请求| GM
        GM -->|写入共享结果| SGS
    end

    CC -->|移动数据| SC
    SC -->|确认、校正与角色状态| CC
    PC -->|Server RPC| SPC
    SPC -->|Client RPC：失败反馈| PC
    SGS -->|属性复制| CGS
    MECH -->|开关、安装状态、附着与平台位移| VIEW
~~~

GameInstance 位于每个进程，管理配置与连接生命周期；合作进度由 GameState 发布。客户端的连接入口经 `ClientTravel` 加入 DS；进入游戏后，玩法由上图中的权威规则与复制链路接管。

### 状态由谁保管

| 模块 | 保存 / 判定什么 | 客户端如何使用 |
| --- | --- | --- |
| GameMode | 目标登记、终点人数规则、胜利复核、合法重开 | 客户端通过 GameState 共享快照获取结果 |
| CoopGameState | 目标总数、已完成数量、胜利状态 | 属性复制与 RepNotify；UI 绑定后补读 |
| GameInstance | 配置、服务器地址、连接阶段、重试计时与操作编号 | 本地菜单显示连接状态与操作反馈 |
| PlayerController | 所属玩家请求、重开等待、本地胜利展示状态 | Server RPC 提交请求，Client RPC 接收失败；只创建本地 UI |
| 机关 Actor | 服务器端安装、开关、激活条件和运动状态 | 复制必要结果，客户端重建对应表现 |
| PlayerOccupancyComponent | 服务器区域内的角色重叠记录与玩家资格 | 占用表留在服务器，由机关发布最终结果 |
| TransporterComponent | 服务器轨道位移、方向与根组件速度 | 由平台 Actor 的移动复制传递 |

### 钥匙、压力板与合作门

- **钥匙自动归位**：玩家触碰钥匙后，服务器确认触碰者为有效玩家角色且插槽绑定有效，将钥匙安装到预设 `DestinationSocket`。安装与目标登记经过提交保护；失败保留可重试状态，重复触碰不重复登记。安装位置使用 `KeyDisplayPoint`，客户端恢复安装状态与原生 Actor 附着。
- **共用占用统计**：以 `TMap<TWeakObjectPtr<ACharacter>, FOccupantRecord>` 记录每个角色的重叠次数和玩家资格。同一角色多个碰撞体不重复算人；销毁、控制器变化和 EndPlay 对称清理，弱引用不延长 Pawn 生命周期。
- **重绑定恢复**：绑定新触发体时从已有重叠重建记录；人数相同但成员变化仍能通知依赖规则，不要求玩家先走出去再回来。
- **合作门条件**：分别关注压力板开关与占用变化，去重重复配置的压力板，并合并不同玩家集合。同一人覆盖多块板，不会被算成多个合作玩家。
- **按需更新表现**：门与压力板复制开关及初始速度，客户端本地推进网格过渡，到位后停止相关 Tick；共用网格移动辅助函数，避免两套相同插值逻辑。

### 共享进度、胜利与重开

GameMode 完成规则判断后，通过 GameState 的唯一权威入口写入 `FmultiplayerCoopObjectiveState`。进度与胜利使用同一份业务快照及通知入口，减少对多个 OnRep 执行顺序的依赖；其他机关与附着状态通过各自复制路径更新。

服务器写入和客户端 OnRep 分别触发本机通知。广播前保存本次快照及胜利转换，避免监听者同步重入造成重复胜利广播。PlayerController 等待 GameState 就绪，订阅后补读当前结果，独立创建本地胜利界面。

重开由所属 Controller 提交可靠 Server RPC，GameMode 校验条件并去重；失败释放请求状态并反馈所属客户端。主动退出交回 GameInstance，统一停止重连、断开连接并返回菜单。

### 角色与移动平台

角色继承 `ACharacter`，本地输入进入原生 CharacterMovement，复用其预测、服务器校正与未确认移动重放。平台由服务器沿固定轨道运动，使用 Actor Movement Replication，默认更新频率上限为 **30 Hz**。

平台运动组件使用 `PrePhysics` 更新阶段，服务器按实际位移维护根组件速度，客户端从复制运动接收速度。开始、反向与停止时请求网络更新；角色空中制动设为 0，保留起跳时继承的水平惯性，空中继续独立移动。

轨道避障由关卡布置保证；Actor 位置复制与 CMC 预测是两条不同机制。载人诊断分别记录动作是否完成、同一历史 Move 的校正和基座状态，结果见下文。

## DS 连接与生命周期

当前入口为地址直连，连接状态使用显式枚举：

~~~text
Idle → Connecting → Connected
Connected → ReconnectWaiting → Reconnecting → Connected
连接失败 / 重试耗尽 → Idle
主动退出 → Leaving → 本地菜单加载完成 → Idle
~~~

- **输入校验与操作互斥**：仅接受 hostname / IPv4 与端口形式的地址，拒绝混入地图路径和 Travel 参数。忙碌时拒绝新请求，不覆盖现有连接地址。
- **成功确认**：本地 PlayerController 进入 `PlayingState` 后通知 GameInstance；连接期限覆盖“已经发出 Travel，但尚未进入游戏”的情况，不把请求发出当作成功。
- **取消与回调重入**：先提交连接状态，再通知监听者；后续 Travel 核对操作编号，防止通知过程中退出后，旧流程仍继续连接。连接期限回调检查操作编号，重连回调检查连接阶段。
- **全局事件归属**：网络失败按 World / GameInstance / NetDriver 过滤，避免同进程其他实例的错误污染本连接；服务端 Travel 失败转交 GameMode 恢复重开流程。
- **有限重连**：默认按 1、2、4 秒等待后重试，成功清理旧计时器，主动退出取消重试。重新连接后读取仍在运行的 DS 保存的共享进度和机关状态。
- **独立服务器生命周期**：任一客户端退出不关闭 DS，另一玩家继续游戏。服务器进程与本地启动脚本的资源清理由各自入口负责。

## 网络成本优化与工程措施

| 策略 | 实际减少的工作 / 处理的问题 |
| --- | --- |
| 静止机关休眠 | 门与压力板稳定时使用 `DORM_DormantAll`，减少没有变化的 Actor 反复参与复制检查 |
| 变化前刷新休眠 | 修改复制状态前 `FlushNetDormancy`，随后提交状态并请求更新；按状态变化触发唤醒 |
| 平台更新频率控制 | 持续运动平台采用 30 Hz 上限，在指定负载下减少引擎发送量；配置上限不等于实际收包频率 |
| 必要状态复制 | 占用表由服务器维护；门、压力板、钥匙与目标快照分别发布客户端需要的结果 |
| 初始参数复制 | 门和压力板的运行速度用 `COND_InitialOnly` 传递，让客户端表现使用服务器配置 |
| 事件驱动与按需 Tick | 占用、进度与连接由事件推进；门和压力板只在过渡期间更新，减少空闲扫描 |
| 生命周期对称清理 | 解除重叠、销毁、控制器及状态委托，停止计时任务，避免旧 World 或失效 Pawn 残留 |
| 幂等与重入保护 | 钥匙安装、目标提交、胜利通知与重开分别保护自己的提交边界；可靠 RPC 与业务去重分别处理 |

`ForceNetUpdate` 用于请求更新调度，客户端实际接收由远端断言核验。停止 Tick、降低更新频率和网络休眠分别控制不同成本，实验中分开施加与观测。

## 已验证的性能结果

以下为 **DS + 两个远端客户端** 的 Development Editor / NullRHI 记录。每例预热 5 秒、采样约 10 秒，各一次，用于确认迁移后的负载生成、复制与采样链路，与历史 Listen 正式对照分别统计。

复制耗时为 `ServerReplicateActors` 计时域均值；发送量为两条连接、背景机关及测试通信的引擎发送计数。

| 新增负载 | 设置 | 服务器复制均值 / ms | 服务器引擎发送量 / B/s |
| --- | --- | ---: | ---: |
| 500 个静止压力板 | 保持唤醒 | 2.298 | 3,493 |
| 500 个静止压力板 | 网络休眠 | 0.232 | 3,473 |
| 20 个运动平台 | 100 Hz 上限 | 0.572 | 27,323 |
| 20 个运动平台 | 30 Hz 上限 | 0.403 | 15,846 |

### 移动基座修复与时间线验证

平台测试从“站着被运输”扩展到站立、行走、反向、自由起跳与落回；按 Move 时间戳配对服务器处理记录和客户端历史移动，区分局部 / 世界坐标、基座变化与校正事件。

已完成基座速度维护、运动更新阶段与空中制动的行为对照，并采用保留水平惯性的自由跳跃规则。DS 迁移验证中，Normal / Moderate / Harsh 三档的载人动作断言均通过。

## 已完成实验与证据

| 阶段 | 验证的问题 | 结果与决策 |
| --- | --- | --- |
| 静止 / 运动规模对照 | 检查 CPU 与持续发送成本分别如何增长 | 42 轮正式对照；休眠控制静止检查，30 Hz 上限控制运动发送量 |
| 玩法与失败恢复回归 | 并发交互、晚加入、断线与重开是否正确 | 历史 Listen 构建 B 完成 15/15 回归，按版本保存断言和退出记录 |
| 动态载人专项 | 基座速度、空中制动与更新阶段如何影响跳跃 | 修正速度维护与更新阶段，采用保留空中惯性的玩法规则 |
| Move 时间戳与频率对照 | 提高平台频率对校正有什么影响 | 完成 30 / 60 Hz 各三轮交错对照，保留正式平台 30 Hz |
| DS 架构迁移 | 无主机玩家时，合作规则与失败恢复能否运行 | 当时版本 7 场景 × 3 档网络，共 21/21；另有收尾 Flow 2/2 与规模复核 4/4 |

- [网络性能与验证报告](Docs/Performance.md)：实验设计、参数对照、DS 复核和 Move 时间戳分析。
- [历史性能数据](Tests/Evidence/2026-09-27/performance.json)：42 轮规模对照、构建指纹与采样结果。
- [历史玩法回归](Tests/Evidence/2026-09-27/regression.json)：Listen 构建的逐场景断言及退出结果。
- [DS 迁移与验证记录](Tests/Evidence/2026-09-29/DSMigration.json)：DS 与两个远端客户端的回归与短规模复核。
- [公开实验摘要](Tests/Evidence/)：脱敏报告、断言和选定测量；完整本机 CSV、日志与时间线位于 `Saved/NetworkValidation/`。

每份报告保留对应构建、拓扑、玩法版本与逐项结果。当前关卡采用触碰后自动归位，历史 Keys 记录对应当时的携带路径。

## 源码导航

| 功能 | 主要实现 |
| --- | --- |
| 目标登记、胜利与重开 | [GameMode](Source/multiplayer/Core/multiplayerGameMode.cpp) |
| 共享快照与通知重入保护 | [CoopGameState](Source/multiplayer/Core/multiplayerCoopGameState.cpp) |
| 玩家输入与原生移动接入 | [Character](Source/multiplayer/Player/multiplayerCharacter.cpp) |
| 所属玩家 RPC 与胜利展示 | [CoopPlayerController](Source/multiplayer/Player/multiplayerCoopPlayerController.cpp) |
| 钥匙触碰与安装提交 | [CoopKey](Source/multiplayer/Mechanisms/multiplayerCoopKey.cpp)、[KeySocket](Source/multiplayer/Mechanisms/multiplayerKeySocket.cpp) |
| 占用记录与生命周期 | [PlayerOccupancyComponent](Source/multiplayer/Mechanisms/multiplayerPlayerOccupancyComponent.cpp) |
| 合作门与压力板 | [CoopGate](Source/multiplayer/Mechanisms/multiplayerCoopGate.cpp)、[PressurePlate](Source/multiplayer/Mechanisms/multiplayerPressurePlate.cpp) |
| 平台启停与轨道运动 | [MovingPlatform](Source/multiplayer/Mechanisms/multiplayerMovingPlatform.cpp)、[TransporterComponent](Source/multiplayer/Mechanisms/multiplayerTransporterComponent.cpp) |
| 门 / 压力板共用过渡 | [MeshMovement](Source/multiplayer/Mechanisms/multiplayerMeshMovement.h) |
| 终点条件汇合 | [WinArea](Source/multiplayer/Mechanisms/multiplayerWinArea.cpp) |
| 连接、取消与有限重连 | [GameInstance](Source/multiplayer/Network/multiplayerGameInstance.cpp) |
| JSON 校验与配置读取 | [GameplayConfig](Source/multiplayer/Core/multiplayerGameplayConfig.cpp) |
| 菜单与胜利界面 | [ConnectionMenu](Source/multiplayer/UI/multiplayerConnectionMenu.cpp)、[VictoryWidget](Source/multiplayer/UI/multiplayerVictoryWidget.cpp) |
| 自动场景与移动诊断 | [Testing](Source/multiplayer/Testing/)、[统一测试入口](Scripts/RunMultiplayerNetworkTests.ps1) |

函数按生命周期、对外业务入口、引擎事件和内部清理组织。共同机制抽到占用组件与网格移动辅助函数；机关各自保留规则，连接状态不承担合作玩法结算。

## 玩法配置

[Gameplay.json](Content/Config/Gameplay.json) 在启动时读取一次。解析器校验版本、字段类型、范围和字段关系，全部通过后才提交；读取或校验失败整表回退，避免混用部分新配置。不在 Tick 读磁盘。

| 字段 | 默认值 | 含义 |
| --- | ---: | --- |
| SchemaVersion | 1 | 配置格式版本 |
| SessionMaxPlayers | 2 | 兼容字段名，表示 DS 远端玩家上限，不包含服务器 |
| WinRequiredPlayers | 2 | 终点所需不同玩家数 |
| PlatformRequiredPlayers | 1 | 平台自身占用模式的人数要求 |
| PlatformMoveSpeed | 150 | 平台速度，cm/s |
| DoorMoveSpeed | 250 | 门速度，cm/s |
| PlateMoveSpeed | 80 | 压力板速度，cm/s |
| ReconnectDelaysSeconds | [1, 2, 4] | 重试等待秒数；空数组关闭自动重连 |

模型、碰撞、轨道端点、压力板关联和钥匙目标插槽由蓝图 / 关卡配置。JSON 随包作为外置配置部署，修改后重启生效；人数参数与关卡出生点、机关条件配套配置。

## 构建与验证

环境：Unreal Engine 5.5（现有实测为 5.5.4）、Visual Studio 2022、C++ 游戏开发工作负载与 Windows SDK。生成工程文件后，先编译 `multiplayerEditor` 的 Development Editor 配置。

### 本机 DS 与双客户端

在项目根目录运行：

~~~powershell
# 一个 DS + 两个可操作客户端；也可双击 LaunchTwoPlayers.bat。
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777

# 无画面连接冒烟，完成断言后自动退出。
.\Scripts\RunLocalDedicatedServer.ps1
~~~

默认客户端菜单为 `/Game/UI/DSMenu`，游戏地图为 `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`。菜单输入 `127.0.0.1:7777` 连接本机；跨机器使用服务器局域网地址。脚本支持 `-EditorPath` 覆盖引擎路径，批处理支持 `UE_EDITOR` 环境变量。

本机开发与回归采用 **UE 5.5.4 Editor Server**：通过 `-server` 启动独立服务进程，运行模式为 `NM_DedicatedServer`，两个玩家分别由远端客户端连接。

单个客户端退出不会关闭 DS；本地手动启动脚本在两个窗口都关闭或自身退出时，清理它启动的进程。地址连接使用服务器可达地址和游戏端口。

### 自动回归与规模实验

~~~powershell
# 脚本和报告判定自测，不启动 UE。
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest

# DS + 两客户端，七类场景与三档模拟网络。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Normal,Moderate,Harsh -Repeat 1

# 同条件重复的平台动作测试。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -Profiles Harsh -Repeat 3

# 两种负载分别对照，避免变量混杂。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Static -StaticCounts 0,50,200,500 -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Suite Scale -Matrix Moving -MovingCounts 1,5,20 -Repeat 3
~~~

| 场景 | 自动核验内容 |
| --- | --- |
| Flow | 自动归位、不同玩家计数、触发体重绑定、机关开关、占用清理、平台、双端胜利与重开 |
| Keys | 缺绑定 / 安装失败、重试、重复触碰、目标上限、提交重入与远端附着 |
| LateJoin | 后加入者收到既有进度、休眠机关状态与钥匙附着 |
| Reconnect | 单连接真实断包至超时、有限重连、共享状态恢复，DS 与另一玩家保持 |
| ConnectionRetry | 不可达地址失败后释放忙碌状态，再连接有效服务器 |
| Ride / RideMotion | CMC 基座、运输、站立、行走、反向与自由跳跃，输出分阶段数据与历史 Move 记录 |
| Scale | 实际对象数量、客户端接收与运动证据、服务器复制耗时及发送计数 |

Normal / Moderate / Harsh 分别设置 `PktLag / Variance / Loss` 为 `0 / 0 / 0%`、`100 / 20 / 2%`、`200 / 50 / 5%`，参数施加到三个进程，不能直接当作实测 RTT。断线场景另外阻断指定连接的服务器出站包直到真实超时，不以主动调用断线回调代替故障。

通过要求同时满足各端必需断言、完成回执、正常退出和致命日志检查；规模实验同时检查 CPU 采样有效性。自动回归采用 NullRHI，验证状态、动作断言和数值记录。命令、参数和证据口径统一见 [Tests/README.md](Tests/README.md)，设计说明见 [Docs/NetworkDesign.md](Docs/NetworkDesign.md)。
