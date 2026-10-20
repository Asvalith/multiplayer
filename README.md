# multiplayer · UE5 C++ 双人 Co-op

基于 Unreal Engine 5.5 和 C++ 的双人合作 Demo，采用 **一个 Dedicated Server + 两个远端客户端**，实现地址连接、钥匙自动归位、压力板与合作门、移动平台、目标进度、双人胜利与重开。

项目重点是服务器权威的玩法规则、可恢复的状态复制、连接生命周期管理，以及通过重复对照和历史 Move 配对分析网络成本与同步误差。

## 当前可玩入口：双人合作撤离

服务器默认进入 `/Game/Combat/Maps/CoopEscape`。这是原埃及场景的独立关卡副本，保留原地图和原 HUD；玩家需要应对三波小兵，收集四把钥匙，再分别踩住两块压力板开门，一起进入出口。消耗过生命或能量时可领取绿色十字补给。波次为3、4、5名小兵，分批出生，场上存活与待出生总量受上限约束；双方正式加入后才开始，单机允许一人预览。

~~~powershell
# 本机 DS + 两个客户端，先显示地址菜单。在两端输入 127.0.0.1:7777 连接。
.\Scripts\RunLocalDedicatedServer.ps1 -Play -FromMenu -Port 7777
# 省略 -FromMenu 可直接连接；关闭两个客户端后脚本会清理自己启动的服务器。
~~~

WASD移动，鼠标观察，左键攻击。Q防御／起手弹反、E治疗、R范围施法、Shift冲刺均按住预览、松开释放，也可在预览中按左键确认；右键取消预览或可中断技能。B打开成长菜单，冲刺需先学习；队友倒地时靠近并按住F救援。胜利或团灭后按Enter重新开始。该启动脚本暂时静音，声音未作为本轮视觉通过条件。

只演示技能切换时，可运行 `./Scripts/RunLocalDedicatedServer.ps1 -Play -SkillDemo -Map /Game/Combat/Maps/CombatTest`：关闭本局出生器，双端免消耗／冷却，使用初始技能点解锁冲刺，并允许满血自疗查看表现。前后摇、取消、控制和有效落点规则仍保留；该模式不是正式平衡或预测消耗验收，不保存演示配置，关闭进程后恢复普通启动规则。

不传 `-Play` 时，启动脚本会运行三进程的完整撤离自动验证并退出，不是留给玩家操作的服务器。平台专项仍通过 `RunMultiplayerNetworkTests.ps1 -Scenario RideMotion -Map /Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo` 单独运行。`CombatTest` 和原机关地图继续保留用于专项复现。

敌人头顶血条为有场景遮挡的世界空间控件，跟随平滑后的身体；血量内容由属性事件更新，专用服务器不创建显示控件。新关卡的导航在静态网格编译和碰撞就绪后重建，避免导航穿过实际木箱；交互物按真实游玩层地面放置，不把钥匙埋入台阶或把出口吸到高处屋顶。

## 项目亮点

- **服务器权威的合作玩法**：服务器处理钥匙触碰与安装、不同玩家计数、机关激活和胜利判定；客户端接收结果并更新表现。GameMode、GameState、PlayerController、HUD 与机关 Actor 分工明确，分别管理连接就绪与玩法状态就绪。
- **状态同步与晚加入恢复**：目标进度和胜利组成一份业务快照；门、压力板和钥匙分别复制必要状态。界面在绑定事件后补读当前结果，GameState 尚未到达时等待就绪事件，避免依赖某一次历史广播。
- **交互一致性与生命周期治理**：共用占用组件处理多碰撞体重叠、控制权变化、Pawn 销毁和触发体重绑定；合作门合并不同玩家集合。钥匙安装采用提交保护和失败恢复，目标通知防同步重入，重开请求经过服务器校验和去重。
- **DS 连接与失败恢复**：GameInstance 管理地址校验、连接期限、取消、有限重连和退出；按 World / NetDriver 过滤全局失败事件，用操作编号拒绝失效流程。某个玩家退出不关闭服务器，重连后重新获取仍在运行的 DS 保存的共享状态。
- **复制成本优化与数据验证**：静止机关采用网络休眠，变化前刷新休眠；移动平台采用受控的更新频率上限。历史 Listen 正式对照中，500 个合成压力板的复制作用域耗时降低约 **89.0%**，20 个合成平台的引擎发送量降低约 **36.2%**；两项分别统计复制 CPU 与引擎发送量。
- **移动基座与弱网定位**：角色复用原生 CharacterMovement；平台维护根组件速度和更新阶段，角色保留空中水平惯性。测试按同一 Move 时间戳关联双端记录，输出基座状态与校正时间线，并通过频率对照验证同步策略的取舍。
- **配置与自动化验证**：玩法参数集中到 JSON，启动时校验并完整提交，失败整表回退。平台专项脚本编排 DS 与两个客户端，覆盖站立、行走、反向和自由跳跃，检查各端动作、退出状态与采样有效性；既有玩法回归与规模对照保留分版本报告。

