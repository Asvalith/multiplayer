# UE5 Co-op 网络项目统一技术复习文档

> 更新日期：2026-08-10
> 项目路径：`E:\ueprojrct\multiplayer`
> 原则：只把代码和运行证据能够证明的内容写成成果；待蓝图接入或待双端验证的内容明确标注。

---

## 1. 当前完成度与边界

### 1.1 完成度

- C++ 核心系统：约 **80%～85%**。
- 完整可演示双人 Co-op：约 **60%**。
- 完整编译：已通过 UE5.5 UHT、C++ 编译和链接。
- 无界面运行：已进入项目 GameMode 并加载 OnlineSubsystemNull。
- 双客户端 Host / Find / Join 与机关联调：尚未完成，不能当成已验证成果。

### 1.2 已完成的 C++ 模块

| 模块 | 已完成内容 | 主要网络知识 |
|---|---|---|
| Character 网络实验 | NetMode、Role、Authority、Ownership 日志；Server/Client/Multicast RPC；RepNotify；服务端生成 Actor | RPC 方向、所有权、持久状态和瞬时事件 |
| GameInstance Session | Create、Find、Join、Destroy；异步 Delegate Handle；重复建房先销毁；操作防重入 | OSS、Session 生命周期、异步回调 |
| ReplicatedCube | 服务端生成、Actor 复制、移动复制和服务端物理 | 服务端权威生成 |
| PressurePlate | 独立压力板、服务端 Overlap、占用者去重、状态复制和按需动画 Tick | 单一职责、弱引用、事件驱动 |
| CoopGate | 订阅多个独立压力板、不同玩家约束、门状态复制和本地表现 | Delegate 解耦、RepNotify、晚加入 |
| MovingPlatform | 多玩家占用、服务端移动、Transform 复制、按需 Tick | 服务端模拟、移动复制、更新治理 |
| Key / KeySocket | 服务端拾取、Holder 复制、附着、插槽消费和目标进度 | Actor 所有权、对象生命周期 |
| CoopGameState | 目标进度和胜利状态的原子复制 | 全局共享状态、晚加入一致性 |
| WinArea | 双人进入且目标完成后由服务端结算 | 服务端胜负判定、弱引用 |

### 1.3 尚未完成

1. 创建蓝图子类、配置 Mesh、碰撞体和关卡参数。
2. 菜单按钮连接 `HostGame / FindGames / JoinGame / DestroyGameSession`。
3. 两窗口实际验证房间创建、搜索、加入、销毁和地图切换。
4. 两端实际验证压力板、平台、钥匙、插槽和胜利区。
5. 100/200 ms 延迟、丢包、晚加入和断线边界测试。
6. 共享胜利 UI、演示视频、README 和可执行版本。
7. GAS 进阶版本尚未开始；它不阻塞基础 Co-op 版本验收。

明确不声称已完成：Steam 实测、Dedicated Server 部署、客户端预测回滚、延迟补偿、断线重连、GAS。

---

## 2. 整体架构

```text
UmultiplayerGameInstance
├─ 在线子系统初始化
├─ Session 创建 / 搜索 / 加入 / 销毁
├─ 异步 Delegate Handle 生命周期
└─ Session 操作状态机与防重入

AmultiplayerGameMode（仅服务器存在）
├─ 选择 AmultiplayerCoopGameState
├─ 保留 LAN ServerTravel / ClientTravel 调试入口
└─ 承担服务器规则入口

AmultiplayerCoopGameState（服务器和客户端均存在）
├─ ActivatedKeys
├─ RequiredKeys
├─ bGameWon
└─ RepNotify 向 UI 和机关广播共享进度

AmultiplayerCharacter
├─ Server RPC：客户端请求服务端执行
├─ Client RPC：服务端只回执拥有者
├─ Multicast RPC：所有端播放一次瞬时表现
└─ RepNotify：复制可持续的计数状态

关卡机关
├─ AmultiplayerPressurePlate：可独立复用的权威压力板
├─ AmultiplayerCoopGate：订阅多个压力板的协作门
├─ AmultiplayerMovingPlatform：双人同步平台
├─ AmultiplayerCoopKey：可拾取钥匙
├─ AmultiplayerKeySocket：钥匙插槽
└─ AmultiplayerWinArea：双人胜利区
```

这不是严格 MVC。准确描述是：

> C++ 管理权威规则、状态、复制和生命周期；蓝图负责资产配置、关卡组装、UI 和音画表现。

---

## 3. 四种网络手段的完整对比

### 3.1 Server RPC

```text
客户端按键 2
→ RequestServerAction
→ ServerRequestAction_Validate
→ ServerRequestAction_Implementation
→ 服务端增加 NetworkActionCount
```

- 客户端只能提出请求，权威数据由服务器修改。
- RPC 放在玩家拥有的 Character 上，满足 Client-to-Server Ownership 条件。
- Validation 检查对象是否正在销毁；真实项目还要校验冷却、距离、资源和状态。
- 低频关键请求使用 Reliable，高频输入不能无脑使用 Reliable。

### 3.2 Client RPC

```text
服务器完成请求
→ ClientConfirmServerAction(ConfirmedCount)
→ 只在该 Character 的拥有者客户端执行
→ OnServerActionConfirmed 广播给蓝图
```

用途是给请求者回执、显示私有提示或打开只属于该玩家的 UI。它不负责共享状态。

### 3.3 NetMulticast RPC

```text
服务器确认动作
→ MulticastPlayNetworkActionEffect(Location)
→ 服务器和相关客户端收到
→ OnNetworkActionEffect 播放瞬时表现
```

- 使用 Unreliable，因为表现允许偶发丢失，不能阻塞后续关键网络消息。
- 适合粒子、声音和一次性表现。
- 不适合门是否打开、目标是否完成等持久状态，晚加入者会错过历史 Multicast。

### 3.4 属性复制与 RepNotify

```text
服务端修改 NetworkActionCount
→ UE 属性复制
→ 客户端 OnRep_NetworkActionCount
→ OnNetworkActionCountChanged
```

- 适合血量、门状态、任务进度等可持续状态。
- 新客户端加入后能获得当前值。
- Listen Server 修改属性时不会依赖自己的 OnRep，服务端显式调用共用应用函数。

核心结论：

```text
请求权威操作：Server RPC
只通知拥有者：Client RPC
播放一次表现：Unreliable Multicast
保存最终状态：Replication + RepNotify
```

---

## 4. Session 房间系统

### 4.1 创建房间

```text
UI Host
→ GameInstance::HostGame
→ 检查 SessionInterface 和异步操作状态
→ 已有房间则先 DestroySession
→ CreateSession
→ HandleCreateSessionComplete
→ ServerTravel(SessionMapPath + "?listen")
```

Session 保存服务器名称、地图、公开连接数、LAN / Online 配置，并允许 Join In Progress。

### 4.2 搜索和加入

```text
UI Find
→ FindSessions
→ 异步结果转换为 FmultiplayerSessionInfo
→ 蓝图显示名称、Ping、当前/最大人数

UI Join(ResultIndex)
→ JoinSession
→ GetResolvedConnectString
→ PlayerController::ClientTravel
```

蓝图只保存 `ResultIndex`，真正的 `FOnlineSessionSearchResult` 生命周期留在 C++。

### 4.3 销毁和重复创建

```text
DestroyGameSession
→ DestroySession
→ 清除 Delegate Handle
→ 广播结果

再次 Host 且旧 Session 存在
→ 设置 bCreateSessionAfterDestroy
→ 销毁完成后继续 CreateSession
```

### 4.4 异步操作状态机

```text
None
├─ Hosting
├─ Finding
├─ Joining
└─ Destroying
```

`BeginSessionOperation` 拒绝重入，`EndSessionOperation` 在成功、失败和立即返回路径上复位。蓝图绑定 `OnSessionOperationChanged`，在非 `None` 时禁用按钮。

为什么不能只在蓝图禁用按钮：UI 只是表现层，键盘快捷键、重复事件或其他调用者仍可能重入；C++ 规则层必须自我保护。

---

## 5. Co-op 机关系统

### 5.1 双压力板门

```text
独立 PressurePlate 在服务器接收 BeginOverlap / EndOverlap
→ 每块板用 TSet<TWeakObjectPtr<ACharacter>> 保存占用者
→ 去重、清除失效 Pawn，并计算 bPlateActive
→ bPlateActive 通过 RepNotify 驱动两端压力板表现
→ PressurePlate 广播 OnPlateActiveChanged
→ CoopGate 订阅 RequiredPlates 的 Delegate
→ Gate 汇总激活板数量和不同玩家数量
→ 满足 RequiredActivePlateCount 后更新 bGateOpen
→ bGateOpen 通过 RepNotify 驱动两端门表现
```

设计取舍：

- 压力板只回答“当前是否被有效玩家激活”，门只负责组合规则，两者可以分别复用。
- Overlap 是离散状态变化，不使用每帧查询。
- 服务端能直接观察碰撞，不让客户端 RPC 报告“我踩到了”。
- 复制语义状态，不逐帧复制门动画。
- `TSet` 防止角色多个碰撞组件造成重复计数。
- `TWeakObjectPtr` 表示机关观察玩家但不拥有其生命周期。
- 只有压力板或门正在做短距离插值时才临时开启 Tick，到达目标后立即关闭。

### 5.2 双人移动平台

```text
服务器统计 ActivationVolume 中的不同 Character
→ ReplicatedPlayerCount >= RequiredPlayers
→ Transporter::SetTransportActive(true)
→ 仅服务器按需开启 Component Tick
→ SetActorLocation 执行移动
→ ReplicateMovement 同步到客户端
→ 到达目标后关闭 Tick
```

平台位置会影响角色碰撞，因此选择服务器模拟并复制 Transform；这与只复制门开关后客户端本地播放动画的策略不同。

### 5.3 钥匙、插槽与共享目标

```text
服务器检测玩家进入钥匙范围
→ Key.Holder = Character
→ 设置 Owner 并附着到角色 Mesh / Socket
→ Holder 属性复制

持钥匙玩家进入 KeySocket
→ 服务器查找附着钥匙
→ ConsumeAtSocket
→ bActivated = true
→ CoopGameState::RegisterActivatedKey
→ ObjectiveState 复制给所有客户端
```

`CoopGameState` 复制一个原子目标结构，而不是让多个相关变量分别到达，减少客户端看到中间不一致状态的机会。

### 5.4 胜利区

```text
服务器维护胜利区中的不同玩家集合
→ 目标钥匙数量满足
→ 玩家数量满足
→ GameState::TryCompleteGame
→ bGameWon 复制
→ UI 绑定 OnGameWon
```

胜负不能由进入区域的客户端直接决定，否则客户端可伪造结果。

---

## 6. 数据结构和生命周期

| 类型 | 项目用途 | 选择原因 |
|---|---|---|
| `TSet<TWeakObjectPtr<ACharacter>>` | 压力板、平台、胜利区占用者 | 去重，不拥有 Pawn，销毁后可检测失效 |
| `TObjectPtr<UComponent>` | Actor 持有的 UE 组件 | 支持 UObject 反射和 GC 跟踪 |
| `TSharedPtr<FOnlineSessionSearch>` | 非 UObject 的异步搜索对象 | 跨异步回调保持搜索结果生命周期 |
| `IOnlineSessionPtr` | 在线 Session 接口 | 共享引用计数管理接口生命周期 |
| `FDelegateHandle` | Session 异步回调 | 精确解绑，防止重复绑定和对象退出后的回调 |
| `uint8` 位掩码 | 两块压力板激活状态 | 用少量数据表达多个布尔状态 |

