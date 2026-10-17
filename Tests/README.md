# 网络实验与战斗回归

网络入口支持 RideMotion、Combat、CombatLifecycle、CombatReconnect 和 CombatTraffic：一个 DS 和两个独立客户端。Partner 先连接并确认角色与共享状态就绪，Client 执行受测动作；服务器和两个客户端分别记录结果。

## 运行

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Normal,Moderate,Harsh -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -Repeat 3
.\Scripts\RunMultiplayerNetworkTests.ps1 -Profiles Harsh -PlatformMotion Static -Repeat 1
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario Combat -Profiles Normal,Harsh -Repeat 1 -Map /Game/Combat/Maps/CombatTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario CombatReconnect -Profiles Normal,Harsh -Repeat 1 -Map /Game/Combat/Maps/CombatTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario CombatLifecycle -Profiles Normal,Harsh -Repeat 1 -Map /Game/Combat/Maps/CombatTest
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario CombatTraffic -Profiles Normal -Repeat 3 -Map /Game/Combat/Maps/CombatTest
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Port 7777
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Map /Game/Combat/Maps/CombatTest -Port 7777
~~~

运行前编译 multiplayerEditor。默认采用 UE 5.5.4 Editor Server；-EditorPath 可覆盖引擎路径。默认 NullRHI；-Offscreen 使用客户端 DX12 离屏渲染，-Visible 显示客户端窗口，供人工验收 UI 与表现，DS 始终无画面。打包测试须同时提供 GameExecutablePath 与独立 ServerExecutablePath。

RunLocalDedicatedServer 不带 -Play 时执行一次指定网络档位的平台实验；-Play 只启动可操作的 DS 和两个客户端，不生成自动通过结论。两种入口均支持 -PlanOnly；端口为 0 时自动选择。

## 保留的检查

- DS 没有本地玩家，两名玩家由独立远端进程连接，各端必须独立完成并正常退出。
- 原生 CMC 站立、行走、反向、自由跳跃和落回；服务器与客户端都必须观察到对应动作。
- 采集平台/角色位置、速度、基座、移动模式和校正；使用同一 Move 时间戳关联两端记录，不直接比较 World 时钟。
- 测试角色使用实际蓝图角色的关键移动参数，观测组件不改变原生预测、校正或移动容差。
- 缺少阶段指标、非法数值、配置不匹配、超时、强制结束或致命日志均不能通过。

-PlatformSyncMode 支持 OrderedVelocity 和 PlatformInertia（默认）；两者均使用当前平台修复，前者仅在实验中恢复空中制动参数作对照。旧 Baseline 需使用历史代码版本复现。
-PlatformMotion Static 只关闭平台运动，其余角色配置与输入不变，用于静止平台对照。

Normal=0/0/0；Moderate=PktLag 100、Variance 20、Loss 2%；Harsh=200、50、5%。参数施加到三个进程，不是测得的 RTT。

## 输出与边界

Saved/NetworkValidation/<run>/ 保存 report.json、三端日志和 summary.md；平台实验额外生成时间线 SVG、采样、校正与 Move 配对 CSV。报告记录源码/模块指纹、命令、参数及各端断言。

只认最终 report.status。自动动作和数值断言不等于视觉平滑、UI 排版或人工完整通关验收；同机三进程不等于跨机器公网验收。CombatTraffic 统计引擎记账的服务端出站流量，不采集复制 CPU，也不能据此推导玩家容量。

## 战斗检查

`Multiplayer.Combat.CoreRules` 自动化用例覆盖消耗、冷却、防御到期、治疗选目标、真实投射物、友军穿过、死亡禁用及机关占用清理。

行为树使用保存的 `BT_CombatEnemy` 与 `BB_CombatEnemy`，没有用临时内存树替代正式资产：

- `Multiplayer.Combat.BehaviorAssets`：检查默认资产、可编辑图、黑板类型与独立实例值，以及实际 Service、GAS 攻击任务、原生 MoveTo 和等待节点。
- `Multiplayer.Combat.BehaviorDecision`：推进真实行为树，检查目标筛选、实际投射物伤害、GAS 冷却、遮挡，以及玩家解除控制、死亡、销毁后的目标清理。
- `Multiplayer.Combat.BehaviorLifecycle`：检查解除控制停树、重新接管恢复，以及死亡后停止行为树、寻路和新攻击。死亡前已发射的投射物可以继续运行。

