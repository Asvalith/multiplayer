# UE5 双人 Co-op 网络协作 Demo

这是一个使用 Unreal Engine 5.5 和 C++ 开发的双人局域网合作原型。
当前版本实现了局域网房间创建、搜索、加入和退出，以及钥匙目标、压力板、门、移动平台、共享进度、双人胜利、胜利界面和当前关卡重开。所有会影响玩法结果的状态由服务器维护，客户端接收复制状态并更新本地表现。

## 已实现功能

### 局域网会话

- 基于 `OnlineSubsystem NULL` 创建、搜索和加入局域网房间。
- 使用明确的异步操作状态限制重复点击，避免创建、搜索和加入请求相互覆盖。
- 创建新房间前先销毁已有同名 Session，并在完成回调后继续建房。
- 建房只让当前 World 原地成为 Listen Server 并发布当前地图，不自动切换到其他地图。
- 加入成功后解析连接地址，再由本地 `PlayerController` 发起连接。
- 主动退出时先关闭自动重连并销毁本地 Session；主机退出会通知远端执行相同清理，再回主菜单。
- 监听网络失败和地图加载失败，并输出可定位的项目日志。

### 服务器权威的协作玩法

- 玩家进入钥匙的 Overlap 范围后，服务器校验钥匙及其目标插槽，钥匙自动吸附（Snap）到对应插槽并登记目标进度。
- 钥匙插槽只接受符合条件的钥匙，并且每个插槽最多登记一次进度。
- 压力板在服务器统计玩家，门根据关联压力板和共享目标决定是否开启。
- 移动平台由服务器决定启停和位置，客户端接收 Actor 位移同步。
- 胜利区域上报当前玩家数量，`GameMode` 再复核钥匙目标、人数和既有胜利状态。
- `GameState` 将钥匙进度、目标数量和胜利结果作为一份完整快照同步给参与玩家。
- 胜利后每名本地玩家都会看到 C++ 默认界面，可选择重新加载当前关卡或退出房间；蓝图事件只用于扩展美术表现。
- 重开请求由服务器确认当前已经胜利，并只接受两名玩家中的第一个有效请求。

### 并发与生命周期处理

- 钥匙和插槽在修改状态前检查 Authority、钥匙完成状态和插槽状态，避免同一把钥匙或同一插槽被重复计数。
- 区域统计按“不同玩家”计算，并记录同一角色的多个碰撞组件，避免一个角色被重复计数。
- 玩家 Pawn 销毁时主动清理区域占用记录。
- Actor 或组件结束生命周期时移除外部 Delegate，避免残留回调。
- 相同的目标快照不会重复广播或强制网络更新。

### 自动短线重连

- 客户端保存最近一次有效连接地址。
- 意外断线后按照 JSON 配置的间隔有限重试，默认等待 1、2、4 秒；空数组关闭自动重连。
- 只有本地控制器重新进入 `PlayingState`，才认为重连成功。
- 主动退出、地址无效或达到重试上限时结束重连流程。
- 重连后重新读取服务器上的共享目标；不会恢复断线前的位置或未提交的本地表现状态。

## 目录与代码架构

项目只有一个 C++ 游戏模块 `multiplayer`，玩法代码按职责分为五个目录；另设 `Testing` 保存开发测试驱动，不把测试步骤混入机关和会话业务。宝物和插槽与其他机关统一放在 `Mechanisms` 中，同名 `.h` / `.cpp` 放在一起。

下面列出主要维护目录；同名源码文件组省略 `.h` / `.cpp` 后缀，独立文件保留后缀。`Content` 中的美术和关卡资源不逐项展开。

