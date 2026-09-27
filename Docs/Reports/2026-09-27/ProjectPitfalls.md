# 第二部分：Co-op 项目踩坑报告

日期：2026-09-27。配套：[实验报告](NetworkExperimentReport.md) · [证据附录](Appendix.md)。

这份报告记录可由当前源码和测试证据支撑的问题，不补写未经记录的“当时先怀疑了什么”。尤其要区分：实际运行发现的 bug、静态审计找出的风险、已实现的防护，以及测试工具本身的问题。

## 1. 重开的是同一张地图，却不再是同一个网络模式

**证据等级：本轮打包运行真实复现，修复后回归通过。**

现象：首次完整 Flow 已通过宝物、合作门、载人和胜利检查，到了重开，客户端却回不到主机。失败记录保留在原始数据包的 `package-flow-normal`，没有改成通过。

根因：建房使用 `World::Listen` 让当前 World 原地监听，但它没有同步更新引擎记录的 `LastURL`；`RestartGame` 使用 `?Restart`，重新读取这份 URL。地图虽然没变，`listen` 选项却丢了，新世界变成单机。

处理：只在合法的 Listen Server 重开前，把 `listen` 保留到当前 World 对应的 `LastURL`，再执行重开。没有增加自动选图，也没有通过循环重连掩盖主机根本没监听的问题。

验证：最终三档 Flow 均有 `Restart` 与 `ClientRestart`，检查新 World、目标归零和客户端重新入场。源码：[GameMode](../../../Source/multiplayer/Core/multiplayerGameMode.cpp) 的 `RequestRestartCurrentRound`；原地监听在 [GameInstance](../../../Source/multiplayer/Network/multiplayerGameInstance.cpp) 的 `EnsureCurrentWorldIsListening`。

面试追问：“重开和重新建房有什么不同？”要讲清 World、NetDriver、Session 与保存的连接 URL 各自的生命周期；不能只说“补了一个参数”。

## 2. 重连测试通过了，为什么主机房间还是坏了？

**证据等级：本轮真实发现；也是一次验收标准迭代。**

现象：初版断包测试看到客户端使用缓存地址重新连接成功，报告为通过。但核查主机的超时日志和失败处理链后发现，服务器自己的 Session 记录被清理了。

根因：引擎也会向服务器报告某个远端客户端的 `ConnectionTimeout`。旧逻辑把它当作整个主机联网失败，走了全局会话清理。监听的 World 仍然可连接，因此“地址直连成功”掩盖了“房间记录消失”。

处理分两步：服务器收到远端连接丢失或超时时保留主机会话，由引擎清理对应连接和 Pawn；测试新增 `HostSessionRetained`，检查房间存在、主机身份、广告开关和本轮房间 Token。

验证：最终三档真实断包回归均通过新断言。它证明会话记录和广告配置保留，尚不等于断线后又完整执行了一遍搜索菜单。源码：[GameInstance](../../../Source/multiplayer/Network/multiplayerGameInstance.cpp) 的 `HandleNetworkFailure`；[测试场景](../../../Source/multiplayer/Testing/CoopNetTestScenarios.cpp) 的 `HostSessionRetained`。

面试可这样讲：“第一次的测试只回答能否重新连接，进一步检查发现它没有验证房间还在不在。我把连接恢复和会话保留拆成两个条件，修复错误清理，并加入新的断言防止回归。”这是有证据的排查过程，不需要编造试错故事。

## 3. 两块板一直亮着，第二个人进来，门却可能不动

**证据等级：历史静态审计发现依赖缺失；本轮有针对性回归。**

场景：A 同时覆盖两块压力板，两块板都已激活。B 再走上其中一块时，板的开关没有变化，但不同玩家数从 1 变成 2，合作门应该重新判断。若只监听“板激活变化”，就收不到这次变化；离开时也可能漏关门。

根因：门依赖的不仅是两个布尔值，还包括两块板上的不同玩家集合。只对最终开关去重，误删了另一个规则依赖需要的通知。