上述机制用例使用带 AISystem 的独立世界，不直接调用 Service、攻击任务或修改黑板来制造通过结果；不含导航的夹具不验证追击位移。

战斗机制与行为树用例集中在 `CoopCombatTests.cpp`，共用测试世界、生成监听和伤害辅助代码；真实地图验收仍单独使用运行中的游戏世界。源码归拢后重新编译，并复验了全部 7 项机制／地图用例、16 个项目蓝图，以及 Combat 的 Normal、Harsh 双客户端回归，均通过。整理后的记录位于 `Saved/Logs/ArchitectureCleanupBuild.log`、`ArchitectureCleanupTests.log`、`ArchitectureCleanupMapAI.log`、`ArchitectureCleanupBlueprintsFinal.log` 与 `Saved/ArchitectureCleanupNetwork/report.json`。

`Multiplayer.Combat.ProjectileInterpolation` 保留原用例名，当前检查初始发射快照与服务器时间驱动的客户端轨迹重建：不持续复制 FRepMovement，客户端不运行权威碰撞或伤害。`Multiplayer.Combat.ProjectileWall` 检查近墙扫掠销毁与枪口贴墙拒发。命中、伤害和销毁仍由服务器决定；服务器销毁消息抵达前，客户端弹体可能短暂超过实际命中点。这些机制检查不是画面平滑度评测。

`Combat` 三进程场景由真实客户端申请 GAS 技能，两端独立检查攻击结果、防御标签与数值、队友治疗、死亡状态和碰撞清理。四项技能均在本地调用返回前检查真实 PredictionKey 与预测冷却；防御、治疗和范围技能还检查预测能量消耗。客户端不生成权威投射物、不结算伤害或目标回血；服务器接纳后检查同帧双击只结算一次。

拒绝夹具只在服务器制造冷却、能量不足、无可治疗目标或死亡，与客户端旧状态短暂错开。客户端必须真实发出预测请求、收到引擎拒绝回调，并在服务器夹具属性重新复制之前观察到能量和预测效果回滚，不通过手改客户端属性制造通过。每个条件恢复后均验证合法重试；死亡后的重试走主动退出、清理本地票据与新会话，不复活原控制器的尸体。为隔离规则，场景冻结角色并停止靶敌 AI，因此不能用这份报告证明导航或视觉表现。

能量和最大能量只复制给所属玩家，血量和最大血量继续公开。Combat 检查拥有者仍能预测和恢复能量，非拥有者仍收到血量、防御和死亡，但不会收到他人的能量值。另用非默认最大能量验证另一条连接；非拥有者在收到公开最大血量后继续观察一秒。远端角色的能量 Getter 保留本地默认值，不代表服务器真实能量；当前 HUD 只读取本人的资源。

`Multiplayer.Combat.MapAI` 在 `/Game/Combat/Maps/CombatTest` 验证真实导航追击和攻击，属于单进程地图验收；仍需另做人工双客户端游玩验收。

此前行为树迁移阶段重新构建并保存了 CombatTest 的 Recast 导航数据，关卡布局和出生点未改。该阶段 2026-10-15 构建通过，以上三个行为树用例及 CoreRules、ProjectileInterpolation、ProjectileWall 全部通过；不附带 `RebuildNavigation` 命令的地图冷启动用例也通过，观察到真实路径追击 503.8cm，进入射程后经 GAS 将玩家血量从 100 降至 75，并在敌人死亡后停止行为树和移动。历史记录位于 `Saved/Logs/GASBehaviorTreeFinalBuild.log`、`GASBehaviorTreeFinalTests.log` 和 `GASBehaviorTreeFinalMapAI.log`。

该历史构建和导航资产下，DS 加两个客户端的 Combat Normal、Harsh 各一轮也全部通过，报告为 `Saved/GASBehaviorTreeFinalNetwork/report.json`。这组联机测试隔离靶敌 AI，只验证当时的 GAS 同步和预测回归；实际追击由前述 MapAI 独立验证。

## 战斗断线恢复

`CombatReconnect` 关闭主客户端的真实服务器连接，再经引擎 NetworkFailure 入口触发现有 GameInstance 重连；测试不直接设置重连状态或安排重试。重新连接后必须完成所属 PlayerController 的接纳握手，由服务器恢复新角色的血量、能量、剩余冷却和位置；冷却扣除离线经过时间。第二轮在死亡后断开，验证重连仍为死亡状态、碰撞保持关闭，不能借重连复活。