三个指针体系不能互换：

- `TObjectPtr` 服务于 UObject/GC。
- `TWeakObjectPtr` 非拥有地观察 UObject。
- `TSharedPtr` 管理普通 C++ 对象，不应拿来拥有 UObject。

---

## 7. 问题、定位与修复记录

### 7.1 旧 DLL 与源码不一致

- 现象：源码已新增 UCLASS，但编辑器看不到类或节点。
- 定位：比较源码、Binaries 时间和完整构建日志。
- 修复：关闭依赖旧 DLL 的状态，执行 Editor Development 完整构建。
- 收获：Live Coding 不替代新增反射类型后的完整 UHT 构建。

### 7.2 UE5.5 Session API 变化

- 现象：`SETTING_MAPNAME` 和旧 Presence 搜索宏编译失败或弃用。
- 定位：编译器报错指向 Session 名称定义和搜索参数。
- 修复：引入 `Online/OnlineSessionNames.h`，在线搜索改用 `SEARCH_LOBBIES`。
- 收获：课程 API 必须结合当前引擎头文件验证，不能机械照搬旧版本。

### 7.3 Session 重复点击产生异步重入

- 风险：Create/Find/Join 尚未结束时再次点击，可能重复绑定 Delegate 或交叉修改搜索结果。
- 修复：C++ 增加 `EMultiplayerSessionOperation` 状态机；所有立即失败和异步回调都成对复位。
- 蓝图：监听操作状态禁用按钮，但 C++ 仍保留最终保护。

### 7.4 OnRep 在服务器不自动承担本地更新

- 现象：客户端表现更新，Listen Server 本地窗口没有变化。
- 根因：服务器是属性发送方，不依赖接收端 RepNotify。
- 修复：服务端写值后显式调用共用应用函数，客户端仍从 OnRep 进入。

### 7.5 一个玩家被多个碰撞体重复统计

- 现象：胶囊、Mesh 或附属组件可能产生多次 Overlap。
- 修复：保存 Character 的 `TSet`，不使用简单整数自增/自减。

### 7.6 玩家销毁或断线后引用残留

- 风险：集合残留、机关无法复位或悬空访问。
- 修复：使用 `TWeakObjectPtr`，每次求值前清理无效对象；仍需断线运行测试。

### 7.7 DDC / Zen 本地通信导致无界面启动失败

- 现象：默认 DDC 图没有可写节点，Zen 访问本机 IPv6 地址失败。
- 定位：崩溃栈在 DerivedDataCache，和网络 Gameplay 代码无关。
- 验证方式：命令行加入 `-DDC-ForceMemoryCache` 后可进入项目并加载 Null OSS。
- 边界：这是开发环境修复，不是项目网络功能成果。

### 7.8 大体积资产重复

- 现象：`Content/DesertCity` 和 `Content/scene/DesertCity` 可能重复。
- 风险：仓库膨胀、引用混乱、提交噪声。
- 处理：先用 Reference Viewer 验证关卡引用，再清理；不根据目录名盲删。

### 7.9 故障注入与边界问题说明

本节用于双端联调和面试准备。没有在实际开发中出现过的问题，不表述为“我遇到并修复的 Bug”，而标记为“故障注入”或“边界测试”。回答时应区分：

- **实际 Bug**：能够提供提交记录、日志、断点或复现步骤。
- **故障注入**：为了验证架构主动破坏保护条件，再观察系统行为。
- **边界测试**：尚未发现故障，但多人网络必须验证的异常路径。

### 7.10 客户端越权修改机关状态

- 分类：故障注入。
- 注入方法：临时移除 Gate、KeySocket 或 WinArea 的 `HasAuthority()` 判断，让客户端直接修改最终状态。
- 预期现象：单个客户端可能显示门已开启或已经胜利，服务器和其他客户端仍保持旧状态；严重时形成可作弊入口。
- 根因：客户端只应发送意图，不能决定影响所有玩家的 Gameplay 结果。
- 定位：分别打印 `GetLocalRole()`、`HasAuthority()`、Actor 名称和状态修改位置，对比 Host 与 Client 日志。
- 修复：碰撞事实由服务器观察，最终状态只在服务器写入，再通过 Replication/RepNotify 分发。
- 回归：Client 独自触发、Host 触发、两端同时触发，确认所有状态修改日志只出现在服务器。

### 7.11 用 Multicast 代替持久状态复制

- 分类：故障注入。
- 注入方法：门开启、钥匙进度或胜利结果只发送 Multicast，不保存复制属性。
- 预期现象：在线客户端当时能看到表现，但晚加入客户端不知道历史 Multicast，得到关闭的门或错误进度。
- 根因：Multicast 是事件，不是可查询的当前状态。
- 修复：门开关、目标进度、胜利结果使用 RepNotify；粒子、音效等瞬时表现使用 Unreliable Multicast。
- 回归：目标完成后再加入第二个客户端，确认其无需重播历史事件即可恢复当前状态。

### 7.12 Listen Server 本地表现没有更新

- 分类：实际风险，与 7.4 对应。
- 现象：远端客户端收到 OnRep 并更新，Host 本地画面仍为旧状态。
- 根因：服务器负责发送复制属性，不能把自己的本地表现完全寄托在接收端 OnRep 上。
- 修复：抽取 `ApplyReplicatedState()`；服务器写值后直接调用，客户端在 OnRep 中调用同一函数。
- 回归：分别由 Host 和 Client 触发机关，检查 Host 与 Client 的门、UI 和音效一致。

### 7.13 Actor Ownership 不满足导致 Server RPC 被丢弃

- 分类：边界测试。
- 触发：客户端直接在一个自己不拥有的世界机关 Actor 上调用 Server RPC。
- 预期现象：客户端节点执行，但服务器函数没有进入。
- 根因：客户端只能可靠地向服务器调用自己拥有的 Actor 上的 Server RPC，常见入口是 PlayerController、Pawn 或其拥有组件。
- 定位：打印 Actor Owner、NetOwner、Role；在 RPC 首行设断点；使用 `showdebug net` 辅助观察。
- 修复：让服务器直接处理机关 Overlap，或由客户端通过其拥有的 Character/PlayerController 发送交互请求，再由服务器校验距离、目标和状态。
- 回归：合法拥有者请求成功；非拥有客户端调用被拒绝；越距请求不改变状态。

### 7.14 Reliable Multicast 高频堆积

- 分类：故障注入。
- 注入方法：将连续粒子、脚步或高频交互表现改成 Reliable Multicast，并在丢包环境中反复触发。
- 预期现象：可靠消息排队和重传，后续可靠消息被阻塞，延迟突然增大。
- 根因：Reliable 保证到达和顺序，不等于没有成本。
- 修复：关键低频请求使用 Reliable Server RPC；一次性、可丢失表现使用 Unreliable Multicast；可持续查询的结果使用属性复制。
- 回归：在 `Net PktLag=200`、`Net PktLoss=5` 下连续触发，最终 Gameplay 状态一致，短暂特效允许少量丢失。

### 7.15 Session Delegate 未解绑或重复绑定

- 分类：故障注入。
- 注入方法：临时去掉 Create/Find/Join/Destroy 回调中的 `Clear...Delegate_Handle`，重复进入菜单和发起操作。
- 预期现象：一次操作回调多次、重复 Travel、UI 状态被旧回调覆盖，或 GameInstance 退出后仍有残留绑定。
- 根因：Online Session 是异步接口；注册回调和解除回调必须成对。
- 定位：给每次绑定生成序号，打印 `FDelegateHandle`、当前操作类型和回调次数。
- 修复：保存每个 `FDelegateHandle`；成功、失败、立即返回和 `Shutdown()` 路径都精确解绑并复位。
- 回归：连续执行 Host -> Destroy -> Host、Find -> Join -> 返回菜单，每个操作只收到一次完成事件。

### 7.16 Session 异步操作重入

- 分类：实际风险，与 7.3 对应。
- 触发：快速连点 Host、Find 或 Join，或者 Find 未结束便 Join。
- 预期现象：搜索对象被替换、回调顺序交叉、Session 已存在、按钮长期禁用。
- 根因：多个异步操作共享 SessionInterface、SessionSearch 和 Delegate 状态。
- 修复：`EMultiplayerSessionOperation` 作为规则层状态机；非 `None` 状态拒绝新操作；蓝图按钮禁用仅作为用户反馈。
- 回归：自动快速调用同一接口 10 次，只允许第一个操作进入，结束后恢复 `None`。

### 7.17 旧 ResultIndex 加入错误房间

- 分类：边界测试。
- 触发：保存第一次搜索的下标，发起第二次搜索后仍使用旧下标 Join。
- 预期现象：下标越界、加入了新列表中的另一个房间，或者使用失效的搜索结果。
- 根因：ResultIndex 只对当前 `SessionSearch->SearchResults` 有效，不是房间永久标识。
- 修复：每次 Find 前清空旧结果；Join 前验证操作状态、Search 对象和下标范围；UI 列表随搜索结果整体重建。
- 回归：空列表、单结果、多结果、连续两次搜索和越界下标均不会崩溃或误加入。

### 7.18 建房成功但地图 Travel 失败

- 分类：边界测试。
- 注入方法：临时设置错误的 `SessionMapPath` 或不存在的关卡名。
- 预期现象：Session 创建成功，但 Host 没有进入联机关卡，客户端之后无法正确加入。
- 定位：记录 CreateSession 回调、目标 URL、`OnTravelFailure` 和 `OnNetworkFailure`。
- 修复：Travel 前校验地图软路径；绑定 Travel/Network Failure；失败时销毁半成品 Session，并向 UI 广播明确错误。
- 回归：错误地图会回到可重试状态；正确地图以 `?listen` 启动并可被 Client 搜索加入。

### 7.19 同一玩家被多个碰撞组件重复计数

- 分类：实际风险，与 7.5 对应。
- 注入方法：用整数在 BeginOverlap 时直接 `++`，让角色胶囊和 Mesh 同时产生 Overlap。
- 预期现象：一个玩家被当成两个人，独自打开双人门或完成胜利条件。
- 根因：Overlap 事件数量不等于不同玩家数量。
- 修复：把 `OtherActor` 归一为 Character，以 `TSet<TWeakObjectPtr<ACharacter>>` 去重；EndOverlap 只移除对应 Character。
- 回归：单玩家多个组件重叠仍计数 1；两个不同玩家才计数 2。

### 7.20 玩家断线导致机关永久占用

- 分类：边界测试。
- 触发：玩家站在压力板、移动平台或胜利区内直接断线/销毁 Pawn。
- 预期现象：没有正常 EndOverlap，集合保留旧引用，门永久开启或错误胜利。
- 根因：Actor 销毁不保证沿用正常离开区域的流程。
- 修复：占用集合使用弱引用；绑定占用者 `OnDestroyed`；每次求值前清除无效项；必要时由 GameMode 的退出流程通知机关。
- 回归：玩家正常离开、死亡、断线三条路径都会释放占用，机关恢复正确状态。

### 7.21 持钥匙玩家断线或死亡

