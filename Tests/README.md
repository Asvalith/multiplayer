# 当前测试与公开证据（2026-10-24）

Source 来源 9b4105c；以下报告在完整授权资源开发环境执行，main 未独立运行资产/联机验收。

| 检查 | 原环境结果 | 公开摘录 |
| --- | --- | --- |
| Editor / Shipping 原生编译 | 成功；最后增量检查 up to date | [构建与逐项自动化](Evidence/2026-10-24/automation.json) |
| 完整 Automation | 174 clean，0 warned/failed/notRun | 同上 |
| Combat / Lifecycle / Expansion / Reconnect | 每组 Normal、Harsh 各一次，8/8 passed | [分组、case及显式事件](Evidence/2026-10-24/network.json) |

本轮网络环境为本机 Editor Dedicated Server + 两独立客户端，CombatTest、NullRHI。覆盖预测拒绝、取消、冲刺/弹反/救援和存活/倒地/死亡恢复；不作为正式地图画面、完整通关、实测公网或新发布包验收。

Harsh 三端设置 PktLag=200、Variance=50、Loss=5，这是仿真参数而非实测 RTT。报告保留原状态与所需断言，剔除 token、detail、命令、原始日志和机器标识。原报告相对路径/SHA-256 可核对出处；摘录不代表原文件随仓库提供。

## 源码与入口

- `Source/multiplayer/Testing/` 是真实回归与联机场景实现，可沿生产能力、属性与机关接口查看断言，不是第二套玩法。
- `Scripts/RunMultiplayerNetworkTests.ps1 -SelfTest` 是离线脚本契约检查，不启动游戏、不代表网络通过。
- 具备完整资源后，用同一脚本选 Combat、CombatLifecycle、CombatExpansion 或 CombatReconnect；设置 Map 为 `/Game/Combat/Maps/CombatTest`，Normal/Harsh 分别记录结果。
- 可操作入口 `Scripts/RunLocalDedicatedServer.ps1 -Play` 仍需地图/蓝图；省略 Play 的旧原型机关验证不能当作当前原埃及地图完整通关。

## 历史与未覆盖项

`Evidence/2026-09-27`、`Evidence/2026-09-29` 和 [Performance](../Docs/Performance.md) 是旧机关/平台构建的证据，不作为当前 GAS 通过率或优化收益。历史 JSON 和声明按原版本理解，不把500个合成压力板、20个平台当成玩家容量。

尚无本轮公网、完整画面/通关、全部冷加载特效、Linux DS 或新 Shipping 包运行验收；本轮也不宣称新的整局CPU/带宽下降。资源边界见 [ASSET_REQUIREMENTS](../Docs/ASSET_REQUIREMENTS.md)，实现链见 [INTERVIEW_GUIDE](../Docs/INTERVIEW_GUIDE.md)。