Partner 必须始终保持同一控制器和角色，共享目标进度不得重置。另在服务器真实接纳入口检查单活票据拒绝、无效票据拒绝、回执丢失时的备用票据，以及旧备用票据随后失效；票据只保留在内存，不写入日志或连接地址。缓存限时 120 秒，不提供服务器重启后的持久恢复。

2026-10-15 最终构建的 `Combat` 与 `CombatReconnect` 均在 Normal、Harsh 各一轮通过。Combat 覆盖三技能真实预测、9 次拒绝回调、自动回滚与条件恢复后的合法重试；Reconnect 覆盖两次真实连接关闭与自动重连、存活／死亡快照、离线冷却、伙伴连续连接及共享进度。记录分别为 `Saved/CombatNetworkCompletion/CombatFinal/report.json` 与 `Saved/CombatNetworkCompletion/CombatReconnectFinal/report.json`。均为同机 DS + 两个客户端的 NullRHI 校验，不是视觉或公网验收。

同一构建的原机关平台 `RideMotion` Normal、Harsh 回归也通过，报告为 `Saved/CombatNetworkCompletion/RideRegression/report.json`。8 项战斗机制用例、真实关卡 MapAI 与 16 个项目蓝图编译均通过；日志分别为 `Saved/Logs/CombatNetworkFinalMechanisms.log`、`CombatNetworkFinalMapAI.log`、`CombatNetworkFinalBlueprints.log`。MapAI 实际追击 525.6cm，经 GAS 将玩家血量从 100 降至 75；蓝图编译为 0 错误、0 警告、0 加载失败。

## 范围技能生命周期

`CombatLifecycle` 通过真实所属客户端请求验证范围技能预测、重复输入、一次释放、确认前取消、权威效果接管后取消及眩晕打断。为了在 Harsh 档也观察确认后取消，三端各为角色创建独立内存配置副本，仅将范围技能起手设为 3 秒；不修改磁盘资产或全局 CDO，该阶段正式默认起手为 0.8 秒，后续素材接入时序见下文。取消保留已付资源与冷却，服务器在原释放期限之后确认没有延迟伤害。预先施加控制后再允许新客户端加入，验证当前状态恢复。

20 项单世界回归报告位于 `Saved/CombatCompletion/AutomationReview`，不替代 DS 网络、渲染或人工 UI 验收。

## 四技能版本验收（2026-10-16）

| 检查 | 结果 | 本机报告 |
| --- | --- | --- |
| 规则、AI、同步重入、生命周期 | 20 项成功，0 失败 | `Saved/CombatCompletion/AutomationReview/index.json` |
| 四技能预测、服务器拒绝与合法重试 | Normal / Harsh 各 1 轮通过 | `Saved/CombatCompletion/PredictionFinal/report.json` |
| 范围释放、确认前后取消、眩晕与晚加入 | Normal / Harsh 各 1 轮通过 | `Saved/CombatCompletion/LifecycleFinalSnapshot/report.json` |
| 换新 Controller / Pawn 的真实断线恢复 | Normal / Harsh 各 1 轮通过 | `Saved/CombatCompletion/ReconnectReview/report.json` |
| 原关卡移动平台回归 | Normal / Harsh 各 1 轮通过 | `Saved/CombatCompletion/RideRegression/report.json` |
| 客户端 DX12 真实渲染下的技能回归 | Normal 1 轮通过，未做图片观感验收 | `Saved/CombatCompletion/RenderedLifecycle/report.json` |

通过要求包含全部角色的独立断言、成功 DONE 和正常退出，不能只看日志里某一条成功。拒绝夹具在网络发送前保持服务器测试属性不向旧客户端泄露，收到真实预测、引擎拒绝和本地回滚回执后才解锁；不冻结可靠 RPC，不修改客户端资源来制造回滚。

边界修复包括：本地确认后的施法取消只移除同次预测的纯施法标签效果；客户端正常收招不提前结束服务器起手；实例复用时旧调用不推动新技能；死亡清理、受控移动模式和重连接纳分别按各自生命周期处理。编辑器与 Win64 Development 游戏目标均编译通过。

新玩家、敌人的真实 ASC 类型、Quinn 骨架、唯一输出 Slot、动作轨迹、Cue/UI 绑定和 74 包依赖冷读通过。未用特效从本轮导入内容移入 `Saved/CombatAssetImport/UnusedEffectsBackup-20261016-012030`，清单包含校验值，可恢复；导入工具只允许 68 个所需特效包。音效是项目内生成的占位音效。