- 分类：边界测试。
- 触发：钥匙已经附着到角色后，持有者销毁。
- 预期现象：Holder 失效、钥匙悬空或仍被判定为占用，其他玩家无法继续流程。
- 修复：服务器监听 Holder 销毁；执行 Detach、清空 Owner/Holder、恢复碰撞与可拾取状态，再复制最终状态。
- 回归：持钥匙者正常放入、死亡、断线时钥匙分别进入已消耗或可再次拾取状态，不出现双重持有。

### 7.22 两个客户端同时消费同一把钥匙

- 分类：故障注入。
- 触发：两名玩家在同一网络帧将同一钥匙送入插槽。
- 预期风险：重复增加目标计数或同一钥匙激活两个插槽。
- 修复：服务器唯一结算；`bActivated` 和 Key 的 Consumed 状态在修改前再次校验；成功后立即关闭碰撞，使操作幂等。
- 回归：并发触发 20 次，目标计数最多增加 1，所有客户端最终一致。

### 7.23 移动平台在每个客户端独立模拟

- 分类：故障注入。
- 注入方法：移除 Authority 限制，让平台 Tick 在服务器和所有客户端都修改 Transform。
- 预期现象：平台抖动、位置被服务器反复纠正，平台上的角色产生滑动或穿透。
- 根因：多个网络端同时写同一个权威 Transform。
- 修复：服务器计算移动，开启 Replicate Movement；客户端只接收。到达终点后关闭无效 Tick。
- 回归：Host/Client 站上平台，在 100/200 ms 延迟下观察位置误差、角色附着和开关门后的停止状态。

### 7.24 RepNotify 中再次写复制属性形成反馈

- 分类：故障注入。
- 注入方法：在 OnRep 中再次修改同一个复制属性或发起新的服务器状态请求。
- 预期现象：表现重复、状态来回覆盖，复杂情况下形成 RPC/复制反馈链。
- 根因：OnRep 应消费服务器结果并更新本地表现，不应重新成为权威写入口。
- 修复：OnRep 只调用幂等的 `ApplyState()`；所有规则修改集中在 Authority 路径。
- 回归：同一复制值重复应用不会二次播放关键 Gameplay 逻辑。

### 7.25 晚加入客户端状态缺失

- 分类：边界测试。
- 触发：门已打开、钥匙已插入或游戏已胜利后，再加入一个客户端。
- 预期：新客户端接收 Gate/Key/GameState 当前复制值，构造出与在线玩家一致的画面和 UI。
- 失败原因：关键结果只存在临时 Widget、蓝图局部变量或历史 Multicast 中。
- 修复：持续状态放在复制 Actor/GameState；BeginPlay 与 OnRep 都进入同一应用函数。
- 回归：分别在零进度、1/2、2/2、胜利后加入，检查门、钥匙、目标 UI 和输入状态。

### 7.26 弱网下重复结算或顺序异常

- 分类：边界测试。
- 环境：`Net PktLag=100/200`、`Net PktLoss=5`，分别由 Host 和 Client 操作。
- 关注：重复拾取、重复胜利、平台开关抖动、UI 先显示结果后回退。
- 修复原则：服务器权威、状态转换幂等、持久状态 RepNotify、瞬时表现不参与结算。
- 回归：所有端最终值一致；重复请求不会重复消耗资源；UI 允许延迟但不出现永久分歧。

### 7.27 手动 Collect Garbage 不是常规生命周期修复

- 分类：设计追问。
- 风险：用 `Collect Garbage` 掩盖 Widget、Delegate 或 Actor 引用未释放，会造成明显卡顿，且根因仍存在。
- 正确做法：Widget `RemoveFromParent` 后解除 Delegate；Actor 在 EndPlay/Destroyed 清 Timer 和回调；普通 UObject 让 GC 在安全时机回收。
- 定位：对象数量、引用链、Unreal Insights GC 事件和连续进出关卡后的内存曲线。
- 回归：连续进入/退出菜单和联机关卡 10 次，对象数不持续增长，不依赖手动强制 GC 才恢复。

### 7.28 Dedicated Server 迁移时误用本地玩家

- 分类：架构边界。
- 风险：服务器逻辑调用 `GetPlayerController(0)`、创建 UI、播放本地声音或依赖 Listen Server 画面，在 Dedicated Server 上失效。
- 修复：规则层只使用明确传入的 Controller/Pawn 和 Authority 数据；表现用 Client RPC、Multicast 或 OnRep；UI 只在本地控制器创建。
- 回归：先用 Listen Server 双端验证；若后续迁移 DS，再执行无渲染 Server Target 冒烟测试。

### 7.29 问题定位统一流程

遇到“节点执行了但另一端没有反应”时，按固定顺序排查：

```text
1. 确认代码运行在哪一端：Role / HasAuthority / IsLocallyControlled
2. 确认 RPC 所在 Actor 的 Ownership 是否满足
3. 确认 Actor 与属性已开启 Replication
4. 确认写值发生在服务器，且值确实发生变化
5. 确认 OnRep、Client RPC 或 Multicast 是否在预期端进入
6. 确认表现对象有效，且没有被 UI/动画旧状态覆盖
7. 加入延迟、丢包和晚加入重新验证
```

日志最少包含：

```text
时间、NetMode、LocalRole、RemoteRole、Actor、Owner、操作名、旧值、新值
```

这样能把“网络没有同步”拆成权限、所有权、复制配置、异步时序、表现接入五类具体问题，而不是盲目重连节点。

---

## 8. 蓝图与关卡接入清单

### 8.1 Session UI

1. `Get Game Instance`，Cast 到 `multiplayerGameInstance`。
2. Host 按钮调用 `HostGame("Coop Session", 2, true)`。
3. Find 按钮调用 `FindGames(50, true)`。
4. 绑定 `OnFindComplete`，根据 `FmultiplayerSessionInfo` 创建房间列表。
5. Join 按钮传入对应 `ResultIndex`。
6. 退出房间调用 `DestroyGameSession`。
7. 绑定 `OnSessionOperationChanged`；状态非 `None` 时禁用相关按钮。
8. 绑定 Host/Find/Join/Destroy 完成事件显示结果。

### 8.2 网络实验表现

在 Character 蓝图中可选绑定：

- `OnNetworkActionCountChanged`：显示复制后的最终计数。
- `OnServerActionConfirmed`：只给请求玩家显示“服务器确认”。
- `OnNetworkActionEffect`：在所有端播放同一位置的声音或粒子。

### 8.3 机关关卡

1. 为 Gate、MovingPlatform、Key、KeySocket 创建蓝图子类。
2. 替换 Mesh，配置碰撞范围、平台 Offset、速度、RequiredPlayers 和钥匙 Socket。
3. 放置两把 Key、两个 KeySocket、一个 WinArea。
4. GameMode 使用 `multiplayerGameMode`，GameState 应为 `multiplayerCoopGameState`。
5. 胜利 UI 从 GameState 绑定 `OnObjectiveProgressChanged` 与 `OnGameWon`。
6. Authority、复制、占用集合和胜负规则不迁移进蓝图。

---

## 9. 双端验收矩阵

| 编号 | 场景 | 预期 | 状态 |
|---|---|---|---|
| S1 | Host 创建 LAN Session | 建房成功并进入 Listen 地图 | 待验证 |
| S2 | Client 搜索并加入 | 获得地址并 ClientTravel | 待验证 |
| S3 | 重复点击 Host/Find | 第二个异步操作被 C++ 拒绝 | 待验证 |
| R1 | Client 请求 ServerAction | 服务端计数，拥有者收到 Client RPC | 待验证 |
| R2 | Multicast 表现 | Host 与 Client 各触发一次 | 待验证 |
| G1 | 两人分别踩压力板 | 两端门打开 | 待验证 |
| G2 | 同一玩家触发两块板 | 不满足双人约束 | 待验证 |
| G3 | 两人站上平台 | 仅服务器移动，客户端平滑同步 | 待验证 |
| K1 | Client 拾取钥匙 | 服务端确认，所有端看到附着 | 待验证 |
| K2 | 两个插槽激活 | GameState 显示 2/2 | 待验证 |
| W1 | 两人进入胜利区 | 服务端结算，双方显示胜利 | 待验证 |
| L1 | 完成目标后晚加入 | 新客户端获得当前目标/门状态 | 待验证 |
| N1 | 100/200 ms 延迟、5% 丢包 | 无重复结算，最终状态一致 | 待验证 |
| A1 | Client 直接改机关状态 | 服务器拒绝越权结果，其他端不受影响 | 待验证 |
| A2 | Client 在无 Ownership 的机关上发 Server RPC | RPC 不进入服务器，改由拥有者链路或服务端碰撞处理 | 待验证 |
| D1 | Host/Find/Join 连续快速点击 | 仅首个异步操作进入，每个回调只执行一次 | 待验证 |
| D2 | 第二次搜索后使用旧 ResultIndex | 安全拒绝，不误加入、不越界 | 待验证 |
| T1 | 使用错误地图路径建房 | 广播 Travel 失败、销毁半成品 Session、允许重试 | 待验证 |
| O1 | 单玩家胶囊与 Mesh 同时 Overlap | 去重后玩家数仍为 1 | 待验证 |
| O2 | 玩家站在机关中断线 | OnDestroyed/弱引用清理，占用正确释放 | 待验证 |
| K3 | 两名玩家同时提交同一钥匙 | 服务器只消费一次，目标计数只加 1 | 待验证 |
| K4 | 持钥匙玩家死亡或断线 | 钥匙释放或复位，不产生永久占用 | 待验证 |
| M1 | 平台在 200 ms 延迟下承载 Client | 服务器轨迹唯一，无明显双端抢写与持续抖动 | 待验证 |
| G4 | 连续进入/退出菜单和关卡 10 次 | Delegate、Widget、Session 对象数量不持续增长 | 待验证 |

建议 PIE：

```text
Number of Players = 2
Net Mode = Play As Listen Server
Run Under One Process = false
```

---

## 10. 课程映射

- P6-P13：Role、Authority、变量复制、RepNotify、Server RPC、Validation、服务端生成 Actor，代码完成。
- P14-P18：Multicast、Ownership、Client RPC，代码完成，双端表现待验证。
- P19-P37：Null OSS、GameInstance、Create/Find/Join/Destroy、回调、房间列表数据和防重入，代码完成，UI/双端待验证。
- P38-P44：压力板、C++ 构造、每帧检测替换为事件驱动、Delegate 和双人机关，代码完成，关卡待验证。
- P45-P53：Transporter、同步移动平台、双人开启机关，代码完成，蓝图待配置。
- P54-P62：钥匙拾取、插槽、目标状态、胜利区和共享结算事件，代码完成，蓝图待配置。
- P63-P66：进阶关卡设计属于内容制作，不阻塞网络技术闭环。

---

## 11. 面试重点

### Q1：为什么机关有时复制状态，有时复制移动？

门动画不影响服务器碰撞逻辑时，可以复制开关并在本地插值；移动平台会承载角色并影响碰撞，需要服务器模拟位置并复制移动。

### Q2：为什么压力板不使用 Server RPC？

服务端碰撞世界已经能观察事实。让客户端报告只增加作弊面和重复消息。

### Q3：为什么 Session 回调要保存 Delegate Handle？

操作是异步的。Handle 用于完成后或 Shutdown 时精确解绑，避免重复回调和对象生命周期结束后的残留绑定。

### Q4：为什么 Session 需要操作状态机？

