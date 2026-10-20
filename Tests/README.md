# 网络实验与战斗回归

网络入口支持 RideMotion、Combat、CombatLifecycle、CombatReconnect、CombatExpansion、CombatTraffic 和 CoopEscape：一个 DS 和两个独立客户端。Partner 先连接并确认角色与共享状态就绪，Client 执行受测动作；服务器和两个客户端分别记录结果。

当前恢复原埃及地图后的规则与验收入口见下文“原埃及图恢复后的正式技能验收（2026-10-20）”。旧 `CoopEscape` 全流程依赖原型的 `Escape_*` 机关布局，不适用于恢复后的原图；本文此前各日期的通过记录只对应当时的布局、规则和构建。

## GAS 同步回调与属性边界复查

本轮不扩展玩法或再拆生产文件，集中修复真实通知窗口：

- 输入确认先处理请求、后通知预览关闭；通知中的取消和下一技能预览不能被旧确认覆盖。失败提示直接使用这次检查的原因，原生拒绝没有项目标签时显示通用条件说明。
- 同实例准入同时检查 Spec 活动计数与能力实例活动状态，封住 `PreActivate` 拥有标签通知早于计数递增的窗口。具体能力开始执行后的旧栈仍由激活版本复核。
- ASC 统一适配 UE5.5 `PlayMontage` 的 Started 同步通知：已取消请求不认领动画，旧播放不覆盖较新的成功播放，旧预测拒绝不能误停下一轮同资产动画。删除小兵能力内的局部补停，普攻转向通知返回后也先复核本轮。
- 属性集统一处理生命、能量和护盾上限降低；GE 被移除后 current/base 均不能保留超额。成长／被动替换使用原生聚合通知批次，避免中间上限误扣生命，最终上限降低仍正常钳制。

新增 `ReentryAudit` 七项、`Settlement` 三项和 `Targeting` 两项真实回归；已有 `Admission.PreviewUsesNativeActivationInhibition` 增补无项目失败标签时的提示断言。动画回归使用保存的玩家、真实动画实例和生产能力；预测键隔离用例经过非权威 ASC 的原生拒绝接收入口，是窄回调测试，不冒充完整联网激活。成长替换使用真实经验奖励与被动装配。

首次扩展红测 `RedExpanded/index.json` 为 156 通过、7 失败，分别复现动画取消／替换、转向后继续执行、PreActivate 重复激活及资源上限缺口。统一钳制后的 `FirstFixRegression/index.json` 为 163 通过、1 失败，进一步复现成长替换的中间上限问题。`BuildFinal.log` 保留测试调用未导出预测委托函数的链接错误，改用反射校验参数的原生拒绝入口，不开放生产测试接口。

本轮报告根目录为 `C:/UEBuild/CoopGASReentryReview_20261020`。`BuildVerified.log` 编译成功；`RegressionFinal/index.json` 为 166 通过、0 测试警告、0 失败，DX12 离屏运行。脚本 `-SelfTest` 契约检查通过，该检查本身不启动 UE。

本机 Editor DS 加两个独立客户端完成以下 8 轮，均保留原有状态与数值断言：

| 报告目录 | Normal | Harsh | 范围 |
| --- | --- | --- | --- |
| `CombatNormalHarsh` | 通过 | 通过 | 恢复后的 CoopEscape 地图、DX12 渲染客户端与受控技能输入夹具 |
| `LifecycleNormalHarsh` | 通过 | 通过 | 取消、眩晕、死亡与重新激活，NullRHI |
| `ExpansionNormalHarsh` | 通过 | 通过 | 冲刺预测／取消／拒绝、弹反和救援，NullRHI |
| `ReconnectNormalHarsh` | 通过 | 通过 | 真实断线后的存活、倒地超时与死亡恢复，NullRHI |

Harsh 参数为三端 PktLag 200、PktLagVariance 50、PktLoss 5，不是实测 RTT。四组磁盘模块 SHA256 均为 `e613f717e5269720e074b371b88c74561cd00599ea4c94891a222c6ed7585e28`，报告保留 `loadedByRuntimeVerified=false`。296 项清单对比中只有本文因记录测试过程而变化，运行代码、配置、资产与脚本未变；不将不同文档版本的全清单指纹说成相同构建指纹。双端范围预览截图已抽查，仅确认相应界面与范围提示可见。

动画窗口的覆盖限于本节列举的 Started 取消／替换、失败嵌套播放及预测拒绝。旧动画 BlendingOut 通知内启动新技能尚未回归；本轮检查的 C++ 业务没有从该通知启动技能，不宣称任意动画回调都已解决。测试不替代首次冷加载特效、完整地图通关、连续手动操作录像、公网或发布包验收。

## GAS 架构收口

2026-10-20 本轮整理保留原玩法数值、蓝图父类、配置资产和网络边界，收敛规则归属，不增加新的战斗框架。完整阅读顺序和连续追问见 [GAS 架构与调用链](../Docs/Study/GAS架构与调用链.md)。

- `CheckSkillAvailable` 查询实际已授予能力的 `CanActivateAbility`，不再自行实现成本、冷却、标签关系和治疗目标判断。角色业务资格留在 `CheckSkillBusinessRequirements`，治疗目标资格留在治疗能力。
- 成本、冷却和提示统一读取 `GetSkillRule`。`CommitExecute` 以一份局部规则提交本次成本和冷却；激活与提交两个时点的必要校验继续保留。
- ASC 集成实现集中到 `multiplayerCombatAbilitySystem.cpp`，原反射类型与头文件不变。普通攻击发弹归 `Projectile::FireFromSkill`，范围结算归 `AreaAbility::ResolveTargets`，角色不再代管这两项专属执行。
- 范围查询先收集去重弱引用候选，再在每个目标应用前完整检查一次阵营、存活、圆柱范围与遮挡。同轮基础规则与附加效果配置取局部快照，攻击属性仍在各伤害 Spec 创建时捕获，目标合法性实时复核。旧几何测试只迁移生产入口，不降低断言。
- 保留激活版本、预测键、效果句柄、异步任务与重入收尾；没有把取消改成退款或权威伤害回滚。

新增五项 `Multiplayer.Combat.Admission` 测试覆盖派生能力条件、原生输入限制、只读查询、提交二次检查与忽略开关、扣费回调改变等级后的成本和冷却快照。另新增 `Targeting.AreaCandidateBoundary` 和 `Targeting.AreaRuleSnapshot`，验证真实碰撞过滤与同轮结算数值一致。

本轮报告位于 `C:/UEBuild/CoopGASArchitecture_20261020`：

| 检查 | 实际结果 |
| --- | --- |
| Editor 编译 | 最终 `BuildFinalVerified.log` 成功；首轮测试作用域与旧发弹调用迁移错误已修复，原错误日志保留 |
| 完整战斗回归 | 首轮 `Regression/index.json` 153 项通过；补充提交快照测试后的 `RegressionFinal/index.json` 154 项通过，均为 0 测试警告、0 失败；DX12 离屏运行 |
| 测试脚本契约 | `RunMultiplayerNetworkTests.ps1 -SelfTest` 通过；此项本身不启动 UE、不代表网络测试通过 |
| Combat | `CombatNormalHarsh/report.json`：Normal、Harsh 各 1 轮通过，恢复后的 `CoopEscape` 地图、渲染客户端与受控输入夹具 |
| CombatLifecycle | `LifecycleNormalHarsh/report.json`：Normal、Harsh 各 1 轮通过，包含取消、眩晕、死亡与重新激活 |
| CombatExpansion | `ExpansionNormalHarsh/report.json`：Normal、Harsh 各 1 轮通过，包含预测冲刺、弹反与救援 |
| CombatReconnect | `ReconnectNormalHarsh/report.json`：Normal、Harsh 各 1 轮通过，包含真实断线后的存活、倒地超时与死亡恢复 |

网络验证为本机 Editor DS 加两个独立客户端，共 8 轮；后三组使用 NullRHI。Harsh 的配置参数为 PktLag 200、PktLagVariance 50、PktLoss 5，作用于三端，不作为实测 RTT。四组报告的磁盘模块 SHA256 均为 `e48f7594516dc17ff760a0659ae71340e87b92bcd7bef0b21071d04abcf39d6f`，受测源清单指纹均为 `e63643303039f5dab7f9d9acf01eb3df97200de3bd056491902889f5602d7740`；294 个清单文件在联网验证期间无变化。报告保留 `loadedByRuntimeVerified=false`，不把磁盘指纹称为额外的运行时加载证明。验收后补写本节记录，并在提交检查中移除 ASC 实现文件末尾的一个空行，未改变运行逻辑。

渲染夹具保留双端技能截图，本轮抽查了范围预览和界面，不代替连续操作录像、首次冷加载特效、全部动画质量或原埃及地图完整通关验收；也未覆盖公网、Linux DS 或 Shipping 发布包。本轮没有新增帧率或带宽优化数据。

## 集中能力冲突关系（2026-10-20）