自动截图在引擎读回阶段卡住，相关临时入口已删除，失败记录仍保留。DX12 回归不据此宣称 UI 排版、动画观感、音效质量或公网部署已验收；实际画面仍通过普通客户端人工检查。各记录对应各自模块与源码指纹，不把历史三技能流量样本算作四技能表现版本的带宽结果。

## 相机与事件驱动 HUD（2026-10-16）

`Saved/HUDCameraFix/AutomationFinal/index.json` 中 22 项全部通过。新增 `Multiplayer.Combat.CameraObstruction` 加载实际玩家／敌人蓝图，在真实碰撞世界中比对空场、近身角色和墙体：角色实例忽略 Camera 通道，保持 400cm 视距；测试内强制旧 Block 响应可分别复现 33cm／28cm 的贴身镜头，墙体仍使弹簧臂正常收缩。生产修复只调整角色的相机探针响应，不关闭墙体检测、不改变玩法碰撞。

`Multiplayer.Combat.HUDEventLifecycle` 创建真实 `WBP_CombatSkills`，覆盖订阅后初始快照、伤害／能量／防御事件、施法取消、冷却结束停计时、换 Pawn 解绑、眩晕／接纳／死亡、提示到期、Pawn 销毁与 `NativeDestruct` 清理。血量和状态不通过常驻 Timer 轮询，0.1 秒 Timer 仅在活动冷却或读条期间存在；提示使用独立单次 Timer。该 NullRHI 用例验证真实 UMG 逻辑，不代表截图、动画观感或帧率验收。

同一修复构建的 `CombatLifecycle` Normal／Harsh 各一轮通过，报告为 `Saved/HUDCameraFix/NetworkLifecycle/report.json`。客户端使用 DX12 离屏渲染，覆盖正常施放、确认前后取消、眩晕打断及晚加入状态恢复；该自动流程仍不代替人工画面检查。

布局仍保存在可编辑的 UMG 蓝图，只通过显式 `Scripts/UpdateCombatHUDInEditor.py` 更新。工具先备份唯一目标 `WBP_CombatSkills` 及其附属文件，再重建布局并检查其他表现资产与 GameMode 未被保存。原布局备份为 `Saved/CombatAssetImport/BeforeHUDUpdate-20261016-195523-240356`；1080p 设计单位下的 DPI 调整更新记录为 `BeforeHUDUpdate-20261016-200001-152537/UpdateReport.json`，两次均无无关资产变化，全局 UI 缩放配置未改。

## 出生移动与动作接入修复（2026-10-16）

`Saved/CombatPlayabilityFix/Automation/index.json` 中 26 项全部通过。新增三项移动回归加载实际 `BP_CombatPlayer`，经真实 GameMode 出生／Possess／接纳后向 CMC 提交输入并验证位移；覆盖未接纳禁止移动、施法完成与取消、眩晕自然到期、死亡及 Restart 后死亡锁，并保留其他系统原有的 None／Custom 模式。不用强制 Walking 掩盖初始化故障。

生产修复在 `Character::Restart` 之后刷新已有移动锁的恢复快照，再重新检查未接纳／死亡／施法／眩晕，避免接纳后恢复出生前的 `MOVE_None`。没有增加每帧权限判断。

新增 `AnimationIntegration` 加载实际 Montage、Combat AnimBP 和角色蓝图，验证完整素材长度、1 倍片段速率、唯一上半身／全身 Slot、脊柱分层、GAS 实际播放倍率和范围技能配置时序；取消仍结束当前施法动画与锁。普通攻击冷却不再强行加速整段动作，攻击／治疗／范围动作只覆盖脊柱以上，死亡／受击保持全身槽；原移动状态机与原骨架不改。范围技能按现有素材保留约 0.833 秒起手和 1.833 秒后续动作，而非压缩到原来的 1.2 秒。

专项工具 `Scripts/UpdateCombatAnimationsInEditor.py` 先备份五个 Montage、两份 Combat AnimBP 和一份配置，再明确保存这些资产；清单为 `Saved/CombatAssetImport/BeforeAnimationUpdate-20261016-202319-499350/manifest.json`。两份 AnimBP 编译均为 0 错误、0 警告，原 Skeleton、Manny／Quinn 动画蓝图及地图文件 SHA 不变。这些自动检查验证接入正确性，不声称动作观感或帧率已验收。

