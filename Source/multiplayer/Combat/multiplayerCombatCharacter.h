// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffectTypes.h"
#include "Engine/NetSerialization.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatTargeting.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"
#include "multiplayerCombatCharacter.generated.h"

class UAbilitySystemComponent;
class UAnimMontage;
class UGameplayAbility;
class UmultiplayerCombatConfig;
class UmultiplayerCombatCoopComponent;
struct FStreamableHandle;
class UmultiplayerCombatAttributes;
struct FOnAttributeChangeData;
struct FGameplayTag;
struct FGameplayTagContainer;

/** 白名单持续效果的服务器快照；Spec 保留原来源，周期额外保存距下一跳的真实时间。 */
struct FmultiplayerCombatResumeEffect
{
	FGameplayEffectSpecHandle Spec;
	float Remaining = 0.f;
	float NextTickDelay = 0.f;
	float RawDamagePerTick = 0.f;
};

/** 仅服务器保存的短时重连快照，不从客户端接收，也不复制整份历史状态。 */
struct FmultiplayerCombatResumeState
{
	float Health = 100.0f;
	float Energy = 100.0f;
	float Shield = 0.0f;
	float AttackCooldown = 0.0f;
	float DefenseCooldown = 0.0f;
	float HealCooldown = 0.0f;
	float DefenseRemaining = 0.0f;
	float AreaCooldown = 0.0f;
	float StunRemaining = 0.0f;
	float DashCooldown = 0.f;
	bool bDowned = false;
	float DownedRemaining = 0.f;
	FmultiplayerCombatProgressionState Progression;
	// 只保存灼烧与定身，不恢复正在执行的能力、施法锁或任意第三方效果。
	TArray<FmultiplayerCombatResumeEffect> PersistentEffects;
	bool bDead = false;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

UENUM(BlueprintType)
enum class EMultiplayerCombatTeam : uint8
{
	Players,
	Enemies
};

UENUM()
enum class EMultiplayerCombatAnimation : uint8
{
	Attack,
	Heal,
	Area
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FmultiplayerCombatStatsChanged,
	float, Health, float, MaxHealth, float, Energy, float, MaxEnergy);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FmultiplayerDefenseStateChanged, bool, bIsDefending);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FmultiplayerCombatMessage, FText, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerCombatStateChanged);

