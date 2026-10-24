# 运行依赖与蓝图边界

可直接审阅 UE5.5 源码、配置、模块目标及测试。GameplayAbilities、CommonUI 等是 UE 插件，不是随项目自研的网络框架。

完整运行需要独立恢复并授权：

- `/Game/Combat/Maps/CoopEscape`、`CombatTest`、地址菜单及角色、输入、HUD 蓝图。
- 战斗 Data Asset、技能关系、动画/Montage、GameplayCue、特效、音频、敌人和机关蓝图实际引用。
- 原埃及关卡、碰撞、导航和机关位置。旧原型 Escape_* 布局不能冒充当前地图的通关验证。
- `Content/Config/Gameplay.json`。结构示例见 `Examples/Gameplay.json`，运行时仍只有既有配置加载器一处提交，不增加第二份可变参数。

`Scripts/CreateCombatAssetsInEditor.py` 与 C++ 资产 Commandlet 展示项目蓝图/数据的生成契约；审计脚本读取真实接线。资源生成不能替代授权或缺失源素材。资产集成测试、联网实测和画面检查必须在一致资源恢复后执行。

main 不含上述 Content，因此直接启动默认地图或完整自动化可能失败；这不是零成本运行或可玩构建承诺。源码中多个 cpp 组织同一 Character 的实现也不等于多套玩法组件。
