# UE5 双人 Co-op 网络机关 Demo

这是一个使用 Unreal Engine 5 开发的双人合作网络 Demo。两名玩家通过主菜单创建或加入房间，在同一张地图中合作收集钥匙、操作机关、使用移动平台，并在共同完成目标后进入胜利区域。

---

## 项目概览

本项目是一个双人合作网络 Demo。一名玩家创建房间，另一名玩家搜索并加入；两人进入同一张地图后，通过机关配合完成钥匙收集，并在共同进入胜利区域后结束本局。

主要玩法包括：创建和加入房间、双人角色移动、压力板与门联动、钥匙收集与插槽展示、合作移动平台、共享目标和双人胜利判定。C++ 另提供服务器重开接口，当前胜利界面尚未接入。

### 核心特点

1. **服务器权威的协作玩法闭环：** 钥匙、机关、共享进度和胜利结果都由服务器决定，并通过状态门禁处理多人同时交互和重复触发。
2. **按对象选择网络同步方式：** 角色使用 Unreal 自带的移动同步，门和压力板只同步关键状态，移动平台由服务器移动并同步位置；玩家离开或对象销毁时会清理相关引用和事件绑定。
3. **双人联机入口：** 基于 OnlineSubsystem 实现创建、搜索和加入房间，避免异步操作互相重叠，并记录连接与加载失败。

### 游戏目标流程

1. 一名玩家在主菜单创建房间，成为 Host。
2. 另一名玩家搜索房间并加入。
3. Host 将两名玩家一起带入合作地图。
4. 两名玩家通过压力板、门和移动平台互相配合。
5. 玩家收集四把钥匙，钥匙会被安装到对应插槽。
6. 四个插槽全部激活后，共享目标完成。
7. 两名玩家同时进入胜利区域，服务器确认本局胜利。
8. 两端收到胜利状态，由本地 PlayerController 交给胜利界面。

整个过程中，服务器负责决定钥匙是否被拾取、机关是否生效以及游戏是否胜利；客户端负责输入、移动和画面表现。

---

## 架构职责

```text
GameInstance
  └─ 创建、搜索和加入房间
       └─ 进入合作地图
            ├─ GameMode：执行服务器权威规则
            ├─ GameState：复制所有玩家共享的状态快照
            ├─ PlayerController：管理本地胜利界面并提供重开请求接口
            ├─ Character：处理移动、输入并持有 CarryComponent
            └─ 机关 Actor
                 ├─ PlayerOccupancyComponent：统一玩家区域统计
                 ├─ TransporterComponent：移动平台位移
                 └─ Key / Socket / Plate / Gate / Platform / WinArea
```

### Character：玩家控制

主要文件：

- [`multiplayerCharacter.h`](Source/multiplayer/multiplayerCharacter.h)：声明角色拥有的相机、输入和对外访问接口。
- [`multiplayerCharacter.cpp`](Source/multiplayer/multiplayerCharacter.cpp)：创建相机组件，配置 CharacterMovement，并绑定移动、视角和跳跃输入。
- [`BP_ThirdPersonCharacter.uasset`](Content/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.uasset)：配置角色模型、动画蓝图和输入资产。

[`multiplayerCharacter.cpp`](Source/multiplayer/multiplayerCharacter.cpp) 主要负责：

- 角色移动、跳跃和视角控制。
- 第三人称相机和弹簧臂。
- Enhanced Input 输入绑定。
- 使用 Unreal 自带的 CharacterMovement 完成角色移动同步。
- 使用 multiplayerCoopCarryComponent 显式保存当前携带的钥匙，插槽不再扫描角色附着 Actor。

### PlayerController：本地界面和玩家命令

主要文件：

- [`multiplayerCoopPlayerController.h`](Source/multiplayer/multiplayerCoopPlayerController.h) / [`multiplayerCoopPlayerController.cpp`](Source/multiplayer/multiplayerCoopPlayerController.cpp)：本地胜利事件和服务器重开请求。
- [`multiplayerVictoryPresenterComponent.h`](Source/multiplayer/multiplayerVictoryPresenterComponent.h) / [`multiplayerVictoryPresenterComponent.cpp`](Source/multiplayer/multiplayerVictoryPresenterComponent.cpp)：监听 GameState，并保证每局只通知一次胜利 UI。
- [`BP-CoopPlayerController.uasset`](Content/ThirdPerson/Blueprints/BP-CoopPlayerController.uasset)：创建胜利 Widget、设置鼠标和输入模式。
- [`winandquit.uasset`](Content/UI/winandquit.uasset)：胜利界面及中文按钮。

[`multiplayerCoopPlayerController.cpp`](Source/multiplayer/multiplayerCoopPlayerController.cpp) 负责当前玩家自己的界面流程：

- 接收本地胜利通知。
- 将胜利事件交给蓝图创建并显示 UMG。
- 提供可由蓝图调用的服务器重开请求接口；当前胜利 Widget 尚未接线。