/** 玩家与敌人共用的战斗状态；技能交给 GAS，移动仍交给 CharacterMovement。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCombatCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AmultiplayerCombatCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// GAS 从接口获取角色的 ASC；数值接口供血条和技能查询，不在 UI 中修改属性。
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetHealth() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetMaxHealth() const;
	// 能量仅供服务器与所属玩家读取；其他客户端的副本保留默认值，不代表真实资源。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetEnergy() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetMaxEnergy() const;
	// 护盾与血量分别复制，攻击先消费护盾，治疗不会顺带恢复护盾。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetShield() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetMaxShield() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsAlive() const;
	// 握手完成前不允许技能、受击或机关占用；普通角色和原生测试默认就绪。
	bool IsCombatReady() const;
	void SetCombatSessionReady(bool bReady);
	FmultiplayerCombatResumeState CaptureResumeState() const;
	void RestoreResumeState(const FmultiplayerCombatResumeState& State, float OfflineSeconds);
	// 状态直接读取 GAS 标签；剩余时间供拥有者与服务器查询，远端只复制标签不保证完整效果。
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsDefending() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsCasting() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsStunned() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsRooted() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsBurning() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsParrying() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetDashCooldownRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsMovementBlocked() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetRootRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetBurnRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetStunRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetAreaCooldownRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetAttackCooldownRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetHealCooldownRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat") float GetCastProgress() const;
	UFUNCTION(BlueprintPure, Category = "Combat") const UmultiplayerCombatConfig* GetCombatConfigAsset() const;
	const UmultiplayerCombatConfig& GetCombatConfig() const;
	FmultiplayerCombatSkillRule GetSkillRule(EMultiplayerCombatSkill Skill) const;
	TSubclassOf<UGameplayAbility> GetSkillAbilityClass(EMultiplayerCombatSkill Skill) const;
	UFUNCTION(BlueprintPure) UmultiplayerCombatProgressionComponent* GetProgressionComponent() const { return Progression; }
	UFUNCTION(BlueprintPure) UmultiplayerCombatCoopComponent* GetCoopComponent() const { return Coop; }
	// 返回请求是否已提交，不把异步服务器确认冒充成即时业务成功。
	bool ActivateSkill(EMultiplayerCombatSkill Skill);
	// 输入预览只在拥有者本地保存；确认时仍走原 GAS 预测与服务器验证。
	void BeginSkillTargeting(EMultiplayerCombatSkill Skill);
	void BeginSlotTargeting(int32 Slot);
	void ConfirmSkillTargeting();
	void CancelSkillTargeting();
	bool IsTargetingSkill() const { return bSkillTargeting; }
	EMultiplayerCombatSkill GetTargetingSkill() const { return TargetingSkill; }
	FmultiplayerCombatTargetPreview GetSkillTargetPreview(EMultiplayerCombatSkill Skill, const FRotator* Aim = nullptr) const;
	virtual FVector GetCombatViewOrigin(const FRotator& Aim) const;
	FVector GetCombatMuzzleLocation() const;
	AmultiplayerCombatCharacter* GetHealTarget() const;
	bool CheckSkillAvailable(EMultiplayerCombatSkill Skill, FGameplayTagContainer* OutReasons = nullptr) const;
	FText GetSkillFailureReason(EMultiplayerCombatSkill Skill) const;
	FText GetSkillFailureText(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue = -1.f) const;
	void ReportSkillFailure(EMultiplayerCombatSkill Skill);
	UFUNCTION(Client, Reliable) void ClientSkillRejected(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue = -1.f);
	// 两端固定本轮落点，施法期间转动镜头不移动预告圈；服务器结果可纠正预测快照。
	void SetAreaCastTarget(const FVector& Location, const FVector& Normal);
	void SetAttackAimSnapshot(const FRotator& Aim) { ActiveAttackAim = Aim; bHasAttackAim = true; }
	void RefreshCombatPermissions();
	// 生命状态的表现合并到下一帧；规则、碰撞与移动仍由原通知立即提交。
	void RequestLifePresentationRefresh();
	void RecordDamageSource(AmultiplayerCombatCharacter* Source);
	// 玩家与小兵可选择不同攻击技能；授予技能、输入和行为树读取同一个配置。
	TSubclassOf<UGameplayAbility> GetAttackAbilityClass() const { return AttackAbilityClass; }
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetDefenseRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetDefenseCooldownRemaining() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	EMultiplayerCombatTeam GetCombatTeam() const { return CombatTeam; }
	bool IsHostileTo(const AmultiplayerCombatCharacter* Other) const;

	// 本地输入只申请技能，不上传伤害值或直接修改目标血量。
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Attack();
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Defend();
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Heal();
	UFUNCTION(BlueprintCallable, Category = "Combat") void CastArea();
	UFUNCTION(BlueprintCallable, Category = "Combat") void CancelCast();
	UFUNCTION(BlueprintCallable, Category = "Combat") void Dash();
	UFUNCTION(BlueprintCallable, Category = "Combat") void Rescue();
	UFUNCTION(BlueprintCallable, Category = "Combat") void CancelRescue();
	UFUNCTION(BlueprintCallable, Category = "Combat") void SaveProgress();
	UFUNCTION(BlueprintCallable, Category = "Combat") void LoadProgress();
	virtual void Jump() override;
	// 不接受客户端伤害/眩晕数值；这些入口只由权威技能与真实碰撞调用。
	bool ApplyCombatDamage(AmultiplayerCombatCharacter* Target, float Amount, const FHitResult* Hit = nullptr);
	void ApplyStun(float Duration);
	// 业务效果只由服务器提交；返回自己的效果句柄，调用方结束时不能按标签移除其他来源。
	FActiveGameplayEffectHandle ApplyRoot(float Duration);
	FActiveGameplayEffectHandle ApplyBurning(AmultiplayerCombatCharacter* Target, float DamagePerTick, float Duration);
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat") void GrantShield(float Amount);
	FVector GetAreaTarget(FVector* OutGroundNormal = nullptr, const FRotator* AimSnapshot = nullptr) const;
	void ResolveArea(const FVector& Center);
	void SetCastTiming(float Duration);

	// 仅服务器技能调用；先初始化来源和速度，再开启投射物碰撞。
	bool FireProjectile(float Damage, float Speed, const FRotator* AimSnapshot = nullptr);
	// 受击只是表现，不取消技能或结算伤害；已有施法/死亡动作优先。
	void PlayImpactReaction();

	// 技能通过 ASC 播放配置的动作，由 GAS 处理拥有者预测与远端复制，不另发一套 Multicast。
	UAnimMontage* GetActionMontage(EMultiplayerCombatAnimation Action) const
	{
		return Action == EMultiplayerCombatAnimation::Attack ? AttackMontage
			: Action == EMultiplayerCombatAnimation::Area ? AreaMontage : HealMontage;
	}

	// 界面先订阅，再主动读取当前数值；不能假定加入时一定收到变化事件。
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerCombatStatsChanged OnCombatStatsChanged;
	// 预测、服务器拒绝、效果到期与死亡均从同一标签变化恢复表现，不另存一套 UI 防御状态。
	// 标签与血量通知都应刷新界面并读取 IsDefending；死亡状态可能先于防御效果移除到达。
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerDefenseStateChanged OnDefenseStateChanged;
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerCombatMessage OnCombatMessage;
	// 状态提交或复制到达后通知界面；界面读取当前快照，不在 Tick 中轮询战斗状态。
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerCombatStateChanged OnCombatStateChanged;

protected:
	// 生命周期与复制；初始化、控制器就绪、退出清理集中阅读。
	virtual void BeginPlay() override;
	virtual void NotifyControllerChanged() override;
	// Possess/客户端重启先由引擎确定默认移动模式，再重新落实尚未接纳或受控的禁行状态。
	virtual void Restart() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器完成死亡状态提交后，派生敌人停止 AI；客户端表现从复制状态恢复。
	virtual void HandleDeath();
	UFUNCTION(BlueprintImplementableEvent, Category = "Combat") void OnCombatDeath();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UAbilitySystemComponent> AbilitySystem;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UmultiplayerCombatAttributes> CombatAttributes;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UmultiplayerCombatProgressionComponent> Progression;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UmultiplayerCombatCoopComponent> Coop;
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Combat") EMultiplayerCombatTeam CombatTeam = EMultiplayerCombatTeam::Players;
	UPROPERTY(EditDefaultsOnly, Category = "Combat", meta = (ClampMin = "1")) float InitialMaxHealth = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Combat", meta = (ClampMin = "0")) float InitialMaxEnergy = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Combat") TSubclassOf<UGameplayAbility> AttackAbilityClass;
	bool bGrantSupportAbilities = true;

	// 蓝图选择表现资产；不以动画通知决定伤害、消耗或冷却，DS 无需计算骨骼。
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Animation") TObjectPtr<UAnimMontage> AttackMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Animation") TObjectPtr<UAnimMontage> HealMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Animation") TObjectPtr<UAnimMontage> DeathMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Animation") TObjectPtr<UAnimMontage> AreaMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Animation") TObjectPtr<UAnimMontage> HitMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Combat") TObjectPtr<UmultiplayerCombatConfig> CombatConfig;

private:
	// 瞄准意图与激活使用同一个 GAS 请求/预测键，不依赖另一路移动包先更新服务器朝向。
	bool ActivateAimedSkill(TSubclassOf<UGameplayAbility> AbilityClass, const FGameplayTag& EventTag);
	UFUNCTION(Server, Reliable) void ServerCancelRescue();
	UFUNCTION(Server, Reliable) void ServerPersistProgress(bool bLoad);
	UFUNCTION(Client, Reliable) void ClientProgressResult(bool bSuccess, bool bLoad);
	float GetEffectRemaining(const FGameplayTag& Tag) const;
	void HandleAttributeChanged(const FOnAttributeChangeData& Change);
	void HandleDefenseTagChanged(FGameplayTag Tag, int32 NewCount);
	void HandleControlTagChanged(FGameplayTag Tag, int32 NewCount);
	void RefreshMovementLock();
	void RefreshDownedPresentation();
	void ClearTransientEffects();
	void HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons);
	UFUNCTION() void OnRep_Dead();
	UFUNCTION() void OnRep_SessionReady();
	UFUNCTION() void OnRep_CastTiming();
	UPROPERTY(ReplicatedUsing = OnRep_Dead) bool bDead = false;
	UPROPERTY(ReplicatedUsing = OnRep_SessionReady) bool bCombatSessionReady = true;
	UPROPERTY(ReplicatedUsing = OnRep_CastTiming) double CastEndTime = 0;
	UPROPERTY(ReplicatedUsing = OnRep_CastTiming) float CastDuration = 0;
	UPROPERTY(Replicated) FVector_NetQuantize ConfirmedAreaTarget = FVector::ZeroVector;
	UPROPERTY(Replicated) FVector_NetQuantizeNormal ConfirmedAreaNormal = FVector::UpVector;
	UPROPERTY(Replicated) bool bHasAreaTarget = false;
	bool bSkillTargeting = false;
	EMultiplayerCombatSkill TargetingSkill = EMultiplayerCombatSkill::Attack;
	FRotator ActiveAttackAim = FRotator::ZeroRotator;
	bool bHasAttackAim = false;
	bool bMovementLocked = false;
	bool bLifePoseStarted = false;
	FTimerHandle LifePresentationRefreshTimer;
	// 仅保存攻击期间临时替换的 CMC 策略，技能状态本身仍以 GAS 标签为准。
	bool bAttackFacingLocked = false;
	bool bPreviousOrientToMovement = false;
	bool bPreviousControllerDesiredRotation = false;
	uint8 PreviousMovementMode = 0;
	uint8 PreviousCustomMode = 0;
	TSharedPtr<FStreamableHandle> PresentationAssets;
	TWeakObjectPtr<AmultiplayerCombatCharacter> LastDamageSource;
	FGuid DeathRewardId;
	double LastPersistenceRequest = -10.0;
};
