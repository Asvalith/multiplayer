# Co-op 当前架构与源码阅读顺序

本项目使用一个 UE Runtime 模块 `multiplayer`。下面是同一模块内的功能目录，不是六套框架或六个独立模块。先按状态所有者阅读，再追函数，不以类数量或文件长度衡量设计质量。

## 1. 目录与职责

| 目录 | 唯一职责与主要入口 |
| --- | --- |
| `Core/` | `multiplayerGameMode` 接纳玩家、裁决目标与终局、重开及恢复资格；`multiplayerCoopGameState` 复制共享进度与结果；`multiplayerGameplayConfig` 读取 JSON 规则 |
| `Network/` | `multiplayerGameInstance` 管本地地址连接、取消、超时与有限重试；`multiplayerCombatResumeState.h` 只定义服务器恢复数据，不驱动旅行或技能 |
| `Player/` | `multiplayerCoopPlayerController` 是拥有者 RPC 通道并维护重开/离场操作；`multiplayerCharacter` 接地图角色与基础移动输入 |
| `Mechanisms/` | 各机关拥有自身状态；Occupancy 维护有效玩家占用，门/插槽/撤离区域向 GameMode 提交事实；平台与角色基座交互仍由原生移动系统处理 |
| `Combat/` | Character 协调输入与组件，ASC 接入 GAS，GA 执行技能阶段，GE/AttributeSet 结算数值；成长、倒地救援、敌人和遭遇战各有现有所有者 |
| `Testing/` | 自动化、联网场景、只读资产审计和显式生成工具；它们不是第二套正式玩法，不将测试开关当成正式输入入口 |

现有动画、Cue、HUD/Widget 都在 `Combat/multiplayerCombatPresentation.*`，属于本地表现域；文件按相关职责组织，不额外建立 UI Manager 或为每个短类再建组件。

## 2. 状态归属与运行端

| 状态 | 写入者 | 网络与生命周期边界 |
| --- | --- | --- |
| 连接地址、重试阶段、恢复凭证 | 本机 GameInstance | 跨 World 存活，各端独立；不具有战斗裁决权限 |
| 接纳、钥匙进度提交、胜负、重开、恢复资格 | 服务器 GameMode | 本局权威；客户端通过所属 Controller 请求，不直接调用本地 GameMode 裁决 |
| 共享进度与结果 | 服务器 GameState | 复制给客户端；晚加入/重绑定先补读当前快照，再消费后续事件 |
| 生命、能量、护盾、冷却及持续效果 | Character 的 ASC/AttributeSet/GE | GAS 管复制和预测账本；客户端预览不直接写属性 |
| 单次技能的起手、出手、收招及取消 | 对应 GA 实例 | 预测键识别请求，激活版本区分实例复用轮次，句柄限定清理归属 |
| 门、平台、钥匙与玩家占用 | 对应机关 Actor/Occupancy | 服务器提交事实；客户端播放过渡；同一玩家的多个碰撞体不是多人 |
| 动画、Cue、范围预览和界面缓存 | 本地表现对象 | 不裁决命中、不决定技能成功、不反向修改权威属性 |

ASC 当前属于 Character，而非 PlayerState。断线重新创建角色后，用显式白名单快照恢复稳定状态；不宣称任意 GE 存档或跨服迁移。

## 3. 三条主要调用链

### 连接与恢复

GameInstance 发起旅行 → 当前 Controller 握手 → GameMode 验证票据与恢复条件 → Character 恢复支持的状态 → 成功才接纳、轮换票据 → 客户端等待本端对象就绪并补读 GameState。

恢复只覆盖同服务器、同局、有限保留窗口。扣除离线时间，处理支持的持续效果与倒地超时；不续接半次攻击、冲刺、救援或旧任务。`multiplayerCombatResumeState.h` 是数据合同，不是序列化服务，也不接受客户端上传血量。

### 机关与目标

服务器 Overlap/玩家资格变化 → Occupancy 去重 → 机关检查自身条件 → GameMode 提交目标/终局 → GameState 发布结果 → 客户端状态驱动表现。不同生命周期的连接状态、重开状态与机关状态不合并成一个全局状态机。

### 技能与结算

本地输入/范围预览 → Character 的只读资格查询 → 实际 AbilitySpec 的 CanActivateAbility → 原生预测激活与服务器独立准入 → GA Commit 成本/冷却 → 起手任务 → 服务器重建目标并结算 → EndAbility 释放本轮任务和临时状态。

取消不等于退款，也不回滚已结算伤害；技能已生成的独立持续效果按自身规则结束。界面不另写一套成本/冷却/阻断条件。

## 4. 战斗源码阅读入口

以下文件相对于 `Source/multiplayer/Combat/`：

| 文件 | 负责什么 |
| --- | --- |
| `multiplayerCombatCharacter.h/.cpp` | 角色生命周期、能力输入、属性监听、控制状态及恢复协调 |
| `multiplayerCombatCharacterSkillStatus.cpp` | 同一个 Character 的技能可用性与失败说明；不是新增组件或第二个角色 |
| `multiplayerCombatAbilitySystem.h/.cpp` | 已有 ASC 子类的声明和实现：原生标签关系适配、正式失败反馈、动画认领与恢复窄入口 |
| `multiplayerCombatAbility.h/.cpp` | 公共 GA 约束、具体技能阶段及相关小型默认 GE；不再聚合 ASC 类声明 |
| `multiplayerCombatTargeting.h/.cpp` | 共享几何查询与 Character 预览方法，不提交战斗结果 |
| `multiplayerCombatProjectile.*`、`multiplayerCombatAttributes.*` | 权威弹体命中与最终属性结算 |
| `multiplayerCombatProgression.*`、`multiplayerCombatCoop.*` | 成长/配装与倒地/救援，保留各自已有组件和状态所有者 |
| `multiplayerCombatPresentation.*` | Character 的本地动画/特效方法、Cue、动画实例和事件驱动界面；不额外持有权威数值 |

一个 Character 的方法分在多个 cpp，是实现文件组织，不等于拆出了多个组件。两次发生在不同时点的准入与提交复核，也不是重复结算。

## 5. 配置、蓝图与兼容性

- Combat Data Asset 保存技能参数、标签关系和表现资源；JSON 保存已有机关/实验规则。同一字段只维护一个配置源，不把运行状态写回共享配置。
- C++ 定义规则与唯一提交入口，蓝图派生现有角色/机关/界面并配置资源；蓝图不另行扣费、伤害或判胜。
- 本次整理只调整头文件、依赖与实现文件命名。UClass/枚举名称、`/Script/multiplayer.*` 身份、属性/RPC 与资产父类不变，不需要额外 CoreRedirects、兼容聚合头或批量重存资产。
- GameMode 的头只依赖恢复数据，不再通过完整 CombatCharacter 头间接引入角色、动画与输入声明；使用 Character 或 ASC 的实现应直接包含对应头。

## 6. 验证与边界

当前结果见 [Tests/README](../Tests/README.md)。结构整理不等于新的性能优化、弱网或视觉验收；每轮编译、自动化、静态资产契约和多进程网络测试分别记录，不把历史通过移称为本轮重跑。
