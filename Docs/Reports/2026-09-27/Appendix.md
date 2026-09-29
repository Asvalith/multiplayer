# 共用附录：源数据、逐轮结果与文件索引

整理日期：2026-09-27。返回[实验报告](NetworkExperimentReport.md) · [踩坑报告](ProjectPitfalls.md)。

## A. 数据分层与阅读方法

正文的百分比来自已结束的打包版 A/B 实测，后续动态载人专项另用编辑器构建；整理时没有重新跑游戏。为避免把汇总当原始记录，分三层保存：

1. **正文**：打包版性能三轮中位数、正确性结果、动态专项单次对照，以及各自的问题解释和下一步。
2. **本附录与公开 JSON**：逐轮计数、结果、断言、构建指纹。JSON 是脱敏摘录，不是完整原始报告；本附录表格又做了显示位数舍入。
3. **本地源数据包**：原始 report.json、服务器及客户端 CSV、双方日志、构建日志、历史失败记录。原文件字节未修改，每个文件的 SHA-256 在包内 manifest.json 中。

### A.1 原始数据包

本地文件：[source-data.zip](../../../Saved/NetworkValidation/Reports/2026-09-27/source-data.zip)，约 18.34 MiB（19,234,233 字节）。包括 272 个原始文件和一份新生成的索引 manifest.json；解压流逐文件复核通过。此包对应构建 A/B 的旧批次，不包含后续编辑器动态载人专项；专项公开摘录与本地原始目录另见 G 节。

压缩包 SHA-256：`a1028fe2ff14f64b9c0e2678a33735fe5022a69b2760d3094690d0622e0060b2`。

**数据包未脱敏，可能包含本机路径、机器名、地址和运行标识，只供本地复核，不要直接公开上传。**它位于 git 忽略的 Saved 目录，远端仓库不会随文档自动拥有该文件。对外展示优先使用下方公开摘录；需要分享完整数据时，另生成脱敏副本并保留原件。

压缩包结构：

```text
source-data.zip
├── manifest.json
├── runs/
│   ├── package-flow-normal/          重开失败的原始记录
│   ├── regression-20260927/          构建 A 初次回归，缺主机会话保留断言
│   ├── regression-final-20260927/    构建 B 最终回归
│   ├── scale-formal-20260927/        构建 A 正式 42 轮，含双端逐帧 CSV
│   └── scale-final-smoke-20260927/   构建 B 4 轮短验收，含双端 CSV
├── build/                            蓝图、Cook/打包及 Shipping 编译日志
└── public-evidence/                  本次引用的公开证据副本
```

46 份服务器 CSV 的整理时指纹与各自运行报告中记录的指纹匹配；客户端 CSV 也被完整收录并计算了整理时指纹，但原运行报告没有记录其 hash，不能说完成了同等级的历史指纹匹配。正式 42 份服务器 CSV 共 57,486 个有效帧样本；最终短验收另有 1,473 个，不混算。

### A.2 打包版 A/B 的原始报告与公开摘录

以下 SHA-256 对应原始 report.json，不是公开摘录文件自身。原文件在包内 `runs/<目录>/report.json`。

| 原始目录 | 公开摘录 | 原始报告 SHA-256 | 用途 |
| --- | --- | --- | --- |
| scale-formal-20260927 | [performance.json](../../../Tests/Evidence/2026-09-27/performance.json) | `05f8c9c40cde6d0be7fbe31465d55a63b74fa61693638a4d60f4380de3f1ae67` | 正式性能：42/42 |
| regression-final-20260927 | [regression-final.json](../../../Tests/Evidence/2026-09-27/regression-final.json) | `435c9220d979d7db98b2da4eba104395be72ac317cfe8c57777d1c6e244d05eb` | 最终回归：15/15 |
| scale-final-smoke-20260927 | [performance-final-smoke.json](../../../Tests/Evidence/2026-09-27/performance-final-smoke.json) | `4b21747b3261ceb3281370cd37905176b67f56d05c73e2a15137a2c5ca57774e` | 最终短验收：4/4 |
| regression-20260927 | [regression.json](../../../Tests/Evidence/2026-09-27/regression.json) | `b32ad3bfe77704d18a9f946f113e62d81067740d21a6b5af88a5cbf6f4ec565a` | 历史初次回归：不替代最终报告 |

### A.3 打包版 A/B 的构建与配置

| 项目 | 构建 A：正式性能 | 构建 B：最终回归与短验收 |
| --- | --- | --- |
| 二进制 SHA-256 | `6c77e933e57f9a805fc0cf53c6738470c3350fccc3286c60a556d5598578884a` | `e004d62aa0b58d12b4c938b421a2ac3e48498948ffed7744b70730574cb6c883` |
| 源码清单指纹 | `33887c713f1473acaef5e92c1a7b48cc403ec1d5abe3f7a77973840c5fee8722` | `782d209298fd1972da86b7917ba3449cb1df4fc971f67592ea7f26181ed28138` |
| 基础 commit | `05b7cf1077b708367a90f63fe152e467abb6e7b5` | 同左 |
| 工作区状态 | dirty，未提交 | dirty，未提交 |

共同的玩法配置 SHA-256：`97b67b8238706e2f2765446c93b512dbcb39153a3e32676bc7c5b9316f8c0ce0`。两种构建包内文件均与源文件匹配。配置内容可在 [Gameplay.json](../../../Content/Config/Gameplay.json) 阅读；报告保留了实测时配置，不以未来修改过的当前文件代替历史配置。

只有 commit 不能重建 dirty 工作区；源码清单指纹可校验一致性，但它本身不包含源码内容，也不能凭 hash 还原文件。正式复现需要保留匹配的工程快照及构建环境。

公开字段 `runtimeGameplayConfig.loadedByRuntimeVerified=false` 保留原值。运行主机日志另有 `Gameplay config loaded` 及包内路径、session=2、win=2、platform=1、速度 150/250/80 cm/s、3 次重连的记录；这是额外人工日志核对，不是自动字段已经变为 true。

## B. 正式性能：42 轮逐次结果（构建 A）

所有轮次均为 passed。0 档也有原地图、角色和测试通信，不是空场景。

