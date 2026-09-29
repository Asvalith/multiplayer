# UE5 双人 Co-op：服务器权威与网络同步

基于 **Unreal Engine 5.5 / C++** 的双人合作项目，采用 **Dedicated Server + 两个远端客户端**。两名玩家通过拾取与安装钥匙、协作踩板、开启机关和乘坐移动平台推进关卡，完成目标后共同抵达终点。

项目重点是玩法规则的一致性、连接失败恢复、不同对象的同步策略，以及可复现的网络测试。

## 主要亮点

- **服务器权威的玩法闭环**：服务器校验钥匙归位、不同玩家占用、机关条件及胜利结果；客户端按复制状态更新表现。
- **状态复制与事件驱动**：GameState 统一发布目标快照，UI 监听通知并补读当前状态；后加入玩家从当前状态恢复进度。
- **失败恢复与重入保护**：钥匙提交成功后才登记进度，失败保留物品；重开请求去重，连接通知使用操作编号阻止过期流程继续执行。
- **按对象选择同步方式**：角色复用 CharacterMovement，静止机关使用状态复制与网络休眠，移动平台使用服务器运动与 Actor Movement Replication。
- **自动化验证**：一个 DS 与两个真实客户端独立运行，覆盖合作流程、晚加入、断线重连、自由跳跃与复制规模测试，输出断言、计量和时间线。

## 主要架构

| 模块 | 主要职责 | 设计要点 |
| --- | --- | --- |
| GameMode | 目标登记、胜利判断、重开校验 | 服务器规则入口；操作去重与失败恢复 |
| CoopGameState | 目标总数、完成数、胜利状态 | 结构化快照复制；状态转换通知 |
| GameInstance | 地址连接、取消、超时、有限重连 | 跨地图状态机；过滤其他实例的失败回调 |
| Character / PlayerController | 输入、移动、所属玩家请求 | CMC 预测与校正；通过所属 Controller 发起服务器 RPC |
| Mechanisms | 钥匙、插槽、压力板、门、平台、终点 | 组件分工、占用变化事件、复制属性与 RepNotify |
| UI | 连接菜单、胜利界面 | 本地创建；事件绑定、就绪重试和当前状态补读 |
| Testing | 多进程回归、规模采样、移动诊断 | 各端独立断言；源码、配置与报告指纹 |

C++ 维护玩法状态与网络规则，蓝图和关卡负责资源组合、碰撞形状、机关关联与表现。人数、移动速度、重连间隔由 JSON 配置，启动时集中校验和读取。

### 同步策略

| 对象 | 复制内容 | 客户端处理 |
| --- | --- | --- |
| 玩家角色 | CharacterMovement 的移动数据 | 本地预测、权威校正、未确认移动重放 |
| 钥匙与插槽 | 携带、归位状态及 Actor 附着 | 根据当前状态恢复位置与表现 |
| 压力板与门 | 激活状态、运动参数 | 本地过渡；服务器改状态前刷新网络休眠 |
| 移动平台 | 服务器位置与速度 | 接收运动复制；CMC 处理基座及离地运动 |
| 目标与胜利 | GameState 目标快照 | 更新进度和本地胜利 UI |

移动平台的常规复制频率上限为 **30 Hz**。平台速度沿运动复制链路提供给角色基座计算；自由跳跃离地后按角色自身移动逻辑运行。

设计说明见 [网络架构](Docs/NetworkDesign.md)。

## 性能测试与功能回归

### Dedicated Server 验证

测试环境：**UE 5.5.4 Development Editor、同机一个 Editor Server + 两个独立客户端、NullRHI**。数据来自 2026-09-29 的已记录构建。

- 七类场景 × 三档网络模拟，**21 / 21 通过**。
- 收尾正常/重度弱网主流程复核，**2 / 2 通过**。
- 三端分别检查状态、成功结束事件及进程正常退出。
- 覆盖真实 Overlap、不同玩家计数、钥匙失败回滚、双端胜利、重开、后加入和单连接断线恢复。

