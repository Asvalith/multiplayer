# multiplayer · UE5 C++ 双人 Co-op

基于 Unreal Engine 5.5 和 C++ 的双人合作 Demo，采用 **一个 Dedicated Server + 两个远端客户端**，实现地址连接、钥匙自动归位、压力板与合作门、移动平台、目标进度、双人胜利与重开。

项目重点是服务器权威的玩法规则、可恢复的状态复制、连接生命周期管理，以及通过重复对照和历史 Move 配对分析网络成本与同步误差。

## 项目亮点

- **服务器权威的合作玩法**：服务器处理钥匙触碰与安装、不同玩家计数、机关激活和胜利判定；客户端接收结果并更新表现。GameMode、GameState、PlayerController、HUD 与机关 Actor 分工明确，分别管理连接就绪与玩法状态就绪。
- **状态同步与晚加入恢复**：目标进度和胜利组成一份业务快照；门、压力板和钥匙分别复制必要状态。界面在绑定事件后补读当前结果，GameState 尚未到达时等待就绪事件，避免依赖某一次历史广播。
- **交互一致性与生命周期治理**：共用占用组件处理多碰撞体重叠、控制权变化、Pawn 销毁和触发体重绑定；合作门合并不同玩家集合。钥匙安装采用提交保护和失败恢复，目标通知防同步重入，重开请求经过服务器校验和去重。
- **DS 连接与失败恢复**：GameInstance 管理地址校验、连接期限、取消、有限重连和退出；按 World / NetDriver 过滤全局失败事件，用操作编号拒绝失效流程。某个玩家退出不关闭服务器，重连后重新获取仍在运行的 DS 保存的共享状态。
- **复制成本优化与数据验证**：静止机关采用网络休眠，变化前刷新休眠；移动平台采用受控的更新频率上限。历史 Listen 正式对照中，500 个合成压力板的复制作用域耗时降低约 **89.0%**，20 个合成平台的引擎发送量降低约 **36.2%**；两项分别统计复制 CPU 与引擎发送量。
- **移动基座与弱网定位**：角色复用原生 CharacterMovement；平台维护根组件速度和更新阶段，角色保留空中水平惯性。测试按同一 Move 时间戳关联双端记录，输出基座状态与校正时间线，并通过频率对照验证同步策略的取舍。
- **配置与自动化验证**：玩法参数集中到 JSON，启动时校验并完整提交，失败整表回退。平台专项脚本编排 DS 与两个客户端，覆盖站立、行走、反向和自由跳跃，检查各端动作、退出状态与采样有效性；既有玩法回归与规模对照保留分版本报告。

## GAS 基础战斗

`codex/gas` 分支在原有合作角色上接入 GAS，使用独立关卡 `/Game/Combat/Maps/CombatTest` 验证战斗，不替换原机关关卡。

- **左键攻击**：服务器生成能量弹，命中敌人造成 25 点伤害，冷却 0.5 秒；队友不会受到伤害。
- **Q 防御**：消耗 20 能量，持续 3 秒、减伤 50%，冷却 8 秒；所属客户端先预测消耗、冷却和防御效果，服务器确认或拒绝后由 GAS 核对结果。
- **E 治疗**：消耗 25 能量，治疗 600cm 内最低血量比例的受伤友军或自己 30 点，冷却 6 秒；没有合法目标时不消耗，不能穿墙治疗。
- **服务器维护战斗状态**：GAS 管理属性、消耗、冷却和状态标签；玩家与敌人共用伤害和死亡处理。死亡后取消技能、停止移动并退出机关占用，敌人额外停止 AI 并清理尸体。
- **基础敌人**：服务器生成 3 个敌人，使用 NavMesh 追击存活玩家，进入射程且视线无遮挡后通过 GAS 攻击。

~~~powershell
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Map /Game/Combat/Maps/CombatTest -Port 7777
~~~

攻击和治疗使用 ServerOnly，防御使用 LocalPredicted；请求经过所属角色的 ASC，伤害、治疗目标及最终战斗状态由服务器决定。角色移动仍使用原生 CMC。独立战斗 HUD 使用 UMG 蓝图显示血量、能量、防御状态及剩余时间，原合作 HUD 保持不变。