```text
multiplayer/
├── multiplayer.uproject                 UE 项目入口
├── README.md                            项目说明、配置与运行方式
├── LaunchTwoPlayers.bat                 从主菜单启动两个游戏窗口
├── Docs/                               技术设计与实验复盘
│   ├── NetworkDesign.md                网络状态、失败恢复和验证边界
│   ├── Reports/2026-09-27/              实验报告、踩坑报告、动态载人专项与数据附录
│   └── Study/UE网络同步技术博客.md       网络基础、同步模型、UE 机制与项目复盘（单文件）
├── Scripts/                             开发验证工具
│   ├── RunMultiplayerNetworkTests.ps1     双进程回归与单变量压测入口
│   └── ExportNetworkEvidence.ps1          已结束报告的脱敏摘录与 CSV 指纹校验
├── Tests/                               测试说明与报告结构
│   ├── README.md                         命令、断言与结果解读
│   ├── NetworkReport.schema.json         原始报告字段约定
│   └── Evidence/                         按日期保存实测证据与解释边界
├── Config/                             引擎、输入、编辑器和打包设置
│   ├── DefaultEngine.ini
│   ├── DefaultGame.ini
│   ├── DefaultInput.ini
│   └── DefaultEditor.ini
├── Content/                            蓝图、关卡和美术资源
│   ├── Config/Gameplay.json             外置人数、速度和重连参数
│   └── …
└── Source/
    ├── multiplayer.Target.cs            游戏构建目标
    ├── multiplayerEditor.Target.cs      编辑器构建目标
    └── multiplayer/
        ├── multiplayer.Build.cs         模块依赖与头文件搜索路径
        ├── multiplayer                  模块入口（.h / .cpp）
        ├── Core/                        规则、共享状态和配置
        │   ├── multiplayerGameMode                  目标登记、胜利复核与重开规则
        │   ├── multiplayerCoopGameState             共享目标快照复制与本地通知
        │   ├── multiplayerGameplayConfig           JSON 读取、校验与默认值
        │   └── multiplayerLog                      项目日志分类
        ├── Network/                     会话与连接生命周期
        │   └── multiplayerGameInstance             建房、搜索、加入、退出和重连
        ├── Player/                      输入与所属玩家操作
        │   ├── multiplayerCharacter                输入、移动与视角
        │   └── multiplayerCoopPlayerController     连接确认、本地 UI 与重开/退出入口
        ├── Mechanisms/                  宝物与协作机关
        │   ├── multiplayerCoopKey                  宝物触碰处理、归位与状态复制
        │   ├── multiplayerKeySocket                宝物校验与一次性目标登记
        │   ├── multiplayerCoopCarryComponent       备用携带路径的服务器单槽缓存
        │   ├── multiplayerPressurePlate            人数/目标条件与压下状态
        │   ├── multiplayerCoopGate                 压力板组合与开门条件
        │   ├── multiplayerMovingPlatform           平台启停条件与 Actor 位移复制
        │   ├── multiplayerTransporterComponent     服务器端固定轨道移动
        │   ├── multiplayerPlayerOccupancyComponent 区域玩家去重、占用变化与销毁清理
        │   └── multiplayerWinArea                  终点人数统计与胜利检查请求
        ├── UI/                          本地胜利表现
        │   ├── multiplayerVictoryPresenterComponent 状态监听、晚绑定补读与通知去重
        │   └── multiplayerVictoryWidget             默认胜利界面与按钮
        └── Testing/                     开发测试，不承担玩法规则
            ├── CoopNetTestDriver                    双端驱动、回执与采样
            ├── CoopNetTestScenarios.cpp              合成回归场景
            ├── CoopPlatformRideProbe                动态载人四阶段输入、采样与双端验证
            └── CoopRideTestCharacter                记录 CMC 校正的测试角色与移动组件
```

职责边界：

- **配置**：`GameInstance::Init` 读取一次 `Gameplay.json` 并保存配置；各玩法对象读取只读参数，不在 Tick 中读文件。根目录 `Config` 放 UE 设置，`Content/Config` 放外置玩法表，二者用途不同。
- **规则与同步**：机关在服务器处理各自条件；插槽和胜利区域向 `GameMode` 报告目标或人数，`GameMode` 复核后更新 `GameState`。不是所有机关状态都经由 `GameMode`，压力板、门和平台各自维护并同步自己的状态。
- **公共组件**：压力板、平台和终点复用 `PlayerOccupancyComponent` 统计玩家；平台用 `TransporterComponent` 计算移动，再由平台 Actor 复制位置。
- **界面**：`VictoryPresenterComponent` 监听胜利状态并处理绑定时序，调用所属 `PlayerController` 创建本地 `VictoryWidget`；界面本身不决定胜负。
- **宝物路径**：当前关卡以预绑定插槽、触碰后自动归位为主；`CoopCarryComponent` 服务于代码保留的备用携带路径，不代表当前玩法包含背包、装备或手持交互系统。