## GAS 联机战斗

源码阅读与面试讲解见 [GAS 架构与调用链](Docs/Study/GAS架构与调用链.md)，涵盖正式准入、提交、预测取消、伤害结算和短时恢复的实际职责与边界。

`gas-combat` 分支在原有合作角色上接入 GAS，使用独立关卡 `/Game/Combat/Maps/CombatTest` 验证战斗，不替换原机关关卡。

- **左键攻击**：起手约 0.16 秒后由服务器生成能量弹，随后收招约 0.37 秒；命中敌人造成 25 点伤害，冷却 0.5 秒，队友不会受到伤害。
- **Q 防御**：消耗 20 能量，持续 3 秒、冷却 8 秒；保持身体正面，完全格挡来自前方的直接伤害，侧后方、无方向范围伤害和周期伤害仍正常结算。所属客户端先预测消耗、冷却和防御效果，服务器确认或拒绝后由 GAS 核对结果。防御使用对应姿势，不播放粒子特效。
- **Q 起手弹反**：起手约 0.3 秒内，服务器复核身体正前方的入射弹道，将反射后的攻击归属切换给防御者。弹体只在轨迹改变时更新版本、起点、速度和时间，最多反射两次，保留最初的总寿命；背后、过窗和友方弹体不反射。
- **E 治疗**：消耗 25 能量，治疗 600cm 内最低血量比例的受伤友军或自己 30 点，冷却 6 秒；没有合法目标时不消耗，不能穿墙治疗。治疗不播放动作，蓝色环绕特效落在实际治疗目标上。
- **R 范围施法 / 右键取消**：先沿镜头选择可行走地面，最大水平施法距离 650cm；确认后消耗 30 能量，起手约 0.32 秒后，对落点半径 220cm、中心高度差不超过 200cm 且无遮挡的敌人结算一次 35 点伤害与 1.5 秒眩晕，再播放约 1.11 秒释放与收招动作；冷却 5 秒。预览与结算共用候选查询、半径和遮挡规则，确认后锁定本轮中心。起手提交资源与冷却，取消不退款；只取消预览不扣费，已结算的伤害不会倒退。
- **Shift 冲刺**：先在 B 菜单花 1 点学习，再使用第五槽位。默认消耗 15 能量、水平移动 450cm／0.35 秒、冷却 3 秒；本地立即响应，位移通过原生 GAS RootMotionSource 交给 CMC 处理碰撞与纠正。服务器拒绝、禁锢、眩晕、死亡和退出均结束本次任务，不保留推进；右键也可取消，已提交消耗不退款。
- **F 引导救援**：有仍能行动的队友时，玩家致死后进入 20 秒倒地，而不是立即死亡；倒地不参与机关占用、攻击或普通治疗。队友在 250cm 内按住 F 引导 2 秒，复核距离与遮挡，成功恢复 30% 生命，不返还能量与冷却。松键、受伤、眩晕、目标失效或退出会结束引导；同一目标仅一人占用，完成只结算一次。无人可以行动时判负，终局可由 Controller 的 Enter 请求重开。
- **B 成长与配装**：原四技能保持一级可用，冲刺需要学习；五个输入槽可交换装配。升级技能消耗技能点，人物等级须达到目标技能等级；主动最高三级，伤害每额外一级增加 20%，冷却按统一公式缩短。击杀奖励由服务器发给有效击杀来源，使用稳定死亡标识去重；人物升级增加技能点与真实属性。坚韧、强攻两个被动分别增加生命上限与攻击，学习和装配分离，一次只装一个，卸下只撤销自己的 GE。
- **F5 保存 / F9 读取**：也可在 B 菜单操作。版本化存档只保存成长、技能和配装，不保存本局生命、能量、位置、冷却或半次动作；加载失败不覆盖当前状态。单机使用固定本地槽，联机仅由服务器按签发的 ProfileId 访问自己的档案，不接受客户端路径或经验。匿名直连未接账号认证，不能据此声称服务器重启后自动识别原玩家或跨服务器恢复。
- **服务器维护战斗状态**：GAS 管理属性、消耗、冷却和状态标签；玩家与敌人共用伤害和死亡处理。死亡后取消技能、停止移动并退出机关占用，敌人额外停止 AI 并清理尸体。
- **观察与角色转向**：存活时镜头独立，CMC 在移动时将身体转向行进方向，普通攻击接纳后按本次瞄准方向水平转身；防御保持身体正面，范围攻击锁定落点但不强制随镜头转身。死亡后停用 Pawn 的移动、技能和观察输入，保留 Controller 的界面输入；不每帧固定尸体坐标或旋转。
- **行为树敌人**：服务器默认分批生成一波 3 个敌人；联机等首位玩家完成会话接纳后启动，单机启动即生成第一只，其余每只重新随机等待 1～2.5 秒后生成。出生器销毁时取消余下任务，重复调用或其他玩家加入、重连不会重复开波。服务器通过 Sight 感知玩家，行为树与黑板负责巡逻、追击、搜索和攻击请求；原生 MoveTo 通过 NavMesh 和 CMC 移动，服务器 CMC 启用 RVO 局部避让，技能仍交给 GAS。目标死亡、退出或失去控制后清除目标，敌人死亡时停止行为树、感知、寻路和 RVO。
- **近战与远程小兵**：默认前两只沿用实际蓝图的 `DA_EnemyCombatConfig` 和近战技能：攻击距离 160cm、伤害 15、冷却 1.8 秒；起手约 0.34 秒后复核距离、前向和遮挡，只结算一次，再完成约 1.32 秒收招。第三只在出生前选用本实例的远程参数，射程 1100cm，发射现有可弹反投射物，伤害与节奏不变，不修改共享资产。两类仅授予自己的攻击，不授予治疗、防御或范围技能；每只默认奖励 50 经验，两次有效击杀可升二级。眩晕、取消和死亡结束本次任务与移动限制。两类复用同一小兵外观、移动和挥击动作；长柄武器使用 `FengMao_Weapon` 与源骨架的 `FengMaoWeapon_Active` 握持挂点，仅作表现，不用武器碰撞重复结算伤害。可关闭出生器的远程选项恢复全近战。
- **受控与恢复**：眩晕使用单份限时效果，再次施加刷新时长；禁锢按独立 GameplayEffect 计数，多层限制全部解除后才恢复原移动模式。眩晕停止决策并取消技能，禁锢只停止移动与寻路，仍允许射程内攻击。死亡清理真实冷却与控制效果，不仅隐藏界面。
- **配置与动作**：角色实际引用的 CombatConfig 数据资产集中技能规则、特效和音效；正式撤离玩家使用独立的 `DA_EscapeCombat`，专项关卡使用 `DA_CombatConfig`。玩家动作重定向到 Quinn；敌人使用 Advanced ARPG 包的完整 `SKM_CombatMannequin`，复用原骨架、PhysicsAsset 和动画后处理，动作使用本项目副本。薄原生 AnimInstance 读取实际速度和方向；行为树、GAS 和 CMC 仍负责决策、战斗及移动。玩家动画副本移除不兼容的模板足部 IK，避免静止辅助骨将脚锁住；原 Co-op 动画模板不变。攻击、范围施法、受击和死亡使用 Montage，防御使用对应姿势，治疗不播放动作；玩家保留上半身动作层与完整死亡动作，敌人通过全身 Slot 播放。接入内容位于 `Content/Combat/Presentation/ARPG`，不运行素材包的技能或伤害框架。
- **特效与 HUD**：GameplayCue 仅负责音画，弹体自行管理尾迹；已连接 ARPG 包的 Cascade 火球起手、弹体、命中和治疗效果，并保留 Niagara 配置兼容。命中与治疗使用源包声音，其他部分音效仍为占位。沿用原 `Content/Combat/UI/BP_CombatHUD`，原 `WBP_CombatHUD` 只追加 `ARPGCombatVisuals` 子控件，保留原事件图、绑定和准星；生命、能量条和五个技能提示沿用当前布局，不加面板灰底，保留名称、按键和冷却反馈。子控件通过事件更新状态，活动冷却与读条期间才启用短时 Timer。