处理：压力板分别通知开关变化和占用变化；门订阅完整依赖，合并角色时用集合去重。同一人踩两块板仍只算一个人，板列表中的重复配置也不当成两块独立板。

验证：三档 Flow 先检查 `PlateDistinctPlayers`，再让 B 进入时检查 `GateRules`，并由客户端检查 `GateOpen` / `GateClosed` 与网格终点。源码：[PressurePlate](../../../Source/multiplayer/Mechanisms/multiplayerPressurePlate.cpp) 的 `HandleOccupancyChanged`；[CoopGate](../../../Source/multiplayer/Mechanisms/multiplayerCoopGate.cpp) 的绑定与 `EvaluateGateState`。

面试追问：“为什么不每帧检查？”这里规则只随人数、目标和开关事件改变，完整订阅依赖即可；关键是不能把“值没变”误判成“所有相关条件都没变”。

## 4. 两个人触碰同一宝物，怎样避免进度加两次？

**证据等级：已有代码保护，本轮真实 Overlap 路径验证；不声称曾复现过多线程竞争。**

风险：同一服务器 Tick 内，两个人进入同一范围，或外部回调再次进入处理函数。如果等到所有外部调用结束才标记“已处理”，第二次进入可能再次结算。

处理：宝物入口检查 Authority 和安装状态；插槽确认安装成功后，先标记激活并关闭触发碰撞，再通知 GameMode 登记。已经完成的钥匙和插槽拒绝重复操作。

验证：测试在同一服务器 Tick 依次放置两名角色，经过真实碰撞入口完成地图中 4 个预绑定宝物归位，检查每个插槽只加一次进度，三档 Flow 均通过。源码：[CoopKey](../../../Source/multiplayer/Mechanisms/multiplayerCoopKey.cpp)、[KeySocket](../../../Source/multiplayer/Mechanisms/multiplayerKeySocket.cpp)、[测试场景](../../../Source/multiplayer/Testing/CoopNetTestScenarios.cpp) 的 `OverlapKeys`。

面试追问：“是否需要加锁？”这个被测流程是游戏线程中的重复进入与业务状态问题，不是两个 CPU 线程同时写入。当前关卡是碰撞后自动归位到预绑定插槽，不是手持、背包或携带到插槽的玩法。

## 5. 同一条线程里，一次胜利也可能广播两次

**证据等级：历史静态审计修复；本轮没有专项制造该重入顺序。**

场景：两个人先站进终点，再完成最后一个宝物。GameState 广播进度，WinArea 的监听同步触发胜利，嵌套更新 GameState。如果外层广播返回后重新读取 `bGameWon`，就可能把内层已经发过的胜利再发一次。

根因：委托不一定异步，调用监听者时可能立即修改当前对象。广播前后的成员值，不一定属于同一次状态转换。

处理：在调用外部监听者前，保存本次目标快照，计算本次是否从未胜利变为胜利，并更新通知标记；之后只按照保存的本次转换决定是否广播胜利，而不是返回后重新读成员判断。

源码：[CoopGameState](../../../Source/multiplayer/Core/multiplayerCoopGameState.cpp) 的 `HandleObjectiveStateChanged`。本轮普通 Flow 是先收宝物再进入终点，不能冒充这一顺序的专项验证。下一步补反向顺序，并统计每端 `OnGameWon` 次数。

面试追问：“为什么 UI 去重还不够？”重复广播还可能影响音效、统计或其他监听者，应修正状态发布层，而不是只让界面遮住问题。

## 6. GameState 还没到，就不能只返回一次了事

**证据等级：历史时序风险已有处理；本轮普通远端胜利 UI 通过，未强制延后到达。**

风险：客户端开始 Playing 时，GameState 未必已经可用。第一次查不到就返回，如果没有后续绑定路径，会永远错过胜利通知。即使能绑定，事件也可能早于绑定发生。