“实采 s / OutBytes / OutPackets”是原始采样计数；B/s 是 OutBytes 除以实采秒数；CPU 均值和 P95 是每轮服务器 CSV 的派生统计。CPU 单位 ms，只对应 `Exclusive/GameThread/ServerReplicateActors`。编号是完整 case id 的前三位，可在公开 JSON 中唯一定位；完整文件名由 case id 加 `-Host.csv` 或 `-Client.csv` 组成。

### B.1 静止压力板：24 轮

两组频率上限均为 30 Hz，新增移动平台为 0。每个数量 / 设置各 3 轮，预热至少 5 秒，计划采样 30 秒。

| 编号 | 新增板 | 设置 | 轮 | 实采 s | OutBytes | OutPackets | B/s | 复制均值 ms | 复制 P95 ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 001 | 0 | 唤醒 | 1 | 30.039 | 58650 | 1361 | 1952.4 | 0.080505 | 0.1556 |
| 002 | 0 | 唤醒 | 2 | 30.065 | 56320 | 1300 | 1873.3 | 0.109281 | 0.2398 |
| 003 | 0 | 唤醒 | 3 | 30.065 | 58046 | 1346 | 1930.7 | 0.091905 | 0.2042 |
| 004 | 0 | 休眠 | 1 | 30.085 | 56191 | 1297 | 1867.8 | 0.119758 | 0.2506 |
| 005 | 0 | 休眠 | 2 | 30.011 | 54893 | 1264 | 1829.1 | 0.123069 | 0.2466 |
| 006 | 0 | 休眠 | 3 | 30.094 | 56921 | 1317 | 1891.5 | 0.102527 | 0.2299 |
| 007 | 50 | 唤醒 | 1 | 30.029 | 56808 | 1313 | 1891.8 | 0.189546 | 0.4530 |
| 008 | 50 | 唤醒 | 2 | 30.004 | 56705 | 1312 | 1889.9 | 0.198570 | 0.4929 |
| 009 | 50 | 唤醒 | 3 | 30.008 | 55712 | 1285 | 1856.6 | 0.239931 | 0.5262 |
| 010 | 50 | 休眠 | 1 | 30.120 | 55653 | 1283 | 1847.7 | 0.131543 | 0.2674 |
| 011 | 50 | 休眠 | 2 | 30.044 | 56257 | 1300 | 1872.5 | 0.127807 | 0.2531 |
| 012 | 50 | 休眠 | 3 | 30.047 | 55596 | 1282 | 1850.3 | 0.119016 | 0.2426 |
| 013 | 200 | 唤醒 | 1 | 30.021 | 56422 | 1303 | 1879.4 | 0.503874 | 1.2697 |
| 014 | 200 | 唤醒 | 2 | 30.017 | 55809 | 1287 | 1859.2 | 0.552371 | 1.3295 |
| 015 | 200 | 唤醒 | 3 | 30.099 | 56934 | 1316 | 1891.6 | 0.463313 | 1.2012 |
| 016 | 200 | 休眠 | 1 | 30.104 | 56455 | 1304 | 1875.3 | 0.111870 | 0.2415 |
| 017 | 200 | 休眠 | 2 | 30.083 | 55395 | 1278 | 1841.4 | 0.136677 | 0.2703 |
| 018 | 200 | 休眠 | 3 | 30.067 | 56948 | 1317 | 1894.0 | 0.120358 | 0.2534 |
| 019 | 500 | 唤醒 | 1 | 30.075 | 54520 | 1253 | 1812.8 | 1.308937 | 3.2296 |
| 020 | 500 | 唤醒 | 2 | 30.073 | 55566 | 1281 | 1847.7 | 1.209885 | 3.0875 |
| 021 | 500 | 唤醒 | 3 | 30.082 | 55901 | 1288 | 1858.3 | 1.167772 | 3.2224 |
| 022 | 500 | 休眠 | 1 | 30.024 | 55570 | 1282 | 1850.9 | 0.136055 | 0.2694 |
| 023 | 500 | 休眠 | 2 | 30.075 | 56682 | 1310 | 1884.7 | 0.132682 | 0.2721 |
| 024 | 500 | 休眠 | 3 | 30.032 | 56430 | 1304 | 1879.0 | 0.120758 | 0.2538 |

### B.2 运动平台：18 轮

新增静止压力板为 0。100/30 Hz 是复制频率上限，不是实际每秒发送次数。每组 3 轮，预热至少 5 秒，计划采样 30 秒。

| 编号 | 新增平台 | 设置 | 轮 | 实采 s | OutBytes | OutPackets | B/s | 复制均值 ms | 复制 P95 ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 025 | 1 | 100 Hz | 1 | 30.109 | 73366 | 1340 | 2436.7 | 0.148332 | 0.2966 |
| 026 | 1 | 100 Hz | 2 | 30.048 | 72000 | 1316 | 2396.2 | 0.151091 | 0.2889 |
| 027 | 1 | 100 Hz | 3 | 30.052 | 73233 | 1336 | 2436.9 | 0.134489 | 0.2789 |
| 028 | 1 | 30 Hz | 1 | 30.083 | 65774 | 1317 | 2186.4 | 0.136609 | 0.2812 |
| 029 | 1 | 30 Hz | 2 | 30.060 | 65063 | 1296 | 2164.4 | 0.153428 | 0.2957 |
| 030 | 1 | 30 Hz | 3 | 30.085 | 66587 | 1340 | 2213.3 | 0.113446 | 0.2617 |
| 031 | 5 | 100 Hz | 1 | 30.026 | 136010 | 1375 | 4529.7 | 0.139216 | 0.3221 |
| 032 | 5 | 100 Hz | 2 | 30.008 | 134531 | 1356 | 4483.2 | 0.189303 | 0.3655 |
| 033 | 5 | 100 Hz | 3 | 30.120 | 135527 | 1361 | 4499.6 | 0.192977 | 0.3673 |
| 034 | 5 | 30 Hz | 1 | 30.089 | 102751 | 1333 | 3414.9 | 0.182347 | 0.3560 |
| 035 | 5 | 30 Hz | 2 | 30.098 | 102221 | 1339 | 3396.3 | 0.158922 | 0.3333 |
| 036 | 5 | 30 Hz | 3 | 30.092 | 102739 | 1364 | 3414.2 | 0.120504 | 0.2749 |
| 037 | 20 | 100 Hz | 1 | 30.045 | 369421 | 1380 | 12295.6 | 0.238248 | 0.5771 |
| 038 | 20 | 100 Hz | 2 | 30.030 | 368440 | 1376 | 12269.3 | 0.254253 | 0.5869 |
| 039 | 20 | 100 Hz | 3 | 30.044 | 365273 | 1366 | 12158.0 | 0.306622 | 0.6333 |
| 040 | 20 | 30 Hz | 1 | 30.030 | 236005 | 1358 | 7858.9 | 0.251259 | 0.5430 |
| 041 | 20 | 30 Hz | 2 | 30.091 | 235627 | 1358 | 7830.6 | 0.230835 | 0.5272 |
| 042 | 20 | 30 Hz | 3 | 30.044 | 234984 | 1377 | 7821.3 | 0.180940 | 0.4536 |