网络操作尚未结束时再次调用会产生重入。状态机让规则层拒绝交叉操作，UI 禁用只是额外反馈。

### Q5：Reliable 是否越多越好？

不是。Reliable 需要重传并保持顺序，高频使用会造成队头阻塞。关键低频请求用 Reliable；瞬时表现可用 Unreliable；持续状态用属性复制。

### Q6：如何支持晚加入？

把门、钥匙进度和胜利结果保存为复制属性；新客户端生成 Actor/GameState 后接收当前状态，而不是依赖历史 Multicast。

### Q7：为什么目标进度放 GameState，不放 GameMode？

GameMode 只存在服务器；GameState 会复制到客户端，适合所有玩家可见的关卡进度和胜负状态。

### Q8：如果迁移 Dedicated Server，需要改什么？

权威机关规则可复用；需构建 Server Target、部署服务器、处理会话注册和进程生命周期，并移除所有假设服务器拥有本地画面的表现逻辑。

### Q9：Server RPC 为什么不能随便写在任意机关上？

客户端发往服务器的 RPC 受 Ownership 约束。世界中的门和压力板通常属于服务器，不归某个客户端所有。当前方案优先让服务器碰撞世界直接判断事实；必须由客户端发起的交互，则经由其拥有的 Character 或 PlayerController 请求，再由服务器校验目标、距离和状态。

### Q10：属性复制、RepNotify 和 Multicast 如何选择？

- 持续且晚加入也要恢复的状态：复制属性与 RepNotify。
- 关键低频客户端意图：Reliable Server RPC。
- 只给请求者的确认：Client RPC。
- 可丢失的瞬时表现：Unreliable Multicast。

选择依据是数据语义，不是哪个节点接线更方便。

### Q11：为什么机关占用使用 `TSet<TWeakObjectPtr<ACharacter>>`？

`TSet` 解决同一玩家多个碰撞组件造成的重复计数；弱引用避免机关成为 Pawn 生命周期的所有者。它不能替代主动清理，因此仍在 EndOverlap、OnDestroyed 和求值前清除失效项。

### Q12：如何保证钥匙消费和胜利结算不会重复？

所有结算只在服务器执行，修改前检查 `bActivated`、Consumed 和 `bGameWon`，成功后立即写入终态并关闭重复触发入口。也就是说操作设计为幂等：相同请求执行多次，结果与执行一次相同。

### Q13：为什么 Session 管理放在 GameInstance？

GameInstance 跨关卡存在，适合持有 OnlineSubsystem 接口、搜索对象和异步 Delegate。GameMode 在 Travel 后重建且只存在服务器，Widget 生命周期更短，都不适合作为 Session 生命周期的唯一所有者。

### Q14：关卡切换时哪些对象会保留？

GameInstance 通常保留；GameMode、GameState、关卡 Actor 和大多数 Widget 随 World 重建。PlayerController/Pawn 是否保留与 Travel 类型有关。不能假设旧关卡 Actor 引用在新 World 中仍有效，异步回调也必须处理 Travel 和 Shutdown。

### Q15：弱网测试重点看什么？

不只看画面是否卡。重点检查服务器最终值是否唯一、操作是否重复结算、Reliable 是否堆积、RepNotify 是否最终到达、晚加入能否恢复、UI 是否可能被过期回调覆盖。瞬时粒子偶尔丢失可以接受，钥匙和胜利状态分歧不可接受。

### Q16：为什么移动平台和门使用不同同步策略？

门的最终开关是低频离散状态，可以复制状态并让各客户端本地播放动画；移动平台持续影响碰撞和角色承载，必须由服务器模拟权威 Transform 并复制移动。两者的 Gameplay 语义和更新频率不同。

### Q17：如果支持 3～4 人，需要重写网络框架吗？

RPC、Replication 和 Session 框架无需重写；需要把机关的 RequiredPlayers、Session 公开连接数和胜利条件参数化，并测试断线与人数变化。当前玩法设计仍以双人协作为验收目标，不能仅凭 Session 容量为 4 就宣称完成四人玩法。

### Q18：当前项目没有实现哪些高级网络能力？

未实现自定义角色预测回滚、射击延迟补偿、无缝断线重连和 Dedicated Server 部署。角色移动使用 `UCharacterMovementComponent` 内置网络移动；机关同步聚焦服务器权威和最终一致性。面试时必须明确边界，不把原理了解表述成已落地。

### Q19：AI 辅助在项目中承担了什么？

AI 用于检索 API 迁移线索、审查异步回调和边界条件、生成测试清单及辅助整理文档。技术方案必须通过当前 UE 头文件、编译日志和双客户端实验验证；Ownership、Authority、数据语义和最终代码解释由开发者负责。

### Q20：如果再给一周，优先改什么？

先完成双端和弱网测试并修复真实问题，再补可重复自动测试、Session UI 错误反馈和晚加入验证。只有数据表明平台同步或带宽存在瓶颈，才调整 NetUpdateFrequency、条件复制或自定义平滑，不先加入预测回滚等大系统。

---

## 12. 教程完成度与收口边界

### 12.1 当前完成度

| 教程范围 | C++ 状态 | 蓝图/关卡状态 | 双客户端状态 |
|---|---|---|---|
| P6-P18：Role、Authority、Replication、RepNotify、Server/Client/Multicast RPC | 已完成并通过编译 | 仅需绑定可视化事件 | 待验收 |
| P19-P37：Null OSS、GameInstance、Create/Find/Join/Destroy Session | 已完成并通过编译 | 菜单、房间列表和按钮状态待接线 | 待验收 |
| P38-P53：压力板、委托、Transporter、同步移动平台、双人机关门 | 核心规则已完成 | Mesh、碰撞、动画和关卡实例待配置 | 待验收 |
| P54-P62：钥匙、插槽、共享目标、胜利区和结算 | 核心规则已完成 | 拾取/插槽表现、胜利 UI 待配置 | 待验收 |
| P63-P66：移动参数与进阶关卡设计 | 未完成 | 属于内容制作 | 非网络闭环的阻塞项 |

按知识点和 C++ 覆盖约为 **80%～85%**；按可玩、可演示并通过双客户端验证约为 **55%～60%**。因此目前只能表述为“C++ 网络核心已完成”，不能表述为“完整 Co-op 项目已完成”。

### 12.2 基础版 C++ 冻结原则

- 基础版不增加 GAS、预测回滚、复杂重连、通用交互框架或 Dedicated Server 部署。
- 只有蓝图联调或双端测试暴露真实缺口时，才修改 C++。
- 已保留的工程保护仅包括：Session 异步防重入、网络/Travel 失败事件、玩家销毁时解除压力板/平台/胜利区占用，以及持钥匙玩家销毁后的显式释放。
- 这些保护直接避免 UI 无反馈、机关永久占用和残留引用，不作为新的独立系统包装。

### 12.3 GAS 进阶版边界

GAS 是基础版通过双客户端验收后的独立进阶版本，目前只完成方案设计，不能表述为已实现。基础版先建立可回退的 Git 标签，再从独立分支继续开发，避免技能系统影响已验证的 Session 和机关闭环。

进阶版只做一条可解释、可验证的最小链路：

```text
PlayerState 持有 AbilitySystemComponent 和 AttributeSet
→ Character 通过 AbilitySystemInterface 暴露能力系统
→ 服务端授予并激活一项双人玩法相关 Ability
→ GameplayEffect 修改属性或状态
→ 两客户端验证 Authority、Ownership、预测边界和复制结果
```

- 优先选择冲刺、交互或救援中的一项，不一次铺开完整技能树。
- 压力板、门、钥匙和胜利区继续使用服务器权威 Actor 规则，不为了“使用 GAS”而迁移成熟机关。
- GAS 进阶版有独立验收记录；未通过前不计入基础版完成度。

## 13. 下一步收口顺序

1. 完成 Session 菜单最小蓝图接线并进行 S1-S3。
2. 在测试地图放置 Gate、MovingPlatform、Key、KeySocket、WinArea。
3. 完成 R1-R2、G1-G3、K1-K2、W1。
4. 测试晚加入、100/200 ms 延迟和 5% 丢包。
5. 保存双端日志、截图和 60～90 秒演示视频。
6. 整理 README、简历描述和 Git 模块化提交。
7. 为通过验收的基础版创建 Git 标签，再开独立分支实现最小 GAS 链路。

在这些验收前，准确表述是：

> 已完成 UE5 Co-op 的 C++ 网络核心，包括服务端权威 RPC、属性复制、Session 生命周期和多种协作机关；正在完成蓝图配置与双客户端验证。

验收后才可表述为：

> 实现并验证双人 LAN Co-op 的建房、搜索、加入、权威机关、共享目标和弱网下最终一致性。

---

## 14. 多人网络常见技术问题

本章是面试知识库，不代表所有内容都已在项目中实现。回答时使用以下标记：

- **已实现**：代码存在，并应补双客户端证据。
- **引擎能力**：项目使用 UE 内置实现，能够解释原理但没有重写底层。
- **扩展方案**：能够设计，但不能描述为已完成。

### 14.1 Authority、Role 与 Ownership 有什么区别？

- Authority 表示谁拥有 Gameplay 状态的最终决定权，通常是服务器。
- Role 描述当前机器上的 Actor 是 Authority、Autonomous Proxy 还是 Simulated Proxy。
- Ownership 表示 Actor 的网络拥有链，决定客户端能否在该 Actor 上发 Server RPC，以及哪些 OwnerOnly 数据发给谁。
- `IsLocallyControlled()` 回答“这个 Pawn 是否由本机玩家控制”，不能替代 `HasAuthority()`。

常见错误是把“本机控制”误认为“有服务器权威”，Listen Server 上两者有时同时为真，远程客户端上则不同。

### 14.2 Actor 开启复制后，所有成员都会自动同步吗？

不会。Actor 需要 `bReplicates = true`，具体成员还要用 `UPROPERTY(Replicated)` 或 `ReplicatedUsing` 声明，并在 `GetLifetimeReplicatedProps` 中注册。组件也需要满足复制配置。服务器写值后，复制系统根据连接相关性、频率、条件和属性变化发送增量。

### 14.3 RepNotify 什么时候触发？

RepNotify 主要在接收端观察到复制属性变化时触发。它适合把“状态到达”映射成客户端表现。服务器本地表现不应只依赖 OnRep；项目使用服务端直接调用 + 客户端 OnRep 调用同一 `ApplyState()` 的结构。

### 14.4 RPC 方向如何选择？

| 类型 | 发起方向 | 典型用途 | 不适合 |
|---|---|---|---|
| Server RPC | 拥有该 Actor 的客户端 -> 服务器 | 交互请求、开火请求 | 客户端直接决定伤害或胜利 |
| Client RPC | 服务器 -> 指定拥有客户端 | 私有提示、拒绝原因 | 广播公共状态 |
| Multicast RPC | 服务器 -> 当前相关客户端 | 瞬时音效、粒子 | 晚加入需要恢复的持续状态 |
| Replication/RepNotify | 服务器 -> 相关客户端 | 血量、门状态、目标进度 | 每帧高频无差别传输 |

### 14.5 `_Validate` 能否代替完整反作弊？

不能。Validation 只能作为请求的第一层参数检查。服务器仍需验证：请求者 Ownership、目标有效性、距离、冷却、资源、状态机和操作频率。客户端传来的命中结果、伤害值和胜利结果都不能直接信任。