`Saved/CombatPlayabilityFix/NetworkLifecycle/report.json` 中 Normal／Harsh 各一轮通过，使用 DX12 离屏双客户端与本机 DS。新协议在出生接纳、正常收招、确认前取消、确认后取消及眩晕自然到期后，经本地 Controller 注入 W 按下／释放，走真实 EnhancedInput → Character → CMC；两端分别记录本地位移，服务器另核对两个权威 Pawn。五个阶段均超过 150cm 门槛，Harsh 的服务器记录约 574～656cm。不强设 Walking、不直接添加移动输入、不在按键期间传送；每段结束后才重置技能定位夹具。旧的冻结玩家流程通过记录不作为本轮移动验收。

编辑器与 Win64 Development 游戏目标均编译通过。独立进程 `CombatAssetBuild -Validate -AllowCommandletRendering -dx12 -RenderOffscreen` 冷加载验证通过，记录为 `Saved/Logs/CombatPlayabilityAssetValidationDX12.log`。Niagara 的 `IsReadyToRun` 依赖渲染后端，因此不使用 NullRHI 验收其可运行性；最初 NullRHI 返回未就绪的记录保留，不改低校验标准。

同一构建另完成 `Saved/CombatPlayabilityFix/Prediction/report.json` 与 `Reconnect/report.json`：四技能确认／服务器拒绝／预测撤销及真实断线后的换 Controller／Pawn 恢复，在 Normal／Harsh 各一轮通过，所有参与进程正常退出。重连用例覆盖剩余效果、四项冷却的离线计时和死亡状态；该固定站位用例不单独声称验证重连后的连续行走，实际输入移动由上述 Lifecycle 用例验证。

`Saved/CombatPlayabilityFix/MapAI/index.json` 中实际关卡检查通过：使用地图出生器、行为树和 NavMesh，敌人在不被测试直接驱动的情况下沿完整路径前进约 508cm、进入射程并使玩家血量从 100 变为 75；真实致死效果后停止行为树、路径及移动。该检查另算一项，不混入前述 26 项独立世界检查。

## 原 HUD 恢复（2026-10-16）

仅将 `BP_CombatGameMode.HUDClass` 恢复为 `/Game/Combat/UI/BP_CombatHUD`，保留当前玩家、敌人及技能配置。原 `BP_CombatHUD`、`WBP_CombatHUD` 与合作玩法的 UI 文件均未删除或重写，仍与 Git 中原资产一致；原来界面消失是因为此前接入工具改绑到生成的 Presentation HUD。

恢复记录为 `Saved/CombatAssetImport/OriginalHUDRestore.json`，恢复前 GameMode 备份为 `BeforeHUDRestore-20261016-211019-909032`。素材导入前后的 209 个既有 Combat／Characters 资产校验中，仅本次明确修改的 GameMode 发生变化。`IntegrateCombatPresentation.py` 默认保留已有 HUD，只有显式 `-CombatReplaceHUD` 才允许换成生成版本；`-CombatRestoreOriginalHUD` 单独恢复原界面，不回退 Pawn 或出生器。

上面的生成式四技能 HUD 测试与截图是其历史检查记录，不代表当前默认界面；当前重新使用用户原 HUD。Mage 原包完整补齐到 `Content/Mage`，原包仅有两个后处理 AnimBP，不包含完整战斗角色／移动动画蓝图；本轮不自动重接动画。

## ARPG 动画、原 HUD 子控件与特效接入（2026-10-16）

在前述原 HUD 恢复之后，接入 `Advanced ARPG Combat System v3` 的表现素材，不启用源包的角色、输入、技能、伤害或 UI 管理框架。`Content/Combat/Presentation/ARPG` 新增 22 个动画资产：13 个重定向序列、2 个 BlendSpace、2 个 AnimBP 和 5 个 Montage。源动画通过 IK 重定向到现有 Quinn；薄原生 AnimInstance 读取角色实际速度与方向，玩家、敌人分别使用对应的移动混合。E 治疗采用短施法手势，不直接使用源包十秒循环。

当前配置中的普通攻击起手为 **0.159123 秒**、收招为 **0.374210 秒**；范围施法起手为 **0.321208 秒**、后续释放及收招为 **1.112125 秒**。这些是本次素材时序，不将前述 Mage 接入阶段的 0.833／1.833 秒记录改写成当前结果。技能消耗、冷却与伤害仍交给 GAS，动画不另行结算伤害。