### B.3 辅助计数与运行帧节奏

同一编号对应 B.1 / B.2。休眠数和每连接调用数为该轮逐帧均值；FPS 由该轮 FrameTime 均值派生。并非玩家视角的渲染帧率。42 轮的 GameThreadTime 均全零，统一标为不可用，不在表中伪造 GT 耗时。

| 编号 | CSV 有效帧 | 休眠对象均值 | 每帧每连接调用均值 | FrameTime 均值 ms | 派生运行 FPS |
| --- | ---: | ---: | ---: | ---: | ---: |
| 001 | 1388 | 7.00 | 11.0432 | 21.6416 | 46.2072 |
| 002 | 1359 | 7.00 | 11.1214 | 22.1208 | 45.2063 |
| 003 | 1384 | 7.00 | 11.0130 | 21.7217 | 46.0369 |
| 004 | 1361 | 7.00 | 11.1176 | 22.1034 | 45.2419 |
| 005 | 1356 | 7.00 | 11.0383 | 22.1311 | 45.1852 |
| 006 | 1374 | 7.00 | 11.0648 | 21.9014 | 45.6593 |
| 007 | 1362 | 7.00 | 31.7217 | 22.0473 | 45.3570 |
| 008 | 1372 | 7.00 | 31.3703 | 21.8684 | 45.7281 |
| 009 | 1365 | 7.00 | 31.7092 | 21.9829 | 45.4900 |
| 010 | 1359 | 57.00 | 11.1347 | 22.1622 | 45.1219 |
| 011 | 1366 | 57.00 | 11.0966 | 21.9925 | 45.4700 |
| 012 | 1350 | 57.00 | 11.0607 | 22.2566 | 44.9304 |
| 013 | 1369 | 7.00 | 93.6538 | 21.9286 | 45.6026 |
| 014 | 1360 | 7.00 | 94.0529 | 22.0711 | 45.3081 |
| 015 | 1378 | 7.00 | 93.0022 | 21.8405 | 45.7864 |
| 016 | 1369 | 207.00 | 11.0891 | 21.9887 | 45.4778 |
| 017 | 1365 | 207.00 | 11.0615 | 22.0385 | 45.3751 |
| 018 | 1370 | 207.00 | 11.0993 | 21.9451 | 45.5684 |
| 019 | 1357 | 7.00 | 219.7679 | 22.1620 | 45.1222 |
| 020 | 1370 | 7.00 | 215.9752 | 21.9507 | 45.5565 |
| 021 | 1378 | 7.00 | 213.3512 | 21.8274 | 45.8140 |
| 022 | 1361 | 507.00 | 11.1021 | 22.0591 | 45.3328 |
| 023 | 1376 | 507.00 | 11.0807 | 21.8564 | 45.7531 |
| 024 | 1377 | 507.00 | 11.0806 | 21.8081 | 45.8545 |
| 025 | 1370 | 7.00 | 11.8058 | 21.9760 | 45.5041 |
| 026 | 1362 | 7.00 | 11.8223 | 22.0608 | 45.3293 |
| 027 | 1365 | 7.00 | 11.8425 | 22.0152 | 45.4233 |
| 028 | 1368 | 7.00 | 11.5110 | 21.9893 | 45.4767 |
| 029 | 1362 | 7.00 | 11.4831 | 22.0688 | 45.3129 |
| 030 | 1382 | 7.00 | 11.3972 | 21.7686 | 45.9377 |
| 031 | 1377 | 7.00 | 14.5715 | 21.8038 | 45.8635 |
| 032 | 1366 | 7.00 | 14.6632 | 21.9670 | 45.5229 |
| 033 | 1368 | 7.00 | 14.7317 | 22.0158 | 45.4219 |
| 034 | 1353 | 7.00 | 13.2454 | 22.2376 | 44.9689 |
| 035 | 1366 | 7.00 | 13.2072 | 22.0320 | 45.3886 |
| 036 | 1380 | 7.00 | 13.0681 | 21.8050 | 45.8609 |
| 037 | 1380 | 7.00 | 25.2355 | 21.7707 | 45.9334 |
| 038 | 1376 | 7.00 | 25.2900 | 21.8227 | 45.8238 |
| 039 | 1366 | 7.00 | 25.2811 | 21.9933 | 45.4683 |
| 040 | 1366 | 7.00 | 19.2811 | 21.9838 | 45.4880 |
| 041 | 1371 | 7.00 | 19.2670 | 21.9474 | 45.5635 |
| 042 | 1382 | 7.00 | 19.1165 | 21.7389 | 46.0004 |

### B.4 从逐轮值复算正文

- 每组按照 matrix、数量、optimization 分组，各有 3 轮，取三个均值的中位数；P95 一列同样取三个 P95 的中位数。
- 500 板均值：`(1.209885 - 0.132682) / 1.209885 × 100% = 89.0335%`；绝对差 `1.077203 ms`。
- 20 平台发送量：`(12269.252 - 7830.583) / 12269.252 × 100% ≈ 36.1772%`；计算时使用 JSON 全精度值。
- 500 板发送量不是减少：休眠组相对唤醒组约增加 1.7%，不声称存在带宽收益。
- 0 板没有新增负载，两种设置的复制均值仍波动，甚至休眠标签组更高；不能概括为“所有档位都改善”。
- 原构建 A 的 counterSource 写有 `wire packets`，措辞过强。真实口径是引擎发送缓冲字节及配置的包头开销，不是网卡实抓流量；原记录未改写，构建 B 只修正说明文字。