### 14.6 Reliable 为什么会造成卡顿？

Reliable 消息丢失后要重传并保持顺序。高频 Reliable 会让后续消息等待，形成队头阻塞。可靠性应由语义决定：关键请求可靠；连续输入、瞬时粒子或可由下一次更新覆盖的数据通常不需要全部可靠。

### 14.7 NetUpdateFrequency、Relevancy、Priority 和 Dormancy 分别解决什么？

- NetUpdateFrequency：Actor 被考虑发送网络更新的频率。
- Relevancy：某连接是否需要知道这个 Actor。
- Priority：带宽紧张时谁先发送。
- Dormancy：长期不变化 Actor 暂停常规复制，变化前需唤醒或 Flush Dormancy。

优化顺序应先测量，再减少不相关对象、降低稳定对象更新频率、使用条件复制，最后才考虑自定义序列化。

### 14.8 条件复制有哪些典型用途？

- `COND_OwnerOnly`：仅拥有者需要的私有数据。
- `COND_SkipOwner`：拥有者已经本地预测，主要发给其他客户端。
- `COND_InitialOnly`：只在初始复制时需要的稳定配置。
- `COND_SimulatedOnly`：只给模拟代理的表现数据。

条件选择错误可能泄露私有信息，或让 Listen Server/Owner 缺少必要状态。

### 14.9 UE 角色移动为什么比普通 Actor 同步复杂？

`UCharacterMovementComponent` 包含客户端预测、ServerMove、服务器校验、修正和重放未确认输入。项目使用这套内置能力，不能写成“自研预测回滚”。普通机关平台则使用服务器权威位置 + Replicate Movement，没有实现同等级的自定义预测。

### 14.10 插值、外推和回滚分别何时使用？

- 插值：在已收到的两个状态之间平滑显示，稳定但增加显示延迟。
- 外推：根据速度预测短期未来，响应快但误差会积累。
- 客户端预测 + 回滚重放：本地先执行输入，收到服务器纠正后回到权威状态并重放未确认输入。
- 延迟补偿：服务器按客户端开火时刻回看历史碰撞状态，常用于竞技 Hitscan。

当前 Co-op 项目依赖角色移动内置预测，没有实现自定义回滚或射击延迟补偿。

### 14.11 为什么不能只同步 Transform？

只同步 Transform 无法表达离散 Gameplay 语义，例如门是 Opening 还是 Open、钥匙是否已经消费、胜利是否结算。持续运动可复制位置；规则状态应复制明确枚举或布尔值。客户端根据状态播放表现，避免从一个瞬时坐标反推规则。

### 14.12 晚加入如何恢复世界？

晚加入客户端只能接收当前复制状态，收不到加入前发生的 RPC。需要恢复的数据必须存放在复制 Actor、PlayerState 或 GameState 中。门状态、钥匙进度、倒计时和胜负结果都应有当前快照；粒子和声音不需要补播历史。

### 14.13 GameMode、GameState、PlayerState、PlayerController 和 GameInstance 如何分工？

- GameMode：仅服务器存在，保存规则和权威判定。
- GameState：服务器写、客户端读，保存全局可见进度。
- PlayerState：保存所有客户端可见或需要跨 Pawn 重生的玩家状态。
- PlayerController：每名玩家在服务器和其拥有客户端存在，适合拥有者 RPC 和本地 UI 入口。
- GameInstance：跨关卡存在，适合 Session 生命周期和本地全局服务，但不自动网络复制。

### 14.14 Listen Server、Dedicated Server 和 P2P 的取舍是什么？

- Listen Server：部署简单、适合小型 Co-op；Host 有本地延迟优势，Host 退出会终止会话。
- Dedicated Server：公平、稳定、便于反作弊；需要服务器构建、部署、监控和成本。
- P2P/平台联机：平台负责发现和连接，但 Gameplay 权威模型仍需明确；NAT、Host 迁移和安全性更复杂。

当前项目验收目标是双人 Listen Server LAN/Session，不宣称 Dedicated Server 已部署。

### 14.15 Session 和 Gameplay Replication 是什么关系？

Session 负责发现房间、加入、连接信息和房间生命周期；Replication/RPC 负责连接建立后的 Gameplay 状态同步。能建房不代表机关同步正确，机关同步正确也不代表房间搜索和销毁完整，两条链必须分别测试。

### 14.16 Seamless Travel 解决什么？

它减少地图切换时连接断开，并允许特定 Actor 在过渡中保留。它不自动解决所有状态迁移；需要明确哪些状态在 PlayerState/GameInstance，哪些 Actor 允许保留，以及新 GameMode/GameState 如何恢复。当前项目未把 Seamless Travel 作为验收范围。

### 14.17 断线重连需要哪些层次？

扩展方案至少包括：重新发现/连接 Session、识别原玩家身份、恢复 PlayerState、重建 Pawn、发送当前世界快照、处理离线期间的目标变化。仅调用 JoinSession 不等于完成断线重连。

### 14.18 Host 退出如何处理？

Listen Server 的 Host 就是权威服务器，Host 退出后房间通常终止。基础方案向 Client 广播网络失败并返回菜单；真正 Host Migration 需要选新 Host、保存权威快照、重建 Session、重连和恢复状态，当前项目未实现。

### 14.19 大量复制数组如何优化？

小数组可以普通属性复制；频繁增删的大数组可考虑 `FFastArraySerializer`，只复制条目级变化并提供新增、修改、删除回调。是否使用取决于数据规模和更新频率。当前钥匙目标只有少量状态，原子结构比引入 Fast Array 更清楚。

### 14.20 如何定位网络带宽和复制瓶颈？

先固定玩家数、场景和操作脚本，再使用网络统计、Network Profiler/Insights 和日志观察：每秒 Actor/属性/RPC 数、包大小、Reliable 队列、相关 Actor 数和尖峰。然后验证 Relevancy、频率、Dormancy、量化和条件复制，不以“感觉更流畅”作为结论。

### 14.21 网络对象生命周期有哪些高风险点？

Actor 销毁、Player 断线、关卡 Travel、Session Shutdown、Delegate 回调晚到和 Widget 解绑。处理原则是：权威端关闭规则入口，弱引用避免错误拥有，Timer/Delegate 成对清理，异步回调验证当前 World 和操作世代。

### 14.22 网络代码如何测试？

至少覆盖 Host/Client 两种发起者、正常/重复/越权请求、晚加入、断线、延迟、丢包和关卡切换。每项都比较服务器最终值和所有客户端最终值。PIE 单进程只适合早期调试，正式证据应使用独立进程或打包客户端。

---

## 15. 多人网络场景题与实现链

以下题目给出可落地答案。标注“扩展”的功能用于面试设计题，不写入当前完成项。

### S1：实现一个只能由两名不同玩家同时踩下的门

**当前项目已实现。**

```text
服务器监听两块压力板 Overlap
→ 每块板用 TSet 保存不同 Character
→ Gate 汇总板状态和不同玩家身份
→ 满足条件后服务器写 bOpen
→ RepNotify 到客户端
→ 各端 ApplyDoorState 播放门动画
```

必须处理同一玩家触发多个组件、玩家销毁、门动画重复播放和晚加入。

### S2：实现一把只能被一个玩家拾取的钥匙

**当前项目已实现 C++ 核心。**

```text
服务器检测拾取范围
→ 校验 Key 未被持有、Character 有效
→ 设置 Holder / Owner
→ 附着到角色 Socket，关闭世界碰撞
→ Holder RepNotify 更新客户端表现
```

两个玩家同时拾取时，服务器先处理者成功；后到请求在状态校验处失败。持有者死亡或断线时服务器释放钥匙。

### S3：实现钥匙插槽和共享目标进度

**当前项目已实现 C++ 核心。**

```text
持钥匙玩家进入插槽
→ 服务器校验钥匙类型、距离、插槽未激活
→ 原子地消费钥匙并设置 bActivated
→ GameState 增加已激活数量
→ 复制原子 ObjectiveState
→ UI 从 OnObjectiveProgressChanged 更新
```

`bActivated` 和 Consumed 状态保证重复 Overlap 不会重复计数。

### S4：实现两人都到达终点才胜利

**当前项目已实现 C++ 核心。**

服务器维护胜利区中的不同玩家集合，同时检查目标钥匙数。胜利只由 `GameState::TryCompleteGame` 结算，`bGameWon` 只允许从 false 变为 true，并通过 RepNotify 驱动所有端 UI。

### S5：实现服务器权威的开火与伤害

**扩展场景。**

```text
拥有者本地输入并播放可预测表现
→ ServerFire(起点、方向、客户端时间、序号)
→ 服务器校验射速、弹药、状态和方向偏差
→ 服务器执行 Trace 或弹丸生成
→ 服务器修改目标 Health
→ Health RepNotify 到相关客户端
→ Unreliable Multicast 播放公共枪口/命中特效
```

客户端不能上传最终伤害值。竞技场景再考虑历史快照与延迟补偿；普通 Co-op 可以先接受服务器时刻判定。

### S6：同步血量、弹药和受击表现

**扩展场景。**

- Health：服务器权威复制，OnRep 更新 UI/受击表现。
- Ammo：服务器校验；拥有者需要精确值，可 OwnerOnly；其他玩家通常只需换弹/开火状态。
- 受击方向：可复制压缩后的方向或发送一次性 Client/Multicast 表现事件。
- 死亡：复制明确死亡状态，不依赖某次死亡动画 RPC。

### S7：同步 Projectile 与爆炸

**扩展场景。**

服务器生成复制 Projectile，服务器负责碰撞与爆炸伤害；客户端显示复制位置。高速弹丸可用服务器 Trace 结算并只同步表现，避免低频位置复制穿透。爆炸最终伤害在服务器，特效可 Multicast。

### S8：实现可搬运物体

**当前项目已有 Transporter/平台基础，搬运玩法属于扩展。**

服务器确认抓取者和物体 Ownership/状态；服务器模拟或认可受限输入；物体 Transform/物理状态复制。需要限制客户端上传的位置跳变，处理两个玩家同时抓取、放手、断线和休眠唤醒。

### S9：实现同步移动平台

**当前项目已实现 C++ 核心。**

服务器根据激活人数驱动平台移动，`ReplicateMovement` 同步到客户端；客户端不独立 Tick 写位置。弱网测试观察承载角色的相对滑动，并根据数据决定是否提高更新频率或增加本地平滑。

### S10：实现倒计时和共享任务状态

**扩展场景。**

服务器保存结束时间戳而非每帧复制剩余秒数；GameState 复制 EndTime，客户端用同步后的服务器时间本地显示倒计时。任务阶段使用枚举 RepNotify，阶段切换只在服务器发生。

### S11：实现玩家准备和统一开局

**扩展场景。**

每名玩家的 Ready 状态放 PlayerState 并复制；GameMode 统计所有有效玩家，满足人数和全部 Ready 后切换 GameState 的 MatchPhase。客户端按钮只发 ServerSetReady 请求，不能本地决定开始。

### S12：实现复活与观察者模式

**扩展场景。**

服务器确认死亡，PlayerState 保留跨 Pawn 数据；PlayerController 切换到 Spectator，倒计时结束后由 GameMode 选择合法出生点并重新 Spawn/Possess。Health、输入和 UI 在新 Pawn 上重新绑定，旧 Delegate 必须解绑。

### S13：实现聊天或标记系统