沿用原 `BP_CombatHUD` 和 GameMode 的 HUDClass。原 `WBP_CombatHUD` 只追加 `ARPGCombatVisuals` 子控件，原控件、事件图、绑定与准星保留；新增子控件资产为 `/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals`，复用事件驱动父类显示血量、能量、四项技能、冷却与施法进度。生命与状态不使用常驻轮询，0.1 秒 Timer 只在活动冷却或读条期间存在，换 Pawn／销毁时解除旧订阅。

当前配置优先使用源包 Cascade 火球起手、弹体尾迹、命中及治疗表现，保留 Niagara 配置兼容；命中和治疗连接源包声音，其他部分音效仍为占位。Cue 仅播放表现，弹体尾迹随弹体清理，瞬时与持续效果分别管理生命周期，不把特效回收当作伤害逻辑。

接入前备份为 `Saved/ARPGConnect/BeforeConnect-20261016-225300-129330`；`Saved/ARPGConnect/Apply.json` 与独立进程冷读的 `Verify.json` 均为 `verified: true`，保护清单 `protected_changed` 为空。已迁入的 1007 个源包资产、原 Skeleton、原 Manny／Quinn AnimBP、GameMode、原 HUD 蓝图类与 CombatTest 地图未被本次工具保存；原 `WBP_CombatHUD` 的追加子控件是明确的接入目标，不再声称该 Widget 与 Git 原文件字节一致。

| 本次检查 | 结果 | 本机记录 |
| --- | --- | --- |
| 编辑器与 Win64 Development 游戏目标 | 编译通过 | 本次构建日志 |
| 接入及独立进程冷读校验 | 均通过，保护清单无变化 | `Saved/ARPGConnect/Apply.json`、`Verify.json` |
| DX12 离屏 `Multiplayer.Combat.` 完整回归 | 28 项通过、0 警告、0 失败 | `Saved/ARPGConnect/RenderingRegression/index.json` |
| NullRHI 机制回归 | 27 项通过；该轮旧 Socket 警告已修复，最终 DX12 全量回归无警告 | `Saved/ARPGConnect/Automation/index.json` |
| 本次素材版本的双客户端 `CombatLifecycle` | Normal／Harsh 各一轮通过；含施法、取消、眩晕到期后的真实输入移动 | `Saved/ARPGConnect/NetworkLifecycle/report.json` |
| 本次素材版本的四技能预测／拒绝回滚 `Combat` | Normal 一轮、固定端口 Harsh 一轮通过 | `Saved/ARPGConnect/NetworkPrediction/report.json`、`NetworkPredictionRetry/report.json` |

CommonUI 只用于加载源素材类型依赖，当前原生 UMG 界面不使用它的输入路由。保留原 GameViewportClient，通过引擎提供的 `CommonUI.Debug.CheckGameViewportClientValid=0` 关闭不适用的路由提示；这不启用 CommonUI 输入，也不替换现有输入模式。Lifecycle 记录中仍保留设置前的提示。

预测复测的首轮 Harsh 未进入业务验证：脚本指定 61975，服务器实际回退监听 61976，客户端连接超时。确认端口不一致后主动结束该轮，失败报告保留；随后改用已确认空闲的固定端口 17777，独立 Harsh 复测通过，未为此修改玩法代码或降低断言。

新增 Cascade 渲染用例的首次运行因测试夹具直接 `Destroy` 引擎池化 Cue 而失败；夹具改为走 `RouteEndPlay` 后，最终 DX12 全量回归通过。保留首次失败记录，不以删除失败项代替修复。上述加载、绑定和生命周期检查不等于动画衔接、HUD 排版、音效观感或可玩性能验收，这些仍需普通客户端实机确认。

## HUD 紧凑布局与范围落点贴花修复（2026-10-16）

`ARPGCombatVisuals` 调整为左下同一垂直容器：当前 `Scale=2` 设计下，900×540、DPI 0.5 时实际尺寸约 216×160，四个 40×40 技能图标位于生命、能量条上方。技能与属性区去掉灰底，保留条形底槽、名称、按键、冷却数字及细条；不可用时仅调暗图标，不调暗冷却文字。原 HUD 入口、事件图、绑定及准星保留，事件订阅与仅在活动冷却／读条时运行的 0.1 秒 Timer 不变。