已通过编译、基础规则回归和一个 DS 加两个客户端的 Normal、Harsh 战斗同步测试，检查真实预测键的确认、资源不足拒绝、死亡拒绝及两端最终状态。技能与对应效果按攻击、防御、治疗分组，敌人生成和追击集中在同一组源码中。

## 目录结构

~~~text
Config/                                  引擎、地图、输入与打包配置
Content/Config/Gameplay.json              目标数量、人数、机关速度与重连等待参数
Content/UI/DSMenu.umap                    地址连接菜单关卡
Content/UI/WBPmainmenu.uasset             UMG 地址输入、连接与取消按钮
Content/UI/winandquit.uasset              UMG 胜利、重开与返回菜单按钮
Content/UI/BP_MenuController.uasset       菜单界面生命周期与输入模式
Content/UI/BP_MenuGameMode.uasset         菜单控制器配置，不生成 Pawn
Content/UI/BP_CoopHUD.uasset              胜利订阅、快照补读与本地界面管理
Content/UI/BP_CoopGameState.uasset        继承 C++ 共享状态，就绪后通知本地 HUD
Content/ThirdPerson/                      角色、输入与机关蓝图
Content/Combat/                           独立战斗关卡与 UMG 战斗 HUD
Source/
  multiplayer.Target.cs                  游戏目标
  multiplayerEditor.Target.cs            编辑器开发目标
  multiplayerServer.Target.cs            独立服务器目标
  multiplayer/
    Core/                                权威规则、共享快照、JSON 与日志
    Network/                             GameInstance 连接生命周期
    Player/                              第三人称输入、CMC 与所属玩家请求
    Mechanisms/                          钥匙、插槽、压力板、门、平台与终点
    Combat/                              GAS 属性、技能、效果、能量弹与基础敌人
    Testing/                             三进程平台、战斗回归与移动诊断
Scripts/
  RunLocalDedicatedServer.ps1             可操作双客户端 / 平台实验入口
  RunMultiplayerNetworkTests.ps1          平台与战斗双客户端弱网回归
  PlotRideMotionTimeline.py               平台与角色的实测时间线
  ExportNetworkEvidence.ps1               已完成实验的脱敏证据导出
Tests/                                   测试说明、报告结构与公开证据
Docs/NetworkDesign.md                     当前同步方案与职责
Docs/Reports/                            分阶段实验、数据附录与问题复盘
~~~

业务和测试代码位于一个 `multiplayer` Runtime 模块，按职责组织目录。机关规则与共用组件集中在 `Mechanisms/`，复用占用统计和网格过渡逻辑。

## 玩法与同步架构

~~~mermaid
flowchart LR
    subgraph CLIENT["每个远端客户端"]
        CC["Character + CMC：输入、移动预测"]
        PC["PlayerController：请求、RPC 与操作状态"]
        CGS["GameState 副本：共享结果"]
        HUD["BP_CoopHUD：胜利订阅与界面管理"]
        VIEW["机关表现与本地 UI"]
        CGS -->|状态通知与绑定后补读| HUD
        HUD -->|创建界面与管理输入| VIEW
        PC -->|本地操作反馈| VIEW
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
| CoopGameState | 目标总数、已完成数量、胜利状态 | 属性复制与 RepNotify；HUD 绑定后补读 |
| GameInstance | 配置、服务器地址、连接阶段、重试计时与操作编号 | 本地菜单显示连接状态与操作反馈 |
| PlayerController | 所属玩家请求、重开 / 退出操作状态 | Server RPC 提交请求，Client RPC 接收失败；本地委托发布操作反馈 |
| BP_CoopHUD / winandquit | 本地胜利订阅、界面生命周期与输入模式 | HUD 与 GameState 就绪时相互衔接并补读胜利快照；Widget 展示反馈、转交按钮意图 |
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

服务器写入和客户端 OnRep 分别触发本机通知。广播前保存本次快照及胜利转换，避免监听者同步重入造成重复胜利广播。GameMode 指定的 CoopHUD 等待 GameState 就绪，订阅后补读当前结果，负责本地胜利界面的创建、清理与输入模式。

