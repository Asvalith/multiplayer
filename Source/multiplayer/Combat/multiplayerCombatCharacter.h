// Copyright Epic Games, Inc. All Rights Reserved.

// 角色协调接口：连接输入、GAS、生命权限与短时恢复；各技能的执行细节留在能力中。

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
struct FGameplayEffectSpec;

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
	// 建立角色持有的 ASC、属性、成长与合作组件，配置默认攻击类。
	AmultiplayerCombatCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// GAS 从接口获取角色的 ASC；数值接口供血条和技能查询，不在 UI 中修改属性。
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	// 当前生命，来自 GAS 属性集。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetHealth() const;
	// 包含当前成长与被动加成的生命上限。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetMaxHealth() const;
	// 能量仅供服务器与所属玩家读取；其他客户端的副本保留默认值，不代表真实资源。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetEnergy() const;
	// 仅服务器和拥有者保证可见的完整能量上限。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetMaxEnergy() const;
	// 护盾与血量分别复制，攻击先消费护盾，治疗不会顺带恢复护盾。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetShield() const;
	// 护盾容量，与当前护盾和生命上限分别管理。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetMaxShield() const;
	// 同时检查死亡标记与生命，不因复制顺序暂时恢复行动。
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsAlive() const;
	// 握手完成前不允许技能、受击或机关占用；普通角色和原生测试默认就绪。
	bool IsCombatReady() const;
	// 服务器设置接纳状态，并立即落实碰撞及移动权限。
	void SetCombatSessionReady(bool bReady);
	// 从服务器取本局短时重连快照，区别于只保存成长配装的长期档案。
	FmultiplayerCombatResumeState CaptureResumeState() const;
	// 只恢复未接纳的新 Pawn，扣除离线时间；不恢复半次施法或回滚当前局进度。
	void RestoreResumeState(const FmultiplayerCombatResumeState& State, float OfflineSeconds);
	// 状态直接读取 GAS 标签；剩余时间供拥有者与服务器查询，远端只复制标签不保证完整效果。
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsDefending() const;
	// 施法锁是否仍存在，不等同于任意 Ability 都处于活动中。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsCasting() const;
	// 眩晕由限时效果或兼容的控制标签表达。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsStunned() const;
	// 禁锢只表达移动限制，不能用它概括全部技能资格。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsRooted() const;
	// 只要任一来源的灼烧仍存在，就保持燃烧状态。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsBurning() const;
	// 查询独立于长防御效果的短弹反窗口。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsParrying() const;
	// 读取冲刺冷却 GE 的剩余时间。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetDashCooldownRemaining() const;
	// 汇总眩晕、禁锢和统一禁行标签，不改变任何移动状态。
	UFUNCTION(BlueprintPure, Category = "Combat") bool IsMovementBlocked() const;
	// 多份禁锢取最长剩余时间，解锁仍由标签计数决定。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetRootRemaining() const;
	// 查询灼烧的最长剩余时间。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetBurnRemaining() const;
	// 查询眩晕的剩余时间。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetStunRemaining() const;
	// 查询范围技能冷却，不另存本地倒计时。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetAreaCooldownRemaining() const;
	// 查询普通攻击冷却。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetAttackCooldownRemaining() const;
	// 查询治疗技能冷却。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetHealCooldownRemaining() const;
	// 用服务器时间估算当前施法读条，标签消失后归零。
	UFUNCTION(BlueprintPure, Category = "Combat") float GetCastProgress() const;
	// 向蓝图暴露有效只读配置，缺省时仍返回原生默认配置。
	UFUNCTION(BlueprintPure, Category = "Combat") const UmultiplayerCombatConfig* GetCombatConfigAsset() const;
	// 返回配置资产或原生默认值，不在运行时改写共享配置。
	const UmultiplayerCombatConfig& GetCombatConfig() const;
	// 将基础配置与实际 Spec 等级组合为当前有效规则。
	FmultiplayerCombatSkillRule GetSkillRule(EMultiplayerCombatSkill Skill) const;
	// 将技能分类映射到实际能力类，支持小兵自己的近战攻击。
	TSubclassOf<UGameplayAbility> GetSkillAbilityClass(EMultiplayerCombatSkill Skill) const;
	// 返回角色持有的成长与配装组件，不创建临时组件。
	UFUNCTION(BlueprintPure) UmultiplayerCombatProgressionComponent* GetProgressionComponent() const { return Progression; }
	// 返回倒地与救援组件；组件自身负责占用和生命阶段。
	UFUNCTION(BlueprintPure) UmultiplayerCombatCoopComponent* GetCoopComponent() const { return Coop; }
	// 返回请求是否已提交，不把异步服务器确认冒充成即时业务成功。
	bool ActivateSkill(EMultiplayerCombatSkill Skill);
	// 输入预览只在拥有者本地保存；确认时仍走原 GAS 预测与服务器验证。
	void BeginSkillTargeting(EMultiplayerCombatSkill Skill);
	// 按当前配装把槽位映射为预览技能。
	void BeginSlotTargeting(int32 Slot);
	// 确认前查询资格与几何，随后请求 GAS 激活，最后通知预览变化。
	void ConfirmSkillTargeting();
	// 只清除未提交的本地预览，不撤销已结算的技能。
	void CancelSkillTargeting();
	// 查询拥有者本地是否正在预览，不代表服务器已接纳技能。
	bool IsTargetingSkill() const { return bSkillTargeting; }
	// 返回当前本地预览的技能种类。
	EMultiplayerCombatSkill GetTargetingSkill() const { return TargetingSkill; }
	// 统一返回当前瞄准几何和候选；服务器出手时仍须复核目标。
	FmultiplayerCombatTargetPreview GetSkillTargetPreview(EMultiplayerCombatSkill Skill, const FRotator* Aim = nullptr) const;
	// 派生角色可提供不同视点，实际发射点仍独立验证。
	virtual FVector GetCombatViewOrigin(const FRotator& Aim) const;
	// 用身体与方向计算发射点，不能要求 DS 刷新手骨。
	FVector GetCombatMuzzleLocation(const FRotator* AimSnapshot = nullptr) const;
	// 选择范围内可见且受伤的最低生命比例友方，客户端只作预览。
	AmultiplayerCombatCharacter* GetHealTarget() const;
	// 只检查角色自己的业务资格；标签关系、成本和冷却仍由能力的 GAS 准入负责。
	bool CheckSkillBusinessRequirements(EMultiplayerCombatSkill Skill, FGameplayTagContainer* OutReasons = nullptr) const;
	// 输入和 HUD 查询实际已授予能力的 CanActivateAbility，不另维护一套完整准入规则。
	bool CheckSkillAvailable(EMultiplayerCombatSkill Skill, FGameplayTagContainer* OutReasons = nullptr) const;
	// 主动查询不可用原因；可用时返回空文本，不产生提示副作用。
	FText GetSkillFailureReason(EMultiplayerCombatSkill Skill) const;
	// 将已有失败标签转换为文本；可使用服务器附带的权威数值。
	FText GetSkillFailureText(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue = -1.f) const;
	// 已有拒绝结果直接用于提示，不能为显示原因重新运行一遍资格查询。
	void ReportSkillFailure(EMultiplayerCombatSkill Skill, const FGameplayTagContainer* Reasons = nullptr);
	// 只给拥有者发送实际拒绝原因，不广播给其他玩家。
	UFUNCTION(Client, Reliable) void ClientSkillRejected(EMultiplayerCombatSkill Skill, FGameplayTag Reason, float AuthorityValue = -1.f);
	// 两端固定本轮落点，施法期间转动镜头不移动预告圈；服务器结果可纠正预测快照。
	void SetAreaCastTarget(const FVector& Location, const FVector& Normal);
	// 固定本轮普攻的方向，后续镜头转动不改变已经接纳的攻击。
	void SetAttackAimSnapshot(const FRotator& Aim) { ActiveAttackAim = Aim; bHasAttackAim = true; }
	// 生命或合作状态改变后重新计算权限并通知表现。
	void RefreshCombatPermissions();
	// 生命状态的表现合并到下一帧；规则、碰撞与移动仍由原通知立即提交。
	void RequestLifePresentationRefresh();
	// 服务器记录合法伤害来源，供死亡奖励归属使用。
	void RecordDamageSource(AmultiplayerCombatCharacter* Source);
	// 玩家与小兵可选择不同攻击技能；授予技能、输入和行为树读取同一个配置。
	TSubclassOf<UGameplayAbility> GetAttackAbilityClass() const { return AttackAbilityClass; }
	// 防御效果可在 Ability 已结束后继续存在。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetDefenseRemaining() const;
	// 查询防御冷却 GE 的实际剩余时间。
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetDefenseCooldownRemaining() const;
	// 阵营是玩法分类，不等同于网络 Owner 或本地控制权。
	UFUNCTION(BlueprintPure, Category = "Combat")
	EMultiplayerCombatTeam GetCombatTeam() const { return CombatTeam; }
	// 比较玩法阵营，不使用网络 Owner 推断敌我。
	bool IsHostileTo(const AmultiplayerCombatCharacter* Other) const;

	// 本地输入只申请技能，不上传伤害值或直接修改目标血量。
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Attack();
	// Q 请求当前第二槽技能，允许配装改变该槽的技能。
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Defend();
	// E 请求当前第三槽技能，不在按键入口直接回血。
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void Heal();
	// R 请求当前第四槽技能。
	UFUNCTION(BlueprintCallable, Category = "Combat") void CastArea();
	// 撤销预览、救援、范围施法及冲刺，不退还已提交成本。
	UFUNCTION(BlueprintCallable, Category = "Combat") void CancelCast();
	// Shift 请求当前第五槽技能，移动仍由能力任务和 CMC 执行。
	UFUNCTION(BlueprintCallable, Category = "Combat") void Dash();
	// 请求 ServerOnly 救援能力，由服务器选择并占用倒地目标。
	UFUNCTION(BlueprintCallable, Category = "Combat") void Rescue();
	// 松键时向服务器申请结束本角色的救援引导。
	UFUNCTION(BlueprintCallable, Category = "Combat") void CancelRescue();
	// 请求保存服务器持有的长期成长与配装。
	UFUNCTION(BlueprintCallable, Category = "Combat") void SaveProgress();
	// 请求读取长期成长，不回滚本局血量、能量和冷却。
	UFUNCTION(BlueprintCallable, Category = "Combat") void LoadProgress();
	// 只有当前生命与移动资格允许时才交给原生跳跃。
	virtual void Jump() override;
	// 不接受客户端伤害/眩晕数值；这些入口只由权威技能与真实碰撞调用。
	// 直接攻击默认快照来源位置；弹体传真实来向，显式零向量表示范围等无方向伤害。
	bool ApplyCombatDamage(AmultiplayerCombatCharacter* Target, float Amount, const FHitResult* Hit = nullptr,
		const FVector* IncomingDirection = nullptr);
	// 伤害结算和命中反馈共用同一份正面格挡查询；不消费资源或改变本次效果。
	bool ShouldBlockDirectDamage(const FGameplayEffectSpec& Spec) const;
	// 服务器施加眩晕，同目标再次施加按刷新时长处理。
	void ApplyStun(float Duration);
	// 业务效果只由服务器提交；返回自己的效果句柄，调用方结束时不能按标签移除其他来源。
	FActiveGameplayEffectHandle ApplyRoot(float Duration);
	// 服务器为敌对目标施加按来源管理的周期伤害。
	FActiveGameplayEffectHandle ApplyBurning(AmultiplayerCombatCharacter* Target, float DamagePerTick, float Duration);
	// 用瞬时 GE 增加可消耗护盾，不直接改客户端显示余额。
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Combat") void GrantShield(float Amount);
	// 返回同源范围预览的落点与法线，不接受客户端直接指定权威落点。
	FVector GetAreaTarget(FVector* OutGroundNormal = nullptr, const FRotator* AimSnapshot = nullptr) const;
	// 保存预测或服务器施法结束时刻，供跨端恢复读条。
	void SetCastTiming(float Duration);

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
	// 控制器可能晚到，只刷新 ASC ActorInfo，不重复授予能力。
	virtual void NotifyControllerChanged() override;
	// Possess/客户端重启先由引擎确定默认移动模式，再重新落实尚未接纳或受控的禁行状态。
	virtual void Restart() override;
	// 退出时解除监听、取消任务并释放表现预载。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 注册阵营、生命与施法快照复制；数值属性由 AttributeSet 独立复制。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器完成死亡状态提交后，派生敌人停止 AI；客户端表现从复制状态恢复。
	virtual void HandleDeath();
	// 派生蓝图只补死亡表现与关卡响应，不再次结算伤害或奖励。
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
	// 所属连接提交救援取消意图，服务器仍只操作本角色能力。
	UFUNCTION(Server, Reliable) void ServerCancelRescue();
	// 服务器限频处理自身成长档案存取，客户端不能指定快照或磁盘路径。
	UFUNCTION(Server, Reliable) void ServerPersistProgress(bool bLoad);
	// 向拥有者显示实际存取结果，不在本地假定请求成功。
	UFUNCTION(Client, Reliable) void ClientProgressResult(bool bSuccess, bool bLoad);
	// 共用 GE 剩余时间查询，死亡时显示零但不在查询中删效果。
	float GetEffectRemaining(const FGameplayTag& Tag) const;
	// 属性通知触发生命阶段与界面更新，权威死亡先认领再广播。
	void HandleAttributeChanged(const FOnAttributeChangeData& Change);
	// 防御标签跨零时通知表现，不为预测/权威接替重复播放。
	void HandleDefenseTagChanged(FGameplayTag Tag, int32 NewCount);
	// 控制标签变化时取消受限能力并重算移动及攻击朝向权限。
	void HandleControlTagChanged(FGameplayTag Tag, int32 NewCount);
	// 汇总所有限制，最后一项解除后恢复先前 CMC 模式。
	void RefreshMovementLock();
	// 更新本地倒地姿态，不覆盖持久死亡表现。
	void RefreshDownedPresentation();
	// 死亡/恢复前清理本项目的临时效果，不扫描移除第三方任意 GE。
	void ClearTransientEffects();
	// 消费 GAS 正式激活的失败结果，转换为拥有者可读提示。
	void HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Reasons);
	// 死亡状态到达后结束游戏输入、能力、碰撞与瞬态表现。
	UFUNCTION() void OnRep_Dead();
	// 接纳状态到达后恢复或维持当前移动限制。
	UFUNCTION() void OnRep_SessionReady();
	// 时间快照到达后通知界面补读，不假定与施法标签的复制顺序。
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