三档模拟参数为 Normal：0 / 0 / 0%；Moderate：100 ms / 20 ms / 2%；Harsh：200 ms / 50 ms / 5%（PktLag / Variance / Loss，每个进程设置）。

### DS 复制规模对照

固定关卡背景，单独增加静止压力板或持续运动平台；预热 5 秒、采样约 10 秒，每组一次。

| 场景 | 对照条件 | 服务器复制平均耗时 | 服务器发送量 |
| --- | --- | ---: | ---: |
| 500 个静止压力板 | 保持唤醒 | 2.298 ms | 3,493 B/s |
| 500 个静止压力板 | 网络休眠 | 0.232 ms | 3,473 B/s |
| 20 个持续运动平台 | 100 Hz 频率上限 | 0.572 ms | 27,323 B/s |
| 20 个持续运动平台 | 30 Hz 频率上限 | 0.403 ms | 15,846 B/s |

结果体现两种不同取舍：**休眠主要减少静止对象的复制检查；降低移动对象的更新频率上限主要减少发送量。**

另有历史 Listen Server 参数对照，共 **42 轮、每组 3 次**：500 压力板复制平均耗时中位数为 1.210 → 0.133 ms；20 平台发送量中位数为 12,269.3 → 7,830.6 B/s。两种拓扑分别统计。

测量口径：CPU 为 `ServerReplicateActors` 作用域；B/s 为服务器引擎发送计数，DS 数值包含两条连接和关卡背景。DS 表为单轮复核；历史表为三轮中位数。网络模拟参数与复制频率均为配置值，非实测 RTT 或固定发包率。功能通过指状态断言通过，画面体验使用可视客户端评估。

完整方法与公开数据：[性能与验证报告](Docs/Performance.md) · [测试入口](Tests/README.md)。

## 本地运行

环境：Windows、UE 5.5.4、Visual Studio 2022 C++ 工具链、Git LFS。资源使用 Git LFS 管理。

1. 克隆仓库并执行 `git lfs pull`。
2. 为 `multiplayer.uproject` 生成工程，构建 `multiplayerEditor / Development Editor / Win64`。
3. 在项目根目录设置引擎路径，启动一个无画面服务端与两个可操作客户端：

~~~powershell
$env:UE_EDITOR = 'C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe'
.\LaunchTwoPlayers.bat
~~~

本地启动器使用 Editor 的 `-server` 模式运行 DS，端口为 7777。也可使用 `RunLocalDedicatedServer.ps1 -Play -Port 7777 -EditorPath <引擎路径>`。

独立打开游戏客户端时，连接菜单输入 `127.0.0.1:7777`；其他机器使用服务器地址并配置 UDP 可达性。玩家退出只断开自身连接；本地启动器在两个窗口都关闭后回收本次启动的服务器。

### 自动运行

~~~powershell
# 无 UE 进程的报告解析自检
.\Scripts\RunMultiplayerNetworkTests.ps1 -SelfTest

# 一个 DS + 两个远端客户端连接冒烟
.\Scripts\RunLocalDedicatedServer.ps1

# 正常与弱网功能回归
.\Scripts\RunMultiplayerNetworkTests.ps1 -Scenario All -Profiles Normal,Moderate,Harsh -Repeat 1
~~~

脚本读取 `UE_EDITOR`，也支持显式传入 `-EditorPath`。输出位于 `Saved/NetworkValidation/<run>/`，包括报告、日志及计量；运行器只清理自己启动的进程。

## 目录

~~~text
Source/multiplayer/
  Core/          服务器规则、共享快照、配置与日志
  Network/       DS 连接状态机
  Player/        角色输入、移动和所属玩家请求
  Mechanisms/    钥匙、插槽、压力板、门、平台与占用组件
  UI/            连接与胜利界面
  Testing/       多进程测试、故障注入、平台诊断
Content/Config/  Gameplay.json
Scripts/        启动、回归、时间线绘制、证据导出
Tests/          测试用法、报告结构和公开证据
Docs/           架构与性能报告
~~~