## C. 最终正确性回归：15 例（构建 B）

每个模式分别跑三个网络档位，每档一次；总计 156 条成功 ASSERT，每例均有 Host / Client 的成功 DONE。数量是事件数，不是 156 个互相独立的测试场景。requiredAssertions 的名称与各角色原始 detail 保留在 regression-final.json 中。

| 编号 | 模式 | 网络档位 | 用例墙钟 s | ASSERT 数 | 双端 DONE | 结果 |
| --- | --- | --- | ---: | ---: | --- | --- |
| 001 | Flow | Normal | 39.192 | 21 | Host / Client | passed |
| 002 | Flow | Moderate | 36.431 | 21 | Host / Client | passed |
| 003 | Flow | Harsh | 37.812 | 21 | Host / Client | passed |
| 004 | LateJoin | Normal | 12.732 | 6 | Host / Client | passed |
| 005 | LateJoin | Moderate | 13.738 | 6 | Host / Client | passed |
| 006 | LateJoin | Harsh | 15.074 | 6 | Host / Client | passed |
| 007 | Reconnect | Normal | 26.817 | 12 | Host / Client | passed |
| 008 | Reconnect | Moderate | 29.771 | 12 | Host / Client | passed |
| 009 | Reconnect | Harsh | 32.678 | 12 | Host / Client | passed |
| 010 | SessionRetry | Normal | 16.155 | 5 | Host / Client | passed |
| 011 | SessionRetry | Moderate | 17.706 | 5 | Host / Client | passed |
| 012 | SessionRetry | Harsh | 17.986 | 5 | Host / Client | passed |
| 013 | Ride | Normal | 18.593 | 8 | Host / Client | passed |
| 014 | Ride | Moderate | 21.606 | 8 | Host / Client | passed |
| 015 | Ride | Harsh | 23.440 | 8 | Host / Client | passed |

用例墙钟时间包含启动、加入和清理，不是网络延迟或函数耗时。

| 模式 | 判定重点 | 限制 |
| --- | --- | --- |
| Flow | 两端 Join；OverlapKeys；PlateDistinctPlayers / GateRules；远端 GateOpen / GateClosed；DestroyedPawnCleanup；PlatformEndpoint / ClientRide；ClientVictoryState；RestartFailureRecovery；Restart / ClientRestart；ClientLeave / HostLeave | 运行时合成机关与真实地图宝物混合，不等于所有资产与玩家按键流程覆盖 |
| LateJoin | 加入后读到已有目标和压力板状态 | 不覆盖胜利后才加入的 UI |
| Reconnect | OutageApplied；ConnectionLostDetected；HostSessionRetained；ReconnectAfterOutage；ReconnectStateRestored | 会话保留检查不是重新搜索菜单；新连接、新 Pawn，不恢复私人状态 |
| SessionRetry | SessionFailureObserved；SessionRetrySucceeded | 首次失败是开发注入 |
| Ride | 客户端真实 CMC 基座与位移检查 | 站立工况，不等于行走、跳跃或视觉验收 |

两端模拟档位：Normal=0/0/0；Moderate=延迟 100 ms、波动 20 ms、丢包 2%；Harsh=200 ms、50 ms、5%。Reconnect 另外在主机施加 12 秒全丢包，测试连接超时 5 秒。首次重开失败也是开发注入，不能与真实断包超时混称。

六条 ClientRide（Flow 三条 + Ride 三条）均记录 46/46 个有效基座样本。最大横向偏移从进入 Ride 阶段 1 秒后开始统计，结果为 0.0 cm（日志精度），不包含上平台阶段与 Ride 首秒；不据此宣称任意网络与运动下都零误差。

源码通过门槛并非“必须零误差”：基座有效样本占比至少 70%、上述统计段最大横向偏移小于 100 cm、角色 X 方向移动超过 250 cm，并等待平台进入终点 3 cm 容差且 Ride 超过 5 秒后检查。实际观察值与验收门槛分开记录，这组宽松的逻辑门槛不替代视觉体验标准。

## D. 最终构建短验收：4 轮（构建 B）

每组只跑一次，预热 2 秒、计划采样 8 秒。用途是检查最终二进制仍能生成负载、同步并采样，不参与正式三轮中位数或正文的优化百分比。

| 编号 | 系列 / 数量 | 设置 | 实采 s | OutBytes | OutPackets | B/s | 复制均值 ms | 复制 P95 ms | 结果 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 001 | Static / 500 | 唤醒 | 8.033 | 14883 | 343 | 1852.7 | 1.205394 | 3.2383 | passed |
| 002 | Static / 500 | 休眠 | 8.084 | 15290 | 354 | 1891.3 | 0.126237 | 0.2748 | passed |
| 003 | Moving / 20 | 100 Hz | 8.063 | 98657 | 370 | 12235.4 | 0.222530 | 0.5579 | passed |
| 004 | Moving / 20 | 30 Hz | 8.009 | 63027 | 364 | 7869.9 | 0.232369 | 0.4846 | passed |

短验收的 20 平台复制均值从 0.222530 到 0.232369 ms，单轮反而增加。原值如实保留，不挑选其发送量来替代正式性能统计，更不据此宣称 CPU 改善。

## E. 失败记录、代码入口与构建证据

### E.1 踩坑证据索引

