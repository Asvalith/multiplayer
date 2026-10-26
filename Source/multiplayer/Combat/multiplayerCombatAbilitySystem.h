// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "multiplayerCombatAbilitySystem.generated.h"

// 仅扩展 GAS 的标签关系、失败反馈、动画认领与恢复窄入口；技能阶段和资源账本仍归原生 GAS。
// UE5.5禁止客户端任意移除权威GE；只开放“结束本次本地施法”的窄入口，不开放资源/CD移除。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatAbilitySystem : public UAbilitySystemComponent
{
	GENERATED_BODY()
public:
	// 查询集中关系，不提交资源/目标；所属端预检和权威能力准入共用同一份规则。
	bool CheckAbilityTagRelationships(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* Reasons = nullptr) const;
	// 只扩展 GAS 原生阻断/取消参数；开始和结束的引用计数仍由 Super 成对管理。
	virtual void ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags, UGameplayAbility* RequestingAbility,
		bool bEnableBlockTags, const FGameplayTagContainer& BlockTags, bool bExecuteCancelTags,
		const FGameplayTagContainer& CancelTags) override;
	// 服务器按句柄、拥有者预测端按本次预测键解除施法锁，绝不移除其他施法或冷却。
	void EndCasting(FActiveGameplayEffectHandle ServerHandle, const FPredictionKey& ActivationKey);
	// 防御能力已结束时仍可查询其持续状态；重连恢复的无预测键效果也属于可取消的当前防御。
	bool GetDefensePredictionKey(FPredictionKey& OutKey) const;
	// 只由服务器结束对应次防御/弹反状态；不退资源、不清冷却，也不撤销已经授予的护盾。
	void EndDefense(const FPredictionKey& ActivationKey);
	// 重连恢复只调整新效果的下一跳时刻，沿用 GE 自己的 TimerHandle 和清理链路。
	void SetPeriodicEffectDelay(FActiveGameplayEffectHandle Handle, float FirstDelay);
	// 服务器把正式拒绝原因发给技能拥有者，不用一次新的资格查询代替真实失败结果。
	virtual void NotifyAbilityFailed(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability,
		const FGameplayTagContainer& FailureReason) override;
	// UE5.5 的动画开始通知早于 ASC 认领；在这里统一处理同步取消/替换，能力不各自补停。
	// 复用现有动作/激活键处理拒绝；不为延迟动画重新开启可预测的资源提交窗口。
	virtual float PlayMontage(UGameplayAbility* AnimatingAbility, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* Montage, float InPlayRate, FName StartSectionName = NAME_None, float StartTimeSeconds = 0.f) override;
protected:
	// 远程定向技能必须随激活提交瞄准；服务器 AI 的直接激活不经过此 RPC 入口。
	virtual void InternalServerTryActivateAbility(FGameplayAbilitySpecHandle Handle, bool InputPressed,
		const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData) override;
private:
	// 只标识动画播放请求，不替代能力激活版本或网络预测键。
	uint32 MontagePlayRevision = 0;
};
