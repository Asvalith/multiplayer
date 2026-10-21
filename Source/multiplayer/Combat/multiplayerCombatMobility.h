// Copyright Epic Games, Inc. All Rights Reserved.
// 位移技能模块只定义冲刺能力及其消耗、冷却效果，复用通用 GAS 准入与取消。
// 实际推进交给原生 RootMotionSource 和 CMC，不手写瞬移回滚或第二套移动复制。

#pragma once

#include "CoreMinimal.h"
#include "Combat/multiplayerCombatAbility.h"
#include "multiplayerCombatMobility.generated.h"

// 冲刺仍使用同一份能量属性与预测账本，不直接改客户端数值。
UCLASS()
class MULTIPLAYER_API UmultiplayerDashCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 使用通用 Data.Cost 给能量加负值，幅度由本次提交规则决定。
	UmultiplayerDashCostEffect();
};

// 独立冲刺冷却标签参与原生 GAS 检查，持续时间在提交时覆盖。
UCLASS()
class MULTIPLAYER_API UmultiplayerDashCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 定义持续型冷却效果和专用标签，不额外保存冷却计时器。
	UmultiplayerDashCooldownEffect();
};

// GAS 预测激活与消耗，原生 RootMotionSource 交给 CMC 处理碰撞、移动包与服务器纠正。
UCLASS()
class MULTIPLAYER_API UmultiplayerDashAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	// 配置事件、冲刺标签及 GE 类型，具体阻断关系读取集中配置。
	UmultiplayerDashAbility();
protected:
	// 固定本次水平朝向与可达预览，提交成功且未被同步取消后才施加位移源。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
private:
	// 任务自然完成时结束能力，GAS 任务销毁负责清理自己的位移源。
	UFUNCTION() void FinishDash();
};