处理：尚未拿到 GameState 时订阅 World 的 `GameStateSetEvent`；就绪后绑定胜利事件，并补读当前胜利状态。同一 GameState 重绑保留去重标记，换 World 后允许新局展示；EndPlay 同时清理等待就绪和胜利事件两种订阅。不用每帧轮询，也不靠固定延迟猜到达时间。

验证：三档 Flow 的 `ClientVictoryState` 确认客户端收到胜利并将真实 Widget 加入 Viewport。但尚未强制 GameState 晚到；LateJoin 只验证目标与压力板，不是胜利后加入的 UI。源码：[VictoryPresenterComponent](../../../Source/multiplayer/UI/multiplayerVictoryPresenterComponent.cpp) 的 `RefreshBinding`、`BindGameStateSetEvent` 和 `ClearBinding`。

面试追问：“监听事件和读取当前值为什么都需要？”事件告诉后续变化，补读负责绑定以前已经发生的状态。当前 Widget 创建失败后并没有自动重试，也不能宣称 UI 任意失败都能恢复。

## 7. 玩家直接被销毁，不一定会正常走出压力板

**证据等级：已有生命周期治理，本轮真正销毁 Pawn 的专项回归通过。**

风险：只用 BeginOverlap 加一、EndOverlap 减一，会同时遇到两个问题：同一个角色的多个碰撞组件被重复算人；角色销毁或断线时记录没有及时移除。

处理：占用组件按 Character 保存重叠计数，对外按不同角色数统计。首次记录时绑定角色的 `OnDestroyed`，销毁时移除整个角色记录并通知人数改变；重绑触发区时从已有重叠重建记录；结束生命周期时对称解绑。

验证：Flow 在角色仍压板时销毁实际远端 Pawn，检查只剩主机一人且合作门关闭，三档 `DestroyedPawnCleanup` 均通过。重绑后的重建虽有实现，本轮没有独立专项断言。源码：[PlayerOccupancyComponent](../../../Source/multiplayer/Mechanisms/multiplayerPlayerOccupancyComponent.cpp) 的占用记录、销毁与重建路径。

面试追问：“弱引用能解决吗？”弱引用能避免把失效对象当正常对象使用，但不会自动修复业务人数或主动通知门关上，仍需清理与发布变化。

## 8. 写了 Sweep，不代表整个移动平台都能防穿墙

**证据等级：历史审计后明确简化边界；未实现自动避障。**

历史误区：根组件是无碰撞的 SceneComponent，承载网格是子组件。给 Actor 移动开 Sweep，并不等于子网格会按预想的整个平台形状扫掠阻挡路径。

当前选择：保留 Scene 根和蓝图配置的承载面，固定轨道移动显式使用 `SetActorLocation(..., false)`；轨道是否穿过场景由关卡设计保证。到位判断读取实际 Actor 位置，不只检查计划中的新位置。没有改为碰撞根，也没有实现遇障停止、绕行或挤压求解。

这是对项目目标的取舍：这个 Demo 需要沿已设计好的轨道载人，不需要通用运动物理系统。删除不起作用的能力假设，比保留一句“防穿墙”的注释更准确。

验证：Flow 与 Ride 共 6 次站立载人检查通过，不证明墙体阻挡、挤压安全或视觉平滑。源码：[MovingPlatform](../../../Source/multiplayer/Mechanisms/multiplayerMovingPlatform.cpp) 的组件结构、[TransporterComponent](../../../Source/multiplayer/Mechanisms/multiplayerTransporterComponent.cpp) 的 `TickComponent`。

面试追问：“将来真的需要平台遇墙怎么办？”先明确停止、返回还是挤压策略，再设计碰撞根或专门的扫掠检测，并补服务器受阻与客户端表现测试；不要把尚未实现的方案说成现有功能。

## 9. 休眠能省 CPU，但不能把机关状态也“省没了”

**证据等级：本轮优化设计与性能实测；不是已记录的丢状态故障。**

