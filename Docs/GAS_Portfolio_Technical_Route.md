# Co-op GAS Portfolio V1 TODO 与封板标准

> 适用分支：`coop-GAS`
>
> 冻结日期：2026-08-16
>
> 文档职责：唯一任务状态、执行顺序、完成门禁、证据链接和排除项。
>
> 不负责：解释架构或复制面试答案。技术解释见[面试复习资料](GAS_Interview_Completed_Snapshot.md)，项目入口见[README](../README.md)。

## 1. 冻结目标

第一次作品集封板固定为：

```text
3 个正式技能：Damage / Ally Heal / Immunity
+ 1 个正式 BP_CoopGuardianEnemy
+ 1 条双人合作单局闭环
+ Prediction Accept / Activation Reject / Target Reject
+ 0 / 150 / 300ms 与 5% loss
+ 单 Hitscan 有限服务器回溯
+ Dedicated Server + 2 Clients
+ 一个晚加入检查点 + Restart Travel 生命周期
+ 一次 Network Insights 前后对比
+ Development Package、README、证据和 3～5 分钟视频
```

封板目标是“少量功能做深并取得证据”，不是继续扩展技能、敌人和框架数量。

状态只允许：`未开始`、`进行中`、`待人工验收`、`完成`、`阻塞`、`排除`。

## 2. 已冻结完成，禁止重新实现

| ID | 状态 | 已有能力 | 后续允许动作 |
|---|---|---|---|
| LOCK-M0 | 完成 | 双窗口权威伤害与 TargetDummy 基线 | 最终整局冒烟，不再创建第二套伤害基线 |
| LOCK-M1 | 完成 | Enhanced Input、InputTag、AbilitySet、InitStats、基础 HUD 代码 | 只补正式资产/视觉和当前版本回归 |
| LOCK-M2 | 完成 | TeamId、友军保护、本地候选与服务器权威目标链 | Ally Heal 复用同一团队/目标规则 |
| LOCK-M3 | 完成 | 死亡幂等、瞬态状态清理、3 秒复活、ASC Avatar 重绑 | 补 Montage/Travel/Dedicated 生命周期证据 |
| LOCK-M4 | 完成 | 九项属性、ExecCalc、Source Snapshot/Target Live、自定义 Context、Vulnerability | 不创建第二套属性或伤害公式 |
| LOCK-M5 | 完成 | 七个业务 Cue 的预测/确认/生命周期技术链 | 用正式资产替换占位表现，不双设同 Tag 所有者 |
| LOCK-M6 | 完成 | 真 Prediction Reject、Pending 清理、DamageIntent、ShotId、服务器当前世界 Sweep | 补最终可见矩阵、Dedicated 和回溯 |
| FREEZE-01 | 完成 | Content 重复副本已移出正式路径；正式地图加载无丢引用 | 不提交 Saved 备份或再建同名副本 |
| FREEZE-02 | 完成 | 旧 LAN 教学入口删除、KeySocket 提交统一、服务器目标查询单一所有者 | 只有稳定复现或性能数据才重开实现 |
| BUILD-01 | 完成 | 清理批次 Editor/Game、Blueprint Compile、`multiplayer.GAS` 与 M6Intent 回归 | 后续每个阶段仍需增量门禁 |

规则：已有代码但缺证据，只新增测试、日志或资产验收；禁止创建第二套 ASC、AttributeSet、Ability、EffectContext、TargetData、HUD Presenter 或服务器裁决链。

## 3. V1 唯一任务表

### 3.1 正式内容闭环

| ID | 状态 | 类型 | 任务 | 完成门禁 |
|---|---|---|---|---|
| UI-01 | 未开始 | 蓝图 | 正式胜利 UI | 只编辑正式 Character；`On Coop Game Won` 创建/复用 `winandquit`；GameAndUI+鼠标；中文重开调用 `Request Restart Coop Game`；退出使用 Owning Player；Host/Client 各显示一次 |
| GAS-ALLY-01 | 待人工验收 | C++ + 资产 | 将现有 Heal 原位改为 Ally Heal | C++/UHT/契约自动化已完成：本地候选只提交 Actor，服务器复验 Team/Alive/Range/LOS/ASC，权威 GE 结算，Target Reject 单独回执，Task/Timer/Delegate 清理；待正式准星表现、双窗口接受/拒绝与弱网验收 |
| ENEMY-01 | 待人工验收 | C++ | Guardian GAS 基座 | C++/UHT/契约自动化已完成：独立基础 ASC/Attribute/Team、服务器计时 AIController、Acquire/Chase/Leash/Windup/Attack/Cooldown/Dead、GA/GE 近战、持续护盾 GE、奖励幂等与清理；待 EnemyBP 双窗口行为验收 |
| ENEMY-BP-01 | 未开始 | 蓝图/关卡 | `BP_CoopGuardianEnemy` | Mesh、AnimBP、Enemy AbilitySet/属性 GE（可覆盖 C++ 安全默认）、C++ AIController、NavMesh、碰撞和正式地图实例；调用 Register/Unregister Channeling；默认一人持续引导破盾、另一人输出，离开恢复 Shield；蓝图不结算伤害/护盾/奖励 |
| HUD-01 | 未开始 | UMG | 正式战斗信息 | Health、Energy、三技能 Cooldown、Immunity/Vulnerability、Guardian Health/Shield；Widget 只读 ASC/PlayerState，不维护第二份权威状态 |