核心源码位于 [Source/multiplayer](Source/multiplayer)，资源位于 [Content](Content)。`Binaries`、`Intermediate`、`Saved`、`DerivedDataCache` 和 `.vs` 是构建、运行或工具生成内容，不属于玩法架构。

## JSON 玩法配置

配置文件：[Content/Config/Gameplay.json](Content/Config/Gameplay.json)。游戏实例启动时读取一次，修改后重新启动游戏；无需重新编译 C++。下面的默认数值同时也是配置失败时的回退值。

| 字段 | 默认值 | 用途与范围 |
| --- | --- | --- |
| `SchemaVersion` | `1` | 配置格式版本，目前只接受 1 |
| `SessionMaxPlayers` | `2` | 房间总人数，含主机，范围 2～8；同时用于 Session 容量与服务器登录检查 |
| `WinRequiredPlayers` | `2` | 终点所需不同玩家数，1～房间总人数 |
| `PlatformRequiredPlayers` | `1` | 自身占用模式的平台所需人数，1～房间总人数；外部压力板模式不使用此值 |
| `PlatformMoveSpeed` | `150` | 平台移动速度，单位 cm/s，范围 1～5000 |
| `DoorMoveSpeed` | `250` | 门的移动速度，单位 cm/s，范围 1～5000 |
| `PlateMoveSpeed` | `80` | 压力板压下和弹起速度，单位 cm/s，范围 1～5000 |
| `ReconnectDelaysSeconds` | `[1, 2, 4]` | 每次重连前等待的秒数；最多 5 次，每项 0.1～60 秒且不能递减；`[]` 表示关闭 |

例如将 `PlatformMoveSpeed` 改成 `300`，重启后平台按 300 cm/s 运动；把重连间隔改成 `[2, 5]`，则最多尝试两次。两端玩法仍以服务器结果为准。

配置边界：

- 表内人数和速度统一接管对应 C++ / 蓝图数值，同类机关共用这些参数。现有 `HostGame` 蓝图节点保留人数引脚以兼容资产，但实际容量使用 JSON，不再使用旧引脚值。
- 模型、碰撞尺寸、运动端点、压力板与门的关联、钥匙与插槽的关联仍在蓝图或关卡中设置。目标数量仍统计实际插槽，不维护另一份 JSON 目标数。
- 服务器决定人数规则和平台速度；门与压力板在初始复制时将速度发送给客户端，之后仍只同步开关变化，不持续复制网格位置。客户端修改自己的 JSON 不能改变服务器规则。
- 所有字段必填。缺文件、格式错误、未知字段、类型错误或数值越界时，整份配置回退默认值并记录 `LogMultiplayer` 警告，不会混用半份新配置。文件大小上限为 64 KiB。
- 打包配置将 `Content/Config` 作为外置非资产目录复制，位于打包结果的 `multiplayer/Content/Config/Gameplay.json`；保留此文件随包分发，改表后重启生效。不支持运行中热更新，也不提供可视化配置编辑器。
- 修改人数不会自动增加出生点、重排机关或改变门的压力板要求；项目仍以双人关卡为主要验证场景。

配置接入验证（UE 5.5，2026-09-16）：

- Development Editor 与 Shipping 编译通过。
- 在实际合作关卡中无画面启动验证：修改人数/速度及空重连数组生效；类型错误、小数人数、非法速度、缺字段和错误 JSON 均完整回退默认值。
- 本机主客使用不同配置：主机门/板速度为 375/95，客户端本地表均为 900，客户端实际运行值仍为 375/95；2 人上限下第三名玩家收到 `Server full.`。
- 此次验证覆盖配置读取、参数接线和本机直连，不等于完成全部机关交互、菜单、弱网、画面或打包后双机验收。外置文件的打包复制规则已配置，完整打包验收仍需单独执行。

