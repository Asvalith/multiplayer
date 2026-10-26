// Copyright Epic Games, Inc. All Rights Reserved.
// [职责] 组件保存倒地与救援占用，服务器 GAS 能力持有引导任务与取消生命周期。
// 属性仍由 ASC 结算，客户端只消费复制状态；不在这里复制另一套生命或移动系统。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Abilities/GameplayAbility.h"
#include "multiplayerCombatCoop.generated.h"

class AmultiplayerCombatCharacter;

// 只管理合作生命流程：倒地、失血期限、救援占用；数值与技能仍通过角色 ASC。
UCLASS(ClassGroup=(Combat), meta=(BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerCombatCoopComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	// 开启状态复制，合作流程只由事件和定时器驱动。
	UmultiplayerCombatCoopComponent();
	UPROPERTY(EditDefaultsOnly, Category="Combat") bool bEnableDowned = false;
	// 返回已提交的倒地状态；它与终局死亡分开。
	UFUNCTION(BlueprintPure) bool IsDowned() const { return bDowned; }
	// 救援目标存在表示当前正在引导，不表示救援已经成功。
	UFUNCTION(BlueprintPure) bool IsRescuing() const { return RescueTarget != nullptr; }
	// 根据服务器时钟计算失血余时，客户端不独立延长期限。
	UFUNCTION(BlueprintPure) float GetDownedRemaining() const;
	// 从复制的截止时间推算显示进度，不触发救援结算。
	UFUNCTION(BlueprintPure) float GetRescueProgress() const;
	// 服务器在致死结算中尝试倒地，先提交状态防止属性通知重入。
	bool TryEnterDowned();
	// 重连只恢复剩余倒地时间，已超时则直接进入死亡。
	void RestoreDowned(float Remaining);
	// 无副作用检查施救者资格以及是否存在可占用的近邻队友。
	bool CanStartRescue() const;
	// 服务器选定目标并取得独占占用，之后才通知表现。
	bool BeginRescue();
	// 引导期间复核生命、距离、视线和占用归属。
	bool IsRescueValid() const;
	// 服务器确认到时并先释放占用，再恢复目标生命，防止重复结算。
	bool CompleteRescue();
	// 只释放自己的救援占用，同时清除引导显示状态。
	void EndRescue();
	// 死亡或退出时解除双方救援关系，并清除失血定时器。
	void ClearLifeState();
protected:
	// 退出世界时结束合作关系，避免目标仍保留旧施救者。
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	// 复制生命阶段和截止时间，不逐帧复制倒计时。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Props) const override;
private:
	// 组件只操作所属战斗角色，不接受外部角色替代拥有者。
	AmultiplayerCombatCharacter* OwnerCharacter() const;
	// 在可达且未被他人占用的倒地队友中选择最近者。
	AmultiplayerCombatCharacter* FindRescueTarget() const;
	// 倒地只在仍有能战斗的玩家队友时开放。
	bool HasActivePartner() const;
	// 统一检查救援阵营、目标状态、距离及遮挡，不取得占用。
	bool CanReach(const AmultiplayerCombatCharacter* Target) const;
	// 服务器失血到期后执行终局死亡，不再次进入倒地。
	void BleedOut();
	// 权威更新和复制通知共用入口，刷新标签、权限及生命表现。
	UFUNCTION() void NotifyLifeState();
	UPROPERTY(ReplicatedUsing=NotifyLifeState) bool bDowned = false;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) double DownedUntil = 0;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) TObjectPtr<AmultiplayerCombatCharacter> RescueTarget;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) double RescueUntil = 0;
	// [边界] 被救者只在服务器持有占用者弱引用；施救者复制的 Target/Until 只供各端显示引导。
	TWeakObjectPtr<AmultiplayerCombatCharacter> CurrentRescuer;
	FTimerHandle BleedoutTimer;
};

// 引导是服务器 GAS 能力；任务、占用和移动限制在同一个 EndAbility 收尾。
UCLASS()
class MULTIPLAYER_API UmultiplayerRescueAbility : public UGameplayAbility
{
	GENERATED_BODY()
public:
	// 使用服务器独占执行和实例化能力，救援标签由 GAS 生命周期持有。
	UmultiplayerRescueAbility();
	// 合并合作组件的目标资格与 GAS 原生准入，不在预检中占用目标。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		const FGameplayTagContainer* Source=nullptr, const FGameplayTagContainer* Target=nullptr,
		FGameplayTagContainer* Reasons=nullptr) const override;
	// 复用集中标签关系，救援目标和资源规则不混入标签解析。
	virtual bool DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
		const FGameplayTagContainer* Source=nullptr, const FGameplayTagContainer* Target=nullptr,
		FGameplayTagContainer* Reasons=nullptr) const override;
protected:
	// 取得服务器占用后创建延时任务和有效性巡检；同步取消后不继续建任务。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
	// 清定时器、放占用，再交还 GAS；锁定或同步重入时不能重复收尾。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		FGameplayAbilityActivationInfo Activation, bool bReplicate, bool bCancelled) override;
private:
	// 引导到时只尝试一次权威结算，失败与成功都经 EndAbility 释放资源。
	UFUNCTION() void FinishRescue();
	// 低频复核引导资格，目标失效、离开或失去占用即取消。
	void CheckRescue();
	FTimerHandle ValidationTimer;
	bool bEndingRescue = false;
};