### 3.2 动画与 GameplayCue

| ID | 状态 | 类型 | 任务 | 完成门禁 |
|---|---|---|---|---|
| ANIM-01 | 进行中 | C++ + 动画资产 | 玩家预测 Montage | C++ 已完成统一 Montage 配置、ASC 播放、Predicted/Authority/Reconciled/Rejected/Completed/Cancelled 单一表现事件，以及 Damage Target Reject 停止；待为正式 Ability 赋 Montage 并验证 Accept 不双播、Reject/死亡/Travel 停止；Notify 永不驱动权威结算 |
| ANIM-02 | 未开始 | 动画资产 | Guardian 最小动画 | Locomotion 复用；一条 Melee Montage、一条 HitReact；死亡可先用 Ragdoll；服务器攻击阶段与动画表现解耦 |
| CUE-01 | 未开始 | GameplayCue/Niagara/音频 | 最小正式表现包 | Damage/Heal Cast、Normal/Critical Impact、Immunity/Vulnerability/Channeling Aura、Guardian Shield、Telegraph/Death；Critical 只读权威 Context；持续 Cue Removed/死亡/Travel 无残留 |
| CUE-OWNER-01 | 进行中 | C++ + 资产 | 单一 Cue 所有者 | C++ 已提供 `NativeDebugFallback / GameplayCueAssets` 显式所有者开关，正式 Tag 在资产模式不再被本地点光消费，Prediction.Pending 保持独立调试；待创建 CueNotify 资产并完成可见验收 |

### 3.3 网络预测深度

| ID | 状态 | 类型 | 任务 | 完成门禁 |
|---|---|---|---|---|
| NET-MATRIX-01 | 未开始 | 脚本/运行 | 最终 Accept/Reject/TargetReject 矩阵 | Host 和 Remote Client 都覆盖；0ms、约150ms RTT、约300ms RTT、300ms+5% loss；记录 Input/Predict/Server/Reject/CatchUp/Final；验证 Cost/CD/Montage/Cue/Attribute |
| NET-LIFE-01 | 未开始 | C++ + 运行 | 死亡和 Restart Travel 清理 | 持续 Cue/技能中死亡；Travel 前取消 Ability/Task/Montage；新 World 重绑 ASC/UI；连续 10 次 Restart 后 Ability、Timer、Delegate、Widget 不增长 |
| SSR-01 | 未开始 | C++ | 单个 Damage Hitscan 有限回溯 | 20～30Hz 保存约500ms Capsule/关键 HitBox 历史；不移动真实 Actor；复用 DamageIntent/ShotId；当前静态遮挡；超窗/未来/重复/死亡目标拒绝 |
| SSR-EVIDENCE-01 | 未开始 | 运行/数据 | 回溯 A/B | 移动 Guardian，SSR 开/关同条件至少100发；0/150/300ms与5% loss；保存命中率、原始日志和录像；明确不覆盖投射物、近战或全世界回滚 |
| DS-01 | 未开始 | C++ Target + 脚本 | Dedicated + 2 Client | Server/Client Target；服务器无 Viewport/Widget/LocalPlayer/本地音效依赖；真实客户端 ASC 发起三技能、Guardian、死亡、机关、胜利与 Restart；连续至少10轮 |
| JOIN-01 | 未开始 | C++ + 运行 | 一个代表性晚加入检查点 | Vulnerability/Immunity 或 Guardian Shield 激活且钥匙部分完成时加入；恢复 Attribute、Active GE/Stack/Tag、Guardian、Objective；不重播旧瞬时 Impact/Montage |
| INSIGHTS-01 | 未开始 | 工具/性能 | 一次 Network Insights 优化 | 固定 Dedicated+2C+测试 GAS AI、预热10秒、采样60秒、至少3轮；记录 RPC/字节、Actor Channel、GE/Tag/Cue、GameThread/NetTick P50/P95/P99；只改一个真实瓶颈并同条件复测 |

### 3.4 发布与面试交付

| ID | 状态 | 类型 | 任务 | 完成门禁 |
|---|---|---|---|---|
| RUN-01 | 未开始 | 人工运行 | 可见双窗口整局 | Host/Join→三技能→Guardian双人机制→四钥匙/门/平台→两人胜利→两端UI→Client重开→Travel后输入恢复→退出；无 Fatal/Ensure |
| PACKAGE-01 | 未开始 | Build/Cook | Development Package | 可审计 Cook/Stage/Pak；两个打包客户端连接并完成短流程；地图、GAS资产、动画、Cue、UI无缺失 |
| DOC-01 | 完成 | 文档 | 三份主文档 | README、面试复习资料、TODO 各自职责唯一；其他长文已标记归档；Evidence 不承担当前状态 |
| EVIDENCE-01 | 未开始 | 证据 | 封板证据包 | 每项绑定 Commit、BuildId、RunId、拓扑、网络参数、命令、日志/JSON/Trace、截图或视频；历史证据不覆盖 |
| VIDEO-01 | 未开始 | 演示 | 3～5 分钟视频 | 玩法目标、Guardian协作、300ms Reject、ExecCalc/Context、SSR A/B、Dedicated晚加入、Insights前后、遗留边界 |