重开由所属 Controller 提交可靠 Server RPC，GameMode 校验条件并去重；失败释放请求状态并反馈所属客户端。Controller 通过本地 `OnVictoryActionChanged` 委托向 Widget 发布操作反馈。主动退出交回 GameInstance，统一停止重连、断开连接并返回菜单。

界面布局、按钮事件和状态绑定保存在 `WBPmainmenu`、`winandquit` 两个 UMG 蓝图中，直接继承 `UserWidget`。`BP_MenuController` 与 `BP_CoopHUD` 负责创建、清理和输入模式；C++ 只提供 GameInstance / Controller 的请求、查询和状态通知接口，没有自定义 C++ UI 基类。

`BP_CoopHUD` 就绪时尝试获取 GameState；`BP_CoopGameState` 就绪时也通知已有的本地 HUD，覆盖两者不同的到达顺序。HUD 先订阅胜利事件，再读取当前快照，重复初始化不会重复创建界面；退出时解除绑定。这个约定针对本项目每个 World 一份 GameState 的生命周期，不依赖 Tick 轮询。

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

- [网络实验总报告](Docs/Reports/2026-09-27/NetworkExperimentReport.md)：设计、对照、发现与取舍。
- [性能与回归附录](Docs/Reports/2026-09-27/Appendix.md)：逐轮数据、构建指纹与原始文件索引。
- [平台 Move 时间戳分析](Docs/Reports/2026-09-27/PlatformMoveTimestampValidation.md)：误差参考系、对应事件与频率实验。
- [DS 迁移与验证记录](Docs/Reports/2026-09-29/DSMigration.md)：当前拓扑的迁移证据与采样口径。
- [公开实验摘要](Tests/Evidence/)：脱敏报告、断言和选定测量；完整本机 CSV、日志与时间线位于 `Saved/NetworkValidation/`。

每份报告保留对应构建、拓扑、玩法版本与逐项结果。当前关卡采用触碰后自动归位，历史 Keys 记录对应当时的携带路径。

## 源码导航

| 功能 | 主要实现 |
| --- | --- |
| 目标登记、胜利与重开 | [GameMode](Source/multiplayer/Core/multiplayerGameMode.cpp) |
| 共享快照与通知重入保护 | [CoopGameState](Source/multiplayer/Core/multiplayerCoopGameState.cpp) |
| 玩家输入与原生移动接入 | [Character](Source/multiplayer/Player/multiplayerCharacter.cpp) |
| 所属玩家 RPC 与操作状态 | [CoopPlayerController](Source/multiplayer/Player/multiplayerCoopPlayerController.cpp) |
| 钥匙触碰与安装提交 | [CoopKey](Source/multiplayer/Mechanisms/multiplayerCoopKey.cpp)、[KeySocket](Source/multiplayer/Mechanisms/multiplayerKeySocket.cpp) |
| 占用记录与生命周期 | [PlayerOccupancyComponent](Source/multiplayer/Mechanisms/multiplayerPlayerOccupancyComponent.cpp) |
| 合作门与压力板 | [CoopGate](Source/multiplayer/Mechanisms/multiplayerCoopGate.cpp)、[PressurePlate](Source/multiplayer/Mechanisms/multiplayerPressurePlate.cpp) |
| 平台启停与轨道运动 | [MovingPlatform](Source/multiplayer/Mechanisms/multiplayerMovingPlatform.cpp)、[TransporterComponent](Source/multiplayer/Mechanisms/multiplayerTransporterComponent.cpp) |
| 门 / 压力板共用过渡 | [MeshMovement](Source/multiplayer/Mechanisms/multiplayerMeshMovement.h) |
| 终点条件汇合 | [WinArea](Source/multiplayer/Mechanisms/multiplayerWinArea.cpp) |
| 连接、取消与有限重连 | [GameInstance](Source/multiplayer/Network/multiplayerGameInstance.cpp) |
| JSON 校验与配置读取 | [GameplayConfig](Source/multiplayer/Core/multiplayerGameplayConfig.cpp) |
| 菜单、胜利 HUD 与界面 | [Content/UI](Content/UI/) 中的 UMG、菜单控制器与 HUD 蓝图 |
| 平台实验与移动诊断 | [Testing](Source/multiplayer/Testing/)、[统一测试入口](Scripts/RunMultiplayerNetworkTests.ps1) |

