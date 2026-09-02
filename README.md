# UE5 双人 Co-op 网络协作 Demo

这是一个使用 Unreal Engine 5.5 和 C++ 开发的双人局域网合作原型。
当前版本实现了局域网房间创建、搜索和加入，以及钥匙目标、压力板、门、移动平台、共享进度和双人胜利判定。所有会影响玩法结果的状态由服务器维护，客户端接收复制状态并更新本地表现。

## 已实现功能

### 局域网会话

- 基于 `OnlineSubsystem NULL` 创建、搜索和加入局域网房间。
- 使用明确的异步操作状态限制重复点击，避免创建、搜索和加入请求相互覆盖。
- 创建新房间前先销毁已有同名 Session，并在完成回调后继续建房。
- 加入成功后解析连接地址，再由本地 `PlayerController` 发起连接。
- 监听网络失败和地图加载失败，并输出可定位的项目日志。

### 服务器权威的协作玩法

- 玩家进入钥匙的 Overlap 范围后，服务器校验钥匙及其目标插槽，钥匙自动吸附（Snap）到对应插槽并登记目标进度。
- 钥匙插槽只接受符合条件的钥匙，并且每个插槽最多登记一次进度。
- 压力板在服务器统计玩家，门根据关联压力板和共享目标决定是否开启。
- 移动平台由服务器决定启停和位置，客户端接收 Actor 位移同步。
- 胜利区域上报当前玩家数量，`GameMode` 再复核钥匙目标、人数和既有胜利状态。
- `GameState` 将钥匙进度、目标数量和胜利结果作为一份完整快照同步给参与玩家。

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

| 模块 | 主要职责 |
| --- | --- |
| `multiplayerGameInstance` | 会话异步流程、连接地址、网络失败处理和自动重连 |
| `multiplayerGameMode` | 服务器规则、目标数量初始化、进度登记和胜利复核 |
| `multiplayerCoopGameState` | 复制合作目标快照，并向本地表现层广播变化 |
| `multiplayerCoopPlayerController` | 确认本地连接进入可操作状态，并承接胜利表现事件 |
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
→ 本地 PlayerController 收到表现事件
```

C++ 提供 `ReceiveCoopGameWon` 蓝图事件作为 UI 扩展点。是否创建和显示具体 UMG，由对应的 PlayerController 蓝图实现决定。

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

### 自动网络回归

```powershell
# 正常网络，并测试一次自动重连
.\TestTwoPlayers.bat

# 100ms 延迟、20ms 波动、2% 丢包
.\TestTwoPlayers.bat -Profile Moderate

# 200ms 延迟、50ms 波动、5% 丢包
.\TestTwoPlayers.bat -Profile Harsh

# 运行全部网络配置
.\TestTwoPlayers.bat -Profile All
```

测试脚本会启动无画面的 Listen Server 和 Client，检查客户端连接、共享目标初始化、网络参数、自动重连及错误日志，并在 `Saved/TestReports` 生成 JSON 报告。

自动测试使用直接地址连接测试关卡，不覆盖主菜单按钮、Session 搜索流程、完整钥匙目标流程或胜利 UI；这些部分仍需要双窗口人工验证。

## 当前边界

- 当前只使用 `OnlineSubsystem NULL` 验证本机和局域网连接，没有接入 Steam、EOS 或专用服务器。
- 创建房间后的 `ServerTravel` 代码已经存在，但当前地图与蓝图配置下的自动地图切换流程尚未完成，因此不计入已完成功能。
- 胜利状态已经同步到本地表现事件；具体胜利界面取决于蓝图是否实现该事件。
- 自动重连只尝试返回原服务器，不包含主机迁移、会话续期或玩家运行时状态恢复。
- 项目没有实现完整的退出房间、重开游戏和存档系统。

## 环境

- Unreal Engine 5.5
- C++
- Enhanced Input
- OnlineSubsystem / OnlineSubsystemNull
- Listen Server
