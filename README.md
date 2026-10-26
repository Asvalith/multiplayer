# UE5 双人合作闯关 · GAS 战斗与状态恢复

个人项目，基于 UE5.5、C++ 与蓝图实现双人联机合作：钥匙安装、压力板合作门、移动平台、波次战斗、倒地救援与撤离结算。当前使用独立 Dedicated Server 和两个客户端，重点实践服务器权威、GAS 技能生命周期及断线后的状态恢复。

> main 为源码展示快照，Source 与开发提交 2efa683616770e05d915276bb074fbcccff6bc0a 完全一致。Content、第三方素材及本机原始运行记录不随此快照分发；测试来自具备授权资源的完整开发环境。运行依赖见 [ASSET_REQUIREMENTS](Docs/ASSET_REQUIREMENTS.md)，公开范围见 [SHOWCASE_SCOPE](Docs/SHOWCASE_SCOPE.md)。

## 三条实现主线

1. **协作机关**：服务器维护机关与目标进度，客户端消费复制状态。占用关系按不同有效玩家统计，避免多碰撞体重复计数；静止机关休眠，变化前唤醒，晚加入补读当前状态，不依赖历史广播。
2. **GAS 战斗**：普攻、防御/弹反、治疗、范围控制、冲刺及救援统一走能力生命周期。配置中的标签关系接入原生阻断/取消；预览复用真实 CanActivateAbility，能量消耗共用成本 GE，服务器重建目标与遮挡并结算。
3. **预测、取消与恢复**：客户端及时预测自身响应，服务器独立准入；用激活版本、预测键、效果/任务句柄区分本轮执行归属，避免旧回调清掉新技能。重连使用服务器票据和有限快照恢复支持的状态，扣除离线时间，不续接半次攻击或回滚已结算伤害。

## 架构与数据

- C++ 管规则和状态，蓝图接动画、特效与 UMG；客户端范围预览不成为权威命中名单。
- GameMode 管对局、接纳和恢复；GameState 复制共享事实；GameInstance 管地址连接、取消和有限重试；机关与能力各自拥有业务状态。
- 战斗参数、表现资源与能力关系使用 Data Asset；关卡人数、机关速度和连接重试等基础规则使用 JSON。延迟/丢包等实验条件由验证脚本参数指定，不写回正式配置；不重复维护同一字段。
- ASC/AttributeSet 当前在战斗 Character 上，不宣传为 PlayerState 常驻架构。ASC 独立声明在 `multiplayerCombatAbilitySystem.h`；恢复数据合同在 `Network/multiplayerCombatResumeState.h`，GameMode 不再依赖完整角色头。

## 当前验证（2026-10-26）

| 范围 | 实际结果 |
| --- | --- |
| Editor / Shipping | 关闭 Unity 合并编译后均通过，不等于新发布包运行 |
| 完整 Automation | 184/184，0 测试警告、0 failed/notRun |
| 激活与表现归属 | 复用已有技能句柄、激活轮次和预测键；旧取消／旧结果重放及真实异步补载 A→B 隔离通过，不等于实际 UDP 乱序已发生 |
| 最新带渲染联机专项 | CombatLifecycle：Normal 通过 148 项门禁、双端 16 张阶段截图；Harsh 在 R 结果表现观察阶段失败 |
| 失败边界 | Harsh 的服务器扣费和伤害正常、旁观者看到粒子，拥有者未观察到该次原生 Cue 路由；结果 Cue 为不可靠传输，日志不足以证明具体丢包位置 |
| 验证范围 | 本机 Editor DS + 双客户端、CombatTest、DX12 离屏；截图抽查不代表连续动画质量、正式地图完整通关、公网或发布包验收 |

Harsh 的三端参数为 PktLag=200、Variance=50、Loss=5，不是实测 RTT。历史机关规模实验与平台对照保留原构建条件，不是当前整局带宽或 GAS 优化收益。

最新整理删除独立特效轮次与重复动画拒绝登记，保留原生动画播放号、效果句柄及预测机制各自职责；共享材质补载读取当前有效技能，不回放旧预测落点。连接计时读取原生 ACK 估计 RTT／RTO，只调整业务等待与有限重试，不重写可靠包重传。较早的 8/8 NullRHI 网络与资源冷检属于对应历史构建，详细证据及保留的失败记录见 [Tests](Tests/README.md)。

最新公开摘录：[184 项逐项回归](Tests/Evidence/2026-10-26/automation.json)、[正常与弱网原状态](Tests/Evidence/2026-10-26/network.json)、[构建与源码对应关系](Tests/Evidence/2026-10-26/verification.json)。旧证据保留原构建口径。

## 运行与阅读

以下入口仅适用于恢复并授权所需地图、蓝图与资源后的完整环境；main 缺少 Content，不能直接作为可玩构建。

```powershell
# 启动可操作的本机专服与双客户端，在地址菜单连接。
.\Scripts\RunLocalDedicatedServer.ps1 -FromMenu -Port 7777
```

正式入口是 `/Game/Combat/Maps/CoopEscape`；`CombatTest` 用于技能专项。技能为左键攻击、Q 防御/弹反、E 治疗、R 范围、Shift 冲刺；F 引导救援，B 成长菜单。启动脚本默认打开可操作的专服与双客户端，`-Play` 仅兼容旧命令。旧庭院自动路线已经移除，正式地图通关须人工操作验收；技能与网络专项使用 `RunMultiplayerNetworkTests.ps1`。

- [当前架构：目录、状态所有者与三条调用链](Docs/ARCHITECTURE.md)
- [面试讲解：背景、架构、GAS 链路与真实踩坑](Docs/INTERVIEW_GUIDE.md)
- [GAS 能力与效果实现](Source/multiplayer/Combat/multiplayerCombatAbility.cpp) / [ASC 集成](Source/multiplayer/Combat/multiplayerCombatAbilitySystem.cpp)
- [网络同步设计](Docs/NetworkDesign.md) / [公开范围](Docs/SHOWCASE_SCOPE.md)
- [本轮验证结果与测试入口](Tests/README.md)
- [历史机关与平台实验](Docs/Performance.md)

资源遵循各自授权；项目源码不授予第三方素材再分发权。需要 UE5.5 与相应资源，不能把本机已有资产等同于仓库可独立重新分发的内容。

当前快照未跟踪 Content；原有本地文件保留。未重写 Git 历史，旧提交及其他分支可能仍有资源。