## 4. 最少返工执行顺序

| 顺序 | 集中批次 | 包含任务 | 依赖 | 预计有效工作日 |
|---:|---|---|---|---:|
| 1 | UI 与契约冻结 | UI-01、确认正式资产路径、Cue/Montage/Tag 命名 | 已完成基线 | 1～2 |
| 2 | 合作技能 C++ 已完成，补资产/运行 | GAS-ALLY-01、HUD-01 对应字段 | 输入/Team/ASC | 1～2 |
| 3 | Guardian C++ 已完成，补运行 | ENEMY-01 | Team、GE、死亡链 | 1～2 |
| 4 | Guardian 蓝图与表现 | ENEMY-BP-01、ANIM-02、Guardian 部分 CUE-01 | Guardian C++ 接口冻结 | 3～5 |
| 5 | 玩家动画与 Cue | ANIM-01、CUE-01、CUE-OWNER-01 | 三技能接口冻结 | 4～7 |
| 6 | 最终弱网矩阵 | NET-MATRIX-01、NET-LIFE-01 | 正式表现完成 | 3～5 |
| 7 | 有限回溯 | SSR-01、SSR-EVIDENCE-01 | 移动 Guardian | 6～9 |
| 8 | Dedicated/晚加入 | DS-01、JOIN-01 | 功能冻结 | 8～12 |
| 9 | 性能与发布 | INSIGHTS-01、RUN-01、PACKAGE-01 | 网络矩阵通过 | 7～10 |
| 10 | 证据封板 | DOC-01、EVIDENCE-01、VIDEO-01 | 打包整局通过 | 2～4 |

剩余总量约 **29～45 个有效工作日**：全职约 6～9 周；每天约 4 小时约 3～5 个月。资产适配、Dedicated 构建环境或 UE 二进制接线异常不通过压缩测试时间解决。

## 5. 每批统一门禁

任务只有同时满足 Implementation 与 Evidence 才能标为完成：

| 层级 | 最低门禁 |
|---|---|
| C++ | Editor/Game；涉及 Dedicated 时还需 Server/Client Target 构建 |
| Blueprint | 相关资产 Compile/Save，0 error/0 warning/0 failed load |
| Automation | `multiplayer.GAS` 与该批专用单测/功能断言通过 |
| Runtime | 规定拓扑、网络参数和角色方向均覆盖 |
| Lifecycle | EndAbility/Death/Travel/EndPlay 后 Task、Timer、Delegate、Cue、Widget 无残留 |
| Evidence | 新 RunId、Commit、命令、日志、JSON/Trace、结果与遗留问题 |

“编译通过”不能替代运行；Headless 不能替代 UMG/动画/VFX；单次通过不能替代规定轮数；没有执行的项必须写“待验证”。

## 6. Post-V1，不阻塞封板

- 同服断线重连与 30～60 秒状态宽限。
- Seamless 跨地图状态保留。
- Energy 自动恢复和通用 Activation Group/Tag Relationship 框架。
- Striker/Supporter、多敌人、EQS、群体 Attack Token。
- 更多玩家技能、装备、等级、存档和 Buff 离线恢复。
- 10% loss、jitter、reorder 扩展统计。
- Dormancy 第二项优化以及 Replication Graph/Iris 隔离 A/B。
- Listen Server Host Migration。

这些内容可以作为面试场景题或 V1.1 计划，但在取得实现与证据前不能写成项目成果。

## 7. 明确排除

- 商业级反作弊、EAC、客户端完整性检测、录像审计、后端风控和封禁。
- 全世界 Lockstep、确定性世界回滚、投射物/近战通用回滚。
- Aura/Lyra 全量迁移、GameFeature、MVC、大型背包、随机词缀、技能树。
- MMO 后端、跨服、后端容灾和跨机器状态复制。

服务器 Authority、Team、Cost/Cooldown、距离/LOS、ShotId、时间窗和重复请求校验仍必须保留；它们首先是网络正确性，不是商业反作弊产品。

## 8. 更新规则

每次只执行以下事务：

1. 开工时将一个任务改为`进行中`，禁止同时打开多个高度耦合阶段。
2. 实现完成后先构建、运行并生成新 Evidence。
3. Evidence 记录 Commit、BuildId、RunId、命令和实际网络参数。
4. 所有门禁通过后才把本文件状态改为`完成`并附 Evidence 链接。
5. 只有设计含义变化才修改面试复习资料。
6. 只有形成新的可展示版本才修改 README。
7. 其他历史长文不得再写当前状态、下一步或完成百分比。

新会话无需重新扫描全部文档：先读 README，再读本 TODO；只有需要解释或核验证据时才打开面试资料和对应 Evidence。