函数按生命周期、对外业务入口、引擎事件和内部清理组织。共同机制抽到占用组件与网格移动辅助函数；机关各自保留规则，连接状态不承担合作玩法结算。

## 玩法配置

[Gameplay.json](Content/Config/Gameplay.json) 在启动时读取一次。解析器校验版本、字段类型、范围和字段关系，全部通过后才提交；读取或校验失败整表回退，避免混用部分新配置。不在 Tick 读磁盘。

| 字段 | 默认值 | 含义 |
| --- | ---: | --- |
| SchemaVersion | 1 | 配置格式版本 |
| SessionMaxPlayers | 2 | 兼容字段名，表示 DS 远端玩家上限，不包含服务器 |
| RequiredKeys | 4 | 本关需要完成的插槽数量，正整数；服务器写入 GameState 后复制给客户端 |
| WinRequiredPlayers | 2 | 终点所需不同玩家数 |
| PlatformRequiredPlayers | 1 | 平台自身占用模式的人数要求 |
| PlatformMoveSpeed | 150 | 平台速度，cm/s |
| DoorMoveSpeed | 250 | 门速度，cm/s |
| PlateMoveSpeed | 80 | 压力板速度，cm/s |
| ReconnectDelaysSeconds | [1, 2, 4] | 重试等待秒数；空数组关闭自动重连 |

模型、碰撞、轨道端点、压力板关联和钥匙目标插槽由蓝图 / 关卡配置。JSON 随包作为外置配置部署，修改后重启生效；人数参数与关卡出生点、机关条件配套配置。`RequiredKeys` 是胜利所需的目标数量，不再自动统计场景插槽；应与关卡可完成的钥匙 / 插槽配套，填大可能无法通关，填小会提前达到目标。

## 构建与验证

环境：Unreal Engine 5.5（现有实测为 5.5.4）、Visual Studio 2022、C++ 游戏开发工作负载与 Windows SDK。生成工程文件后，先编译 `multiplayerEditor` 的 Development Editor 配置。

### 本机 DS 与双客户端

在项目根目录运行：

~~~powershell
# 一个 DS + 两个可操作客户端；也可双击 LaunchTwoPlayers.bat。
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777

# 无画面平台实验，完成断言与采样后自动退出。
.\Scripts\RunLocalDedicatedServer.ps1
~~~

默认客户端菜单为 `/Game/UI/DSMenu`，游戏地图为 `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`。菜单输入 `127.0.0.1:7777` 连接本机；跨机器使用服务器局域网地址。脚本支持 `-EditorPath` 覆盖引擎路径，批处理支持 `UE_EDITOR` 环境变量。

本机开发与回归采用 **UE 5.5.4 Editor Server**：通过 `-server` 启动独立服务进程，运行模式为 `NM_DedicatedServer`，两个玩家分别由远端客户端连接。

单个客户端退出不会关闭 DS；本地手动启动脚本在两个窗口都关闭或自身退出时，清理它启动的进程。地址连接使用服务器可达地址和游戏端口。

### 平台弱网专项实验

~~~powershell
# 脚本和报告判定自测，不启动 UE。
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest

# DS + 两客户端，站立、行走、反向和自由跳跃，三档模拟网络。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Normal,Moderate,Harsh -Repeat 1

# 同条件重复与静止平台对照。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -PlatformMotion Static -Repeat 1
~~~

服务器和受测客户端分别保存四阶段指标及历史 Move 记录，按移动指令时间戳关联两端状态，输出跳跃、基座切换与校正时间线。Partner 独立确认连接就绪和实验完成。

Normal / Moderate / Harsh 分别设置 `PktLag / Variance / Loss` 为 `0 / 0 / 0%`、`100 / 20 / 2%`、`200 / 50 / 5%`，参数施加到三个进程，不能直接当作实测 RTT。

通过要求同时满足各端必需断言、完成回执、正常退出、致命日志检查与阶段采样有效性。默认 NullRHI 的数值实验不替代人工视觉验收。命令与口径见 [Tests/README.md](Tests/README.md)，设计见 [Docs/NetworkDesign.md](Docs/NetworkDesign.md)；既有玩法与规模实验见前面的分版本报告。
