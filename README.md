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
- 意外断线后按照 1、2、4 秒间隔进行有限次数重试。
- 只有本地控制器重新进入 `PlayingState`，才认为重连成功。
- 主动退出、地址无效或达到重试上限时结束重连流程。
- 重连后重新读取服务器上的共享目标；不会恢复断线前的位置或未提交的本地表现状态。

## 代码结构

源码按功能分为五个目录，同一个类的 `.h` 和 `.cpp` 放在一起，仍属于同一个 `multiplayer` 模块。宝物和插槽与其他机关统一放在 `Mechanisms` 中。

下方目录树中的类文件省略 `.h` / `.cpp` 后缀，每项对应一对头文件和实现文件。

```text
Source/multiplayer/
├── Core/         GameMode、GameState、公共日志
│   ├── multiplayerGameMode
│   ├── multiplayerCoopGameState
│   └── multiplayerLog
├── Network/      会话创建、搜索、加入、退出和重连
│   └── multiplayerGameInstance
├── Player/       角色输入、玩家控制器
│   ├── multiplayerCharacter
│   └── multiplayerCoopPlayerController
├── Mechanisms/   宝物、插槽、携带组件、压力板、门、平台及区域判定
│   ├── multiplayerCoopKey
│   ├── multiplayerKeySocket
│   ├── multiplayerCoopCarryComponent
│   ├── multiplayerPressurePlate
│   ├── multiplayerCoopGate
│   ├── multiplayerMovingPlatform
│   ├── multiplayerTransporterComponent
│   ├── multiplayerPlayerOccupancyComponent
│   └── multiplayerWinArea
├── UI/           胜利状态展示、胜利界面
│   ├── multiplayerVictoryPresenterComponent
│   └── multiplayerVictoryWidget
├── multiplayer.Build.cs
└── multiplayer.h / multiplayer.cpp
```

| 模块 | 主要职责 |
| --- | --- |
| `multiplayerGameInstance` | 会话异步流程、连接地址、网络失败处理和自动重连 |
| `multiplayerGameMode` | 服务器规则、目标数量初始化、进度登记和胜利复核 |
| `multiplayerCoopGameState` | 复制合作目标快照，并向本地表现层广播变化 |
| `multiplayerCoopPlayerController` | 确认本地连接、显示胜利界面，并提交重开或退出操作 |
| `multiplayerPlayerOccupancyComponent` | 统一处理区域内玩家筛选、去重和销毁清理 |
| `multiplayerTransporterComponent` | 只负责服务器上的平台位移 |
| Key / Socket / Plate / Gate / Platform / WinArea | 各自负责单一机关规则，并把最终判定交给服务器规则层 |

核心源码位于 [Source/multiplayer](Source/multiplayer)，蓝图和关卡资源位于 [Content](Content)。

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

脚本会从项目目录定位 `multiplayer.uproject`，并启动两个窗口进入主菜单。房间创建、搜索和加入需要在两个窗口中手动操作验证。

### 手工双实例验证

1. 在第一个窗口创建局域网房间，在第二个窗口搜索并加入。
2. 依次检查钥匙自动归位、压力板与门、移动平台，以及两名玩家进入胜利区域后的界面。
3. 分别检查当前关卡重开、客户端退出和主机退出是否能回到主菜单。

这些步骤是建议的人工检查顺序，不代表仓库随附自动测试结果。

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