**扩展场景。**

聊天通过拥有者 Server RPC，上限频率和长度由服务器校验，再按队伍或房间发送 Client/Multicast。世界标记由服务器校验目标位置和可见范围，复制轻量标记 Actor 或结构；不应把任意客户端文本/位置无条件广播。

### S14：实现背包和物品同步

**扩展场景。**

服务器拥有背包真实数据；客户端发送拾取/使用意图。少量物品可复制数组，大量频繁变化物品考虑 Fast Array。UI 从复制回调更新。需要处理容量、重复物品 ID、并发拾取、掉落、重连和晚加入。

### S15：实现 3～4 人房间和按人数开启机关

**扩展场景。**

Session 的 PublicConnections 设置为 4；机关的 RequiredPlayers 数据化；服务器根据当前有效 PlayerState/Pawn 统计。玩家中途退出时重新求值。要测试 2/3/4 人、最后一个位置竞争、Host 退出和人数不足时的降级规则。

### S16：实现晚加入后的完整状态恢复

**当前项目必须验证。**

门、钥匙、目标、胜利结果放复制状态；玩家加入后 Actor Channel 建立并获得当前快照。UI 在绑定 GameState 后主动读取一次当前值，再等待 Delegate，避免绑定发生在首次 OnRep 之后导致空白。

### S17：实现断线后的清理

**当前项目必须验证。**

```text
连接断开
→ Pawn/Controller 销毁或退出
→ 机关移除弱引用占用者
→ 释放钥匙和交互权
→ GameMode 重新计算人数与胜负条件
→ 剩余客户端收到最终复制状态
```

基础版返回菜单并显示错误；真正重连恢复属于后续扩展。

### S18：实现低带宽下的大量可交互物

**扩展场景。**

先根据距离和玩法相关性裁剪复制对象；稳定机关进入 Dormancy；只复制状态变化；减少每帧 RPC；对数值做合理量化；大量列表使用增量序列化。用 Profiler 对比每秒字节数和 Actor 更新数，不凭空降低 NetUpdateFrequency。

### S19：两个玩家同时操作同一机关，如何解决竞态？

服务器串行执行最终状态修改；每次操作先检查当前版本/状态，成功后立即写终态，使请求幂等。若操作需要顺序，可增加服务器操作序号或状态版本。客户端预测失败时回滚表现，不回滚服务器状态。

### S20：如何把 Listen Server 项目迁移到 Dedicated Server？

**扩展场景。**

复用服务器权威规则；增加 Server Target 和部署配置；剥离服务器上的 UI、音频和本地玩家假设；接入服务器注册/匹配；处理进程生命周期、日志、监控和无玩家时回收。先做无渲染启动和两客户端冒烟测试，再讨论规模化部署。

---

## 16. 场景题统一作答模板

面试官给出任何多人功能时，按以下顺序回答：

```text
1. 谁拥有最终权威？
2. 状态放在哪个框架对象？
3. 客户端发送什么意图，服务器校验什么？
4. 最终结果用 RPC、RepNotify 还是 Replicate Movement？
5. 晚加入如何恢复？
6. 重复请求、同时请求和断线如何处理？
7. 弱网下允许什么延迟，绝不能丢什么状态？
8. 用什么日志、断点和双端矩阵验证？
```

只要这八项完整，答案就不再是“调用一个 UE API”，而是包含权限、数据、时序、生命周期和验证的工程方案。

---

## 17. 会话菜单与地图切换排错记录（2026-08-04）

本轮目标是完成主菜单中的三个最小联网入口：

```text
创建房间
加入第一个可用房间
退出游戏
```

界面只负责收集操作和展示状态，真正的 Session 生命周期由
`UMultiplayerGameInstance` 管理。这样关卡切换后会话对象仍然存在，
也避免把 Online Subsystem 逻辑分散到多个 Widget 和 Level Blueprint。

### 17.1 主菜单职责与调用链

Widget 构造时只初始化一次引用和委托：

```text
Event Construct
→ Get Game Instance
→ Cast To multiplayerGameInstance
→ 缓存为 SessionGI
→ Bind Event to OnFindComplete
→ Bind Event to OnSessionOperationChanged
```

创建房间：

```text
Host 按钮 OnClicked
→ 禁用 Host / Join 按钮
→ SessionGI.HostGame("MyRoom", 2, true)
→ GameInstance 发起异步 CreateSession
→ 成功后 ServerTravel(MapPath + "?listen")
```

加入房间：

```text
Join 按钮 OnClicked
→ 禁用 Host / Join 按钮
→ SessionGI.FindGames(20, true)
→ 等待 OnFindComplete(WasSuccessful, Results)
→ WasSuccessful == true
→ Results.IsValidIndex(0) == true
→ Results[0]
→ Break Multiplayer Session Info
→ 将 ResultIndex 传给 SessionGI.JoinGame
```

退出游戏：

```text
Exit 按钮 OnClicked
→ Get Owning Player
→ Quit Game
```

`Quit Game` 的 `Specific Player` 需要 `PlayerController`。Widget 的
`Get Owning Player` 正好返回控制该界面的本地玩家控制器，不能传
`PlayerState`，也不应使用场景中任意 Pawn 代替。

### 17.2 为什么不能在点击 Join 后立即读取搜索结果

`FindSessions` 是异步操作。调用函数时只是提交搜索请求，结果会在后续帧由
Online Subsystem 通过完成委托返回。因此以下逻辑是错误的：

```text
FindGames
→ 立刻读取 Results
→ JoinGame
```

正确做法是在 `OnFindComplete` 中消费本次搜索结果。`ResultIndex` 只对当前
搜索结果数组有效；重新搜索以后旧索引不能继续使用，也不能把索引当作永久房间 ID。

### 17.3 按钮禁用与异步状态恢复

点击 Host 或 Join 后立即禁用两个按钮，防止用户在异步请求尚未完成时重复创建、
重复搜索或交叉调用。GameInstance 使用会话操作状态作为第二层保护：

```text
Idle / None
Creating
Finding
Joining
Destroying
```

Widget 监听 `OnSessionOperationChanged`：

```text
NewOperation == None
→ Host 和 Join 按钮重新启用

NewOperation != None
→ Host 和 Join 按钮保持禁用
```

UI 禁用解决重复点击体验问题，C++ 状态机解决逻辑重入问题。两层不能互相替代。

### 17.4 本轮遇到的蓝图错误

#### 问题一：按钮里的 Cast 没有 Object

**现象：** `Cast To multiplayerGameInstance` 节点报错，HostGame 无法调用。

**根因：** Cast 只做运行时类型检查，不会自动寻找对象。未给 Object 输入时，
节点不知道要检查哪个实例。

**修复：** 在 Construct 中通过 `Get Game Instance` 完成一次 Cast，并缓存
`SessionGI`；按钮事件直接使用缓存引用。

#### 问题二：OnFindComplete 签名不匹配

**现象：** 编译提示自定义事件与委托签名不匹配，或 `Results` 没有返回值。

**根因：** C++ 委托参数修改后，旧蓝图事件仍保存旧签名；手动创建的同名事件
也不等于该委托要求的事件类型。

**修复：** 删除旧事件，从 `Bind Event to OnFindComplete` 的 Event 引脚拖线，
让编辑器重新生成完全匹配的自定义事件，再重新连接 `WasSuccessful` 和 `Results`。

#### 问题三：Branch 条件被写死为 True

**现象：** 搜索失败或结果为空时仍然继续 Get(0)，可能加入失败或出现数组越界。

**修复：** 第一层 Branch 接 `WasSuccessful`；第二层 Branch 接
`Results.IsValidIndex(0)`。只有两层都为真才读取 `Results[0]`。

#### 问题四：JoinGame 参数类型连接错误

**现象：** `Multiplayer Session Info` 不能直接连接到 `JoinGame.ResultIndex`。

**根因：** 当前 C++ 接口接收搜索结果索引，而不是完整的蓝图会话结构。

**修复：** `Results[0] → Break Multiplayer Session Info → ResultIndex → JoinGame`。

#### 问题五：Quit Game 的对象类型错误

**现象：** 编译提示当前蓝图不是 PlayerState，或 Specific Player 必须连接。

**根因：** 将 `PlayerState`、`Pawn`、`PlayerController` 的职责混淆。

**修复：** Widget 中使用 `Get Owning Player`。Controller 管输入、视角和本地 UI；
Pawn 是被控制的场景实体；PlayerState 保存需要复制的玩家数据。

### 17.5 地图路径、打包与 ServerTravel

地图必须使用 Unreal 包路径，而不是 Windows 文件路径：

```text
/Game/UI/mainmenu
/Game/ThirdPerson/Maps/ThirdPersonMap
/Game/DesertCity/Levels/ExampleLevel1
```

服务器创建成功后的旅行形式为：

```text
ServerTravel("/Game/DesertCity/Levels/ExampleLevel1?listen")
```

不添加 `.umap`，不使用 `Content/...`，也不使用磁盘绝对路径。需要联机加载的
主菜单和游戏地图都应加入 Packaging 的地图列表，否则编辑器内可用不代表打包后可用。

GameInstance 暴露地图选择枚举，由蓝图选择测试地图或正式地图；C++ 负责把枚举
解析为经过校验的包路径。这样不会让 Widget 保存任意字符串并直接触发旅行。

### 17.6 复制地图后出现栈溢出

**现象：** 原始 DesertCity 地图可以打开，复制到
`/Game/scene/DesertCity/...` 后打开即退出，日志为 `EXCEPTION_STACK_OVERFLOW`，
调用栈反复出现 CoreUObject、Engine 和 Landscape。

**判断：** 这不是 Zen/DDC 网络通信错误。Zen 错误发生在缓存节点不可写阶段；
本问题发生在地图包加载和对象解析阶段。重复调用栈说明存在递归加载、循环引用，
或复制后的 Landscape / World Partition / External Actors 包关系不一致。

**处理：**

```text
1. 停止使用损坏副本，不把它加入打包地图列表。
2. 继续使用能够稳定打开的原始地图包。
3. 如需副本，只在 UE Content Browser 内执行 Duplicate / Migrate。
4. 修复重定向器并重新保存目标地图。
5. 分离检查 Landscape、PCG 和外部 Actor，避免文件管理器直接复制资产目录。
```

地图资产是由包名、GUID、软硬引用和外部 Actor 共同组成的对象图，不是若干普通文件。
因此“磁盘上复制成功”并不等于“UE 资产关系复制成功”。

### 17.7 验证矩阵

```text
1. 主菜单启动后 SessionGI 有效，委托只绑定一次。
2. Host 点击一次后按钮禁用，不产生重复 CreateSession。
3. CreateSession 成功后进入带 ?listen 的目标地图。
4. 第二实例搜索成功，Results 至少有一个合法元素。
5. JoinGame 使用本次结果的 ResultIndex，客户端完成 ClientTravel。
6. 搜索失败、结果为空、加入失败时按钮恢复并输出明确日志。
7. 原始 DesertCity 地图可单独打开，也可被 ServerTravel 打开。
8. 打包版本包含主菜单和所有可选游戏地图。
9. Host 与 Client 能在同一关卡互相观察移动，确认不是两个独立单机实例。
```

### 17.8 面试表达

这部分的价值不在“接了几个 UI 节点”，而在于识别并处理了三个工程边界：