| 案例 | 已保存的依据 | 当前实现入口 |
| --- | --- | --- |
| 重开后丢 Listen 模式 | 包内 runs/package-flow-normal 的失败 report 与双端 log；最终 Flow 的 Restart / ClientRestart | [GameMode](../../../Source/multiplayer/Core/multiplayerGameMode.cpp)，RequestRestartCurrentRound，当前约 132 行起，listen 修复约 170 行 |
| 远端超时误清主机 Session | 包内 runs/regression-20260927/007-Reconnect-Normal-0-1-Combined-1-Host.log 的服务器超时；最终 HostSessionRetained 断言 | [GameInstance](../../../Source/multiplayer/Network/multiplayerGameInstance.cpp)，HandleNetworkFailure，约 700 行起 |
| 板开关未变但人数改变 | 最终 Flow 的 PlateDistinctPlayers / GateRules 和客户端 GateOpen / GateClosed | [PressurePlate](../../../Source/multiplayer/Mechanisms/multiplayerPressurePlate.cpp)，HandleOccupancyChanged，约 178 行；[CoopGate](../../../Source/multiplayer/Mechanisms/multiplayerCoopGate.cpp)，EvaluateGateState，约 282 行 |
| 宝物重复登记保护 | 最终 Flow 的 OverlapKeys，真实重叠 4 个预绑定宝物 | [KeySocket](../../../Source/multiplayer/Mechanisms/multiplayerKeySocket.cpp)，先置激活再登记 |
| 胜利通知同步重入 | 当前源码可核实；本轮未专门验证反向完成顺序和广播次数 | [CoopGameState](../../../Source/multiplayer/Core/multiplayerCoopGameState.cpp)，HandleObjectiveStateChanged，约 64 行 |
| GameState 晚绑定 | 就绪事件和补读实现；普通远端 ClientVictoryState 通过 | [VictoryPresenter](../../../Source/multiplayer/UI/multiplayerVictoryPresenterComponent.cpp)，RefreshBinding，约 20 行 |
| Pawn 销毁占用残留 | 最终 Flow 的 DestroyedPawnCleanup | [PlayerOccupancy](../../../Source/multiplayer/Mechanisms/multiplayerPlayerOccupancyComponent.cpp)，约 189 / 252 / 310 行 |
| Sweep 与固定轨道选择 | 当前实现与注释；载人断言不证明自动避障 | [Transporter](../../../Source/multiplayer/Mechanisms/multiplayerTransporterComponent.cpp)，SetActorLocation 显式 false，约 49 行 |

函数名优先于行号，后续修改可能使行号移动。历史审计发现的风险与当前防护可核实，不代表本地仍保存每个旧版本的运行复现视频或失败日志。

### E.2 构建与配置证据

原始日志在压缩包 build/；逐文件指纹也可从 manifest.json 查到。

| 文件 | 结果或用途 | SHA-256 |
| --- | --- | --- |
| BlueprintCompile.log | 96 个蓝图，其中 /Game 下 17 个；0 错误、0 警告、0 加载失败 | `9953def67142aa08846ada7ef84ffc57ef42f3baaff7b8a82b22dc873ce26ff3` |
| Package-restart-fix-20260927.log | 构建 A 的重开修复后构建/打包记录 | `e631f24acf7805fba1ed78cd4e7e28a99f6878c7c67669bda4b3dae6108a81fb` |
| Package-final-20260927.log | 构建 B 的编译与 Stage 成功 | `7e963a14d1438845109e80e7eb5883fafeee5ee6a58887b30aa50e1a811e2d3c` |
| Shipping-final-20260927.log | 最终 Shipping 编译通过 | `c4c785a095aecd12acec7e94c96cbb1c4caabacb62f84537189b36b08eb152e8` |

完整 Cook 在前面的打包阶段已完成；最终 C++ 修复后的 Stage 复用了已有 Cook 结果，不描述为每次都完整重新 Cook。Shipping 测试启动入口关闭，不声称二进制剔除了全部 UHT 测试类型。

## F. 构建 A/B 的解释边界与验收空白

- 本节 A/B 批次为同机双进程、NullRHI，非双物理机、公网或专用服务器；后续 D 专项也为同机 NullRHI，但使用另一编辑器构建。
- 合成机关关闭交互碰撞且始终网络相关，数量不是正式关卡容量结论。
- 属性状态和 Widget 入 Viewport 的通过，不能证明菜单按钮操作、像素显示、平台视觉平滑与门碰撞过渡都正确。
- 未强制 GameState 晚到，未专测胜利通知嵌套重入次数，未验收胜利后晚加入的界面。
- 发送量是整个被测服务器对其连接的引擎计数，不是平台独占字节，不证明成功送达或网卡实际流量。
- 3 轮性能样本保留散布但样本量有限，且按组执行存在时间漂移；不作跨机器显著性与玩家容量承诺。
- 数据中的初次回归、历史失败与最终结果一并保留，不修改旧记录让报告更好看。

## G. 动态载人专项（编辑器构建）

本节是晚于构建 A/B 的独立实验：同机 Listen Server 与客户端、编辑器 `-game -NullRHI`、一个水平移动平台、每阶段约 4 秒。测试 Pawn 复制实际关卡角色的五项关键移动参数，但不覆盖完整蓝图资产和相机。公开 JSON 是原报告的脱敏摘录，不与前述 `source-data.zip` 或 15/42 批次合并计数。

### G.1 对照、结果与文件

| 模式和网络档位 | 双端站立 / 行走 / 换向 / 跳跃 | 公开摘录 | 原始报告 SHA-256 |
| --- | --- | --- | --- |
| Baseline / Normal | 通过 / 通过 / 通过 / 失败 | [baseline.json](../../../Tests/Evidence/2026-09-27/PlatformRide/baseline.json) | `3f59d1f86816c0d0f2d104f3b67516cbb29dd0fa7657d9293c61523838a59bbf` |
| OrderedVelocity / Normal | 通过 / 通过 / 通过 / 失败 | [ordered-velocity.json](../../../Tests/Evidence/2026-09-27/PlatformRide/ordered-velocity.json) | `a2043cd2d534cefbaa077fd1b753cce728b1efbf6020dd1ef9073be1e7401782` |
| PlatformInertia / Normal、Moderate、Harsh | 三档各四阶段全部通过 | [inertia.json](../../../Tests/Evidence/2026-09-27/PlatformRide/inertia.json) | `49dc9a5fd359c495e92e37a97d6288b323e2c7f85951e1f649be640d1893b4dd` |
| 原合作 Flow / Normal | 双端通过；不是全 15 例重跑 | [flow-regression.json](../../../Tests/Evidence/2026-09-27/PlatformRide/flow-regression.json) | `7c4d3defffaf8b3f4713d7291c5af0a36491404afd1eac4c05a7ce69f0c80fc7` |