问题：静止对象没发新属性，不代表复制系统没有检查它。500 个压力板保持唤醒时，复制作用域均值中位数约 1.210 ms；休眠后约 0.133 ms。但只把对象设为休眠而忽略后续状态变化，会埋下同步风险。

处理：稳定时休眠；改变门、压力板的复制属性前先 `FlushNetDormancy`，然后写状态、更新本地表现并 `ForceNetUpdate`。不把 ForceNetUpdate 当成“客户端在这一行后立即收到”的同步屏障。关闭 Tick 与网络休眠是两项独立选择。

验证：静止对照有 24 轮；Flow 由客户端确认门开关与网格终点，LateJoin 读取已激活压力板。还需补高次数激活/复位与更多到达时序。源码：[PressurePlate](../../../Source/multiplayer/Mechanisms/multiplayerPressurePlate.cpp) 的 `EvaluatePlateState`、[CoopGate](../../../Source/multiplayer/Mechanisms/multiplayerCoopGate.cpp) 的状态提交。

面试追问：“带宽也降了 89% 吗？”没有。89% 指合成 500 板场景的复制 CPU 作用域；发送量仍接近背景，不能换一个指标继续使用同一百分比。

## 10. 自动测试跑绿，不代表真正测了要测的东西

**证据等级：验证方式本身的修正；不要与玩法 bug 混写。**

| 旧判断或容易产生的误判 | 为什么不够 | 现在的处理与剩余边界 |
| --- | --- | --- |
| 直接调用登记进度就算宝物流程通过 | 绕过碰撞、宝物和插槽检查 | 改测真实 Overlap 与一次计数；不覆盖备用携带玩法 |
| 平台到终点就算客户端载人正确 | 服务器运动正确，客户端可能没站稳 | 等客户端解析平台、确认 CMC 基座，再检查实际载人；不等于画面平滑 |
| 配置了 100 Hz，就当每秒发了 100 次 | 进程帧节奏、复制调度与网络条件会约束实际更新 | 记录约 45 FPS 的运行节奏，将 100/30 Hz 明确为上限设置 |
| GameThreadTime 全零，就说 GT 没成本 | 该运行配置下计数器不可用 | 原始值保留，正文标不可用；复制 CPU 使用有效的独立计数器 |
| 只写最终 commit 就足够复现 | 实测基于 dirty 工作区，而且正式性能 A 与最终 B 不同 | 保存二进制、源码清单、配置与 CSV 指纹，明确两版数据归属 |

载人测试还遇到过测试场景自身配置问题：生成的平台默认使用外部压力板激活，却没有绑定板；正确做法是生成完成前明确使用自身占用激活模式，而不是修改正式平台规则。客户端平台引用的到达也需要明确回执，不能把服务器生成成功当成客户端已经有可用对象。

所有案例必须有本轮 Token、正确角色/模式、必要 ASSERT 和双端 DONE，缺失或超时不能算通过。完整通过条件见 [Tests/README.md](../../../Tests/README.md)，原始结果见附录。

## 11. 哪几个最适合面试讲

优先讲三个：

1. **重开丢监听模式**：从“客户端连不回去”追到引擎 URL 与 World 生命周期，而非只调重连次数。
2. **重连成功但房间被清理**：发现第一次测试标准不完整，区分连接层和 Session 层，修复代码并补断言。
3. **板亮着但人数变了**：从业务条件反推事件依赖，既保留事件驱动的简洁，也保证规则完整。

如果追问性能，再讲静止机关休眠和平台频率对照，带上指标定义、原始数据和适用条件。第 5、6 项可以讲“审计发现的风险与已有处理”，不能讲成已经完整复现验证的真实运行事故。

讲述结构保持简单：具体场景 → 哪里不符合预期 → 查到的根因 → 为什么这样改 → 用什么证据确认 → 还有什么没覆盖。工程能力体现在这条证据链，而不是把问题说得越复杂越好。