```text
异步边界：Session 结果只能在完成回调中消费。
类型边界：Widget、GameInstance、PlayerController、Pawn、PlayerState 各负其责。
资源边界：网络旅行依赖稳定包路径和完整资产引用，而不是磁盘文件路径。
```

如果继续扩展，应优先补充错误状态展示、Host 退出后的会话销毁、搜索结果列表和
双实例日志证据；当前阶段不自研协议栈，也不把地图加载崩溃误归因于网络层。

### 17.9 Zen / DDC 阻断导致编辑器无法启动

**现象：** 编辑器在项目内容加载前崩溃，提示
`InstalledDerivedDataBackendGraph` 没有可写节点，调用栈位于
`DerivedDataCache`；本机 Zen 页面使用 `http://[::1]:8558/`。

**根因判断：** Zen 是 UE 的本地派生数据服务，编辑器需要通过 IPv6 回环地址
`::1` 与其通信。代理软件、系统代理或防火墙规则如果把本地回环流量送入代理，
编辑器就可能把“本地缓存服务不可达”表现为“没有可写 DDC 节点”。这与 Gameplay
联网代码、Steam Session 和远程服务器均无关。

**定位过程：**

```text
查看崩溃栈是否首先落在 DerivedDataCache
→ 使用 -DDC-ForceMemoryCache 验证能否绕过持久缓存启动
→ 关闭代理后复测 ::1:8558
→ 检查缓存目录写权限和剩余空间
→ 再分别启动 UnrealEditor 与 UnrealEditor-Cmd
```

**永久处理：** 让 v2rayN/系统代理绕过 `localhost`、`127.0.0.1` 和 `::1`，并允许
`UnrealEditor.exe` 与打包会使用的 `UnrealEditor-Cmd.exe` 访问本地回环；确认 DDC
目录可写。`-DDC-ForceMemoryCache` 只用于诊断或临时进入设置，不作为长期启动参数。

**收获：** 先按崩溃阶段分层。编辑器初始化期的 DDC 故障、地图包加载期的栈溢出、
运行期的 Session 失败是三类问题，不能因为都表现为“打不开”就归为网络问题。

### 17.10 双实例启动不等于联机成功

**现象：** 批处理能够打开两个窗口，但两个角色不能互相观察移动，或者两个窗口显示
不同的 UI / 地图。

**根因：** 两个进程存在只证明启动成功，不证明它们进入了同一个 Session。常见情况包括：

```text
两个实例都以 Standalone 身份进入默认地图
Host 没有使用 ?listen
Client 搜索条件与 Host 的 LAN / Presence 条件不一致
Join 成功回调未触发 ClientTravel
启动命令和项目配置选择了不同的默认地图
```

**正确证据链：**

```text
Host: CreateSession 成功
→ ServerTravel(...?listen)
→ 日志确认 Listen Server
Client: FindSessions 返回合法结果
→ JoinSession 成功
→ 解析 ConnectString
→ ClientTravel
→ 两端 CurrentPlayers == 2
→ 两端能观察同一角色移动或同一机关状态
```

测试时应保存两份独立日志，并给日志加实例标识。编辑器 PIE 适合快速验证，独立进程或
打包版本更适合验证真实 Travel、端口、配置和资源打包问题。Steam OSS 还需满足平台账号
和运行环境要求；Null OSS 的 LAN 测试则要求 Host 与 Find 的 LAN 标志一致。

### 17.11 两个窗口显示不同主菜单或地图

**现象：** 使用 `-game -windowed -ResX=960 -ResY=540 -log` 启动两个实例后，界面
与编辑器当前看到的不一致，或者更换地图后直接回到另一个默认关卡。

**根因：** UE 同时存在多个入口配置：

```text
Editor Startup Map：只决定编辑器打开时展示哪个地图
Game Default Map：决定游戏/打包程序默认进入哪个地图
Server Default Map：决定服务器默认地图
命令行地图参数：优先指定本次进程进入的地图
ServerTravel / ClientTravel：决定联网后的目标地图
```

**修复：** 明确将 `/Game/UI/mainmenu` 作为游戏入口，把实际玩法地图加入打包列表；
批处理要么显式传入主菜单包路径，要么统一依赖 `GameDefaultMap`。同时确认 Project
Settings 中 `Game Instance Class` 指向自定义 `multiplayerGameInstance`，否则 Widget
中的 Cast 会失败，即使地图本身可以打开。

**回归：** 编辑器启动、Standalone、两个命令行实例和打包版本都先进入同一主菜单；
Host 后进入玩法地图，Client Join 后进入与 Host 相同的地图。

### 17.12 Session 残留、重复创建与退出语义

**现象：** 第一次 Host 成功，返回菜单后第二次 Host 失败；或者一次点击触发多次 Travel。

**根因：** Named Session 仍然存在，或者异步 Delegate 重复绑定。关闭一个 Widget、切换
地图或隐藏菜单不会自动销毁 Online Session。

**正确生命周期：**

```text
Host 请求
→ 若已有 Named Session
   → DestroySession
   → OnDestroySessionComplete
   → CreateSession
→ 否则直接 CreateSession
```

C++ 为 Create / Find / Join / Destroy 分别保存 `FDelegateHandle`，在完成、立即失败和
`Shutdown()` 路径精确解绑。Widget 的 Construct 只绑定一次；如果 Widget 会重复创建，
还应在 Destruct 中解除自身绑定，避免旧界面继续收到回调。

当前菜单中的“退出游戏”调用 `QuitGame`，语义是结束进程。若以后增加“退出房间”，
正确语义应是：

```text
DestroySession
→ 等待销毁完成
→ ClientTravel / OpenLevel 返回主菜单
```

不能把“退出房间”和“退出应用程序”混为同一个按钮行为。

### 17.13 C++ 反射接口修改后蓝图节点失效

**现象：** C++ 只修改了 Delegate 参数或 `UFUNCTION` 签名，蓝图却提示签名不匹配、
事件没有输出参数，甚至旧节点仍显示但无法编译。

**根因：** 蓝图节点保存的是编译时生成的反射签名。修改 `UCLASS`、`UPROPERTY`、
`UFUNCTION`、动态 Delegate 或头文件后，需要 UHT 重新生成元数据；旧节点不会总是
自动重建。Live Coding 更适合函数体实现变更，不适合依赖它刷新所有反射结构。

**处理顺序：**

```text
保存并关闭相关蓝图
→ 对 Editor Development 执行完整编译
→ 重开编辑器
→ 删除旧 Bind / Event 节点
→ 从 Delegate 的 Event 引脚重新生成事件
→ Refresh All Nodes / Compile / Save
```

这解释了“只改一点代码却编译很慢”：反射头变化会触发 UHT、模块编译、链接和蓝图
依赖刷新，不等同于普通 C++ 单文件增量编译。

### 17.14 Travel 与网络失败必须可观测

**现象：** 点击 Host 或 Join 后界面消失、关卡没有打开，表面上像“直接退出”，但仅凭
窗口行为无法判断是成功 Travel、地图加载崩溃、Join 失败还是进程真正退出。

**改进：** 为每个异步阶段记录结构化日志：

```text
[Session] Operation / Result / SessionName
[Travel] SourceMap / TargetURL
[Join] ResultIndex / ConnectString / JoinResult
[Failure] NetworkFailureType / TravelFailureType / Error
```

同时监听引擎的 Network Failure 与 Travel Failure，UI 在失败后恢复按钮并显示可重试
状态。不要在发起异步调用后立即打印“成功”；只有对应完成回调返回成功才算该阶段完成。

**回归用例：** 正确地图、错误地图、空搜索结果、无效索引、Host 已存在、Client Join
失败、Host 中途退出、200ms 延迟和少量丢包。每条失败路径都应回到可操作状态，且不会
遗留 Session 或重复回调。

### 17.15 UI 生命周期与按钮恢复遗漏

**现象：** Host / Join 点击后按钮被禁用，但失败路径没有重新启用；返回菜单后旧 Widget
仍响应 Session 回调。

**根因：** 只处理了成功路径，或把 Widget 从 Viewport 移除误认为对象已经立即被 GC。
`RemoveFromParent` 只是从界面树移除；只要仍有强引用或 Delegate 绑定，对象就可能存活。

**修复：** 按钮状态只由 `OnSessionOperationChanged` 驱动，任何终态都把 Operation 复位
为 `None`；Widget Destruct 时解除绑定并清空缓存引用。不主动调用 `CollectGarbage` 解决
普通菜单销毁问题，因为强制 GC 会引入卡顿，也掩盖真正的引用生命周期错误。

### 17.16 本轮统一排错方法

遇到“节点没反应、地图打不开、两个窗口不同步”时，按以下顺序缩小范围：

```text
1. 类型：Target / Object / Owning Player 是否是接口真正需要的类型。
2. 生命周期：对象是否已经创建，引用是否仍有效，Delegate 是否重复或过期。
3. 异步：是否在完成回调之前读取结果，失败路径是否复位。
4. 权限：操作应在本地 UI、拥有客户端还是服务器执行。
5. 配置：GameInstance、默认地图、OSS、LAN/Presence 参数是否一致。
6. 资源：地图包路径、打包列表、重定向器和外部 Actor 是否完整。
7. 环境：Zen/DDC、代理、端口、写权限是否在项目逻辑运行前就已失败。
8. 证据：编译日志、Session 日志、Travel 日志和双实例行为能否互相印证。
```

这套顺序的核心是先确认问题属于哪一层，再修改代码。它避免用重接蓝图解决环境故障，
也避免用清缓存掩盖 Session 状态机或地图引用问题。

---

## 18. Co-op Demo V1 收口说明（2026-08-10）

本节记录 Demo V1 在关卡机关、共享目标、胜利结算和胜利 UI 方面的当前实现。状态分为：

- **已实现**：C++ 调用链存在，并通过 `multiplayer Win64 Development` 构建。
- **需蓝图配置**：C++ 已暴露属性或事件，但关卡实例、资源引用或 Widget 节点必须在编辑器中设置。
- **待双端验证**：需要 Host 和 Client 两个独立窗口共同运行后才能形成验收证据。

### 18.1 最终玩法闭环

```text
四把钥匙分别被玩家收集
→ 每把钥匙安装到唯一 DestinationSocket / KeyDisplayPoint
→ 服务器 KeySocket::RegisterActivatedKey
→ GameState.ActivatedKeys 达到 RequiredKeys（默认 4）
→ 最终机关允许被激活
→ 两名不同玩家同时进入 WinArea
→ 服务器 TryCompleteGame
→ GameState.bGameWon = true
→ 属性复制到两个客户端
→ 每个本地 VictoryPresenter 创建胜利 UMG
→ 玩家可选择重新开始或退出游戏
```

共享结果由服务器决定。客户端只能通过自己拥有的 Character 发起“重新开始”请求，不能直接修改
`ActivatedKeys`、`bGameWon` 或机关最终状态。

### 18.2 机关职责已经拆分

