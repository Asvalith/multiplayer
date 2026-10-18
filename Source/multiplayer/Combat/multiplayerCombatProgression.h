// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/SaveGame.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "Combat/multiplayerCombatConfig.h"
#include "multiplayerCombatProgression.generated.h"

class AmultiplayerCombatCharacter;
class UAbilitySystemComponent;

UENUM(BlueprintType)
enum class EMultiplayerCombatPassive : uint8 { None, Fortitude, Power };

// 成长快照只包含长期数据，不保存生命、位置、冷却或本次战斗已经付出的资源。
USTRUCT(BlueprintType)
struct FmultiplayerCombatProgressionState
{
	GENERATED_BODY()
	UPROPERTY(SaveGame, BlueprintReadOnly) int32 Version = 1;
	UPROPERTY(SaveGame, BlueprintReadOnly) FGuid ProfileId;
	UPROPERTY(SaveGame, BlueprintReadOnly) int32 Level = 1;
	UPROPERTY(SaveGame, BlueprintReadOnly) int32 Experience = 0;
	UPROPERTY(SaveGame, BlueprintReadOnly) int32 SkillPoints = 2;
	// 原四技能保留一级；冲刺需要花点从零解锁，装配用于调整输入槽位。
	UPROPERTY(SaveGame, BlueprintReadOnly) TArray<int32> ActiveLevels{1, 1, 1, 1, 0};
	UPROPERTY(SaveGame, BlueprintReadOnly) TArray<EMultiplayerCombatSkill> Slots{
		EMultiplayerCombatSkill::Attack, EMultiplayerCombatSkill::Defend, EMultiplayerCombatSkill::Heal,
		EMultiplayerCombatSkill::Area, EMultiplayerCombatSkill::Dash};
	UPROPERTY(SaveGame, BlueprintReadOnly) TArray<int32> PassiveLevels{0, 0};
	UPROPERTY(SaveGame, BlueprintReadOnly) EMultiplayerCombatPassive EquippedPassive = EMultiplayerCombatPassive::None;
	// 只保留最近 64 个服务器奖励标识，覆盖当前战斗/重连窗口，不声称永久保存所有历史奖励。
	UPROPERTY(SaveGame, BlueprintReadOnly) TArray<FGuid> ClaimedRewards;
};

// 原生无限效果定义可正常参与 GAS 复制；成长和被动各持一个独立句柄，不改属性基础值。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatProgressionEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerCombatProgressionEffect();
};

// 路径只由服务器生成；存档没有客户端提交的经验值、文件名或战斗资源。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatProgressionSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY(SaveGame) FmultiplayerCombatProgressionState Progression;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerProgressionChanged);

// 一个组件集中管理技能授予、成长、被动和长期存档；ASC 仍负责执行与网络预测。
UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerCombatProgressionComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UmultiplayerCombatProgressionComponent();
	// 由角色在 ASC ActorInfo 初始化之后显式调用，重复调用不会重复授予。
	void InitializeLoadout();
	const FmultiplayerCombatProgressionState& GetState() const { return State; }
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") FmultiplayerCombatProgressionState CaptureState() const { return State; }
	bool RestoreState(const FmultiplayerCombatProgressionState& Snapshot);
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") bool ActivateSlot(int32 Slot);
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") EMultiplayerCombatSkill GetSlotSkill(int32 Slot) const;
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetSkillLevel(EMultiplayerCombatSkill Skill) const;
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetSlotLevel(int32 Slot) const;
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetPassiveLevel(EMultiplayerCombatPassive Passive) const;
	// 客户端只能请求枚举和槽位，最终等级、点数和角色资格由拥有该组件的服务器重新检查。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestLearnSkill(EMultiplayerCombatSkill Skill);
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestEquipSkill(int32 Slot, EMultiplayerCombatSkill Skill);
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestLearnPassive(EMultiplayerCombatPassive Passive);
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestEquipPassive(EMultiplayerCombatPassive Passive);
	// 只有服务器玩法代码能发奖；重复死亡回调使用同一个稳定标识，不再重复增加经验。
	bool AwardExperience(int32 Amount, FGuid RewardId);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat|Progression") bool SaveProgress();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat|Progression") bool LoadProgress();
	UPROPERTY(BlueprintAssignable, Category = "Combat|Progression") FmultiplayerProgressionChanged OnProgressionChanged;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_State) FmultiplayerCombatProgressionState State;
	UFUNCTION() void OnRep_State();
	UFUNCTION(Server, Reliable) void ServerLearnSkill(EMultiplayerCombatSkill Skill);
	UFUNCTION(Server, Reliable) void ServerEquipSkill(int32 Slot, EMultiplayerCombatSkill Skill);
	UFUNCTION(Server, Reliable) void ServerLearnPassive(EMultiplayerCombatPassive Passive);
	UFUNCTION(Server, Reliable) void ServerEquipPassive(EMultiplayerCombatPassive Passive);
	AmultiplayerCombatCharacter* Character() const;
	bool CanRequest() const;
	bool CanChangeLoadout(bool bRequireReady) const;
	static bool IsStateValid(const FmultiplayerCombatProgressionState& Candidate);
	bool CommitState(const FmultiplayerCombatProgressionState& Candidate);
	void RefreshAbilities();
	void RefreshBonuses();
	FActiveGameplayEffectHandle ApplyBonus(float Health, float Attack);
	FString SaveSlot() const;
	TArray<FGameplayAbilitySpecHandle> GrantedAbilities;
	FActiveGameplayEffectHandle GrowthHandle;
	FActiveGameplayEffectHandle PassiveHandle;
	int32 AppliedLevel = 1;
	EMultiplayerCombatPassive AppliedPassive = EMultiplayerCombatPassive::None;
	int32 AppliedPassiveLevel = 0;
	bool bInitialized = false;
	// 只保护同步 GAS 更新过程，通知发出前解除，外部监听者始终读取已经提交的状态。
	bool bUpdatingLoadout = false;
};
