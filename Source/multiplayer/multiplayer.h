// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/*
 * 项目阅读顺序：GameInstance 管连接；GameMode 判规则；GameState 复制共享结果；
 * Key/Socket、Plate/Gate、Platform/Transporter 负责具体机关；Controller 转交所属请求，HUD/Widget 负责本地表现。
 * CombatCharacter 协调战斗入口，ASC 管激活与预测，Ability/Effect/AttributeSet 分别管理行为、效果与属性结算。
 *
 * 注释约定：
 * (*)  表示常见面试考点和架构选择，例如 GameMode/GameState 分工、RPC 与属性复制的区别。
 * (**) 表示实现时容易出错的边界，例如重复 Overlap、同步回调重入和外部 Delegate 生命周期。
 * 头文件说明使用前提与职责，实现处说明状态变化和选择原因；注释应以当前代码能力为限。
 */