调试时在 Development 日志中搜索 `Gameplay config` 查看加载或回退原因；Shipping 是否输出日志取决于构建的日志设置。

## 网络设计

### 规则与共享状态分离

`GameMode` 只存在于服务器，负责判断操作是否有效；`GameState` 负责把已经确认的共享结果复制给客户端。机关 Actor 不能直接修改胜利结果，只能向规则层报告当前事实。

### 按对象选择同步方式

- 角色移动使用 `CharacterMovementComponent` 自带的预测与服务器校正。
- 压力板和门只复制影响表现的关键状态。
- 钥匙复制完成状态，并在需要时同步服务器确认的位置。
- 移动平台由服务器移动，通过 Actor Movement Replication 同步位置。
- 目标进度和胜利结果由 `GameState` 统一复制。

### 控制无意义更新

- 静止机关不持续 Tick。
- 钥匙只在尚未归位时启用旋转 Tick，吸附到插槽后关闭。
- 移动平台仅在尚未到达目标点时启用 Tick。
- 相同状态不重复提交，不依赖高频 RPC 刷新机关表现。
- 门和压力板在状态不变时网络休眠；服务器在修改复制属性之前刷新休眠，再提交新状态。网络休眠与关闭本地 Tick 是两件事。

各对象的职责、同步限制与失败恢复见 [网络设计](Docs/NetworkDesign.md)。平台仍使用普通 Actor 位置复制，没有新增自研预测或平滑算法。

## 关键流程

### 钥匙目标

```text
玩家进入钥匙的 Overlap 范围
→ 服务器检查钥匙状态和预绑定的目标插槽
→ 钥匙自动吸附（Snap）到对应插槽
→ 插槽完成一次性登记
→ GameMode 推进权威进度
→ GameState 复制新的目标快照
```

当前关卡使用预绑定目标插槽：Overlap 触发后，钥匙由服务器校验并自动吸附归位。
这个流程没有独立交互按键，也不属于背包、装备或手持物品系统。

### 胜利判定

```text
钥匙目标完成
→ 足够数量的玩家进入胜利区域
→ WinArea 请求 GameMode 复核
→ GameMode 首次提交胜利
→ GameState 同步胜利状态
→ 本地 PlayerController 显示默认胜利界面
→ 玩家选择重开当前关卡或退出房间
```

C++ 默认胜利界面不依赖蓝图即可工作；`ReceiveCoopGameWon` 仍保留为动画、音效和自定义美术的可选扩展点。重开使用服务器的当前关卡 URL，不负责从主菜单自动选择或切换到玩法地图。

## 运行方式

### 编辑器双实例

1. 使用 Unreal Engine 5.5 打开 `multiplayer.uproject`。
2. 编译 Development Editor。
3. 在系统环境变量或当前终端中设置 `UE_EDITOR` 为 `UnrealEditor.exe` 的完整路径。
4. 运行：

```powershell
.\LaunchTwoPlayers.bat
```

脚本会从项目目录定位 `multiplayer.uproject`，并启动两个窗口进入主菜单。它适合检查菜单的创建、搜索和加入，不会自动把主机送入合作地图。

### 手工双实例验证

1. 先检查两个菜单窗口的创建、搜索和加入。建房发布的是主机当前地图，不能把“加入主菜单”当作已经进入合作关卡。
2. 验证玩法时，另行让主机在合作地图运行并监听，客户端连接该主机；地图的选择由开发者手动完成。自动测试入口会从合作地图启动主机并调用会话 API，不要求为测试增加自动选图逻辑。
3. 在合作关卡中依次检查钥匙自动归位、压力板与门、移动平台，以及两名玩家进入胜利区域后的界面。
4. 分别检查当前关卡重开、客户端退出和主机退出是否能回到主菜单。

仅检查关卡玩法时，可在 Development 的控制台中手动直连（这条路径不验证 Session 菜单）：