[`multiplayerVictoryPresenterComponent.cpp`](Source/multiplayer/multiplayerVictoryPresenterComponent.cpp) 在本地控制器上监听 GameState 的胜利状态，并保证同一局只通知一次胜利 UI。

胜利链路如下：

```text
服务器确认胜利
→ GameState 复制胜利状态
→ 本地 VictoryPresenter 收到通知
→ PlayerController 蓝图显示胜利界面
```

### GameState：共享进度

主要文件：[`multiplayerCoopGameState.h`](Source/multiplayer/multiplayerCoopGameState.h)、[`multiplayerCoopGameState.cpp`](Source/multiplayer/multiplayerCoopGameState.cpp)。

[`multiplayerCoopGameState.cpp`](Source/multiplayer/multiplayerCoopGameState.cpp) 保存：

- 已安装钥匙数量。
- 本局需要的钥匙数量。
- 游戏是否已经胜利。

GameState 只保存、校验并复制 GameMode 计算出的共享状态快照；RepNotify 再把进度和胜利变化广播给本地表现层。

### GameMode：本局规则

主要文件：[`multiplayerGameMode.h`](Source/multiplayer/multiplayerGameMode.h)、[`multiplayerGameMode.cpp`](Source/multiplayer/multiplayerGameMode.cpp)。

[`multiplayerGameMode.cpp`](Source/multiplayer/multiplayerGameMode.cpp) 只在服务器运行，负责：

- 指定本局使用的 Character、PlayerController 和 GameState 类型。
- 根据当前地图中实际放置的 KeySocket 数量配置钥匙目标，并使用 RequiredKeys 作为无插槽地图的回退值。
- 接收 KeySocket 的激活报告并推进权威进度。
- 同时验证目标进度与 WinArea 人数后提交胜利。
- 处理重新开始请求。
- 通过服务器地图迁移让所有已连接玩家一起进入新一局。

如果两名玩家同时点击重新开始，GameMode 的单次请求标记会忽略后续请求，避免重复发起地图迁移。

### GameInstance：房间和连接

主要文件：[`multiplayerGameInstance.h`](Source/multiplayer/multiplayerGameInstance.h)、[`multiplayerGameInstance.cpp`](Source/multiplayer/multiplayerGameInstance.cpp)、[`WBPmainmenu.uasset`](Content/UI/WBPmainmenu.uasset)。

[`multiplayerGameInstance.cpp`](Source/multiplayer/multiplayerGameInstance.cpp) 对应的 GameInstance 在地图切换时不会被销毁，负责：

- 创建房间。
- 搜索可加入的房间。
- 加入选中的房间。
- 记录连接和地图加载失败。
- 防止创建、搜索、加入等异步操作互相重叠。

再次创建房间时，GameInstance 会在内部先销毁同名 Session；项目没有独立的“退出房间”界面流程。

Host 创建房间成功后，服务器切换到合作地图，并带上已经连接的玩家。Client 加入成功后，根据 OnlineSubsystem 返回的地址连接服务器。

项目使用 OnlineSubsystem NULL 完成本地与局域网房间联调。

---

## 机关系统

机关采用同一个基本原则：服务器修改真正的游戏状态，客户端收到结果后更新画面。

### 压力板

主要文件：[`multiplayerPressurePlate.h`](Source/multiplayer/multiplayerPressurePlate.h)、[`multiplayerPressurePlate.cpp`](Source/multiplayer/multiplayerPressurePlate.cpp)、[`pressplate.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/pressplate.uasset)。

[`multiplayerPressurePlate.cpp`](Source/multiplayer/multiplayerPressurePlate.cpp) 统计站在板上的玩家，并将是否激活同步给两端。

multiplayerPlayerOccupancyComponent 统一处理 PressurePlate、MovingPlatform 和 WinArea 的玩家筛选、多碰撞组件计数及角色销毁清理。客户端不再运行这些纯服务器逻辑触发器。

### 门

主要文件：[`multiplayerCoopGate.h`](Source/multiplayer/multiplayerCoopGate.h)、[`multiplayerCoopGate.cpp`](Source/multiplayer/multiplayerCoopGate.cpp)、[`bpcoopgate.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/bpcoopgate.uasset)。

[`multiplayerCoopGate.cpp`](Source/multiplayer/multiplayerCoopGate.cpp) 监听与它关联的压力板，由服务器计算是否满足开门条件。

门复制打开状态，客户端收到状态后在本地完成开关移动。

门可以配置为：

- 需要一个或多个压力板同时激活。
- 打开后保持开启。
- 必须先完成钥匙目标才能开启。

### 钥匙