R 起手的地面范围标记改用 `M_CombatAreaMarker` 静态贴花，读取施法上下文的命中位置与地面法线，半径沿用 220cm；取消、眩晕、死亡或施法结束时清理。不再把源包的追踪毒矛特效当作落点圈，释放后的 `NS_Posion_Magic_Area1` 只作装饰，不代表精确范围；伤害、消耗和冷却规则不变。

第三方动画与特效素材不随源码分发。`Content/Combat` 的表现资产仍引用本地 `Content/AdvancedARPGCombat`、`Content/Mage` 和 `Content/Pack_Effects_2`；复现完整画面需自行取得素材授权并按原路径迁入。原始压缩包、预览视频及烹饪生成日志也不提交。

~~~powershell
.\Scripts\RunLocalDedicatedServer.ps1 -Play -Map /Game/Combat/Maps/CombatTest -Port 7777
~~~

从地址菜单开始验证双人连接时，在上述命令追加 `-FromMenu`；两个客户端先进入原连接界面，分别填写 `127.0.0.1:7777` 后连接同一 DS。省略该开关仍保留原来的直接连接方式。

连接菜单使用仅界面输入模式；实际战斗 HUD 在开始播放时恢复游戏输入并隐藏菜单光标，再继续原控件初始化，避免换地图后仍由视口拦截 WASD 和鼠标。该切换只在界面进场时发生，不在属性刷新或 Tick 中反复抢焦点。