原 `NS_Posion_Magic_Target` 是追踪毒矛表现，不适合作为地面范围圈；改用 `/Game/Combat/Presentation/Materials/M_CombatAreaMarker`，由 `Config.AreaMarkerMaterial` 指定。贴花位置取施法 Context Hit，方向贴合地面法线，半径沿用 220cm；取消、死亡、眩晕或施法结束时随 Cue Remove 清理。源 `NS_Posion_Magic_Area1` 只作释放装饰，不作为精确范围可视化。

备份位于 `Saved/HUDGroundFix-20261016/Before`；`AreaMarker.json` 为 `verified: true`、`skill_rules_unchanged: true`，确认范围、伤害、资源与冷却规则未改。编辑器和 Win64 Development 游戏目标构建通过；`Saved/HUDGroundFix-20261016/Regression/index.json` 中 DX12 离屏 29 项通过、0 警告、0 失败，包含实际 ARPG HUD 的事件/Timer 生命周期及新增 GroundMarker 位置、法线、半径、权威上下文更新与回收检查。结果不并入上节 28 项历史记录，也不把组件属性检查当作画面观感验收。

本次构建另在 `Saved/HUDGroundFix-20261016/NetworkLifecycle/report.json` 完成一轮 Harsh 的 DS＋双客户端回归，施法、取消、眩晕恢复及真实按键移动均通过；未据此声称新增贴花已完成逐像素的双端视觉对齐验收。

普通游戏窗口（900×540、正常时间倍率）另完成基础视觉检查：四技能在生命/能量上方、底部卡片灰底消失、冷却数字清晰，R 的静态圆环位于前方地面且与施法落点一致。原左上 HUD 面板和准星保留；此检查不扩展为所有动画或多分辨率的完整美术验收。预览日志为 `Saved/HUDGroundFix-20261016/Preview.log`。

## 2026-10-17：足部 IK、护盾与敌人表现

`Saved/CharacterPresentationFix-20261017/Regression/index.json` 的 DX12 离屏回归为 32 项通过、0 警告、0 失败；新增真实 Q 脚底定位、角色缩放和附着跟随，以及源骨骼敌人、兼容 Montage、真实 WidgetComponent 与 GAS 血条立即更新、重绑和退出解绑检查。`NetworkLifecycle/report.json` 为 DS 加两个独立客户端的 Harsh 施法生命周期一轮通过；该联网记录早于最终足部 IK 资产修复。

后续用户反馈“脚不动”后，对相同真实移动输入做 ControlRig 开关对照：原图左右足骨相对角色的前后摆幅只有 0.165/0.168cm，关闭模板 Foot IK 后为 77.021/71.892cm；辅助 IK 足骨本身基本静止。旧测试只要求腿部任一骨骼变化，膝盖变化即可通过，不能验证双脚真正迈动。

修复只在本项目 ARPG 动画副本中旁路不兼容的 `CR_Mannequin_BasicFootIK`，保留状态机、缓存、上半身层和 Montage，不修改源 Rig、原 Co-op 模板或敌人独立动画。临时对照测试的源码和报告保存在 `Saved/PlayerWalkFix-20261017`，未把诊断代码长期留在业务测试中；`WalkingPose` 改为分别要求左右脚摆幅大于 10cm，并在有速度、无加速度时检查足骨仍在变化。

最终 `Saved/PlayerWalkFix-20261017/Regression/index.json` 再次为 32 项通过、0 警告、0 失败。该组步行采样测得左右脚摆幅 77.01/66.28cm，无输入加速度阶段的足骨位移 20.44/17.39cm。它验证真实 CMC 与动画图的输出，不代表已完成所有斜坡、步幅匹配或多人视觉观感验收。

## 战斗流量对照

`CombatTraffic` 停止地图敌人与移动，仅在此测试模式将服务器帧率上限设为 30，确认两个客户端就绪后预热 3 秒，采样 15 秒。服务器每 0.5 秒经真实发射路径生成投射物，共 30 枚；每 0.25 秒通过 ASC 切换能量，共 60 次。该固定夹具不经过技能输入和冷却，不能替代 Combat 的真实 GAS 请求验证。

