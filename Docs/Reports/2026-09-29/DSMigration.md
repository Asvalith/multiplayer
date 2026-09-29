# Dedicated Server 迁移与网络回归

日期：2026-09-29。范围：现有双人 Co-op；不加入 GAS，不重写平台运动或角色预测算法。

## 1. 结论与交付边界

项目主流程已改为 **一个独立 DS 进程 + 两个远端客户端**：菜单只连接服务器地址，退出任一客户端不关闭 DS。旧 Listen 建房/搜房业务和旧菜单资产已移除，没有维护两套入口。

这里的“已运行 DS”指安装版 UE 的 Editor Server，实测为 NM_DedicatedServer、零本地玩家。**不是独立 Server 发布包已经成功构建，更不是已部署 Linux 公网服务器。** 当前引擎实际构建 multiplayerServer 时返回：`Server targets are not currently supported from this engine distribution.` Server Target 已添加，独立包仍需支持该目标的引擎构建。

## 2. 改了什么

| 部分 | 当前实现 |
| --- | --- |
| 启动与地图 | RunLocalDedicatedServer.ps1 启动 DS 和两客户端；LaunchTwoPlayers.bat 转向该入口；客户端默认 DSMenu，服务器默认合作关卡 |
| 客户端连接 | GameInstance 管理地址校验、Connecting/Connected/重连/退出状态；20 秒连接期限、有限重试、同步通知重入保护和实例归属过滤 |
| 房间入口 | 移除 HostGame/FindGames/JoinGame 与 OnlineSession 回调、显式 Null 子系统依赖；不是宣称 UE 内部再也没有 Online 模块 |
| 服务器生命周期 | 玩家退出只结束所属连接；DS 继续维护另一玩家和共享进度；两人都离开时保留当前局，未增加空房回收系统 |
| 游戏重开 | 继续由所属 Controller RPC 请求，GameMode 校验；删除重开时补写 listen 的旧逻辑 |
| UI | 原生地址连接菜单；进入游戏明确恢复 GameOnly 输入；各客户端独立显示胜利界面，DS 不创建本地界面 |
| 测试 | Server、Partner、Client 三进程；两人均真实远端连接，以网络标识区分协作玩家，不依赖重开后的登录顺序 |
| 规模采样 | 保留服务器复制耗时与发送计数；兼容 DS CSV 缺失总 GameThreadTime，缺失值为 null，不冒充零成本 |

机关、钥匙、共享进度、晚绑定补读、CMC 预测/校正/重放继续复用。保留 SessionMaxPlayers 配置键和 LeaveCoopSession 蓝图入口名是兼容已有调用，不代表仍有 Listen 建房流程。

## 3. 怎么验证

同机三个独立进程，UE 5.5.4 Development Editor，默认 NullRHI。服务器断言自身为 DS 且没有 LocalPlayer；客户端分别确认接收到状态和所属角色。每例要求必需断言、三端成功 DONE、正常退出；缺事件、强制结束、致命日志或非零退出均不能通过。

| 模拟档 | PktLag | Variance | Loss |
| --- | ---: | ---: | ---: |
| Normal | 0 | 0 | 0% |
| Moderate | 100 | 20 | 2% |
| Harsh | 200 | 50 | 5% |

这是每端模拟参数，不是实测 RTT。Reconnect 另对主测玩家连接的服务器出站包设 100% 丢失，直到客户端真实超时；另一玩家连接保留。客户端超时缩短为 5 秒以便回归，不代表生产超时建议。

### 功能矩阵

| 用例 | Normal | Moderate | Harsh | 核验范围 |
| --- | --- | --- | --- | --- |
| Flow | 通过 | 通过 | 通过 | 合作门/人数变化/占用清理、平台、双端胜利与重开、退出后 DS 保持 |
| Keys | 通过 | 通过 | 通过 | 拾取携带、安装/消费、失败回滚、去重、销毁和重拾 |
| LateJoin | 通过 | 通过 | 通过 | 后加入者补齐进度、机关状态和钥匙附着 |
| Reconnect | 通过 | 通过 | 通过 | 真超时、有限重连、共享状态恢复；保留原 DS 与协作玩家 |
| ConnectionRetry | 通过 | 通过 | 通过 | 不可达端口连接失败后解除忙碌，再连接正确 DS |
| Ride | 通过 | 通过 | 通过 | 实际 CMC 基座、位移、偏移、到达 |
| RideMotion | 通过 | 通过 | 通过 | 站立/行走/反向/自由跳跃，双端分阶段数据和 Move 记录 |

初始迁移矩阵 **21/21 通过**。收尾版另跑 Normal、Harsh 的 Flow，**2/2 通过**，新增 ClientInputReady 断言确认连接菜单的 UIOnly 已释放；同版本 DS + 两客户端连接冒烟也通过，三个进程均 exit 0，无强制终止。输入标志检查不是系统键鼠或像素验收。

连接状态单测 Coop.Connection.State 通过，0 个错误、1 个预期路径警告（重试耗尽）。脚本自测另覆盖错误角色不能代答、旧 token 不能冒充新结果、缺失/无效复制 CSV 不能通过、DS 缺失 GT 列必须保持不可用。

### 规模工具复核

修复后的四例全部通过。每例预热 5 秒、实际采样约 10 秒，Normal 网络，同机 DS + 两客户端；原关卡背景保留，仅增加指定数量的合成对象。

| 合成对象 | 对照设置 | 结果 | 服务器复制耗时均值 | 服务器总出站量 |
| --- | --- | --- | ---: | ---: |
| 500 静止压力板 | Baseline（保持唤醒） | 通过 | 2.298 ms | 3493 B/s |
| 500 静止压力板 | Dormancy（休眠） | 通过 | 0.232 ms | 3473 B/s |
| 20 持续运动平台 | Baseline（100 Hz 上限） | 通过 | 0.572 ms | 27323 B/s |
| 20 持续运动平台 | Frequency（30 Hz 上限） | 通过 | 0.403 ms | 15846 B/s |