五项主动技能均使用 LocalPredicted：所属客户端预测自身消耗、冷却、施法与防御表现；请求经过所属角色的 ASC，服务器检查存活、接纳、控制状态、资源、冷却与合法目标。接受或拒绝后由 GAS 的 PredictionKey 接管或撤销预测效果。范围技能只局部清理同次预测键的纯施法状态，不向客户端开放资源、冷却或伤害效果的任意移除。救援另为 ServerOnly 引导；客户端不预测目标扣血、治疗回血或权威投射物生成，不提供世界回滚。角色移动仍使用原生 CMC，UMG 蓝图管理布局，展示层负责订阅、初始化与退出解绑。

普通攻击、防御、R 与冲刺将确认时的方向随 GAS 事件激活一起提交，共用原激活预测键，不依赖移动包先更新服务器朝向；起手后转头不会改写这一轮方向。服务器只读取方向意图，发射起点、射程、伤害与命中对象仍由服务器决定，缺失或非法瞄准数据会被拒绝。第三人称相机原点按同一春臂配置重建，实际弹源收敛到准星射线，不发送客户端命中结果；显式短射程也限制真实弹体寿命。非实体机关触发盒不遮挡 R 的落点或伤害。出生重叠使用服务器弹体位置补齐命中上下文，避免特效落到世界原点。

瞄准与判定集中在 `CombatTargeting`：普攻和 Q 沿用原准星、技能提示与实际动作，不叠加调试射线和扇区；E只标记最低血量比例的合法治疗对象，并用文字说明三维范围；R显示落点伤害边界、射程与候选人数；冲刺只显示胶囊可达终点与方向。静态障碍前的冲刺使用同一可达距离，不持续推墙后滑向另一终点；移动障碍和目标仍在执行时由服务器复核。近战小兵起手单独显示红色扇区，不复用玩家R落点圈，也不把锁定单目标攻击描述成群伤。

`CombatSkillStatus` 通过真实能力的 `CanActivateAbility` 查询资格并统一失败提示，不再复制成本、冷却和关系规则。角色只补连接接纳、生命、学习与实际移动模式等业务前提；标签关系、能量与冷却分别接入 GAS 原生检查，治疗目标资格留在治疗能力。提示可区分未学习、无受伤队友、能量不足、冷却剩余秒数、无地面及路径阻挡；服务器拒绝会将权威原因与资源／冷却值只发给拥有者，不用客户端旧预测余额代替服务器结果。预览属于本地表现，不复制、不扣费、不创建预测键；GAS仍负责确认后的实际技能执行。