两个失败对照的 Host 与 Client 均有 `MotionJump=false`，没有将预期失败改写为成功。最终三档各有双方 `DONE passed=true` 与 `MotionServerObserved`。五个载人案例及一个 Flow 案例记录的磁盘 `UnrealEditor-multiplayer.dll` SHA-256 均为 `7fbe6e58f591bca60145c0ddced1f2f2360afc7015f3523db0e1519fb575376d`；公开字段 `loadedByRuntimeVerified=false`，不能说进程内加载模块也完成了哈希校验。各轮源码清单另存于本地完整报告。原始目录在 `Saved/NetworkValidation/` 下，依次为 `ride-motion-control-baseline-final`、`ride-motion-control-orderedvelocity-final`、`ride-motion-inertia-30hz-final`、`ride-motion-flow-regression-final`；本地日志与构建记录未全部公开。

### G.2 最终方案客户端逐阶段指标

距离单位 cm；“在基座帧”在跳跃阶段减少是正常的。下表只摘录客户端，完整的主机和客户端 24 段指标在 [inertia.json](../../../Tests/Evidence/2026-09-27/PlatformRide/inertia.json) 的 `motionMetrics` 中。服务器不接收客户端校正，其校正字段不适用，不能与客户端计数相加。

| 网络 | 动作 | 采样帧 / 在基座帧 | 校正次数 | 最大可比较校正误差 | 最大平台逐帧位移 | 最大角色相对逐帧位移 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Normal | 站立 | 179 / 179 | 0 | 0 | 12 | 0 |
| Normal | 行走 | 179 / 179 | 0 | 0 | 12 | 1.80 |
| Normal | 换向 | 176 / 176 | 0 | 0 | 11 | 0 |
| Normal | 跳跃 | 178 / 114 | 2 | 4.75 | 12 | 19.13 |
| Moderate | 站立 | 181 / 181 | 0 | 0 | 18 | 0 |
| Moderate | 行走 | 181 / 181 | 2 | 1.76 | 17 | 2.87 |
| Moderate | 换向 | 182 / 182 | 0 | 0 | 17 | 0 |
| Moderate | 跳跃 | 181 / 116 | 8 | 43.68 | 16 | 43.68 |
| Harsh | 站立 | 240 / 240 | 0 | 0 | 15 | 0 |
| Harsh | 行走 | 240 / 240 | 0 | 0 | 15 | 1.00 |
| Harsh | 换向 | 240 / 240 | 0 | 0 | 15 | 0 |
| Harsh | 跳跃 | 240 / 155 | 13 | 72.12 | 23 | 65.12 |

本轮这些阶段的 `comparableCorrections` 与校正次数相同，`baseChangeCorrections` 为 0；这不保证其他运行都无基座切换。最大可比较误差以对应历史 Move 的相同参考系计算，**不是画面拉回距离**。`maxRelativeStepCm` 含主动行走和跳跃位移，也不是抖动幅度。Normal/Moderate 实际约 44～45 FPS，Harsh 约 60 FPS，且各仅一次，不据此计算丢包与校正的定量因果或稳定性成功率。

### G.3 代码与引擎核查入口

| 核查对象 | 入口 | 本次用途 |
| --- | --- | --- |
| 平台运动与速度 | [Transporter](../../../Source/multiplayer/Mechanisms/multiplayerTransporterComponent.cpp)、[MovingPlatform](../../../Source/multiplayer/Mechanisms/multiplayerMovingPlatform.cpp) | PrePhysics、按实际位移更新速度、客户端接收速度、停下后清零 |
| 角色玩法规则 | [Character](../../../Source/multiplayer/Player/multiplayerCharacter.cpp) | 空中制动与普通跳跃手感的取舍 |
| 双端动作与观测 | [RideProbe](../../../Source/multiplayer/Testing/CoopPlatformRideProbe.cpp)、[测试 CMC](../../../Source/multiplayer/Testing/CoopRideTestCharacter.cpp) | 四段动作、PostPhysics 采样、历史 Move 校正统计；不替换 CMC 校正规则 |
| UE 5.5 源码 | `Character.cpp` 的 `MovementBaseUtility::AddTickDependency` / `GetMovementBaseVelocity`；`ActorReplication.cpp` 的 `GatherCurrentMovement` / `PostNetReceiveVelocity`；`CharacterMovementComponent.cpp` 的 `ApplyImpartedMovementBaseVelocity` / `PhysFalling` / `OnClientCorrectionReceived` | 核对更新依赖、基座速度传递、空中制动和校正对应关系；版本变化时需复查 |

`FormerBaseVelocityDecayHalfLife` 并非本次掉落的直接原因；它涉及 Root Motion 覆盖时的辅助基座速度处理。本项目的对照显示先是缺基座速度、随后是普通空中制动消掉已继承速度，不把名称相近的引擎参数当作修复依据。

## H. 自由跳跃后续定位与被否决的候选（2026-09-28 补记）

本节保留 G 之后追加的证据，不覆盖 G 的 13 次 / 72.12 cm 历史结果。原始目录均位于 `Saved/NetworkValidation/`，下表状态来自各自 `report.json`；这些本地原始包尚未随单文件复习文档公开。编辑器 `-game -NullRHI`、同机 Listen 主机与远端客户端、每阶段约四秒。所有案例都保留在报告中，不把被否决的候选删除后只展示有利数字。

| 原始目录 | 条件 | 案例数 / 状态 | 客户端 Jump：校正次数；最大可比较误差 cm |
| --- | --- | --- | --- |
| `20260927-134523-8db191a2` | 自由跳跃，Normal，30 Hz，运动平台 | 1 / passed | 2；7.0044 |
| `20260927-134821-8371ab83` | 自由跳跃，Moderate，30 Hz，运动平台 | 1 / passed | 8；40.0088 |
| `20260927-134201-3a66fe39` | 自由跳跃，Harsh，30 Hz，运动平台 | 1 / passed | 10；84.2316 |
| `20260928-031122-8676c0f0` | 自由跳跃，Harsh，60 Hz，运动平台 | 3 / passed | 11；64.5565 / 13；79.4676 / 13；77.4590 |
| `20260928-031710-4e33e598` | AirBase，Harsh，30 Hz，运动平台 | 1 / passed，但玩法方案否决 | 4；11.0753 |
| `20260928-081302-1c97b874` | 恢复自由跳跃，Harsh，30 Hz，静止平台 | 1 / passed | 0；0 |

