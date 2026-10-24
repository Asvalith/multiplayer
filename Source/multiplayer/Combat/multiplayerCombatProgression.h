// Copyright Epic Games, Inc. All Rights Reserved.
// 成长模块集中保存技能等级、槽位和被动，只有服务器可以提交成长快照。
// 能力执行交给 ASC，永久加成使用独立 GE 句柄；长期存档不混入本局战斗资源。

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
	// 定义可撤销的永久属性加成，幅度由服务器 Spec 提供。
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
	// 关闭逐帧更新，初始化固定技能授予记录并启用复制。
	UmultiplayerCombatProgressionComponent();
	// 由角色在 ASC ActorInfo 初始化之后显式调用，重复调用不会重复授予。
	void InitializeLoadout();
	// C++ 只读访问当前快照；修改必须经过服务器提交入口。
	const FmultiplayerCombatProgressionState& GetState() const { return State; }
	// 向蓝图返回成长副本，不暴露可修改的权威状态引用。
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") FmultiplayerCombatProgressionState CaptureState() const { return State; }
	// 服务器加载或重连接纳已验证快照，不复用旧能力实例。
	bool RestoreState(const FmultiplayerCombatProgressionState& Snapshot);
	// 经角色统一激活入口请求槽位技能，返回值不是服务器命中确认。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") bool ActivateSlot(int32 Slot);
	// 读取槽位对应技能；无效槽位的显示默认值不能用于实际激活。
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") EMultiplayerCombatSkill GetSlotSkill(int32 Slot) const;
	// 未学习或无效技能返回零，不在查询时自动授予。
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetSkillLevel(EMultiplayerCombatSkill Skill) const;
	// 由同一槽位映射读取技能等级，HUD 不另存一份配装。
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetSlotLevel(int32 Slot) const;
	// None 或无效被动返回零，学习等级与是否装配分开。
	UFUNCTION(BlueprintPure, Category = "Combat|Progression") int32 GetPassiveLevel(EMultiplayerCombatPassive Passive) const;
	// 客户端只能请求枚举和槽位，最终等级、点数和角色资格由拥有该组件的服务器重新检查。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestLearnSkill(EMultiplayerCombatSkill Skill);
	// 只请求槽位交换，服务器复核已学习状态和重复槽位。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestEquipSkill(int32 Slot, EMultiplayerCombatSkill Skill);
	// 只请求学习某个被动，不接收客户端点数或等级值。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestLearnPassive(EMultiplayerCombatPassive Passive);
	// 请求切换单个被动；None 表示卸下，服务器决定最终效果。
	UFUNCTION(BlueprintCallable, Category = "Combat|Progression") void RequestEquipPassive(EMultiplayerCombatPassive Passive);
	// 只有服务器玩法代码能发奖；重复死亡回调使用同一个稳定标识，不再重复增加经验。
	bool AwardExperience(int32 Amount, FGuid RewardId);
	// 服务器写入版本化长期成长，不保存位置、生命和冷却。
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat|Progression") bool SaveProgress();
	// 读取当前服务器档案，失败保持现有成长和战斗状态。
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat|Progression") bool LoadProgress();
	UPROPERTY(BlueprintAssignable, Category = "Combat|Progression") FmultiplayerProgressionChanged OnProgressionChanged;

protected:
	// 只撤销本组件持有的效果句柄和能力授予。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 成长快照仅复制给拥有者，其他玩家通过属性和动作观察结果。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_State) FmultiplayerCombatProgressionState State;
	// 已提交或复制到达后通知界面，不在通知中重新授予能力。
	UFUNCTION() void OnRep_State();
	// 服务器检查技能点、等级和活动能力，再提交学习后的完整快照。
	UFUNCTION(Server, Reliable) void ServerLearnSkill(EMultiplayerCombatSkill Skill);
	// 服务器交换已有技能的两个槽位，不允许复制同一技能占槽。
	UFUNCTION(Server, Reliable) void ServerEquipSkill(int32 Slot, EMultiplayerCombatSkill Skill);
	// 服务器扣点学习被动，不自动改变当前装配。
	UFUNCTION(Server, Reliable) void ServerLearnPassive(EMultiplayerCombatPassive Passive);
	// 服务器替换或撤销本组件的被动效果，不影响其他来源加成。
	UFUNCTION(Server, Reliable) void ServerEquipPassive(EMultiplayerCombatPassive Passive);
	// 从组件拥有者取得战斗角色，ASC 与存档都不接受外部角色替代。
	AmultiplayerCombatCharacter* Character() const;
	// 本地只有拥有者可请求，最终资格仍在服务器复核。
	bool CanRequest() const;
	// 提交中或有活动能力时禁止换装；恢复入口可不要求战斗就绪。
	bool CanChangeLoadout(bool bRequireReady) const;
	// 校验快照版本、点数收支和槽位唯一性，不自动修补损坏数据。
	static bool IsStateValid(const FmultiplayerCombatProgressionState& Candidate);
	// 先提交完整成长，再更新 GAS，最后通知外部，阻止半提交状态重入。
	bool CommitState(const FmultiplayerCombatProgressionState& Candidate);
	// 只授予或更新本组件记录的技能 Spec，不重建其他业务能力。
	void RefreshAbilities();
	// 批量替换成长和被动 GE，避免中间上限引起错误资源钳制。
	void RefreshBonuses();
	// 用 SetByCaller 幅度创建独立永久 GE，移除句柄即可撤销。
	FActiveGameplayEffectHandle ApplyBonus(float Health, float Attack);
	// 由服务器决定固定单机槽或档案槽名，不接收客户端文件路径。
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