能量及最大能量、成长与装配快照仅复制给所属连接，血量、护盾、死亡、倒地与公开状态供其他玩家观察。能量弹复制最新轨迹段的位置、速度、服务器发射／截止时间和版本，初次相关或弹反时更新；客户端按估算的服务器时钟重建直线段，服务器继续执行扫掠、伤害与销毁，不恢复逐帧位置流。此策略针对无重力、分段恒速的当前弹道，不支持任意追踪曲线；估算时钟和销毁通知仍有网络延迟，不承诺双端实时零误差。

断线恢复使用本局服务器签发的短时票据。GameInstance 负责有限重试，所属 Controller 完成握手，GameMode 从服务器快照恢复血量、能量、位置、死亡及剩余冷却、防御和眩晕；离线时间正常扣除，不恢复半次施法，也不返还已付资源。票据缓存保留 120 秒，票据不进入地址或日志，在线会话不能被同票据重复领取；恢复回调失败会归还旧票据。这是匿名直连 Demo 的会话恢复，不是账号认证：丢弃票据仍可作为新访客加入。

战斗扩展增加可消耗护盾、周期灼烧和独立禁锢。伤害先计算攻击与护甲，再判断正面直接伤害格挡；未被格挡的部分先扣护盾，只有溢出部分扣生命，灼烧不被正面格挡免除。同一来源灼烧刷新寿命但不重置下一跳，不同来源分别保留。重连白名单保存每份灼烧／禁锢的原始效果与剩余时间，离线灼烧继续结算并消耗已存护盾，不因重连刷新效果；致死后恢复死亡状态，不补资源或重放未完成动作。此处只覆盖这些明确支持的效果，不是任意 GameplayEffect 或完整世界存档。

新增规则由各角色的 CombatConfig 控制：`DefenseShieldAmount` 为 Q 补盾量，`BurningDamagePerTick`／`BurningDuration` 为 R 灼烧，`AreaRootDuration` 为 R 禁锢。三个增量数值默认 0，未覆盖原玩家与小兵资产的平衡参数；非零配置接入现有 Q／R 结算，相关集成测试只使用内存配置副本。原 HUD 继续由事件刷新，状态文字可同时显示护盾、眩晕、禁锢与灼烧；没有新增独立轮询 HUD 或替换原界面。

后续阶段已经补入冲刺预测与中断、弹反、技能学习／装配、被动成长、倒地救援与长期存档，代码分别集中在 Mobility、Projectile、Progression、Coop 与既有 Presentation 中。原 HUD 和蓝图布局保留，B 菜单是附加层，按事件刷新和退出解绑。短时重连还恢复成长配装、冲刺剩余冷却和倒地期限，不重新授予重复被动，不续接断线前的冲刺或救援。这些是明确业务的预测与恢复，不是任意技能或完整世界的通用回滚框架。