本轮参考 [Lyra 5.5 的 Ability Tag Relationship Mapping](https://dev.epicgames.com/documentation/en-us/unreal-engine/abilities-in-lyra-in-unreal-engine?application_version=5.5)，把静态关系嵌入现有 `UmultiplayerCombatConfig::AbilityRelationships`，不引入完整 Lyra、Experience 或一套重复的状态机。玩家五技能和救援均使用实际配置；现有配置资产未存储新字段时继承原生构造默认，不改变数值和素材引用。

每条关系由能力分类匹配，父分类和多个匹配行追加生效：

- `ActivationRequiredTags`：拥有者必须全部具备的状态。
- `ActivationBlockedTags`：拥有者具备任一状态就不能开始。
- `AbilityTagsToBlock`：活动期间阻止哪些能力分类，由 GAS 原生引用计数成对增减。
- `AbilityTagsToCancel`：开始时取消哪些在途能力；不等于清除它们留下的独立持续 GE，也不自动退还已提交资源。

| 默认关系 | 行为 |
| --- | --- |
| `Ability.Combat` 父分类 | 死亡、倒地、眩晕、施法、救援中阻断新的战斗请求 |
| Q | 已防御时不能叠加；不取消在途普攻 |
| E / R | 替换在途普攻，旧任务取消后不能迟到发弹；Q 的持续 GE 不被移除 |
| Dash | 禁锢、禁行、已有冲刺时不能起手；不新增“所有技能互斥” |
| Rescue | 额外禁止冲刺/禁行/禁锢时起手；开始时替换普攻，引导期间阻断 `Ability.Combat`；取消/完成后按原生计数解除自己的阻断 |

关系查询经 `CanActivateAbility → Super → DoesAbilitySatisfyTagRequirements` 接入，按标签筛选、输入预检与实际准入共用规则，不在这两个函数分别执行同一查询。ASC 只扩展传给 `Super::ApplyAbilityBlockAndCancelTags` 的集合，不覆盖原有能力/蓝图条件、不维护第二份活动能力或 bool 锁。任务、蒙太奇、预测键、GE 句柄、CMC 根运动仍在各能力 `EndAbility` 收尾。

本轮同时删除公开激活分发层和治疗派生 `CanActivate` 的相邻重复查询；未授予 Spec 的入口仍明确提示失败。客户端预览、服务器准入和提交后实时复核属于不同边界，保留其必要校验。救援释放占用时支持同步再次取消，清理完成前不重新激活同一实例。

边界说明：

- 角色存活/连接接纳和治疗目标、视线、范围、地面/胶囊扫掠不是单靠 Tag 能表达的条件，原有实时业务检查继续保留。
- 新增控制状态是否立即中断在途技能，仍须在统一状态事件中明确执行；仅把标签放进激活阻断列表不会自动取消已启动技能。现有 Stun→取消活动能力、Root/禁行→取消 Dash 保留，不擅自把 Root 提升为禁止所有原地施法。
- 活动能力期间不替换或编辑关系表，否则开始和结束计算的阻断集合可能不同；角色配置在运行期只读。
- 没有把输入改成 InputTag，也没有实现全局世界/攻防双向 RuleResolver。裸 `UGameplayAbility` 若不继承项目战斗基类或救援实现，仍须显式接入本查询，不能声称任意能力都已接管。
- 保留现有救援中途 Root 的策略，不新增 PreActivate 尚未完成阶段的取消意图状态机；这些不能用当前通过项代替未实现能力。

新增自动化组为 `Multiplayer.Combat.Relationships`，用真实角色蓝图、实际 GAS 能力/任务和克隆配置验证父子规则、正式配置默认值、阻断引用计数、E/R/救援替换普攻、Guard/Root 共存、缺 Spec 提示和救援同步取消。另删除 Dash 派生类提前拒绝 `MOVE_None` 的重复检查，保留统一的 `Failure_MovementBlocked` 原因，并用实际授予的 Dash 验证移动锁解除前后的准入。

本轮报告根目录为 `C:/UEBuild/CoopLyraConflict_20261020`：

| 检查 | 本轮结果与记录 |
| --- | --- |
| Editor 编译 | 首轮局部变量与 `Dash` 配置成员同名，改为 `DashRelationship` 后重编译通过；`Build.log`、`BuildRetry.log` 均保留 |
| 新增冲突用例 | `Relationships/index.json`：7 成功、0 警告、0 失败，使用 NullRHI |
| 完整战斗回归 | `Regression/index.json`：147 成功、0 警告、0 失败，含上述 7 项；DX12 离屏运行 |
| Combat | `CombatNormalHarsh/report.json`：Normal／Harsh 各 1 轮通过；恢复后的 `CoopEscape` 地图、渲染客户端与受控输入夹具 |
| CombatLifecycle | `LifecycleNormalHarsh/report.json`：Normal／Harsh 各 1 轮通过 |
| CombatExpansion | `ExpansionNormalHarsh/report.json`：Normal／Harsh 各 1 轮通过，含冲刺取消／拒绝、弹反和救援 |
| CombatReconnect | `ReconnectNormalHarsh/report.json`：Normal／Harsh 各 1 轮通过，含存活／死亡后的真实断线恢复 |

四组网络报告共 8 轮，磁盘项目模块 SHA256 均为 `d02fe1c3c36b0887a4a7a46aac77b7cb626e7f3d3610565f7b60e55dad505181`，受测源清单指纹均为 `caeabae8853bf73dae84444d703ad043c8739d3d843cb480564f85898fd0b5d3`。报告保留 `loadedByRuntimeVerified=false`，不把磁盘指纹说成额外的运行时加载证明。验收后只更新本文和启动脚本的说明注释，不修改受测运行代码或资产。

这些结果属于本机 Editor DS＋两个独立客户端的状态／数值回归；后三组使用 NullRHI。渲染夹具的通过也不等于全技能首帧粒子、完整原图通关、连续录像、公网或 Shipping 验收。原图恢复后的旧 `Escape_*` 全流程入口限制及首次冷加载特效未覆盖项仍保留在下文。

## 运行

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest
# 旧 Escape_* 原型布局专用；恢复原埃及图后不使用此全流程入口。
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario CoopEscape -Profiles Normal -Repeat 1 -Map /Game/Combat/Maps/CoopEscape -Offscreen -TimeoutSeconds 600
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

RunLocalDedicatedServer 不带 -Play 时调用旧 `Escape_*` 原型布局的自动撤离验证，不适用于恢复后的原埃及地图。当前原图请使用 -Play：只启动可操作的 DS 和两个客户端；-FromMenu 显示原地址连接菜单，不生成自动通过结论。技能回归使用上述 Combat 系列入口，平台实验使用 RunMultiplayerNetworkTests 的 RideMotion 入口。两种脚本均支持 -PlanOnly；端口为 0 时自动选择。

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

## 2026-10-17：定向施法、近身命中与预测拒绝

普通攻击与 R 使用原生 GAS 事件激活，在同一条激活请求、同一个 PredictionKey 内提交按键时的方向。服务器只读取方向，不读取客户端的发射起点、伤害或命中对象；缺少事件数据的旧远程激活入口会拒绝这两种技能。AI 的服务器直接激活仍使用本地朝向快照。校验发生在激活与扣费之前，重复或非法新请求不能覆盖正在执行的快照。

弹体改从真实起点发射，交给已有 ProjectileMovement 扫掠，不再跳过前 75cm。起点已在实体墙内时拒发，近身敌人与友军不作为实体阻挡。非扫掠初始重叠没有真实表面接触点，使用服务器弹体位置构造命中上下文；真实扫掠结果不变。R 的落点、地面投影与伤害视线改为尊重实体 Visibility 响应，非实体触发盒不会截断目标，真正阻挡的动态门仍然生效。

`Saved/CombatHitAccuracy-20261017/FinalRegression/index.json` 的 DX12 离屏全量回归为 **36 项通过、0 警告、0 失败**。新增用例为：

- `AimEventBoundary`：真实事件激活、延迟出手、转头后的方向保持、R 的实际伤害，以及缺失、多份、错误类型、非归一化和非有限数据拒绝；重复请求不重复扣费。
- `ProjectileSweepBoundaries`：8000cm/s、150ms 单次世界 Tick 下的静止目标扫掠，墙前后顺序、友军穿过、多组件重叠及单次结算。
- `ProjectileCloseRange`：0／30／70cm 敌人的实际伤害与 GE 命中上下文；在非原点位置检查目标、命中坐标与一次 GE，避免只验证扣血却漏掉特效错位。
- `TriggerOcclusionBoundary`：真实触发盒不改变 R 落点或挡住伤害、眩晕；动态实体门仍截断目标。

联网 `Combat` 增加缺少瞄准事件的真实客户端预测请求：服务器拒绝后检查原键效果撤销、动作停止、跨过原出手期限后无晚到弹体，再通过正常入口合法重试。`CombatLifecycle` 增加按下攻击后立即转头，以及真实 CMC 横移靶的单次命中；服务器保持故意错误的旧控制朝向，分别核对发射方向、原始预测键和双客户端扣血结果。横移靶只在实际出弹后设置一次初速度，以 CMC 的强制满加速模式持续移动，不在运动中传送或调用伤害入口。

最终编辑器与 Win64 Development 游戏目标编译通过，脚本契约 SelfTest 通过。独立 DS 加两个 DX12 离屏客户端的完整报告如下，四轮均满足全部必需断言，12 个参与进程均正常退出：

| 场景 | 网络档位 | 结果与本机报告 |
| --- | --- | --- |
| 四技能确认、拒绝撤销与合法重试 | Normal、Harsh 各一轮 | 均通过；`Saved/CombatHitAccuracy-20261017/Prediction/report.json` |
| 施法生命周期、转头快照与 CMC 横移靶 | Normal 一轮 | 通过；`Saved/CombatHitAccuracy-20261017/FinalLifecycleNormal/report.json` |
| 同上 | Harsh 一轮 | 通过；`Saved/CombatHitAccuracy-20261017/FinalLifecycleRetry/report.json` |

Harsh 使用 `PktLag=200`、`PktLagVariance=50`、`PktLoss=5`，参数施加于三个进程，不等于实测 RTT。最终两轮命中前的靶敌实际横移为 Normal 120.82cm、Harsh 103.78cm，均超过胶囊直径，双方各收到一次扣血。`FinalLifecycle/report.json` 所在尝试被中断，报告未完成，不计为最终通过；本表采用随后分别重跑的完整报告。重跑前确认源文件、资产清单与游戏模块校验值均未变化。

这些检查针对服务器实时碰撞与 GAS 预测状态撤销，不是历史目标位置回溯、客户端弹体预测或完整世界回滚；高速静止靶扫掠不保证任意两物体在帧间交叉均不漏判。方向快照也不等于第三人称相机准星的视差修正。

## 2026-10-17：完整 ARPG 敌人素材

`BP_CombatEnemy` 改用 Advanced ARPG 包的完整 `SKM_CombatMannequin`，保留源 `SK_CombatMannequin`、`PA_Mannequin` 与 `ABP_Manny_PostProcess`。移动和五类 Montage 使用 `ARPG/EnemySource` 下的本项目动作副本，移除源包业务通知，位移仍由 CMC 负责；行为树、GAS、玩家、HUD 和源素材未替换。

编辑器构建、独立进程 `CombatAssetBuild -Validate` 冷读通过。`Saved/EnemyARPGFull-20261017/Regression/index.json` 的 DX12 离屏回归为 **37 项通过、0 警告、0 失败**：`EnemyHealthPresentation` 检查实际完整网格、骨架、物理资产、后处理和实时血条；`EnemyLocomotion` 使用真实角色输入和世界 Tick，移动 **386.29cm**，组件空间左右足前后摆幅分别为 **78.40／80.62cm**，并验证 GAS 攻击、取消、受击和死亡时对应 Montage 实际播放与推进。始终更新骨骼仅用于离屏测试夹具，不修改业务角色的更新策略。

`Saved/EnemyARPGFull-20261017/MapAI/index.json` 的真实关卡回归 **1 项通过**，使用已保存 NavMesh 和实际出生器蓝图：敌人追击 **516.8cm** 后攻击，玩家生命 **100→75**。这些自动检查验证动画和玩法链路，不代表完整的人工画面验收。

接入前备份 20 个目标资产至 `Saved/EnemyARPGFull-20261017/Backup`；`AssetsBefore.json` 和 `AssetChanges.json` 核对全部 2,837 个 Content 文件，实际写入仅限敌人蓝图与 19 个 EnemySource 资产，其他内容校验值不变。

`Saved/EnemyARPGFull-20261017/NetworkLifecycle/report.json` 记录本轮完整模型版本的独立 DS 与两个 DX12 离屏客户端：`CombatLifecycle` 的 Normal、Harsh 各一轮均通过全部必需断言，六个参与进程全部正常退出，未由脚本强制结束。覆盖施法释放与单次伤害、取消／眩晕／死亡打断、后续移动恢复、按键方向快照及 CMC 横移目标命中。Harsh 的进程参数为 `PktLag=200`、`PktLagVariance=50`、`PktLoss=5`，不等于实测 RTT。`FinalSourceVerification.json` 确认网络运行期间业务源码与资产未变，清单差异仅为更新测试说明的 `Tests/README.md`。

## 2026-10-17：错峰生成与 RVO 避让

出生器第一只立即生成，后续默认每 0.35 秒生成一只；Timer 每帧最多执行一次生成回调，卡顿时不会同帧补刷整波。重复请求不启动第二波，达到数量上限后停止，出生器销毁时清理未完成任务。现有敌人蓝图在服务器 BeginPlay 通过 CMC 注册 RVO，死亡后禁用，旧记录按引擎 TTL 回收；客户端继续接收原有 Character 移动同步。

编辑器构建通过，`Saved/StaggerRVO-20261017/Regression/index.json` 的 DX12 离屏回归为 **40 项通过、0 警告、0 失败**。新增 `SpawnStaggering` 验证出生时刻 **0.00／0.40／0.75 秒**（测试步长 0.05 秒）、重复请求、销毁取消和数量上限；`EnemyRVOLifecycle` 检查实际蓝图的注册与 GAS 死亡后的退出；`RVOCrossing` 使用两只真实敌人、地板与相同 CMC 输入，对比开启和关闭 RVO：胶囊尚未接触时横向速度峰值为 **139.47／0.59cm/s**，开启时实际绕行 **82.57cm**，最小中心间距 **122.35cm**，两只敌人均通过对方。

`Saved/StaggerRVO-20261017/MapAI/index.json` 的真实关卡回归 **1 项通过**：正常等待错峰生成完成，敌人沿保存的 NavMesh 追击 **546.7cm** 后通过 GAS 攻击，玩家生命 **100→75**。RVO 对照是受控的相向行走用例，不代表包围站位、复杂拥堵或导航边缘的全面验收；错峰出生也不等于持续控制所有敌人的攻击顺序。

`Saved/StaggerRVO-20261017/NetworkLifecycle/report.json` 的独立 DS 加两个 DX12 离屏客户端，在 Normal、Harsh 各一轮 **全部通过**，六个进程正常退出，未由脚本强制结束。该回归检查技能释放、取消、眩晕／死亡打断、移动恢复、按键方向快照与横移靶命中，没有将它计为复杂群体拥堵测试。`FinalSourceVerification.json` 确认网络测试期间业务源码和资产未变，清单差异仅为测试说明文档。

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

## 独立镜头与死亡输入（2026-10-17）

`Saved/PlayerInputFix-20261017-174449/RegressionFinal/index.json` 为 **42 项通过、0 警告、0 失败**。新增 `CameraFacing` 使用真实本地玩家、保存的 Enhanced Input 映射及 PC 输入入口，验证静止观察不转身体、不改角色位置，W 按镜头方向移动并转身，已接纳攻击转向本次瞄准方向，冷却拒绝不额外转身。

`DeathInput` 先以真实伤害触发死亡，再提交鼠标、移动、跳跃和四技能输入：镜头、角色朝向与位置不再受输入影响，Pawn 输入哨兵和 GAS 激活均不再被触发，而 Controller 输入哨兵仍工作。覆盖死亡先于控制器接管及客户端 Restart，并验证同一 Controller 接管新存活 Pawn 后输入可恢复；不手动 EnableInput 或强制 Walking，也不代表新增个人复活玩法。

`NetworkCombat/report.json` 中 Normal 与 Harsh 各一轮通过，耗时分别为 **116.870／158.529 秒**，6 个参与进程均正常退出。两端观察到死亡状态后，Pawn 输入关闭而 Controller 输入保持开启；既有预测确认、拒绝撤销、死亡清理及新会话用例同时通过。这些是状态和输入链路检查，不等同于全部动画观感或网络纠正下的转向画面验收。

## 菜单进入战斗后的视口输入（2026-10-17）

菜单的 `UIOnly` 会在共享游戏视口上保留输入阻断；原合作 HUD 会恢复 `GameOnly`，实际战斗 HUD 原先没有，因此可能握手成功但 WASD 和鼠标都失效。只向 `PlayerController::InputKey` 提交按键会绕过这个阻断，旧输入测试不能证明视口交接正确。

新增 `MenuViewportInput` 使用真实离屏 `GameViewportClient / SViewport / FSceneViewport`：先设置与菜单相同的 `UIOnlyEx`，确认 W 被视口阻断；再由真实 Controller 创建实际 `/Game/Combat/UI/BP_CombatHUD`，执行原 BeginPlay，并从视口入口提交 W。`Saved/MenuInputFix-20261017/Before/index.json` 在未修资产时复现三项失败（视口仍阻断、光标未隐藏、不能移动）；窄修 HUD 后，`Regression/index.json` 为 **43 项通过、0 警告、0 失败**，该用例实际移动 **121.60cm**。

HUD 只追加三个蓝图节点并接回原执行链，不重建控件或原事件图。`CombatAssetBuild -FixCombatInput` 的重复运行返回 `alreadyApplied=1 saved=0`；原 WBP、菜单 Controller 与 ARPG 子控件哈希不变。网络 `CombatLifecycle` 的移动输入也改为视口入口，并检查视口输入已开放，不再绕过视口直接向 PC 提交移动按键。

同一构建的 `NetworkLifecycle/report.json` 中 Normal／Harsh 各一轮通过，耗时 **86.863／127.321 秒**，6 个参与进程均正常退出；两名客户端在加入、施法结束、取消与眩晕恢复后均经视口输入产生真实 CMC 位移，服务器也确认了两人的位移。既有方向快照和命中检查同时通过。这里不把离屏自动化称为真人按键或完整画面验收。

## 普通近战小兵（2026-10-17）

实际 `BP_CombatEnemy` 改用 `UmultiplayerEnemyMeleeAbility` 与独立 `DA_EnemyCombatConfig`，保留本项目的行为树、GAS 和 CMC。素材接线复用单手近战四向移动、挥击、受击和死亡，原包的业务通知不运行。出手点取原剑兵 `AttackTraceNS_C` 的 0.342609 秒，收招补足完整 1.666667 秒动画；规则为 15 伤害、160cm 距离、1.8 秒冷却。近战只使用服务器的目标、距离、前向和遮挡，不生成玩家投射物，治疗与范围动作已解绑。

`Saved/MinionMelee-20261017/RegressionFinal/index.json` 为 **47 项通过、0 警告、0 失败**。新增四项实际小兵蓝图用例覆盖技能授予、起手后单次伤害、移远／墙挡／背后／目标销毁／未就绪／变阵营、眩晕与死亡取消，以及自己的 Casting 句柄清理与行为树恢复。首次全量检查保留在 `Regression`：HUD 测试临时日志捕获设备混入旧输出而失败；同步日志队列后重跑，未删除计时器清理断言或修改生产 HUD。原生无骨骼角色的武器空挂点警告改在实际有骨骼的蓝图上配置插槽后消除。

`MapAI/index.json` 单独通过真实 `CombatTest` 地图检查：出生器实际生成近战小兵，行为树沿 NavMesh 追击 **781.4cm**，在距离 **119.0cm** 时通过自己的 GAS 近战将玩家血量从 **100 降到 85**；之后实际致死并验证寻路、行为树和移动停止。测试没有在追击过程中调用 MoveTo、Attack 或移动敌人。

原 HUD、玩家表现蓝图与规则、两张地图和全部 1009 个源素材文件校验未变。该阶段尚未绑定武器网格，`EnemyWeapon` 只准备好右手挂点；后续长柄接线见下节。不把上述自动化或冷加载检查称为完整双客户端画面验收。

同一模块的 `NetworkLifecycle/report.json` 中，DS＋两个客户端的 Normal／Harsh 各一轮通过，耗时分别为 **110.900／133.953 秒**。这组用例继续验证玩家预测、取消、眩晕恢复、视口输入与方向快照；受控敌人夹具不等于真实小兵追击或双端挥击画面已验收。真实小兵追击由前述 MapAI 单独证明，武器与完整联机画面仍待人工验收。

## 长柄武器接线（2026-10-17）

按用户选择给实际小兵绑定已有 `FengMao_Weapon`，使用原骨架 `FengMaoWeapon_Active` 插槽（父骨骼 `hand_r`），保留源握持偏移；组件本地变换为单位变换，无碰撞、无 Overlap，不另开武器伤害入口。原包资源未修改，玩家、HUD、玩家规则与两张地图六项文件校验未变。

`Saved/PolearmAttachment-20261017/Regression/index.json` 为 **48 项通过、0 警告、0 失败**。新增 `MinionWeaponAttachment` 加载实际保存蓝图与武器，在真实 Idle／Attack 动画及角色平移旋转时验证组件跟随手部插槽，防止武器留在世界坐标。源插槽的只读导出位于 `Saved/PolearmAudit-20261017/SourceSockets.json`。编辑器实际视口已查看右手持械外观；这不代表已逐帧排除挥击穿插，也不代表双客户端完整画面验收通过。实际 DS 双客户端人工会话另行启动，测试报告不冒充人工结果。

## 视觉感知、搜索与巡逻（2026-10-17）

敌人控制器接入 UE Sight，默认发现距离 1500cm、丢失距离 1800cm、单侧视野 75°。行为树服务只从当前感知对象中选择有效敌对玩家，并复核视线；丢失目标后清空 Actor 追踪，只保留最后可见位置，最后看见后最多 5 秒未重新发现便清除搜索记忆。没有目标时，围绕自己的出生点在 700cm 内选择导航可达位置，由原生 MoveTo 移动并短暂停留。导航失败进入等待，不额外添加逐帧寻路。暂未增加听觉、仇恨或包围系统。

`Saved/AIPerception-20261017/Regression/index.json` 为 **49 项通过、0 警告、0 失败**。新增 `SightBoundary` 通过真实 Sight 验证背面、首次遮挡、拆除遮挡后的发现及玩家就绪筛选；原有决策测试验证墙后位置不再更新、搜索期限到达、重新发现与失效目标清理。独立测试世界显式推进真实感知系统，不直接注入成功感知事件。控制恢复、近战结算、武器跟随与随机出生间隔等既有检查继续通过。

同目录 `MapAI/index.json` 的实际 `CombatTest` 地图检查 **1 项通过、0 警告**：先隔离玩家，观察行为树自主生成的巡逻位置、真实非 partial 路径及 **100.7cm** 位移；随后将玩家放在视野内、无遮挡且可达的 900cm 位置，敌人实际追击 **806.5cm**，在距离 **98.1cm** 时由自己的 GAS 近战造成 **100→85** 伤害，死亡后停止行为树、路径和移动。该测试观察到巡逻移动，不冒充完整巡逻路线或双端画面验收。

`CombatAssetBuild -UpdateCombatAI` 只更新保存的黑板和行为树；`-ValidateCombatAI` 在新进程冷加载验证 6 个业务键、5 个分支、16 个主图节点及中断、等待参数。旧两个 AI 资产已备份至 `Saved/AIPerception-20261017/Backup`。原 HUD、玩家蓝图与配置、两张地图的六项校验保持不变。

同一构建的 `NetworkLifecycle/report.json` 中 Normal／Harsh 各一轮通过，耗时 **87.917／102.918 秒**，6 个参与进程均正常退出。该用例继续覆盖原有玩家施法、取消、控制恢复、视口输入和方向快照；它会移除关卡出生器并使用受控敌人，不能据此宣称真实小兵的弱网巡逻或双端动画观感已经验收。真实巡逻、感知后追击与近战由前述 MapAI 单独验证，双客户端画面另行实看。

## 护盾、持续效果与重连恢复（2026-10-17）

本批深化现有战斗和状态恢复，不增加另一套技能框架。新增可消耗的 Shield／MaxShield、周期 Burning 与独立 Root；攻击仍经现有统一伤害入口，减伤后先盾后血。Root 与 Stun 共同贡献移动限制，分别结束自己的 GE；Root 不停整棵行为树，仍允许射程内攻击，追击、搜索和巡逻分支由保存的黑板 CanMove 条件中止。只更新 BB／BT 两个 AI 资产，备份位于 `Saved/CombatDepth-20261017/BeforeAI-221048-6a03a9`。

`DefenseShieldAmount`、`BurningDamagePerTick`、`AreaRootDuration` 默认 0，原玩家和小兵配置资产未重新保存；非零配置分别接入 Q／R。`ConfiguredStatusSkillIntegration` 使用内存配置副本实际激活技能，不用 CDO 或持久资产替测试改平衡。原 HUD 布局保留，原状态文本通过既有事件显示护盾和多种状态，没有增加逐帧属性轮询。

`Saved/CombatDepth-20261017/RegressionFinal/index.json` 的 NullRHI 回归为 **61 项通过、0 警告、0 失败**。新增检查包括先盾后血与溢出、减伤顺序、扣盾回调中的嵌套伤害和取消资格、基础值与上限约束、同源灼烧刷新但不推迟首跳、异源分别计时、不同 Root 与 Stun 的到期次序、死亡停止周期伤害、配置技能接线，以及真实小兵被 Root 后仍能攻击。`CameraFacingMovingAttack` 使用实际保存蓝图和 Enhanced Input，横移期间真实攻击保留出手朝向，完成、取消和眩晕后恢复原转向策略；正常攻击阶段实测横移 260cm，最大朝向误差 0°。这是自动检查，不等同于完整转向动画的人工画面验收。

恢复白名单只包含已支持的 Burning／Root。保存每份实际 GE、来源攻击快照、剩余寿命和下一跳相位；离线期间先按当时防御是否有效计算应到期的伤害，再消耗保存的护盾与生命，致死则恢复死态。原来源失效后仍保留伤害捕获，不改成角色自己施加；原生 GAS 已不合并空来源，无需额外恢复效果类。没有恢复旧 AbilityTask、半次施法或重放伤害事件。

首次回归保留在 `Regression`，当时存在灼烧整数到期漏算与近战测试提前移走目标的两项失败。后者改为等待真实出手，不删除距离复核。随后 `Regression2` 的连续对照进一步暴露真实截止值 `1.75999999` 与下一跳 `0.76000005` 的浮点边界；生产代码统一使用实际 Timer 截止和周期，仅最终到期沿用 GAS 的末跳容差，不提前普通周期。`Regression3` 的 60 项通过后，追加 `PendingFinalTickMatchesContinuous`，在 Duration／Period 剩余均为零、最后一跳仍待执行的真实截止帧保存，与持续在线目标比较；最终 61 项均通过。保留到期前 50 微秒不扣血、整数 2／3 秒到期前后恢复、原来源销毁后攻击加成及离线致死检查，未只改期望凑通过。

`MapAIFinal/index.json` 单独通过真实 `CombatTest` 地图检查：自主巡逻 100.1cm，感知后追击 763.6cm，在距离 136.7cm 时经自己的 GAS 近战造成 100→85 伤害。`ValidateAISameBuild.log` 冷加载确认 7 个业务键（含引擎 SelfActor 共 8 个）、5 个分支、16 个主图节点、9 个装饰器；没有重新保存地图。

同一构建的 `NetworkReconnect/report.json` 使用一个 DS 和两个独立客户端，Normal／Harsh 各一轮均通过全部 **38 个必需断言**，耗时 57.456／60.840 秒，6 个进程全部正常退出。测试将同一来源的真实 Burning 分别施加给将断线的玩家和持续在线的对照目标，确认旧 Pawn 已消失、替代会话未接纳期间确有周期执行；重连后服务器比较实际护盾、剩余寿命及下一跳，客户端分别验证自己的复制结果，不用整数秒推测首跳，也不要求观察者读取远端完整 GE。普通／弱网护盾分别为 57／54，均与连续对照相同；下一跳误差均约 16.4ms，原有 73 生命、37 能量、票据、冷却、死亡与共享进度检查继续保留。

同一模块的 `NetworkLifecycle/report.json` 中 Normal／Harsh 各一轮通过全部 **91 个必需断言**，耗时 74.071／91.840 秒，另外 6 个进程全部正常退出。继续覆盖原有释放、取消、眩晕／死亡打断、视口输入恢复、按键方向快照、真实 CMC 横移靶和双方观察到一次权威伤害；没有把这些夹具称为新预测冲刺或历史命中回溯。两份报告的模块哈希一致；最终检查各自清单中的 241 项文件，差异仅为随后更新的 `Tests/README.md`，没有边改业务源码或资产边宣称同版本回归。

Harsh 参数为 `PktLag=200`、`PktLagVariance=50`、`PktLoss=5`，不等于实测 RTT。网络检查为 NullRHI 状态回归，不是双客户端画面验收。以上为护盾阶段的历史范围记录，不是任意效果的通用存档或预测位移回滚；快照在现有 Logout／服务器 Tick 链路采集，不承诺在伤害属性广播中同步退出和保存时的通用事务原子性。新增冲刺、弹反、成长配装、倒地救援与长期存档的当前结果见下一节。

## 联机战斗扩展闭环（2026-10-17）

本批在前述护盾／灼烧／限制叠加基础上补入预测冲刺、弹反、技能学习与装配、被动成长、倒地救援、版本化存档和终局失败。运行时代码只增加 Mobility、Progression、Coop 三组职责文件，继续使用现有 Projectile、BT／GAS／CMC 和 Presentation；没有另建伤害框架或复制第三方业务。原 HUD 和保存的蓝图布局不变，B 菜单是附加层，界面订阅状态变化，换 Pawn、重建和关闭时解绑。

操作为：B 打开菜单，花 1 点学习初始未解锁的冲刺，再用 Shift；Q 起手约 0.3 秒弹反；F 按住引导救援、松开取消；F5／F9 或菜单保存／读取成长配装。五槽可交换，原四技能仍一级可用。主动技能最高三级，人物等级须达到目标技能级；技能伤害和冷却实际读取 Spec.Level。成长及两个被动通过各自的无限 GE 句柄增减属性，装配只保留一份，不直接堆属性 Base。奖励只接受服务器正数与稳定标识，最近 64 条去重；不将这个有界窗口称为永久历史账本。当前按有效击杀来源发奖，没有助攻分摊或独立离线奖励队列。

冲刺使用原生 `UAbilityTask_ApplyRootMotionConstantForce`，不是客户端瞬移或自写位置 RPC。拒绝时 GAS 撤销本次预测消耗／冷却并结束任务；禁锢、眩晕、死亡和未接纳也终止自己的根运动来源，位置仍由 CMC 碰撞、移动包和纠正链路处理。不承诺任意副作用的通用回滚，也没有新增历史命中回溯。

弹反只在权威碰撞中判定敌对、就绪、时间窗口、正面角度和最初配置的次数上限。成功后改变来源／Owner／Instigator与碰撞忽略列表，以递增 Revision 复制新的直线轨迹段，仍不逐帧复制位置，不延长最初总寿命。原 Sweep 可能缓存墙命中，故在回调栈外再恢复新段，避免旧方向的残余通知错误销毁反射后的弹体。表现恢复基于当前段和服务器估算时钟，不等于客户端弹体预测或零误差同步。

玩家有可行动队友时才进入倒地，HP1但非战斗就绪，停止碰撞与机关占用；普通治疗不复活倒地者。F 是服务器 GAS 引导：独占目标、持续复核距离／遮挡，释放、受伤、眩晕、死亡或退出统一收尾，完成只恢复 30%生命而不补能量／冷却。最后一名可行动玩家死亡才写入共享 bGameLost，失败后拒绝钥匙提交和胜利，结果可补读且重复通知不重复发布。Controller 的 Enter 重开入口仍由 GameMode 校验和去重。

长期 SaveGame 只保存成长、技能和被动，不包含本局 HP、位置、资源、冷却或旧任务；版本、等级、点数收支、槽位唯一性、档案身份不合法时保持当前状态。单机固定本地槽，联机仅访问服务器签发 ProfileId 对应的档案，没有客户端文件路径。匿名直连尚无账号认证，不声称服务器重启或换服后自动识别原玩家；保存／读取和本局票据恢复是不同流程。

`Saved/CombatComplete-20261017/RegressionIsolated/index.json` 为 **87 项通过、0 警告、0 失败**；新增冲刺4项、弹反5项、合作生命6项、成长5项、菜单2项及小兵类型4项。覆盖真实墙碰撞／Falling重力、取消不残留根运动、来源切换／旧来源受伤／次数上限／旧Sweep墙通知、倒地和救援的排他／动态距离遮挡／六种中断／失血期限／团灭、GAS Spec授予和被动撤销、拥有者复制条件、真实菜单按钮与换绑清理。磁盘存档验收只使用先验不存在的随机服务器档案槽，覆盖多次加载不重复、HP／能量／冷却不返还、版本及身份等坏数据拒绝，结束清理测试自己的槽；没有读写正式 `CombatProgress_Local`。第一次 `Regression` 在测试直接读取复制注册时缺少 UClass RepIndex 初始化而触发引擎断言；只修夹具前置步骤，未放宽 OwnerOnly／Always 复制断言。`Regression2` 的82项、追加终局后的83项和隔离修复前的87项保留为中间记录。

为使正常关卡能使用弹反并体验成长，出生器默认前两只近战、第三只远程，继续复用同一保存 BP、骨架、武器、动作、行为树和 CMC。远程在出生前复制实例配置，射程1100cm／伤害15／成本0／冷却1.8s，保留原起收招时间；只授予实际攻击，不改共享资产。每只经验默认50，两次实际击杀达到二级。四项新增检查覆盖实际BP配置、真实Sight→BT→GAS发弹及眩晕恢复、错峰／重复／数量上限／关闭远程／销毁取消，以及两种真实小兵击杀奖励和重复归零不再发奖。

`MapAIPlayable/index.json` 单独通过真实地图检查：自主巡逻100.0cm，感知后追击749.9cm，在距离150.5cm时由小兵自己的 GAS 近战造成100→85伤害，之后死亡退出决策和路径。此处没有在追击时替行为树发 MoveTo／Attack，不是完整群体战斗画面验收。编辑器和 Win64 Development 游戏目标均编译通过；本轮未保存额外蓝图或第三方资产。

本轮三进程 NullRHI 网络结果（每轮一个本机 DS、两个独立客户端）：

| 场景 | 必需断言／每轮 | Normal | Harsh |
| --- | ---: | ---: | ---: |
| CombatExpansion | 78 | 通过，61.551s | 通过，81.441s |
| CombatReconnect | 66 | 通过，111.281s | 通过，119.864s |
| CombatLifecycle | 91 | 通过，88.155s | 通过，92.634s |
| Combat（隔离修复后） | 222 | 通过，64.539s | 通过，106.397s |

Expansion 验证真实客户端学习 RPC、同键冲刺预测接受／禁锢中断／资源拒绝撤销、双端收到反射轨迹与单次伤害、拥有者成长与远端隐私，以及救援取消／眩晕中断／成功。测试三端配置只在内存延长观察窗口：冲刺1.2s／冷却至少6s、救援4s／失血45s；服务器弹反 GE 窗口2s，用于隔离命令时延，**不冒充客户端按Q的竞技时序验收**。生产默认值由本地功能回归检查，资产与全局 CDO未改。

Reconnect 先由客户端真实学习 Dash／Fortitude、服务器发奖升级，再在存活、倒地、死亡三种状态分别真实断开连接，通过原 GI／票据／握手生成新 Pawn。核对同一服务器档案、技能与被动、公开max125／HP73／能量37、剩余冲刺冷却、离线灼烧和原有共享进度；半次冲刺与救援不恢复。倒地断线前，伙伴真实8s救援已被双方观察；旧目标失效后业务清理旧救援，新 Pawn 的失血期限仍为原始45s，Normal／Harsh实测分别在45.09／45.04s自然死亡，而非重连后再给45s。上述三份 `Network*Final/report.json` 的18个参与进程均正常退出，没有脚本强杀。

这三组报告及 `MapAIPlayable` 对应模块 SHA256 `b6ff790fcd08f26d679101a9ba9dec4f451c571ae472a1f7e7c76de992bd5211`。随后旧 `Combat` 的 Harsh 轮在治疗阶段超时，失败保留于 `NetworkCombatFinal/report.json`，不计为通过。排查发现该场景未隔离原关卡小兵：第一名玩家接纳后在真实地图等待18.4秒，之后测试却直接假定其生命仍为100；原日志未采集治前HP，不能断言具体是哪一次小兵攻击造成了何种血量。修复仅限测试：在Combat模式宣布服务器就绪前清理地图出生器／敌人／遗留弹体，新增双方满血满能量与治前40HP的必需断言；服务器治疗结果不再等待客户端满足预期后才检查。没有重置资源来遮盖污染，没有修改治疗业务或放宽70HP结果。其他真实地图和正常游玩不清场。

隔离修复后模块为 `c79af74b4f2defcbaa769d6995e4904be9a2439e9b860fd9b653c6b5142f2c52`，全87项回归再次通过，编辑器与游戏目标均编译通过。与已通过报告的254项源清单比较，差异仅为测试Driver、Combat场景、运行脚本及本说明，业务源码、配置和所列资产未变；不把两种测试模块说成同一二进制。

`NetworkCombatIsolated/report.json` 的Normal／Harsh各一轮通过全部222个必需断言，6个进程均正常退出。两轮实际记录双方初始HP100，队友真实受伤后HP40，治疗后HP70、施法者能量55；后续预测接受／拒绝／撤销、合法重试和资源隐私检查全部保留。运行脚本契约自测通过，缺失新增基线或准备断言仍判失败。本轮上表共8个通过轮次、24个正常退出进程；失败报告仍独立保留，没有算入成功数量。收尾时没有遗留本轮UE测试进程。验证开始于10月17日，最终隔离复测于10月18日完成，目录沿用开始日期。

Harsh统一为 `PktLag=200`／`PktLagVariance=50`／`PktLoss=5`，它们是模拟器参数，不是实测RTT。网络场景、磁盘状态和UMG构造检查都不等同于真人操作或渲染画面验收；新冲刺／救援动画观感、双端美术衔接仍需实看。旧带宽微基准仍属于对应历史版本，不能直接当成本轮增加弹反和成长后的性能收益。

## 撤离关卡、世界血条与最终画面复核（2026-10-18）

新增独立 `/Game/Combat/Maps/CoopEscape`，不覆盖原 Egypt 地图或原 HUD。玩法为两人接纳后开始 3／4／5 只小兵三波战斗，按间隔错峰生成，活动敌人加待生成预约总数限制为 8；玩家在战斗中领取真实重叠补给、收集四把钥匙，两名不同的有效玩家同时踩板开门，再共同进入出口撤离，Enter 经原权威入口重开。机关仍使用本项目 Actor／组件与服务器规则，未为了使用 GAS 重写整套机关。敌人保留自有 BT＋GAS＋CMC，复用已有长柄外观和动作。

敌人血条改为世界空间组件，本地朝向各自镜头，跟随已平滑的骨骼身体，并按胶囊高度放置；不复制血条旋转。使用有遮挡材质、固定物理尺寸、无碰撞／阴影／射线追踪，手动重绘；生命或上限不变不刷新，超距隐藏后再次可见时补读当前值，死亡和退出解绑并停用。Dedicated Server 不创建 UI。原 HUD 继续通过事件订阅更新，底部布局不重建。倒地仅复用已有 DeathMontage 进行本地表现，救起只停止这一条蒙太奇，不增加 ragdoll 或改动物理／移动；失血死亡承接原倒地姿态，退出时在组件 EndPlay 清理后撤销唯一 next-tick 表现任务。

`Saved/CoopEscape-20261018/RegressionPoseFloorFinal98/index.json` 为 **98 项通过、0 警告、0 失败**，包含世界血条、波次预约／上限、补给唯一领取／满资源／倒地资格、终局停止、晚读取快照，以及重复倒地通知、救援恢复、失血死亡承接、RepNotify 顺序和 DS 不播表现。退出检查保留对象但真实调用 RouteEndPlay，再推进世界，确认旧姿态未被遗留回调停止或重播。首轮 `RegressionDowned98` 的随机错峰时间失败未删除：夹具在世界首次 Tick 前创建 Timer，Pending 激活额外增加一帧；测试现在先正常推进一帧，仍保留随机间隔与原 `.08–.17s` 断言，未修改生产出生间隔或放宽门禁。该测试文件只在 `WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR` 编译，其全部用例均为 EditorContext，不把 PIE 专用 API 编进非编辑器游戏目标。

最终正式技能画面使用保存的 `BP_CombatPlayer`、`BP_CombatEnemy`、实际动画／蒙太奇和原布局的 Escape HUD；只在开发验收 World 的 GameMode 实例装配，不改生产 GameMode、CDO 或资产。Basic Combat 的高空隔离夹具仅补无碰撞可见灰色表面；另两组沿用已有实际地板。倒地／救起先验证原生命与状态，再分别等待 0.8／0.35 秒观察动画混合，不修改伤害、回血、救援时间或请求。两端每个阶段必须等本轮唯一 token 的非空 PNG 保存后才回执，Expansion 的救援经真实 F 按住／释放入口。

| 最终场景 | 网络／画面 | 必需断言 | 结果与本机报告目录 |
| --- | --- | ---: | --- |
| Combat | Normal，正式角色／HUD，6 PNG | 231 | 通过；`FinalSkills/PoseFloorCombatNormal` |
| CombatLifecycle | Normal，正式角色／HUD，8 PNG | 102 | 通过；`FinalSkills/PoseFloorLifecycleNormal` |
| CombatExpansion | Normal，正式角色／HUD，10 PNG | 92 | 通过；`FinalSkills/PoseFloorExpansionNormal` |
| Combat | Harsh，NullRHI | 222 | 通过；`FinalNetwork/PoseFloorCombatHarsh` |
| CombatLifecycle | Harsh，NullRHI | 91 | 通过；`FinalNetwork/PoseFloorLifecycleHarsh` |
| CombatExpansion | Harsh，NullRHI | 78 | 通过；`FinalNetwork/PoseFloorExpansionHarsh` |
| CombatReconnect | Normal／Harsh，NullRHI | 66／66 | 两轮通过；`FinalNetwork/PoseFloorReconnect` |

上表目录均位于 `Saved/CoopEscape-20261018`，各 case 错误数为 0，进程自然退出。实际 Editor 项目模块 SHA256 均为 `5ae0ed3f3b2ba593ce70b35206dd7987455fbd170613c99cc29426f8f490846e`；报告源指纹在仅限 Editor 的测试 guard 修正前后不同，不声称所有源清单完全相同。实图确认正式 HUD、治疗蓝色粒子、R 地面标记与施法条、倒下／躺地救援／38/125 站起；旧空原生外观夹具与首帧过早截图仍保留，不计为正式视觉通过。受控 BasicDefense 图对 Q 表现仍只作部分证明，不能拿它代替下述真实撤离场景的蓝盾画面。

### Windows 打包客户端完整撤离

Win64 Development／Shipping 游戏目标与 Cook／Stage 已完成，但当前实际联机验收拓扑是 **一个 Editor Dedicated Server＋两个真实 Windows Development 打包客户端**，不是独立 Server Target 成品，也不是云端部署验证。显式 `ServerExecutablePath=UnrealEditor.exe` 的运行记录包含服务器构建种类与哈希；曾把普通 Game exe 加 `-server` 的尝试被 `DedicatedAuthority` 门禁正确拒绝，旧红报告 `PackagedFinalNormal` 保留。

首次混合包体用例 `PackagedClientsEditorDSNormal` 虽通过状态门禁，初始截图曾只有地形和 UI、人物与静态场景延迟出现，因此不计为完整视觉通过。UE5.5 的 PSO 默认策略会在预缓存未就绪时不创建 Static／Skinned Mesh 代理，且该策略不用于 Editor。当前只设置 `r.PSOPrecache.ProxyCreationWhenPSOReady=0`，保留预缓存、取消延迟代理：优先保证首次进入时人物与场景可见，代价是首次绘制可能同步创建 PSO 并卡顿，**不是性能优化数据**，也不代表已完成通用 PSO 预热或消除首帧停顿。

同一 `PackageVisible` 内容、同一客户端 exe SHA256 `8c103910c34d0851fc3b5f13cbe3e50a8dbec066e3cdc7c8ddc07af42ec6c9ab`，随后三轮完整撤离均通过全部 **38 个必需断言**，每轮保存两端合计 20 张 1280×720 PNG：

| 轮次 | 档位 | token | 报告目录 |
| --- | --- | --- | --- |
| 1 | Normal | `20261017-215931-e69b4a48-1` | `PackagedVisibleNormal` |
| 2 | Moderate | `20261017-221035-482cbb29-1` | `PackagedVisibleRound2Moderate` |
| 3 | Normal | `20261017-221328-d5cfebca-1` | `PackagedVisibleRound3Normal` |

实看双方 Ready／Combat／Gate／Victory／Restart，首次人物、长柄小兵和场景完整可见；Q 蓝色护盾在真实战斗场景中包住身体，三波与钥匙 0→4、双人开门／撤离、Enter 重开后 0/4 均有真实状态与画面依据。它们是玩法和画面验收，不是 CSV／Insights 性能测试、固定 60FPS 结论或录像逐帧质量保证。Moderate 为配置 `PktLag=100`／`PktLagVariance=20`／`PktLoss=2`；Harsh 为 200／50／5，均不等于实测 RTT。

后两轮日志检索未见 PSO 创建等待、PSO hitch 报告或 VSM overflow，但 **两端在重开 PendingNetGame 握手期间各有 3 条 `CreateSavedMove: Hit limit of 96 saved moves` 警告**，之后成功切图并恢复新局；该问题仍保留待定位，不宣称网络零警告。Development 图中的旧蓝图 `true/false` PrintString 也未用全局屏蔽消息隐藏。

Shipping 编译／打包通过，但本轮菜单烟测被首次启动的 Windows 安全中心防火墙权限弹窗阻挡，**未确认菜单、未连接服务器，不计为菜单或完整联机通过**。没有代点安全权限或修改系统设置，仅核对实际路径后关闭本次自有 bootstrap 与 Shipping 进程。该受阻记录不能被上述三轮 Development 联机闭环替代，反之也不能拿 Shipping 的构建成功证明其菜单或联网已经验收。

收尾重新校验原 `Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap` SHA256 为 `e2d04e4905867717252f9552af9cc43a3ea5bcfecd3dabb0a681901af321cd6d`，原 `Content/Combat/UI/WBP_CombatHUD.uasset` 为 `6c264e7dd2646cfd1599e57ad3ce2157f937efd64a0ea86cfa7c070b1619ef79`，与修改前一致。新关卡、HUD 副本与素材迁移失败记录单独保留，没有覆盖原件。

## 技能瞄准、范围与失败反馈（2026-10-18）

本轮将技能的几何查询集中到 `multiplayerCombatTargeting`，资格查询与失败文案集中到 `multiplayerCombatSkillStatus`。本地预览、能力确认和服务器结算共用规则，不另外维护一套 HUD 伤害范围；客户端只提交瞄准方向，不能指定权威伤害、目标或任意落点。没有把机关重写成 GAS，也没有引入通用世界快照回滚框架。

- 左键普攻：镜头准星选点，真实发射点收敛到选点，预览与服务器使用同半径扫掠；发射点贴墙有明确拒绝原因。
- Q／E／Shift：按住预览、松开释放，右键取消。Q 区分全向减伤与短时正面弹反；E 显示 6 米三维治疗范围及实际选择的受伤友方（含自己）；Shift 显示胶囊宽度的可达走廊，未学习时提示按 B，静态障碍截断距离。
- R：按键选地面、左键确认、右键取消。确认后本次落点锁定；水平半径 220cm、有限高度及遮挡过滤由预览和实际目标收集共用，实际结算仍复核移动目标。精确结果圈保留 2 秒，素材参考半径独立配置为 400cm，当前爆发粒子缩放 0.55，不再把视觉大爆炸冒充伤害边界。
- 预览和取消不扣能量、不开始冷却、不产生能力预测键。已确认施法的取消仍沿用原有不退款规则；新预览清除上一技能的短时失败消息。未就绪、死亡、倒地、救援、眩晕、冷却、能量不足、未学习、无治疗对象、无有效地面或冲刺受阻均有具体反馈；服务器拒绝使用服务器原因和权威资源／剩余冷却值。
- 原 UMG 布局和事件驱动生命／能量更新保留；只有本地瞄准几何跟随镜头更新，范围投影按场景遮挡裁剪，不复制预览。小兵仍走自己的 BT＋GAS，起手红色扇区不冒充本轮单体锁定目标。

最终专项报告根目录为 `Saved/SkillTargetingFix_20261018`。`FinalRegression/index.json` 的 111 项全部成功、0 警告、0 失败；其中新增 11 项目标几何／真实命中测试和 2 项失败反馈测试，原 HUD 生命周期用例补充“新预览清除旧反馈”检查。`FinalCombat/report.json` 的 Normal／Harsh 各 270 项必需门禁通过，`FinalLifecycle/report.json` 的 Normal 102 项通过；三组参与进程均自然退出、未由脚本强制停止。Editor 与 Win64 Development 游戏目标均构建通过。

FinalCombat／FinalLifecycle 记录的磁盘 Editor 模块 SHA256 均为 `7926541e8e67268f7a223e6af927562c7c71a387de7ddc538e4ebde08f52c122`。报告的 `loadedByRuntimeVerified=false` 保持原义，不把磁盘指纹扩写成运行时模块加载身份测量。后续 Expansion 测试隔离修正会改变测试模块，不与上述二进制混称同源。

真实按键验收经所属 PlayerController 的正式输入绑定完成：双端分别按住 Q／E／R／Shift，保存本轮 token 的 PNG，再右键取消、松开原按键，检查未误扣资源或启动能力。逐张核对了双方的 Q 扇区、E 球形范围与所选目标、R 锁定圈和缩小后的结果粒子，以及解锁 Shift 的 4.5 米走廊；不是只检查文件存在。测试使用隔离靶场，不把灰色夹具表面误称正式关卡丢失，也不据此宣称动画连续观感、实际公网或性能帧率全部验收。

失败记录没有删除：`VisualInput` 的首帧复合预览失败缺少子项诊断，不能事后断言其根因；`VisualInputFinal` 则被新增严格门控发现声明高度 4000 与敌人实际 3992.15 不符。修正夹具摆位、等待独立复制依赖与两次真实 World Tick 后，`VisualInputRetry` 的 Normal／Harsh 均通过。等待条件不轮询预览结果，不能靠“等到成功”掩盖错误。

`FinalExpansion` 的追加复测在学习阶段超时，截图已有“本局失败”，证明专项开始前存在正式场景污染；旧日志没有治前生命记录，不能指认具体受伤过程。后续只把 Expansion 的敌人／刷怪器清场提前到首位玩家连接前，并新增双端 HP100／能量100／可行动且未终局的强制基线，不重置血量或终局来抹掉污染。该测试模式隔离不影响正常游戏中的敌人。

修正后 `FinalExpansionIsolated/report.json` 的 Normal **103 项必需门禁全部通过、0 错误**，包含两端真实学习 Shift、各自按住预览／取消不误放、实际冲刺接受／中断／拒绝撤销，以及原有弹反、成长和救援流程。`FinalRegressionIsolated/index.json` 再次确认 **111 项成功、0 警告、0 失败**。此时 Editor 模块为 `e4dc53ddb9021cb24ebc2278007a455cfba185343adbf0fe28000cc728648e7c`；与 FinalCombat 的源清单逐项比较，差异仅为网络测试脚本、`CoopCombatExpansionScenario.cpp` 和 `CoopNetTestDriver.cpp` 三个测试文件，业务／表现／配置与所列资产均未变化，不把不同测试模块说成同一二进制。

收尾的可操作会话为 `Saved/DedicatedServer/20261018-201654-2e1c5a1e`：`127.0.0.1:7777`、一个 Editor DS 和两个普通客户端，服务器日志读回 `EnemyCount = 1`，两名玩家均正常加入并完成战斗接纳。没有使用自动测试技能或无敌开关，也未抢占正在使用的 Insights 窗口；此会话供用户操作，不自动生成通过结论。`-EnemyCount 1` 只改当次地图实例，Enter 重载会恢复地图默认数量；如需继续单敌人验收，关闭双方客户端后用同一启动参数重开。

Harsh 为模拟器参数 `PktLag=200`、`PktLagVariance=50`、`PktLoss=5`，不是实测 RTT。共享规则保证相同输入与场景状态下的查询一致，不承诺预览时移动目标必在稍后命中；服务器仍按结算时状态复核。冲刺提示的是预计水平距离，落差和动态障碍仍由 CMC 碰撞、重力和权威校正处理。

### 普攻方向、治疗结果与 R 输入修复（2026-10-18，后续专项）

本轮报告独立保存在 `Saved/SkillFeedbackFix_20261018`，不引用上一轮 111 项或网络门禁作为本轮通过证据。普攻起手 Cue 使用本次权威发射的位置／方向快照，Cascade 与 Niagara 使用相同旋转；治疗结果保留施法者上下文，资源和冷却仍用原预测键，未预测播放的权威结果 Cue 不再继承该键后被拥有者误去重。瞬时 Cue 缓存本次表现配置，异步补载时不重新从已退出的施法者推导配置。

当前操作统一为 Q／E／R／Shift 按住预览、松开释放；预览期间仍可左键确认或右键取消。五槽技能栏独立置于底部中央，生命／能量留左下，透明背景及事件更新逻辑不变。普通波次默认全近战，小兵的远程变体保留为显式选项。专项资产刷新脚本先逐个备份并验证哈希，仅更新专用表现副本、小兵受击反馈和治疗 Cue；原 HUD、地图和其他本项目资产必须不变。

新增测试覆盖真实 R 释放、左键确认后松键不重复释放、取消后松键、槽位交换，以及实际范围粒子位置。联机视觉模式额外测试真实 E 自疗：保留原伤害和消耗状态，等待治疗冷却自然结束，暂移受伤队友，再经跨帧 E 输入验证预测消耗及权威结算；拥有者与旁观者都必须观察本轮新活跃的正式治疗粒子。截图在观察到短粒子时立即发起，生命、预测确认和落盘回执分别检查，不重放粒子凑截图。

首轮 `RegressionRendered/index.json` 记录 120 项中 119 成功、1 失败；失败属于新增 `HealResultCueOwnerAndObserver` 的网络角色夹具：仅更改 Actor Role 没有刷新 ASC 缓存的权威身份，不能当作客户端接收分支。该报告及失败细节保留；后续通过结果另录，不覆盖此失败。正常撤离关卡的移动施法与冷加载观感仍需单独验收，隔离夹具不能代替完整玩法验证。

修正后的 `RegressionFinal/index.json` 为 **120 成功、0 警告、0 失败**（实际渲染，不用 NullRHI 跳过粒子项）。专用 RPC 接收 ASC 在 Proxy 角色下注册并明确验证引擎缓存非权威；旧本地预测键确实被过滤，独立权威结果键各产生一次拥有者／旁观者组件。保存的 `GCN_Heal` 只对不同施法者隔离实例，避免两位施法者冷加载时互相覆盖，不引入通用事件队列。

`Combat/report.json` 的 Normal／Harsh 各 **285 项必需门禁通过、0 错误**，包含真实 E 输入、预测消耗、服务器结算、两端新治疗组件及截图；`Lifecycle/report.json` 的 Normal／Harsh 各 **106 项通过、0 错误**，包含真实 R 按下／释放、原生预测、两端正式范围粒子、一次伤害及取消／眩晕／死亡收尾。四组的 Server／Client／Partner 均 exitCode 0、未由 runner 强停。

实际逐图检查确认五槽与生命能量分离，R 双端结果粒子出现在落点、施法者黄圈清楚。首轮发现默认操作说明盖住技能栏，最终将两种说明由视口高度 84% 上移至 62%；后续 Lifecycle 双端图确认不再重叠。Client SelfHeal 图可见脚底到身体的治疗粒子；Partner SelfHeal PNG 未清楚显示短粒子，虽然该端正式组件位置／活跃断言已通过，不把这张图宣称为清晰的治疗画面。BasicAttack PNG 是命中后画面，起手方向的证据来自真实 Cascade／Niagara 组件快照测试，不能替代多角度连续操作录像。

版本边界：Combat 的磁盘模块为 `e37a08001f4d966aaeb0c2c85e3fc020feb917623d150d2697ceec6a2682d9cd`，Lifecycle 为 `9ac52d88889c4155404424ea0ecd6ba88baba94af569f88c2a4c9d76c4e36d53`；两报告源清单比较仅 `multiplayerCombatPresentation.cpp` 不同，对应两处提示 Y 坐标调整，技能规则、网络代码、测试及资产不变。报告仍标明 `loadedByRuntimeVerified=false`，不将磁盘哈希说成运行时模块证明。原 HUD 与埃及原地图哈希保持原值，专项四资产均有校验过的原稿备份；所有首次失败记录保留。

## 正式 HUD、技能特效与朝向修复（2026-10-19，冻结验收）

本轮只修现有 GAS 技能与表现，不增加另一套架构。排查正式资产发现 `/Game/Combat/UI/BP_CombatHUD` 仍继承普通 `AHUD`，因此原生范围绘制根本没有运行；此前专项夹具替换了 HUD，夹具通过不能证明正式游戏通过。现将同一个 BP 的父类改为 `AmultiplayerCombatHUD`，保留原图表和 UMG 创建流程，原生控件字段改名 `NativeCombatWidget`，避免与原蓝图变量同名。原 `WBP_CombatHUD` 和埃及原地图哈希仍分别为 `6c264e7dd2646cfd1599e57ad3ce2157f937efd64a0ea86cfa7c070b1619ef79`、`e2d04e4905867717252f9552af9cc43a3ea5bcfecd3dabb0a681901af321cd6d`。

特效只校准 `Combat/Presentation/Effects` 下的三个项目副本：`P_CombatFireProjectile`、`P_CombatFireCast`、`NS_CombatDefenseShield`。投射物去掉长网格尾焰和大烟雾，保留紧凑火焰；护盾去掉无关地面水浪。原三个第三方素材前后哈希一致，逐包备份和校验位于 `Saved/CombatAssetImport/BeforeSkillPresentationRepair-*`。不是靠转 90° 掩盖素材轴向，也没有改动原包。普攻起手、瞄准几何与真实投射物共用身体前方发射点；胸前到前移点先做同半径扫掠，不能跳过贴墙或贴身目标。

Q／R 不再把施法者强行转到镜头方向，受击目标也不随施法者镜头转向；已接纳的普攻仍按本次瞄准快照转身。Q 的防御姿态由实际 GE／标签驱动上半身基础层，不用瞬时 Ability 的结束冒充防御结束，也不强停其他动作。R 界面同时说明当前配置的 **6.5 米最大选点射程、当前落点距离、2.2 米伤害半径**；它们读取与原生判定相同的配置。默认操作提示移到上方，避开人物成长文字，不能把装饰粒子边缘当成伤害边界。

正式无 AI 双客户端录制与网络夹具分开。`Round2Demo` 首轮 Observer 朝天且被自身遮挡，判为取景失败，原帧保留；`Round2DemoRetry` 先经真实 S／D 输入分开玩家，再使用稳定观察朝向，两端各录约 40 秒、366 帧，目标采集频率 10Hz，视频按真实帧请求时间编码。逐图可见远端向前发射的紧凑火焰、Q 防御姿态与身体护盾、E 脚底反馈、R 贴地结果圈及取消后的清理。Owner 私人预览本就不向 Observer 复制。该录制用于表现检查，不是 10FPS 性能结论、无 AI 命中准确性证明或完整敌人战斗验收；采集完成标记不自动等于视觉通过。

报告根目录为 `Saved/SkillPresentationRepair_20261019`。冻结模块的 `FrozenRegression/index.json` 为 **131 项成功、0 警告、0 失败**，包含正式 HUD 接线、项目副本冷读取、防御姿态生命周期、Q／R 朝向与 R 真实致死后不再播放 Area 结果 Cue 等检查。`FrozenCombat/report.json` 的 Normal／Harsh 和 `FrozenCombatModerate/report.json` 的 Moderate 均为 **288 项必需门禁通过**，含两端真实 AnimInstance 防御姿态及拥有者预测拒绝后姿态撤销；`FrozenLifecycle/report.json` 的三档均为 **106 项通过**。六组 Server／Client／Partner 均 exitCode 0、`stoppedByRunner=false`。上述网络报告与最终录像的磁盘模块 SHA256 均为 `692768833dd31ff4ac97c71226d732616968d3a61398d50f8291ac0962a0a84c`；没有测量进程内已加载模块哈希，不把磁盘指纹扩写为运行时证明。

最终第三段录制为 `FrozenDemo`，采用 Harsh、正式地图与原 HUD、无 AI、仅本进程免消耗／冷却；Owner／Observer 各约 40 秒、361 张原帧。`FrozenDemo/Owner/Review.mp4`、`FrozenDemo/Observer/Review.mp4`、动作清单、UTC／帧时间及启动参数均保留，录像前后模块哈希一致，三进程已按本轮身份核对后关闭。取证仍约 10Hz、无音轨，不作为运行帧率或音效验收。正式规则测试没有使用这些试玩开关。

最终关键帧检查可见：Owner 的 Q／E／R／Shift 预览、Q→E 切换后仅剩 E 范围、R 取消后的无圈状态，以及 Q 内镜头转 90°但角色 yaw 差值为 0；Observer 可见紧凑前向火焰、Q 防御动作与身体盾、E 脚腿反馈、R 贴地圈内粒子。观察端第 030／037／263 帧支持前向火焰，第 053／233 帧支持防御，第 090／163 帧支持治疗，第 117／212 帧支持范围结果。首次普攻的粒子未取得清楚画面，不声称每发视觉都已证明；Harsh 起手取消在 Owner 约 15.104 秒发送，Observer 约 15.854 秒收完表现，不能称远端瞬时退出。机器生成的 `visual_passed=false` 不改写成自动通过，另存 `VisualInspection.json` 区分图像检查、执行结果及未覆盖项。

失败结果不覆盖：`Regression/index.json` 留有 **127 成功、2 失败**，分别暴露副本发射器在重建后被重新启用和旧素材名称断言；修复后再冷读取，而非放宽门禁。早期 `Combat` 的 Harsh 自疗粒子观察遗漏也保留。后续通过不能推导原生瞬时治疗 Cue 在丢包下必达；该不可靠表现事件的传输保证没有改变。R 致死收尾本轮有原生回归，但尚无独立的双客户端 R 致死网络专项。所有弱网参数都是本机模拟，不是公网或性能压测结果。

复查使用新输出目录，保留旧证据；下面的技能录像使用正式 `CombatTest`／HUD、无 AI、临时免资源和冷却，不保存试玩开关。网络 `Combat` 仍是独立受控夹具，不能用它代替正式录像：

```powershell
./Scripts/RunSkillPresentationReview.ps1 -OutputDirectory 'E:/ueprojrct/multiplayer/Saved/SkillPresentationRepair_20261019/RecheckDemo'
./Scripts/RunMultiplayerNetworkTests.ps1 -Scenario Combat -Profiles Normal,Harsh -Repeat 1 -Map /Game/Combat/Maps/CombatTest -TimeoutSeconds 480 -Offscreen -VisualAcceptance -OutputDirectory 'E:/ueprojrct/multiplayer/Saved/SkillPresentationRepair_20261019/RecheckCombat'
```

### 去除外围辅助线与普攻水平前射（2026-10-19，17:09 用户录像后）

根据实机反馈删除普攻常驻射线、重复准星、防御预览扇区、治疗搜索球、R 最大射程外圈及连接线、冲刺走廊边线；保留原 UMG、失败文案、治疗对象小标记、冲刺终点和 R 实际落点伤害圈。R 生效后不再把预告圈和结果圈重复描边。空闲时不做已经不再展示的普攻预览扫掠。敌人近战危险预告不在本轮删除范围内。

玩家普攻改为本轮出手快照的水平前射：保留激活时按相机 yaw 转身一次，但相机 pitch 不再把弹体导向脚下。预览查询、真实弹体和起手 Cue 仍共用同一次起点／方向；贴身目标、近墙扫掠和敌人原有三维瞄准不变。项目起手粒子副本改为局部 +X 的紧凑喷发，清除世界重力、径向散射及内部位置偏移；未以粒子转轴掩盖真实弹道差异。

专项报告在 `C:/UEBuild/CoopForwardFix_20261019`：`Targeting/index.json` 为 **20 成功、0 警告、0 失败**；随后 `RegressionRendered/index.json` 为 **133 成功、0 警告、0 失败**。包含五种相机 pitch 下实际能力产生的水平弹体、同源起手 Cue、前方真实命中、不同 pitch 的近墙阻挡，以及敌人保留高度差瞄准。`CastForwardBurst` 冷读项目副本验证前喷分布和零重力，不将其当作渲染结论。`-VfxOnly` 资产修复的备份报告在 `Saved/CombatAssetImport/BeforeSkillPresentationRepair-20261019-174201-508285`：非特效规则、原 HUD、当前动画、地图和第三方原素材均未变。

`DemoNormal` 与 `DemoHarsh` 使用同一磁盘模块 `07c158114531a22302854ab669dffac70f2fbe980711b22995ba6089b934be35`，正式 CombatTest／HUD、DS 加两客户端、无 AI、临时免消耗冷却，分别留下 Owner／Observer 约 40 秒、350／352 与 357／358 帧。两个 `ReviewReport.json` 执行检查通过；自动 `visual_passed=false` 保持原义，不伪造自动美术验收。原始 PNG、按实际时间编码的录像和输入清单均保留。

实际画面抽查：Normal Observer 8–10 帧显示前向飞行的空中火焰，Owner 28／65／86 帧分别显示无大扇区的 Q、仅目标小圈的 E、无外围射程圈的 R；Observer 240 帧支持转向后的前射。Harsh Observer 27–29 帧同样可见前向飞行，Owner 89 帧保留单个 R 落点圈、122 帧取消后无旧圈。Harsh 早期画面出现着色器准备提示，不据此声称冷启动每发粒子均清楚可见。两档均为本机模拟，不证明公网、每发瞬时 Cue 必达或敌人战斗完整验收；方向／真实命中的依据另有上述原生测试。

玩家前射策略改变后，旧 `CombatTraffic` 的 pitch=90 朝天负载不再等价，后续性能对照须重建水平空旷走廊基线，不能直接复用旧数字。

### 治疗表现复用与紧凑技能标题（2026-10-19，后续专项）

按用户要求把原 Q 护体粒子配置给 E 治疗，Q 保留实际防御标签驱动的动作和音效，不再显示该粒子；治疗不再播放 HealMontage，但保留原消耗、冷却、目标选择和权威治疗。治疗粒子从实际受治疗者脚底生成并跟随其根组件，保持世界缩放，使用 0.8 秒表现时长及原有瞬时组件清理。没有改变瞬时 GameplayCue 的不可靠传输保证。

五个技能标题调整为技能名与按键紧凑同行，例如“治疗 / E”，居中位于原图标上方。两种正式视觉控件只改标题排列，不重建控件树，不改图标、冷却条、透明背景及事件订阅。`UpdateSkillHeadingsInEditor.py` 通过专用原生命令更新两份控件，逐包备份与哈希保护记录在 `Saved/CombatAssetImport/BeforeSkillHeadingUpdate-20261019-192503-426326/SkillHeadingUpdateReport.json`；原 HUD、地图和其他 Combat 资产未被该操作改动。前两次 Python WidgetTree 属性读取失败发生在保存前，原日志保留，没有将失败重写为成功。

本轮报告根目录为 `C:/UEBuild/CoopHealHUD_20261019`。最终 `RegressionFinal/index.json` 为 **134 成功、0 警告、0 失败**，使用实际渲染而非 NullRHI，新增测试明确验证可播放的治疗蒙太奇不再被能力启动，以及实际受治疗者的粒子位置、跟随、缩放与结束清理。`DemoNormal` 使用正式 CombatTest／HUD、DS 加双客户端、无 AI、临时免消耗冷却，两端各录约 40 秒，Owner 356 帧、Observer 355 帧，执行检查通过。最终磁盘模块 SHA256 为 `98fcdcf8a1bad524abee3cd67f28e1ba1cb9b472409c4213d36b89d7a47e0a46`，录像前后未变；未测量进程内已加载模块哈希。

实际抽查 Owner／Observer 第 38 帧可见 Q 防御动作且无护体粒子；双方第 73 帧、Owner 第 76 帧与 Observer 第 147 帧可见 E 的蓝色护体粒子，人物没有治疗起手姿态，五槽标题紧凑居中。该抽查不等于每帧连续观感、音效、弱网、实际治疗数值或完整敌人战斗验收，测试与画面证据分开记录。Owner 第 76 帧有着色器准备提示，不声称冷加载零提示或稳定帧率。自动报告的 `visual_passed=false` 不改写，具体人工图像检查另存 `DemoNormal/VisualInspection.json`。

## 原埃及图恢复后的正式技能验收（2026-10-20）

本轮以恢复原埃及布局的 `/Game/Combat/Maps/CoopEscape` 为对象，原机关位置和引用须单独核对。正式玩家为 `BP_EscapePlayerVariant`，使用独立的 `DA_EscapeCombat` 和 Escape HUD；母配置 `DA_CombatConfig` 的结果不能代替这份实际配置。原型三波路线脚本依赖 `Escape_Key_*`、`Escape_Plate_*`、`Escape_Gate` 和 `Escape_Exit`，恢复原图后应明确报告不适用，不能临时重建这些机关或放宽条件制造全流程通过。`RunLocalDedicatedServer` 不带 `-Play` 的旧自动入口同样不作为恢复原图的完整验收。

当前 Q／E 规则：

- Q 在防御生效期间完全挡住来自角色身体正面的直接攻击，血量和现有护盾均不应减少；侧后方、没有方向的伤害和持续伤害（DoT）不受该格挡保护，照常先扣护盾再扣生命。短时正面投射物弹反仍受原窗口限制；身体正面与镜头方向分开判断。
- Q 保留防御动作与音效，两类粒子配置均为空。原 Q 蓝色护体特效交给 E，出现在实际受治疗者脚底并跟随角色；E 不播放治疗动作。
- E 按住预览、松开确认，右键取消预览；基础规则为消耗 25 能量、冷却 6 秒、治疗 30 生命，搜索半径 6 米。治疗量与冷却可随技能等级变化，数值回归应明确当前等级。
- E 只选择范围内受伤且可行动的友方（包括自己），优先最低血量比例；队友须可见，自疗不做自身遮挡射线。满血且无受伤队友时拒绝，不能以演示模式的满血自疗行为作为正式规则。
- 倒地需要队友按住 F 救援，E 不负责复活；倒地、死亡、未接纳、救援中、眩晕、施法中、未学习、能量不足、冷却或无合法治疗目标均须给出对应失败原因。图标亮度只反映所属角色的资源、冷却和状态，不保证当前存在治疗目标；目标资格在预览和确认时重新判断，满血提示与无可治疗目标提示区分。拒绝请求不扣新能量，也不启动新冷却。

正式技能受控联机入口示例：

~~~powershell
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario Combat -VisualAcceptance -Map /Game/Combat/Maps/CoopEscape -Profiles Normal,Harsh -Repeat 1 -Offscreen -TimeoutSeconds 600 -OutputDirectory 'C:/UEBuild/CoopFormalFix_20261020/CombatVisual'
~~~

该入口使用实际保存的 Escape GameMode、玩家、独立配置和 HUD，不替换它们；不启用 `CoopSkillDemo`、`AbilitySystem.IgnoreCosts` 或 `AbilitySystem.IgnoreCooldowns`。Combat 仍是隔离目标、站位与输入的受控场景，不能把其通过称作恢复原图的机关、导航或完整闯关通过。

本轮需分别核对以下证据：

- 原生格挡回归检查正面直接伤害为零，侧后、无方向与持续伤害照常结算；Q 动作、Q 无粒子和 E 蓝色特效另看实际双端画面。
- `Multiplayer.Combat.EscapeHUDHealFailure` 使用正式 Escape 角色和 HUD，检查满血无目标及能量不足的可见失败反馈、资源图标状态，以及拒绝后资源和冷却不变；另验证无冷却且 HUD 无定时器时，受伤队友进出范围会改变真实治疗资格，图标不会停留在过期的目标灰色状态，不增加全局轮询。此原生用例走确认函数，真实 E 键链另由网络场景验证。
- 网络场景经所属 PlayerController 的 E 按下／松开，分别检查预览不提交、预测扣费、服务器实际回血与单次冷却、拥有者和观察者收到本轮治疗 Cue 及对应截图。自疗链保留真实受伤的 90 生命基线，验证 90→100；能量 55→30，不能仅看到预览或记录输入就算治疗成功。
- 原图机关、平台、钥匙、出口与实际通关仍需按恢复后的原位置和配置单独验证，不能复用旧原型路线的通过记录。

本轮报告根目录为 `C:/UEBuild/CoopFormalFix_20261020`。最终模块 SHA256 为 `28FBF8AA9EC3B2886B3F5A5BFBC46D78C793EB222F6AFA486CA0739D135B4710`，下列结果只对应这次构建，不引用历史通过记录：

- `RegressionVisualFinal/index.json`：140 项通过、0 警告、0 失败。新增 `Multiplayer.Combat.Guard.BlockedHitPresentation` 用真实 Combat/Escape 蓝图核对：正面格挡不损失血盾、不发布伤害 Cue、不创建命中粒子或受击蒙太奇；侧后命中仍保留正常伤害与表现。判定在提交前固定，血量回调中的同步转身不追溯改变本次结果。
- `EscapeCombatNormalVisualFinal/report.json` 与 `EscapeCombatHarshVisualFinal/report.json`：正式 Escape 配置的受控双端 Combat 各 1 轮通过。Harsh 是本机模拟 `PktLag=200`、`PktLagVariance=50`、`PktLoss=5`，不是公网实测或实际 RTT 指标。真实输入、预测拒绝/纠正、单次消耗、队友 40→70、自疗 90→100 均保留严格断言。
- `RestoredMapAuditFinal.log`：独立读取原图和恢复副本，原有 21 个机关/出生点位置不匹配数为 0；包含 4 钥匙、4 插槽、5 压力板、3 平台、2 门、2 出生点、1 终点。原图文件 SHA256 仍为 `E2D04E4905867717252F9552AF9CC43A3EA5BCFECD3DABB0A681901AF321CD6D`。仅副本修复负数触发盒尺寸及 DS 上无本地控制器时的输入初始化，原图文件未回写。
- 人工核对最终 Normal 截图：Q 防御动作不再被本次被格挡攻击的伤害火焰覆盖；后续自疗截图能看见蓝色护体效果。**首次队友治疗 BasicHeal 截图仍有 `Preparing Shaders (10)` 且未见蓝球，首次冷加载特效视觉不登记通过**。测试准备已非阻塞等待真实资源、Niagara/GPU 编译及已提交 shader 任务，但不能据此保证首次绘制的新 shader/PSO，或把组件存在当作像素可见。此次也不登记完整原图通关、连续录像、声音或云服务器验收。

首轮旧血量预期与侧后规则夹具失败报告均保留在该报告根目录，未用重跑覆盖失败证据。地图和配置恢复前的备份见 `Saved/CombatAssetImport/BeforeOriginalLayoutRestore-20261020-003433-505389/RestoreReport.json`；本地输入修复前的副本另保存在报告根目录 `BeforeLocalInputRepair.umap`。

## 历史实验

既有玩法、连接、UMG、DS 冒烟和规模压测报告仍保存在 Docs/Reports、Tests/Evidence 和本机 Saved/NetworkValidation；对应测试源码已从当前工程移除。复现须使用报告对应的历史代码版本，不能拿旧报告宣称当前构建已通过这些回归。