### H.1 原始报告与构建指纹

| 原始目录 | report.json SHA-256 | 磁盘项目模块分组 |
| --- | --- | --- |
| `20260927-134523-8db191a2` | `73cbe27651dd536be736784d70c57849d58c4a3c0517b77b291570f39e06fe73` | M1 |
| `20260927-134821-8371ab83` | `b74e1163f322dc90cf9e5391928005480cc897a466bb2a2d2a4751399ebfba04` | M1 |
| `20260927-134201-3a66fe39` | `c689c64eae54aa30a1e051bd1a3b350a3af011f8482c00e75ec53d3355e88274` | M1 |
| `20260928-031122-8676c0f0` | `39a65626fefc09871cadd89db5639c66aab393886377c8d2f89a5676901bf61d` | M1 |
| `20260928-031710-4e33e598` | `225b62b6cf49668d57f7bff91144e0813b81c332c57659c65519e787f3877441` | M2 |
| `20260928-081302-1c97b874` | `dd3b28e04877b9cbd6c3539df6bec496368107843698b60cdb99edbb33e6280c` | M3 |

- M1：`da7bc16a412770fc0ad4897bb2a1061ac5499f94c803a3db48ed3f652b9c022f`，同 Move 观测与自由跳跃频率对照。
- M2：`2d96211d58b137c23a2dccbcb1e794ffaaa6c4759953e6b1896563c70a45178b`，保留空中基座候选；此模式已退出当前运行入口。
- M3：`e9075284300b14686d29cad5d5be65322ad8a6feab201db652f84bb54fbb4dbe`，撤回候选接入、增加 XYZ / 模拟时长记录与静止对照；本节只有静止 Harsh 单轮，未重新完成运动平台矩阵。

这些是磁盘 `UnrealEditor-multiplayer.dll` 指纹，不是进程内模块核验。不同源码及帧节奏不能混成同一修复前后统计，1 次与 3 次样本也不能直接给出稳定改善百分比。

### H.2 指标与边界

- `motionMetrics` 中按 `role=Client`、`metrics.phase=Jump` 读取校正；主机不接收客户端校正，其零计数不是优化成果。
- AirBase 四条校正的 `errorCm` 依次为 11.0753、0、0、0，`errorX` 均为 0。旧记录未拆 Y/Z，不能推定误差只在垂直方向；也不能声称四次可见拉回。
- 静止对照双端 `landedAfterJump=true`、`maxPlatformStepCm=0`。虽然沿用四阶段脚本，静止组的换向阶段不代表真的换向。一次零校正不等于弱网运动平台问题修复。
- 同一 Move 的完整配对与未配对标记在 `timeline-*-move-pairs.csv`、`timeline-*-all-moves.csv`；客户端采样和校正在 `*-samples.csv`、`*-corrections.csv`。零校正时没有对应校正 CSV 是允许情况，不能补造记录。
- M3 原始 Move 快照增加 XYZ、速度、移动模式和模拟时长；旧报告缺少的维度在导出时保持空值，不补 0。未保存的历史值不能靠现在重新跑一轮冒充。
- 所有这批结果都没有双端录像、完整角色模型或镜头平滑验收，没有测得屏幕拉回距离。该批结束时后续优化暂停；2026-09-29 恢复后的同构建频率对照见 I。不将 AirBase 的数值列为自由跳跃的最终优化收益。

## I. 平台频率与跳跃校正对照（2026-09-29）

本节对应[Move 时间戳专项报告](PlatformMoveTimestampValidation.md)的后续验证，不与 G/H 的不同构建混算改善幅度。原始根目录为 `Saved/NetworkValidation/PlatformFrequency-20260929/`。每个子目录均保留 `report.json`、双端日志、`timeline-*.svg`、采样及逐 Move CSV；这些文件是本地原始证据，不包含在早期 `source-data.zip` 中。

### I.1 设计与逐轮结果

同机 Listen Server + Client，Editor-game、NullRHI，`RideMotion / PlatformInertia / Moving`。双端 Harsh 参数为 `PktLag=200 / PktLagVariance=50 / PktLoss=5`；不是实测 RTT。顺序为 30、60、60、30、30、60，仅改变测试平台的 `NetUpdateFrequency` 和 `MinNetUpdateFrequency`。正式平台仍为 30 Hz；输入、空中惯性与 CMC 校正规则不变。

下表只统计客户端 Jump 段，误差单位 cm。“误差 > 1”是额外描述统计，不是通过阈值。校正总数包含位置误差为零的状态校正，不能换算成肉眼回拉次数。

| 原始子目录 | Hz 上限 | 校正次数 | 可比较误差 > 1 的次数 | 最大可比较误差 | 观察到的平台位置变化/秒 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `01-Harsh-30Hz` | 30 | 11 | 2 | 79.2585 | 14.9654 |
| `02-Harsh-60Hz` | 60 | 11 | 2 | 80.7417 | 17.6510 |
| `03-Harsh-60Hz` | 60 | 12 | 2 | 64.9211 | 16.9277 |
| `04-Harsh-30Hz` | 30 | 19 | 6 | 65.1718 | 13.6736 |
| `05-Harsh-30Hz` | 30 | 12 | 2 | 67.1594 | 14.4590 |
| `06-Harsh-60Hz` | 60 | 13 | 2 | 76.5380 | 17.6248 |

六轮 `status=passed`，双端动作断言通过并落回平台，十二个进程均正常退出，无脚本强杀。78 次校正全部唯一配对，各种截断计数为 0。NullRHI 结果不等于画面平滑验收，也不是 GPU 性能测试。