主要文件：[`multiplayerCoopKey.h`](Source/multiplayer/multiplayerCoopKey.h)、[`multiplayerCoopKey.cpp`](Source/multiplayer/multiplayerCoopKey.cpp)、[`bpkey.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/bpkey.uasset)。

[`multiplayerCoopKey.cpp`](Source/multiplayer/multiplayerCoopKey.cpp) 由服务器处理拾取和安装。

钥匙保存“当前持有者”和“是否已经安装”两个复制状态；Character 的 CarryComponent 保存服务器当前携带槽。两名玩家即使几乎同时碰到同一把钥匙，服务器也会依次处理事件：第一个成功后，后续事件会因为钥匙已被持有、安装或携带槽已占用而结束，因此不会出现一把钥匙被计算两次。

如果持有钥匙的玩家掉线或角色被销毁，钥匙会解除附着、清除持有者并重新开放拾取，不会永久消失。

### 钥匙插槽

主要文件：[`multiplayerKeySocket.h`](Source/multiplayer/multiplayerKeySocket.h)、[`multiplayerKeySocket.cpp`](Source/multiplayer/multiplayerKeySocket.cpp)、[`bpkeysocket.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/bpkeysocket.uasset)。

[`multiplayerKeySocket.cpp`](Source/multiplayer/multiplayerKeySocket.cpp) 从 Character 的 CarryComponent 取得钥匙，并向 GameMode 报告一次插槽激活。

插槽激活后会立即关闭碰撞，并通过 `bActivated` 阻止重复提交。同一插槽无论被触发多少次，都最多只会给共享目标增加一次进度。

### 移动平台

主要文件：[`multiplayerMovingPlatform.h`](Source/multiplayer/multiplayerMovingPlatform.h)、[`multiplayerMovingPlatform.cpp`](Source/multiplayer/multiplayerMovingPlatform.cpp)、[`multiplayerTransporterComponent.h`](Source/multiplayer/multiplayerTransporterComponent.h)、[`multiplayerTransporterComponent.cpp`](Source/multiplayer/multiplayerTransporterComponent.cpp)、[`bpmovingplatform.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/bpmovingplatform.uasset)。

[`multiplayerMovingPlatform.cpp`](Source/multiplayer/multiplayerMovingPlatform.cpp) 负责判断平台什么时候启动，例如由外部压力板控制，或者需要一定数量的玩家站上平台。

PlayerOccupancyComponent 负责平台人数，[`multiplayerTransporterComponent.cpp`](Source/multiplayer/multiplayerTransporterComponent.cpp) 只负责把所属平台从起点移动到终点。平台规则、占用统计和移动实现保持独立。

平台的位置由服务器控制并同步给客户端；到达目标后停止更新，减少无意义的 Tick。

### 胜利区域

主要文件：[`multiplayerWinArea.h`](Source/multiplayer/multiplayerWinArea.h)、[`multiplayerWinArea.cpp`](Source/multiplayer/multiplayerWinArea.cpp)、[`winaera.uasset`](Content/ThirdPerson/Blueprints/gameplayelements/winaera.uasset)。

[`multiplayerWinArea.cpp`](Source/multiplayer/multiplayerWinArea.cpp) 读取 PlayerOccupancyComponent 的不同玩家数量。

当钥匙目标完成且区域内达到要求人数时，WinArea 请求 GameMode 验证并提交胜利；GameState 只复制最终状态。

正式地图中的 `winaera` 是该 C++ 类的蓝图子类，用于配置碰撞范围并放置到关卡中。

---

## 网络设计

### 状态同步

项目采用服务器权威的状态同步：

- CharacterMovement 负责角色移动预测和服务器校正。
- 压力板、门、插槽等机关只同步关键状态。
- 客户端根据同步结果播放门、压力板和 UI 表现。
- 移动平台由服务器移动，并同步实际位置。
- GameState 统一同步共享目标和胜利结果。

### C++ 与蓝图的分工

C++ 负责：

- 服务器规则和状态检查。
- 网络复制与多人地图迁移。
- 重复触发保护。
- 玩家离开、对象销毁和 Delegate 的清理。
- 给蓝图提供少量稳定的表现接口。

蓝图负责：

- 角色模型、动画和输入资产配置。
- 机关模型、位置、移动端点与关卡引用。
- 主菜单和胜利界面。
- 按钮、鼠标、音效及视觉表现。

---

## 关键流程

### 收集钥匙

```text
玩家进入钥匙范围
→ 服务器确认钥匙尚未被拿走
→ Character CarryComponent 记录当前钥匙
→ 钥匙安装到目标插槽
→ 插槽只登记一次
→ GameMode 增加权威进度
→ GameState 复制新快照
→ 两端更新钥匙和机关表现
```

### 完成本局

```text
四个钥匙插槽激活
→ 两名玩家进入胜利区域
→ WinArea 请求 GameMode 完成游戏
→ GameMode 验证并更新 GameState
→ GameState 复制胜利状态
→ 两端 PlayerController 显示胜利界面
```

### 创建和加入房间

```text
Host 创建房间
→ 创建成功后服务器进入合作地图
→ Client 搜索并选择房间
→ Client 连接服务器
→ 两名玩家进入同一局游戏
```