这里是 **4/4 工具链复核，不是正式性能结论**：每组只有一次，未重跑 0/50/200/500、1/5/20 全档位与三次重复。总 GameThreadTime 在 DS CSV 中缺失，全部保留 null；上表是独立的 ServerReplicateActors 计时域，不是整个 GT。出站量是两条连接、背景机关及测试通信的引擎总发送计数，不是网卡流量；100/30 Hz 也不是实测发包频率。

### 平台跳跃：换 DS 没有自动消除校正

以下来自初始迁移矩阵，每档一次 Jump 阶段，约 4 秒，不是稳定统计：

| 档位 | 接受的校正次数 | 同一历史 Move 可比较次数 | 最大可比较误差 | 已观察到起跳后落地 |
| --- | ---: | ---: | ---: | --- |
| Normal | 2 | 2 | 5.00 cm | 是 |
| Moderate | 7 | 7 | 41.04 cm | 是 |
| Harsh | 12 | 12 | 70.13 cm | 是 |

三档基座切换校正计数均为 0。距离来自同一历史 Move、可比较坐标空间的记录，**不是两台客户端屏幕上的实时距离，也不是画面跳动幅度**。三档“通过”只说明动作断言及记录完整；当前断言没有要求校正为零。因此不能宣称平台弱网问题已经修复，也不能据此把根因唯一归结为起跳速度。

## 4. 迁移中处理的问题

1. **测试原来把主机玩家当成协作玩家。** DS 没有本地玩家，改成 Partner 真实连接先入场，再让 Client 入场；重开后按网络身份重新定位，避免依赖登录顺序。
2. **旧“会话重试”不再属于当前产品入口。** ConnectionRetry 改测真实不可达地址、超时释放和后续正确连接；不把它称作重测 OnlineSession 创建失败。
3. **断线不能连带停掉整个服务器。** 故障只施加到主测连接，检查 DS 与另一玩家继续存在；客户端只做有限重连。
4. **菜单 UIOnly 可能残留到游戏视口。** 在本地控制器进入 PlayingState 时恢复 GameOnly，随后再进行胜利绑定；新增输入状态断言。
5. **DS CSV 不含旧脚本要求的总 GT 列。** 首次规模运行四例因采样解析失败，原报告完整保留；实际 CSV 有 ServerReplicateActors。将复制耗时保持为必需测量，总 GT 缺失明确为不可用，补解析自测后重新实跑。没有修改旧失败报告，也没有用 FrameTime 代替 GT。

初始矩阵和收尾复核的模块指纹分别保留；收尾含输入恢复、无用字段/依赖清理。源码与模块指纹是磁盘记录，不额外宣称运行时验证了模块哈希。旧 Listen 数据不与本次 DS 两条远端连接的出站量直接比较。

## 5. 还没完成什么

- 独立 Server 可执行文件、Linux 构建及公网部署：当前安装版引擎不支持 Server Target，未下载/编译另一套引擎。
- 人工双窗口操作、菜单排版、平台视觉平滑与手感验收；NullRHI 不能替代这些。
- 弱网平台自由跳跃的同步算法进一步优化；本轮没有采纳客户端权威起跳速度，也没有强制保留空中基座。
- 完整档位至少三次的性能对照、跨机器测试、玩家容量认证。
- 账号鉴权、匹配/NAT 服务、服务器池、空房回收、进程重启后的持久化恢复。

## 附录：证据与恢复

所有路径相对于项目根目录。可提交的精简证据为 `Tests/Evidence/2026-09-29/DSMigration.json`：保留原报告 SHA-256、模块指纹、退出状态、断言名称和选定测量；它不是原始完整日志。

| 记录 | 原始位置 |
| --- | --- |
| Normal 7 例 | Saved/NetworkValidation/DS-Migration-Normal01/report.json |
| Moderate/Harsh 14 例 | Saved/NetworkValidation/DS-Migration-Weak01/report.json |
| 收尾 Flow 2 例 | Saved/NetworkValidation/DS-Migration-FinalFlow/report.json |
| 首次规模采样失败 | Saved/NetworkValidation/DS-Migration-ScaleSmoke/report.json |
| 修复后的规模复核 | Saved/NetworkValidation/DS-Migration-ScaleVerified/report.json |
| 连接冒烟 | Saved/DedicatedServer/20260929-185338-ece0c32b/report.json |
| 连接状态单测 | Saved/TestReports/DSConnection/index.json |
| 独立 Server 构建限制 | Saved/DSMigration-ServerBuild.log |
| 旧菜单引用检查/移除 | Saved/DSLegacyMenuAudit.log / Saved/DSLegacyMenuRetired.log |

报告同目录还有各进程日志、CSV 和 summary.md；Normal01 与 Weak01 目录保留自动生成的平台时间线 SVG、历史 Move 配对 CSV。

初始两轮报告残留的 outageSeconds=25 是未再参与 DS 故障控制的旧元数据，**不是实际断包时长**。当前脚本已移除该参数，改记录 outageModel；真实断包过程以上述日志和超时回执为准。

旧 mainmenu、WBPmainmenu、bpmainmenupawn、bpmaingamemode 四个资产在资产注册表检查无集合外引用、逐个核对备份哈希后移除。完整迁移前备份在 `Saved/BeforeDSMigration-20260929-181149`（资产在其 UI 子目录）。Saved 被 Git 忽略，本地备份与原始日志需要随工程另行保存；历史已提交资产也可从 Git 恢复。本轮未提交或推送。
