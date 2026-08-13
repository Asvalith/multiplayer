# GAS M1 正式输入与基础 HUD 验收报告

> 分支：`coop-GAS`
>
> 日期：2026-08-13
>
> 前置基线：M0 Target Enemy、服务器伤害结算和双端 Health 复制已通过

本报告只把已执行检查标记为通过。双窗口交互条目在用户实际操作确认前保持“待验证”。

## 1. 本阶段交付

- `UmultiplayerInputConfig`：数据化 `InputAction -> InputTag`。
- `IMC_GAS_Abilities`：鼠标左键伤害、`Q` 治疗、`E` 免疫。
- `DA_GAS_DefaultAbilitySet`：三个技能及 `InitStats` 初始化效果。
- `UmultiplayerGASHUDWidget`：Health、Energy、Cooldown、Immune 状态展示。
- `UmultiplayerGASHUDPresenterComponent`：仅本地创建 HUD，负责重复初始化防护和 EndPlay 解绑。
- `CreateGASM1Assets.py`：可重复执行的资产生成/接线脚本。

## 2. 自动化和构建证据

| 编号 | 检查 | 实际结果 | 状态 |
|---|---|---|---|
| M1-PRE-01 | Editor Development 编译 | 2026-08-13 通过 | 通过 |
| M1-PRE-02 | Win64 Development Game 编译 | 2026-08-13 通过 | 通过 |
| M1-PRE-03 | InputConfig 三个动作与标签 | 自动化加载资产并检查 3 项 | 通过 |
| M1-PRE-04 | MappingContext 三个键位 | 自动化加载资产并检查 3 项 | 通过 |
| M1-PRE-05 | AbilitySet 三能力和 InitStats | 自动化检查 3 Ability + 1 Effect | 通过 |
| M1-PRE-06 | Character 蓝图默认值接线 | 自动化确认 InputConfig、IMC、AbilitySet 指针 | 通过 |
| M1-PRE-07 | `multiplayer.GAS.Configuration` | 找到 1 项，`Result={成功}` | 通过 |

## 3. 双窗口运行矩阵

| 编号 | 操作 | 预期 | 状态 |
|---|---|---|---|
| M1-UI-01 | 启动 Host 和 Client | 两个本地玩家各自只出现一个 GAS HUD | 待验证 |
| M1-UI-02 | 按 `E` | Energy 下降；HUD 显示 Immune 和 Immunity CD，持续时间结束后 Immune 消失 | 待验证 |
| M1-UI-03 | 按 `Q` | Energy 下降；HUD 显示 Heal CD；Health 不超过 MaxHealth | 待验证 |
| M1-IN-01 | 按 `7` 后鼠标左键攻击 | 正式 InputAction 通过 InputTag 激活 Damage；目标扣血，Energy 和 Damage CD 更新 | 待验证 |
| M1-IN-02 | 运行 Developer Harness 自动输入 | 仍走相同 InputTag/AbilitySpec 链，不在 Character 复制技能逻辑 | 重构后 M5 `20260814_015249` 自动序列完成；正式输入的非 Headless 人工验收仍待补 |
| M1-LIFE-01 | 重复触发 `InitializeAbilitySystem` | HUD 不重复创建，属性/Tag 委托不重复绑定 | 代码门禁和自动化配置通过；运行待验证 |
| M1-LIFE-02 | Client 退出 | Widget 清理委托，Host 不崩溃 | 待验证 |

## 4. 真实问题记录

### GAS-M1-001：编辑器资产脚本无法直接构造 GameplayTag

- 现象：Python 创建 InputConfig 时，`GameplayTag(name=...)` 和写 `tag_name` 均失败。
- 根因：UE5.5 Python 中 `FGameplayTag` 没有构造参数，反射字段 `TagName` 只读。
- 排除项：不是原生 Tag 未注册；配置自动化已确认 Tag 有效。
- 工具：Python commandlet traceback、`dir(unreal.GameplayTag)` 只读反射检查。
- 最终方案：使用标准 `GameplayTag.import_text('(TagName="...")')` 导入已注册原生 Tag。
- 验证：脚本输出 `GAS_M1_ASSETS_READY`；资产存在且自动化加载内容通过。

### GAS-M1-002：Enhanced Input 附加参数回调签名不匹配

- 现象：绑定 `InputAction + GameplayTag` 时 C++ 编译找不到 `BindAction` 重载。
- 根因：UE5.5 的附加参数模板要求回调接收按值 `FGameplayTag`，第一版使用 `const FGameplayTag&`。
- 最终方案：输入回调改为按值，Character 仍只把 Tag 转发给 ASC。
- 验证：Editor/Game 两目标编译通过。

## 5. 当前边界

- HUD 已可直接运行，但视觉是 C++ 基础布局；作品集最终皮肤仍可用 WBP 子类替换。
- 技能正式配置与自动化统一来自 AbilitySet；缺失配置会 fail-fast，不再维护 C++ 授予 fallback。
- 当前 Cooldown HUD 显示“是否冷却”，尚未显示精确剩余秒数。
- M1 退出仍需完成上面的双窗口人工运行矩阵。