当前扩展批次 `Saved/CombatComplete-20261017` 保留 **87 项功能回归通过、0 警告、0 失败**，独立真实地图 AI 检查通过，编辑器与 Windows Development 游戏目标均编译通过。独立 DS＋双客户端的战斗、技能生命周期、扩展技能、重连四类场景，在 Normal／Harsh 各一轮通过，共8轮。末轮修复的是旧战斗测试未隔离地图小兵的干扰，保留失败报告和更严格的准备／治疗断言，没有更改治疗规则；不同测试模块及业务源码未变的核对口径见 [本轮记录](Tests/README.md#联机战斗扩展闭环2026-10-17)。这些是自动状态检查，不是新动作与双端美术衔接的人工画面验收。

此前护盾阶段 `Saved/CombatDepth-20261017` 的61项回归和两类网络记录继续保留为对应历史版本，不与当前结果混算。[历史测试说明](Tests/README.md#护盾持续效果与重连恢复2026-10-17)。

2026-10-17 命中修复阶段的 DX12 离屏 `Multiplayer.Combat.` 回归为 **36 项通过、0 警告、0 失败**。保留护盾定位、敌人事件血条和双脚迈动检查，新增方向快照、非法瞄准数据、高速扫掠、近身命中位置及触发盒遮挡边界。对应构建的 DS 双客户端 `Combat`、`CombatLifecycle` 在 Normal／Harsh 各一轮通过，共四轮；网络记录按对应构建分阶段保留，自动断言不等于完整视觉验收。[记录与边界](Tests/README.md)。技能与效果集中在 Ability 源码，敌人决策与生成集中在 Enemy 源码，音画与界面订阅集中在 Presentation 源码；不在行为树或界面里另写一套技能结算。

同日换用完整 ARPG 敌人后，DX12 离屏回归 **37 项全部通过**，真实关卡 `MapAI` 通过。新增检查实际 CMC 位移、左右足骨摆动、原动画后处理，以及攻击／受击／死亡 Montage 的播放与推进；敌人血条仍由 GAS 属性事件驱动。该模型版本的独立 DS 双客户端 `CombatLifecycle` 在 Normal、Harsh 各一轮通过。

2026-10-15 三技能版本的固定负载对照使用同机 DS + 两个客户端、NullRHI、Normal 网络：预热 3 秒，采样 15 秒，30 发真实能量弹与 60 次能量更新，前后各取两个有效样本。与既有 30Hz 位置复制版本相比，初始发射状态方案的服务器总出站字节率中位数由 **6,603.85 降至 3,083.97 B/s（约 53.3%）**，实测服务器帧率为 **30.03 → 29.94 FPS**，包率为 **60.06 → 59.88 包/秒**。计数包含两条连接的协议、玩法及测试流量，不是单独弹体流量，也不代表新增表现后的带宽；不据此推导 CPU、人数容量或视觉平滑度。[样本与测量口径](Tests/Evidence/GASProjectileTrajectory.json)

行为树资产为 `Content/Combat/AI/BT_CombatEnemy`，黑板为 `BB_CombatEnemy`。黑板保存当前目标、可否攻击、最后可见位置、搜索截止时间、出生点与巡逻落点；不在敌人类、行为树和 GAS 中各存一套目标或冷却。

树的优先级为：**攻击 → 追击 → 搜索最后可见位置 → 巡逻**。Sight 默认发现距离 15m、单侧视野 75°，遮挡会阻断视线；看见玩家后追击，进入射程且无遮挡才请求 GAS 攻击。丢失目标后只前往最后可见位置，最后一次看见满 5 秒后结束搜索；平时在出生点附近 700cm 内选择导航可达点巡逻，到点停留再选下一点，选点或寻路失败也等待后重试。黑板条件变化会中止旧分支；决策只在服务器执行，GAS 负责消耗、冷却、命中与死亡，CMC 负责移动，客户端显示复制结果。

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
- **成功确认**：本地 PlayerController 进入 `PlayingState` 后发起所属会话握手，服务器接纳并完成恢复的回执到达后才通知 GameInstance；不把地图加载、收到 Pawn 或发出 Travel 当作连接完成。
- **取消与回调重入**：先提交连接状态，再通知监听者；后续 Travel 核对操作编号，防止通知过程中退出后，旧流程仍继续连接。连接期限回调检查操作编号，重连回调检查连接阶段。
- **全局事件归属**：网络失败按 World / GameInstance / NetDriver 过滤，避免同进程其他实例的错误污染本连接；服务端 Travel 失败转交 GameMode 恢复重开流程。
- **有限重连**：默认按 1、2、4 秒等待后重试，成功清理旧计时器，主动退出取消重试并清理该地址的个人票据。短时断线重新连接后恢复本局服务器保存的个人快照，并重新接收共享进度和机关状态；整体回合重开使用新回合状态。
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
| 敌人生命周期、行为树节点与刷怪 | [CombatEnemy](Source/multiplayer/Combat/multiplayerCombatEnemy.cpp) |
| 所属玩家 RPC 与操作状态 | [CoopPlayerController](Source/multiplayer/Player/multiplayerCoopPlayerController.cpp) |
| 钥匙触碰、插槽接收与安装提交 | [CoopKey](Source/multiplayer/Mechanisms/multiplayerCoopKey.cpp)（保留钥匙和插槽两个 Actor，源码就近组织） |
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

# 无画面双人撤离验证，完成玩法断言后自动退出。
.\Scripts\RunLocalDedicatedServer.ps1
~~~

默认客户端菜单为 `/Game/UI/DSMenu`，游戏地图为 `/Game/Combat/Maps/CoopEscape`；原 Egypt 地图仍保留。菜单输入 `127.0.0.1:7777` 连接本机；跨机器使用服务器局域网地址。脚本支持 `-EditorPath` 覆盖引擎路径，批处理支持 `UE_EDITOR` 环境变量。

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
