# UE5 双人合作闯关 · GAS 战斗与状态恢复

个人项目，基于 UE5.5、C++ 与蓝图实现双人联机合作：钥匙安装、压力板合作门、移动平台、波次战斗、倒地救援与撤离结算。当前使用独立 Dedicated Server 和两个客户端，重点实践服务器权威、GAS 技能生命周期及断线后的状态恢复。

> `main` 为源码展示快照，不是开箱即玩的资源包。源码取自 `7477ca9`；地图、蓝图、模型、动画等 Content 不在当前快照分发，测试数据来自具备授权资源的完整开发环境。[公开范围](Docs/SHOWCASE_SCOPE.md)与[资源依赖](Docs/ASSET_REQUIREMENTS.md)说明可审阅和可运行的边界。

## 三条实现主线

1. **协作机关**：服务器维护机关与目标进度，客户端消费复制状态。占用关系按不同有效玩家统计，避免多碰撞体重复计数；静止机关休眠，变化前唤醒，晚加入补读当前状态，不依赖历史广播。
2. **GAS 战斗**：普攻、防御/弹反、治疗、范围控制、冲刺及救援统一走能力生命周期。配置中的标签关系接入原生阻断/取消；预览复用真实 CanActivateAbility，能量消耗共用成本 GE，服务器重建目标与遮挡并结算。
3. **预测、取消与恢复**：客户端及时预测自身响应，服务器独立准入；用激活版本、预测键、效果/任务句柄区分本轮执行归属，避免旧回调清掉新技能。重连使用服务器票据和有限快照恢复支持的状态，扣除离线时间，不续接半次攻击或回滚已结算伤害。

## 架构与数据

- C++ 管规则和状态，蓝图接动画、特效与 UMG；客户端范围预览不成为权威命中名单。
- GameMode 管对局、接纳和恢复；GameState 复制共享事实；GameInstance 管地址连接、取消和有限重试；机关与能力各自拥有业务状态。
- 战斗参数、表现资源与能力关系使用 Data Asset；机关/网络实验参数使用已有 JSON 配置。前者适合 UE 资产引用，后者便于外部实验；不重复维护同一字段。
- ASC/AttributeSet 当前在战斗 Character 上，不宣传为 PlayerState 常驻架构。ASC 独立声明在 `multiplayerCombatAbilitySystem.h`；恢复数据合同在 `Network/multiplayerCombatResumeState.h`，GameMode 不再依赖完整角色头。

参数例子见 [Examples/Gameplay.json](Examples/Gameplay.json)，只展示配置结构；运行时需要恢复到资源环境对应位置，不是第二套配置源。

## 当前验证（2026-10-24）

| 范围 | 实际结果 |
| --- | --- |
| Editor / Shipping | 原生编译通过，不等于新发布包运行 |
| 完整 Automation | 174/174，0 测试警告、0 failed/notRun |
| 资产契约 | 42 条审计，43 个所查包内容未改变；战斗/AI 资产校验通过 |
| 战斗联机专项 | `9b4105c` 的 Combat、Lifecycle、Expansion、Reconnect，Normal/Harsh 共 8/8 通过；本次结构整理未重跑 |
| 验证边界 | 本机 Editor DS + 双客户端、CombatTest、NullRHI；没有据此宣称正式地图画面、完整通关、公网或发布包验收通过 |

Harsh 的三端参数为 PktLag=200、Variance=50、Loss=5，不是实测 RTT。历史机关规模实验与平台对照保留原构建条件，不是当前整局带宽或 GAS 优化收益。

本次结构整理后的构建、174 项回归与资产契约见 [架构验证摘要](Tests/Evidence/2026-10-24/architecture.json)。以下是 `9b4105c` 的历史同日证据：[174项逐条摘要](Tests/Evidence/2026-10-24/automation.json)、[8轮战斗/恢复摘录](Tests/Evidence/2026-10-24/network.json)。保留原报告 SHA-256、原状态和显式断言，不上传测试 token、启动命令或原始日志。

## 运行与阅读

以下命令仅供恢复授权资源后的完整开发环境使用，main 缺少默认地图和蓝图时不能直接运行。源码与自动化测试可以审阅，但这里不声称重新独立运行了该快照。

```powershell
# 启动可操作的本机专服与双客户端，在地址菜单连接。
.\Scripts\RunLocalDedicatedServer.ps1 -Play -FromMenu -Port 7777
```

正式入口是 `/Game/Combat/Maps/CoopEscape`；`CombatTest` 用于技能专项。技能为左键攻击、Q 防御/弹反、E 治疗、R 范围、Shift 冲刺；F 引导救援，B 成长菜单。预览与释放操作、专项模式限制见脚本和历史说明。不要省略 `-Play` 后把旧机关原型的自动验证当成当前地图完整通关。

- [当前架构：目录、状态所有者与三条调用链](Docs/ARCHITECTURE.md)
- [面试讲解：背景、架构、GAS 链路与真实踩坑](Docs/INTERVIEW_GUIDE.md)
- [GAS 能力实现](Source/multiplayer/Combat/multiplayerCombatAbility.cpp) / [属性结算](Source/multiplayer/Combat/multiplayerCombatAttributes.cpp)
- [网络同步设计](Docs/NetworkDesign.md) / [公开范围](Docs/SHOWCASE_SCOPE.md)
- [本轮验证结果与测试入口](Tests/README.md)
- [历史机关/平台实验](Docs/Performance.md)

资源须独立取得授权，源码不授予第三方素材再分发权。此次只从 main 的当前 Git 跟踪中移出 Content，本地文件保留；没有重写旧提交，因此旧历史/其他分支仍可能含资源，不属于当前快照的分发承诺。