报告的 `combatTrafficMetrics` 保存同一 NetDriver 的 `OutTotalBytes/OutTotalPackets` 起止值、差值和实际采样时长，也记录连接数、发射数、属性变化数、配置频率及实际服务器帧率。样本期间更换 Driver、断开连接、负载不足、调度偏差超限或实际帧率越界会失败；两个客户端还须分别看到投射物和采样后的公开血量。比较时还应确认前后实际帧率接近，不能只检查相同的帧率配置。

当前弹体只复制初始位置、速度和服务器发射时间，客户端按服务器时间重建直线匀速轨迹。报告额外标记 `trajectoryMode=initialLaunchState` 和 `replicateMovement=false`；30Hz 字段是 Actor 相关性检查的调度上限，不是持续位置流。两端观察到真实发射快照后只在本地记录，额外契约回执安排在采样结束后，保持 15 秒采样窗口内的测试 RPC 负载与旧版一致。解析器仍能读取没有新标记的历史基线，但不会将其声称为新同步方案。

计数包含两个连接的整个场景流量、包开销、ACK 和测试通道；弱网下也可能包含模拟器记账及重传，不是网卡抓包或单独的 GAS/投射物流量。前后使用同一夹具和网络档位，多次比较字节率的中位数及范围。投射物频率是复制调度上限，不保证实际达到该频率；降低位置更新也不减少服务器弹道扫掠计算。

### 历史对照：资源所属连接与投射物 60Hz→30Hz（2026-10-15）

UE 5.5 Editor Server、同机两个独立客户端、NullRHI、Normal 网络，使用 `projectile-energy-v2` 固定负载；每组各取两个通过有效性检查的样本。

| 指标（中位数） | 优化前 | 优化后 |
| --- | ---: | ---: |
| DS 出站字节率（B/s） | 7,894.53 | 6,608.33 |
| DS 出站包率（包/秒） | 59.76 | 59.70 |
| 实测服务器帧率（帧/秒） | 29.88 | 29.85 |

该固定混合负载下，出站字节率降低约 **16.3%**，包率基本不变。两项改动同时启用，不能分别归因各自省了多少；小样本也不能推导人数上限或正式部署收益。另有两个基线尝试未通过采样有效性检查，未计入结果；早期帧率不匹配的 v1 样本也全部排除。

这组数据比较的是先前的公开资源／60Hz 位置复制与所属资源／30Hz 位置复制，不能作为本轮“初始发射状态重建轨迹”的收益。新方案必须与旧 30Hz 构建使用相同 v2 固定负载另测。

[计数起止值、样本、指纹与排除记录](Evidence/GASNetworkOptimization.json) 保留具体口径和本机原始报告位置。正常与 Harsh 网络下的战斗回归通过，覆盖能量所属连接、防御预测确认与拒绝回滚；原有战斗规则、插值机制及贴墙碰撞检查通过。上述检查仍不等于画面平滑度验收。

### 本轮对照：30Hz 位置流→初始发射状态（2026-10-15）

使用与上述历史 v2 相同的固定负载；本轮基线已启用能量 OwnerOnly，不重复归因。前后各两个有效样本，均通过实际帧率和调度检查：

| 指标（样本中位数） | 旧 30Hz 位置复制 | 初始发射状态 |
| --- | ---: | ---: |
| DS 总出站字节率（B/s） | 6,603.85 | 3,083.97 |
| DS 总出站包率（包/秒） | 60.06 | 59.88 |
| 实测服务器帧率（帧/秒） | 30.03 | 29.94 |

该微基准中字节率降低约 **53.3%**，包率基本不变。基线另一次尝试实际服务器帧率为 21.74，未通过检查，排除不计。固定窗口未覆盖最后一批弹体的完整寿命，不能换算“每发总成本”；直接发弹与能量更新也不代表完整技能请求或 AI 战斗负载。初始轨迹仍有生成、相关性、销毁及服务器碰撞成本，客户端使用估算时钟，存在延迟及视觉过冲边界。

[原始计数、样本、模块指纹及排除记录](Evidence/GASProjectileTrajectory.json)；本机原始报告位于 `Saved/CombatNetworkCompletion/TrafficBefore`、`TrafficBeforeExtra` 和 `TrafficAfter`。

## 历史实验

既有玩法、连接、UMG、DS 冒烟和规模压测报告仍保存在 Docs/Reports、Tests/Evidence 和本机 Saved/NetworkValidation；对应测试源码已从当前工程移除。复现须使用报告对应的历史代码版本，不能拿旧报告宣称当前构建已通过这些回归。
