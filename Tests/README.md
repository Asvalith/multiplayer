# 当前测试与公开证据（2026-10-26）

Source 与开发提交 2efa683616770e05d915276bb074fbcccff6bc0a 完全一致，Source 树为 704ea2b98c0a35f78f2539d4e6f5be80f77b5218。报告在提交前的完整授权开发环境执行，最终源码清单核对无差异；main 缺少 Content，未独立重跑资源或联机验收。

| 检查 | 原环境结果 | 公开摘录 |
| --- | --- | --- |
| Editor / Shipping 非 Unity 编译 | 完成；不等于发布包运行 | [构建与源码对应关系](Evidence/2026-10-26/verification.json) |
| 完整 Automation | 184 clean，0 warned/failed/notRun | [逐项名称及状态](Evidence/2026-10-26/automation.json) |
| CombatLifecycle Normal，DX12 离屏 | 148 项门禁通过，91.314 秒，三个进程正常退出 | [案例与显式断言](Evidence/2026-10-26/network.json) |
| CombatLifecycle Harsh，DX12 离屏 | **失败**，108.899 秒；R 的拥有者结果表现未被观察，超时后由 runner 结束 | 同上 |

## 机制与证据边界

- 复用现有 SpecHandle、ActivationRevision 与 PredictionKey，没有新增施法计数器。原生动画播放号仍区分具体播放；Commit 的重复拒绝登记已移除。
- 真实 A／B 激活后，在原接收入口确定性重放旧取消和旧结果，B 不被清除、A 不复活。真实异步加载用例观察未完成的 A 请求，再验证 B 接替。它们不证明实际 UDP 乱序已经发生。
- Normal 的真实键盘输入、粒子登记、双端 A／取消／B 保留及取消均通过；原环境保存 16 张阶段 PNG，并抽查部分画面。原始截图与日志不发布，截图抽查不代表连续动画质量。
- Harsh 为三端 PktLag=200、Variance=50、Loss=5，不是实测 RTT。服务器扣费及伤害正确，Partner 观察到原生路由与粒子，Owner 未观察到同次路由。观察点早于项目身份过滤，不能归因为自定义身份误删，也不能单凭日志确定具体丢包位置；原生结果 Cue 不保证送达。
- 未新增确认／补发层，未将结果改成可靠 RPC，未降低门禁或把失败算成通过。先前配置验证顺序、输入帧和表现缺失失败保留于开发环境。
- 116 个受保护素材未因最新专项变化。资产须独立授权；仓库的地图配置基线 JSON 不包含实际资源包。

## 使用入口

Source/multiplayer/Testing 保留真实回归和联机场景；脚本的 SelfTest 仅检验契约，不启动游戏。恢复授权资源后方可运行 Combat、CombatLifecycle、CombatExpansion、CombatReconnect，或用 RunLocalDedicatedServer.ps1 -FromMenu 操作双客户端。

## 历史证据

Evidence/2026-10-24 及更早目录保留各自构建与拓扑，不作为当前通过率或优化收益。旧的 NullRHI 8/8 不能代替本轮带渲染 Harsh 的失败。当前没有公网、正式地图完整通关、Linux DS、新发布包运行或新的整局性能收益验收。

公开摘录只保留白名单字段和原始状态，不上传 token、机器标识、命令、绝对路径、原始 detail 或日志；报告相对路径与 SHA-256 可核对来源。实现见 [ARCHITECTURE](../Docs/ARCHITECTURE.md)、[INTERVIEW_GUIDE](../Docs/INTERVIEW_GUIDE.md)，资源边界见 [ASSET_REQUIREMENTS](../Docs/ASSET_REQUIREMENTS.md)。
