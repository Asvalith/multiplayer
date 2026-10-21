#pragma once

// 开发态技能录制工具：通过真实输入采集双端视口与状态证据，不修改战斗规则。
// 仅显式开发演示启用；不授予技能、不改数值或关卡，不替代正式规则与弱网验收。
void StartCoopSkillPresentationReview();
// 模块退出时释放按键、截图委托和 Ticker，未完成录制明确标记中止。
void StopCoopSkillPresentationReview();