| Actor | 负责内容 | 不负责内容 |
|---|---|---|
| `AmultiplayerPressurePlate` | 玩家 Overlap、按压状态、下压动画、一次锁存、目标完成限制 | 不直接移动门或平台 |
| `AmultiplayerCoopGate` | 订阅压力板、组合开启规则、门移动、开放状态复制 | 不承担玩家检测盒 |
| `AmultiplayerMovingPlatform` | 平台移动、平台占用或外部压力板激活 | 不与门共享移动点 |
| `AmultiplayerCoopKey` | 拾取、安装、持有者复制、旋转表现 | 不直接保存全局钥匙数量 |
| `AmultiplayerKeySocket` | 唯一插槽、防重复激活、注册共享进度 | 不决定游戏胜利 |
| `AmultiplayerWinArea` | 统计终点内不同玩家并请求结算 | 不允许客户端直接宣布胜利 |
| `AmultiplayerCoopGameState` | 复制钥匙进度和胜利结果、广播事件 | 不保存只属于服务器的关卡规则对象 |

拆分后的关卡引用关系如下：

```text
普通门.RequiredPlates[] ──引用──> 压力板实例
移动平台.ActivationPlate ──引用──> 外部压力板实例
钥匙.DestinationSocket ──引用──> 对应钥匙架插槽实例
WinArea ──运行时获取──> CoopGameState
VictoryPresenter ──运行时监听──> CoopGameState.OnGameWon
```

### 18.3 压力板配置

关键实例属性：

| 属性 | 默认值 | 语义 |
|---|---:|---|
| `PressedOffset` | `(0, 0, -8)` | 相对初始位置的按下偏移；Z 更负表示下压更深 |
| `PressMoveSpeed` | `80` | PlateMesh 向按下/释放位置插值的速度 |
| `bLatchOnceActivated` | `false` | 开启后只激活一次，玩家离开也不复位 |
| `bRequireObjectiveComplete` | `false` | 只有四把钥匙完成后才允许激活 |
| `bRequirePlayerControlledCharacter` | `true` | 只接受玩家控制角色，不让普通 AI 触发 |

普通协作机关应保持 `bLatchOnceActivated=false`，玩家必须持续踩住。最终钥匙机关应同时设置：

```text
bRequireObjectiveComplete = true
bLatchOnceActivated = true
```

若最终门也必须永久保持打开，再把该门的 `bStayOpenOnceActivated` 设置为 `true`。

### 18.4 门和移动平台的移动点

普通门包含 `ClosedPoint` 与 `OpenPoint`。它们是门 Actor 内部的 SceneComponent，保存的是两个目标位置；
运行时真正移动的是 `DoorMesh`。编辑关卡时应：

```text
ClosedPoint = 门关闭时的位置
OpenPoint   = 门完全打开时的位置
RequiredPlates[0..N] = 控制该门的压力板实例
RequiredActivePlateCount = 需要同时激活的压力板数量
```

组件必须是 `Movable`。如果点被选中后没有坐标轴，应确认当前不是播放状态、视口变换工具处于移动模式，
并确认选中的是蓝图实例内的继承组件而不是 Content Browser 资产。

移动平台使用独立的 `StartPoint` 与 `TargetPoint`，支持两种激活源：

- `PlatformOccupancy`：直接统计站在平台检测盒中的玩家数量。
- `ExternalPressurePlate`：订阅关卡中的独立压力板，适合“一人持续踩板、另一人乘平台”。

平台与门可以复用移动组件的思想，但不共享门 Actor，也不把压力板和平台 Mesh 放进同一个 Actor。

### 18.5 钥匙与钥匙架

钥匙实例需要设置唯一的 `DestinationSocket`。玩家进入 `PickupTrigger` 后，服务器优先执行：

```text
DestinationSocket::StoreCollectedKey(Key)
→ Key::InstallAtSocket(KeyDisplayPoint)
→ Socket.bActivated = true
→ RegisterActivatedKey()
```

这样钥匙架位置一开始没有已安装钥匙；收集后，原钥匙 Actor 被附着到对应 `KeyDisplayPoint` 并开始在那里显示。
钥匙的视觉旋转由以下属性控制：

| 属性 | 默认值 | 说明 |
|---|---:|---|
| `RotationSpeedDegrees` | `90` | 每秒旋转角度；设置为 0 停止 |
| `RotationAxis` | `(0,0,1)` | 本地旋转轴；默认绕 Z 轴 |

旋转只修改 `KeyMesh` 的本地旋转，因此钥匙安装到架上以后仍然旋转，不会改变 Socket 的世界位置。
每把钥匙必须指向不同的 Socket；同一 Socket 的 `bActivated` 可以阻止重复增加全局计数。

### 18.6 胜利判定和公开接口

GameMode 在服务器 `BeginPlay` 中调用：

```cpp
CoopState->ConfigureRequiredKeys(RequiredKeys); // 默认 4
```

WinArea 只在服务器维护 `PlayersInside`。胜利条件为：

```text
PlayersInside.Num() >= RequiredPlayers（默认 2）
AND
ActivatedKeys >= RequiredKeys（默认 4）
```

GameState 对蓝图提供的读取和事件接口：

| 接口 | 类型 | 用途 |
|---|---|---|
| `GetObjectiveState()` | `BlueprintPure` | 读取钥匙数、需求数和胜利状态 |
| `IsObjectiveComplete()` | `BlueprintPure` | 判断钥匙目标是否完成 |
| `ConfigureRequiredKeys()` | `BlueprintAuthorityOnly` | 服务器配置钥匙需求 |
| `OnObjectiveProgressChanged` | `BlueprintAssignable` | UI 或机关监听钥匙进度 |
| `OnGameWon` | `BlueprintAssignable` | 本地 UI 监听胜利结果 |
| `On Game Won UI` | `BlueprintImplementableEvent` | 可选的 GameState 蓝图扩展入口 |

`RegisterActivatedKey()` 和 `TryCompleteGame()` 保持为 C++ 内部接口，没有暴露为普通客户端可调用的
`BlueprintCallable`。这是有意的权限边界，不是接口遗漏。

`bGameWon` 先检查旧值再写入，因此服务器只结算一次；VictoryPresenter 也检查现有 Widget，避免本地重复创建。

### 18.7 胜利 UI、重新开始与退出

每个本地 Character 都包含 `VictoryPresenter` 组件。它只在 `IsLocallyControlled()` 为真时绑定
`GameState.OnGameWon`，因此 Listen Server 和远端 Client 会各自创建一份本地胜利界面。

角色蓝图必须把 `VictoryPresenter.VictoryWidgetClass` 设置为 `Content/UI/winandquit`。胜利时组件会：

```text
CreateWidget
→ AddToViewport(100)
→ Show Mouse Cursor
→ Set Input Mode Game And UI
```

重新开始按钮的正确蓝图链：

```text
OnClicked
→ Get Owning Player Pawn
→ Cast To multiplayerCharacter
→ Request Restart Coop Game
→ ServerRestartCoopGame RPC
→ GameMode::RestartCoopGame
→ ServerTravel(CurrentMap + "?listen")
```

退出按钮的正确蓝图链：

```text
OnClicked
→ Get Owning Player
→ Quit Game
```

静态资源检查已经发现 `winandquit` 中存在 `OnClicked` 和 `QuitGame`，但没有发现
`RequestRestartCoopGame` 节点。重新开始按钮仍需要在编辑器中连接、编译并保存后再做双端验收。

### 18.8 机关碰撞基线

| 组件 | Collision Enabled | Object Type | Pawn 响应 | Generate Overlap Events |
|---|---|---|---|---|
| PressurePlate.PlateMesh | Query and Physics | WorldDynamic | Block | Off |
| PressurePlate.ActivationTrigger | Query Only | WorldDynamic | Overlap | On |
| CoopGate.DoorMesh | Query and Physics | WorldDynamic | Block | Off |
| MovingPlatform.PlatformMesh | Query and Physics | WorldDynamic | Block | Off |
| MovingPlatform.ActivationVolume | Query Only | WorldDynamic | Overlap | On |
| CoopKey.KeyMesh | No Collision | WorldDynamic | Ignore | Off |
| CoopKey.PickupTrigger | Query Only | WorldDynamic | Overlap | On |
| KeySocket.ActivationTrigger | Query Only | WorldDynamic | Overlap | On |
| WinArea.WinTrigger | Query Only | WorldDynamic | Overlap | On |

代码或 Timeline 驱动的门、压力板和平台均保持 `Simulate Physics=false`、`Mobility=Movable`。实体 Mesh
负责站立和阻挡；独立 Box/Sphere 负责 Overlap 事件，不能让同一个碰撞组件同时承担两种职责。

### 18.9 双人验收矩阵

| 用例 | 预期结果 |
|---|---|
| 只收集 3/4 把钥匙，两人进入 WinArea | 不胜利 |
| 收齐 4 把钥匙，只有玩家 1 进入 | 不胜利 |
| 收齐 4 把钥匙，只有玩家 2 进入 | 不胜利 |
| 收齐 4 把钥匙，两人同时进入 | 两端各显示一次胜利 UMG |
| 同一个玩家多个碰撞组件进入 | 人数不能重复增加 |
| 一名玩家离开 WinArea | 在另一名玩家进入前不能胜利 |
| 胜利后再次进入/离开 | 不重复结算，不重复创建 Widget |
| Client 点击重新开始 | 请求到达服务器，两端重载同一地图 |
| Client 点击退出 | 只退出该客户端进程 |
| Host 点击退出 | Listen Server 结束，Client 应收到断开连接 |

可使用 `TestTwoPlayers.bat` 启动两个相同尺寸窗口。最终验收仍需保存 Host 和 Client 两份日志，确认：

```text
Coop objective configured: RequiredKeys=4
WinArea Evaluate: Players=2 RequiredPlayers=2 KeysComplete=true
OnGameWon 在两个本地客户端各执行一次
```

### 18.10 当前已知缺口

1. `TSet` 可以防止同一 Character 的多个碰撞组件在 BeginOverlap 时重复加人；但当前 EndOverlap 收到任意
   一个组件离开事件后就会移除整个 Character。若角色确实有多个参与 Pawn Overlap 的组件，需要增加组件级
   引用计数，或确认 Actor 已完全离开 Trigger 后再移除。WinArea、压力板和平台都应做相同审计。
2. `winandquit` 的重新开始按钮仍需连接 `RequestRestartCoopGame`，然后 Compile/Save。
3. 需要在 `BP_ThirdPersonCharacter` 中确认 `VictoryWidgetClass=winandquit`。
4. 需要确认玩法地图 World Settings 没有使用错误的 GameMode Override；项目默认值已经指向
   `/Script/multiplayer.multiplayerGameMode`。
5. 独立游戏目标已经构建成功，但机关、钥匙、胜利、重开仍需完整双窗口运行证据。

---

## 19. Git 与二进制资产策略

Unreal 的 `.uasset` 和 `.umap` 是项目运行所需的二进制资产，不是可由 C++ 自动重新生成的中间文件。
它们包含蓝图、关卡、材质、Mesh 引用和编辑器序列化数据，因此必须在“可复现项目”与“仓库体积/授权”之间
做明确选择。

Demo V1 分支使用 Git LFS：

```gitattributes
*.uasset filter=lfs diff=lfs merge=lfs -text
*.umap filter=lfs diff=lfs merge=lfs -text
```

这样 Git 提交保存小型指针，真实二进制由 LFS 存储；232 MB 的 BuiltData 不再受到普通 GitHub 单文件
100 MB 限制。资源按源码配置、自制蓝图/UI、地图、纹理和模型分批提交，便于失败后续传和定位。

注意：Git LFS 解决技术上传限制，不自动解决第三方商城资产的再分发授权。公开仓库发布前仍需核对
Stylized Egypt 资源许可；若许可不允许公开再分发，应只提交自制资产和依赖说明。