| 原始子目录 | 主机 / 客户端采样次数/秒 | 平台非零阶跃间隔中位数 / 最大值（ms） | 首次 JumpPressed Move 平台 X 差（cm） | 落地 Move 平台 X 差（cm） |
| --- | --- | --- | ---: | ---: |
| `01-Harsh-30Hz` | 44.93 / 45.64 | 62.64 / 140.45 | 79.2411 | 71.4638 |
| `02-Harsh-60Hz` | 45.01 / 45.25 | 46.34 / 124.57 | 69.3761 | 80.7417 |
| `03-Harsh-60Hz` | 45.46 / 45.56 | 46.99 / 188.24 | 64.9210 | 55.7135 |
| `04-Harsh-30Hz` | 46.91 / 47.48 | 62.96 / 203.85 | 65.1718 | 69.9707 |
| `05-Harsh-30Hz` | 45.08 / 45.37 | 63.09 / 140.98 | 67.1594 | 62.0654 |
| `06-Harsh-60Hz` | 45.44 / 46.42 | 62.11 / 124.32 | 76.5379 | 69.7304 |

六轮首次记录到按下跳跃的 Move，两端角色 X 速度差绝对值均小于 `0.00001 cm/s`。这里只检验 X 分量，不扩展为三维速度完全相同。平台 X 差与校正误差是不同变量，不要求每行落地差都等于该轮最大校正。

### I.2 统计口径与判断边界

- 采样次数/秒为 `frameSamples / elapsedSeconds`，不是渲染 FPS。进程上限 60 FPS 不代表稳定达到 60，也不代表 60 Hz 复制实际发出 60 包/秒。
- 观察到的平台位置变化/秒为 `platformPositionChanges / elapsedSeconds`；非零阶跃间隔取 Jump `timelineSamples` 中 `platformStepCm > 0.01` 的相邻采样时间差。它们都不是精确的 NetDriver 收包频率或收包间隔，单帧可能处理多次更新。
- 同 Move 配对以 `abs(ClientMoveTimeStamp - ServerMoveTimeStamp) < 0.001` 且仅一个候选为准。首次 JumpPressed Move 不保证正好是内部状态切换前的那一帧；落地选起跳后首次 `!startBased && endBased` 的记录。
- 平台差来自同一 Move 在双端各自处理时的快照，不是同一物理时刻的测量；不能除以平台速度便声称测出了单向网络延迟。
- 30/60 Hz 的校正次数中位数均为 12；每轮最大误差的中位数分别为 67.1594 / 76.5380 cm。这不是合并全部事件后的 P95。
- 每档只有三轮，且没有控制丢包随机种子。本条件下未见提高频率的稳定收益，不宣称 60 Hz 必然更差或频率优化普遍无用。未采集本轮带宽与 CPU 收益，不引用旧规模压测数据冒充。

### I.3 构建与报告指纹

运行前 Editor 编译检查为 up to date，运行脚本 SelfTest 通过。六轮源码、配置和磁盘模块指纹一致：

| 对象 | 指纹 |
| --- | --- |
| Git 基准（工作区有未提交改动，不代表只运行该提交） | `8ff9ea15ff8f667a695ec17d56d3133758060afc` |
| 报告记录的源码清单指纹 | `5c1b5a62ae536ea91ea26638b4b667ce45a0b5c3db22a9d9bd9e96c9727eb517` |
| 磁盘项目 DLL SHA-256 | `582f73865a2632be8f9bb8e35e676562bf1dee59b4948e4b4f93f4bc4210f61d` |
| 玩法 JSON SHA-256 | `97b67b8238706e2f2765446c93b512dbcb39153a3e32676bc7c5b9316f8c0ce0` |

`loadedByRuntimeVerified=false`，仍不声称完成进程内加载模块哈希核验。

| 原始子目录 | report.json SHA-256 |
| --- | --- |
| `01-Harsh-30Hz` | `8d5e01bf818c76dc488c12bf7789507e80e2213c1e50fd273f053278ffd25409` |
| `02-Harsh-60Hz` | `6270f12eea1eb14717afe70679e6b06de09b57296180b21a3034936d24a5469c` |
| `03-Harsh-60Hz` | `a5caf76334e81e00aee38aec4491e2499e78218729b501c17f8a1e8e01ab492f` |
| `04-Harsh-30Hz` | `2aef789c1985087a50e14b04701163a73b8feb5390e2854e8205425fd0fae40c` |
| `05-Harsh-30Hz` | `de0bc286f83b0fbd50264ccec859474b1e9972211dd6347ccf8052d1405f5abc` |
| `06-Harsh-60Hz` | `fd9da7d437e7e19dca7785c92f1bc6da7c44dae26033dbf76199d5a4a5062eb7` |

### I.4 复现入口

在工程根目录的 PowerShell 中执行现有脚本；使用新目录，避免覆盖本次证据。重新运行会产生新时间戳与新的模拟丢包结果，不期待逐数字复现。

```powershell
./Scripts/RunMultiplayerNetworkTests.ps1 -SelfTest
if ($LASTEXITCODE -ne 0) { throw 'Runner self-test failed.' }

$experimentRoot = 'Saved/NetworkValidation/PlatformFrequency-Recheck'
if (Test-Path -LiteralPath $experimentRoot) { throw 'Choose a new output directory.' }
$frequencies = @(30, 60, 60, 30, 30, 60)
for ($runIndex = 0; $runIndex -lt $frequencies.Count; $runIndex++) {
    $frequency = $frequencies[$runIndex]
    $runDirectory = Join-Path $experimentRoot ('{0:D2}-Harsh-{1}Hz' -f ($runIndex + 1), $frequency)
    ./Scripts/RunMultiplayerNetworkTests.ps1 `
        -Scenario RideMotion -Profiles Harsh -Repeat 1 `
        -PlatformNetHz $frequency -PlatformSyncMode PlatformInertia `
        -PlatformMotion Moving -OutputDirectory $runDirectory
    if ($LASTEXITCODE -ne 0) { throw "Run failed: $runDirectory" }
}
```

当前结论是保持正式平台 30 Hz，下一项验证运动段同步与时间一致性；本轮没有实现该候选，也未宣布自由跳跃问题已解决。