```text
主机：open /Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo?listen
同机客户端：open 127.0.0.1:7777
```

跨机器时把 `127.0.0.1` 替换为主机局域网地址，并检查 UDP 端口是否可达。

这些步骤是建议的人工检查顺序，不能用自动断言代替全部画面和操作验收。

### 自动网络验证

运行命令和断言范围见 [Tests/README.md](Tests/README.md)。测试驱动在 Development 中按命令行显式启用；普通运行不启动，Shipping 禁用测试入口和回执行为。

- 回归覆盖设计：会话失败重试、真实断包后的重连、钥匙重叠归位、门和压力板、客户端载人、胜利状态、当前关卡重开及退出。
- 动态载人专项：显式 `-Scenario RideMotion` 检查平台上站立、行走、换向、起跳落回，不加入默认 `Scenario All`。`Baseline`、`OrderedVelocity`、`PlatformInertia` 三种模式分别用于原行为、中间修复和保留空中惯性的最终方案对照，命令与指标解释见 [测试说明](Tests/README.md#移动平台与角色弱网同步ridemotion)。
- 压测分为两条单变量曲线：静止压力板的休眠开关、运动平台的更新频率。正式对照每组至少三次，单次短跑只用于检查工具链。
- 默认使用同机双进程和 `NullRHI`，结果只证明报告中的具体断言，不证明跨物理机、视觉平滑或真实玩家容量。已实现测试模式不等于该模式已经通过，以对应版本报告为准。

先前构建实测见 [2026-09-27 网络验证报告](Tests/Evidence/2026-09-27/README.md)：当时打包构建 15 项回归通过，性能基线完成 42 轮单变量对照；报告分别记录构建指纹、结果和未覆盖的边界。这些历史结果不等于后续修改后的构建已重新完成整套验证。

后续新增 [移动平台与角色弱网同步专项](Docs/Reports/2026-09-27/PlatformRideSync.md)：`PlatformInertia` 在 Normal/Moderate/Harsh 各 1 次通过，Normal 的两种对照模式在 Jump 阶段失败。Harsh Jump 仍出现 13 次校正、最大可比较误差约 72.12 cm，不能宣称完全平滑。数据来自双端四阶段、每段约 4 秒的 `motionMetrics`；30 Hz 是配置上限，不是实测发送率。测试 Pawn 读取真实关卡蓝图的关键移动参数，但不等于完整蓝图、摄像机与动画验收；本轮也不提供 CPU／带宽优化结论。独立证据见 [PlatformRide](Tests/Evidence/2026-09-27/PlatformRide)，不与旧 15／42 批次混计。

按问题与解决过程阅读，见 [实验与踩坑报告](Docs/Reports/2026-09-27/README.md)：第一部分整理实验设计、结果和下一步方案，第二部分整理项目踩坑；逐轮数据与源文件索引单列附录。

从原理到项目系统阅读，见 [网络同步技术博客（单文件，五章）](Docs/Study/UE网络同步技术博客.md)：网络基础、帧同步与状态同步、UE 网络机制、项目方案与边界、踩坑与优化复盘。

用于 Notion 导入的交付版本见 [单文件完整版 Markdown](Docs/Exports/UE网络同步技术博客_Notion完整版.md)：五章正文、Q 补充、最新平台实验与历史逐轮数据附录合并在一个文件，已将本机源码链接改为路径定位文字；这是本地文件，不代表已上传或发布到 Notion。

## 当前边界

- 当前只使用 `OnlineSubsystem NULL` 验证本机和局域网连接，没有接入 Steam、EOS 或专用服务器。
- 项目有意不实现“创建房间后自动切换到指定玩法地图”；Session 发布并加入的是主机当前地图。
- 自动重连只尝试返回原服务器，不包含主机迁移、会话续期或玩家运行时状态恢复。
- 重开仅重新加载当前合作关卡，不包含跨关卡流程、存档或断点恢复。

## 环境

- Unreal Engine 5.5
- C++
- Enhanced Input
- OnlineSubsystem / OnlineSubsystemNull
- Listen Server
