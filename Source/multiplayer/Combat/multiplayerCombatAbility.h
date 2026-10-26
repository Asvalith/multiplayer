// Copyright Epic Games, Inc. All Rights Reserved.

// 公共准入／提交、攻击与支援技能阶段，以及配套原生 GE；输入和 HUD 不在此处执行玩法。
// 冲刺阶段在 CombatMobility，ASC 的标签关系和动画认领在 CombatAbilitySystem。

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Combat/multiplayerCombatConfig.h"
#include "multiplayerCombatAbility.generated.h"

class AmultiplayerCombatCharacter;
class UAbilitySystemComponent;

// 复用 GAS 句柄/预测键与能力已有激活轮次；没有另外生成一套施法编号。
// AuthorityRevision 是服务器该能力实例的轮次，客户端预测阶段为 0；预测键仅用于两者关联。
USTRUCT()
struct MULTIPLAYER_API FmultiplayerCombatActivationIdentity
{
	GENERATED_BODY()
	UPROPERTY() FGameplayAbilitySpecHandle SpecHandle;
	UPROPERTY() uint32 AuthorityRevision = 0;
	UPROPERTY() int16 PredictionKey = 0;
	UPROPERTY() bool bServerInitiated = false;

	// 有效的授予句柄加权威轮次，或拥有者正在预测的非零键。
	bool IsValid() const;
	// 两边都有权威轮次时精确匹配；预测端只通过原有键接到对应权威结果。
	bool Matches(const FmultiplayerCombatActivationIdentity& Other) const;
	// 容器/快照的相等关系必须是完整字段比较，不能把上述部分关联当作等价关系。
	bool operator==(const FmultiplayerCombatActivationIdentity& Other) const;
	// 只序列化关联值，不调用会向旁观端筛除拥有者预测键的 FPredictionKey::NetSerialize。
	bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess);
};

template<> struct TStructOpsTypeTraits<FmultiplayerCombatActivationIdentity> : TStructOpsTypeTraitsBase2<FmultiplayerCombatActivationIdentity>
{
	enum { WithNetSerializer = true, WithCopy = true };
};

// 结果与施法 Cue 显式携带同一激活身份；普通 GE 仍沿用原生 Context，不改全局分配器。
USTRUCT()
struct MULTIPLAYER_API FmultiplayerCombatCueContext : public FGameplayEffectContext
{
	GENERATED_BODY()
	UPROPERTY() FmultiplayerCombatActivationIdentity ActivationIdentity;
	virtual UScriptStruct* GetScriptStruct() const override;
	virtual FmultiplayerCombatCueContext* Duplicate() const override;
	virtual bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess) override;
	// 只读本项目派生上下文；旧素材/原生 Cue 不做不安全的强制转换。
	static const FmultiplayerCombatCueContext* Get(const FGameplayEffectContextHandle& Context);
};

template<> struct TStructOpsTypeTraits<FmultiplayerCombatCueContext> : TStructOpsTypeTraitsBase2<FmultiplayerCombatCueContext>
{
	enum { WithNetSerializer = true, WithCopy = true };
};

// 五个战斗技能共用 GAS 本地预测、连接接纳与死亡限制；最终玩法结果仍由服务器决定。
// 静态冲突关系在 CombatConfig，GAS 继续管理每次激活的真实生命周期。
UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerCombatAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 配置实例复用和本地预测；各技能构造函数再声明自身分类及效果。
	UmultiplayerCombatAbility();
	// 返回固定技能分类，供配置查询及失败反馈识别能力，不表示当前激活阶段。
	EMultiplayerCombatSkill GetCombatSkill() const { return Skill; }
	// 动画、Cue 和取消共用已有激活身份；它不创建预测窗口，也不生成新的编号。
	FmultiplayerCombatActivationIdentity GetActivationIdentity() const;
	// 本端已有轮次仅用于关联接替；不把客户端轮次假定为服务器轮次。
	uint32 GetLocalActivationRevision() const { return ActivationRevision; }
	// 先查连接与角色业务资格，再交 GAS 检查标签、成本和冷却；查询不修改状态。
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 校验定向事件的数据形状及有限、归一化朝向；不接收客户端伤害或目标裁决。
	virtual bool ShouldAbilityRespondToEvent(const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayEventData* Payload) const override;
	// 原生按标签筛选/调试查询也必须看到中央关系，不能只在最终激活入口额外判断。
	virtual bool DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* Reasons = nullptr) const override;
protected:
	// 所有技能的资源与冷却从同一份规则读取；效果继续交给 GAS 预测与确认。
	virtual bool CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 保留 GAS 冷却查询，只补充项目可显示的失败原因。
	virtual bool CheckCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 独立成本提交入口读取当前有效规则，最终修改仍经预测 GE 执行。
	virtual void ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	// 独立冷却提交入口使用规则时长，不自行维护冷却计时器。
	virtual void ApplyCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	// CommitCheck 放行后，用同一份局部规则先扣费再挂冷却；回调返回时检查本轮是否仍有效。
	virtual void CommitExecute(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) override;
	// 读取角色结合成长后的有效数值；不是跨阶段固定的完整技能快照。
	FmultiplayerCombatSkillRule GetRule() const;
	EMultiplayerCombatSkill Skill = EMultiplayerCombatSkill::Attack;
	// InstancedPerActor 会复用实例；同步取消后再激活时，旧调用栈不能继续推动新一轮技能。
	uint32 ActivationRevision = 0;
private:
	// CommitExecute 用同一份局部规则提交成本和冷却，不把提交快照保存成跨激活成员。
	void ApplyRuleCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const;
	// 按调用者提供的规则挂冷却，保证一次 Commit 不会混用扣费前后的等级数值。
	void ApplyRuleCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const;
};

// GE 定义结算方式与标签，运行数值由 CombatConfig／成长规则填入 Spec。
// Blueprintable 允许派生 GE；创建派生资产不会替换下方能力显式使用的原生效果类。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 伤害由服务器通过 Data.Damage 写入本次效果。
	UmultiplayerDamageEffect();
};

// 护盾是独立持续属性；获得护盾使用 GE，承伤顺序由同一属性集负责。
UCLASS()
class MULTIPLAYER_API UmultiplayerShieldEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 声明瞬时护盾增量；剩余护盾由属性集管理。
public: UmultiplayerShieldEffect();
};

// 同一来源刷新持续时间但不推迟下一跳，不同来源各自保留效果句柄。
UCLASS()
class MULTIPLAYER_API UmultiplayerBurningEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置服务端周期伤害及按来源刷新规则。
public: UmultiplayerBurningEffect();
};

// 禁锢与眩晕共用禁行权限，但禁锢不会取消攻击或治疗。
UCLASS()
class MULTIPLAYER_API UmultiplayerRootEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置禁锢和共享禁行标签；每份效果独立持有自己的标签引用。
public: UmultiplayerRootEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerAttackCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 普通攻击的默认冷却为半秒；实际提交时长由技能有效规则覆盖。
	UmultiplayerAttackCooldownEffect();
};

// 普通攻击：提交冷却后生成服务器投射物，伤害在命中时结算。
UCLASS()
class MULTIPLAYER_API UmultiplayerAttackAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明普通攻击分类、事件及默认成本／冷却效果。
	UmultiplayerAttackAbility();

protected:
	// 固定本轮瞄准，验证枪口后提交，并等待出手；只有服务器生成真实弹体。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	// 只结束匹配预测键的激活；取消时停止本能力拥有的动作，任务交 GAS 释放。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 起手回调先认领本次出手，再由服务器发弹；重复回调不会重复生成。
	UFUNCTION() void ReleaseProjectile();
	// 收招结束本地或权威能力；客户端正常结束不提前裁掉服务器起手。
	UFUNCTION() void FinishAttack();
	FRotator AimRotation = FRotator::ZeroRotator;
	bool bProjectileReleased = false;
};

// 小兵近战：行为树提供目标，GAS 管理起手、一次命中和收招；客户端只接收动作与结果。
UCLASS()
class MULTIPLAYER_API UmultiplayerEnemyMeleeAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	// 小兵近战仅在服务器运行，不复用玩家的远程投射物流程。
	UmultiplayerEnemyMeleeAbility();
	// 在公共准入外检查黑板目标、距离和遮挡，不在查询阶段消费资源。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
protected:
	// 固定目标、提交并挂施法锁，等待本轮近战出手。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 先结束任务再解开自己的施法锁；结束中的保护避免行为树同步重启时污染下一轮。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 出手时重新验证服务器目标，只命中一次，挥空也进入收招。
	UFUNCTION() void ReleaseMelee();
	// 完成服务器收招，让行为树按技能结果继续决策。
	UFUNCTION() void FinishMelee();
	TWeakObjectPtr<AmultiplayerCombatCharacter> AttackTarget;
	FActiveGameplayEffectHandle CastingEffect;
	bool bReleased = false;
	bool bEndingMelee = false;
};

// 所有技能共用同一种能量成本；金额由本次提交的 Data.Cost 提供。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerEnergyCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerEnergyCostEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 配置防御冷却标签和默认八秒时长，提交时可按有效规则覆盖。
	UmultiplayerDefendCooldownEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendingEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 默认三秒内授予防御标签，正面完全格挡由属性结算层执行；实际时长由技能填入。
	UmultiplayerDefendingEffect();
};

// Q 的短弹反窗口独立于较长格挡效果；结束时只移除自己的标签引用。
UCLASS()
class MULTIPLAYER_API UmultiplayerParryEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置独立于较长防御状态的短弹反窗口。
public: UmultiplayerParryEffect();
};

// 防御：限时 GE 授予正面格挡；正常准入阻止重复施放，GE 独立于瞬时能力存活。
// 客户端先预测消耗、冷却和自身防御；服务器拒绝时由同一个预测键撤销这些效果。
UCLASS()
class MULTIPLAYER_API UmultiplayerDefendAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明防御分类、定向事件；能量成本共用，冷却标签独立。
	UmultiplayerDefendAbility();

protected:
	// 提交后授予防御和短弹反，结束当次能力但保留限时 GE；可选护盾仅服务器添加。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};

// 治疗：治疗效果、消耗、冷却和技能。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 读取本次 Data.Heal 修改血量，属性集负责限制上限与禁止复活。
	UmultiplayerHealEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 配置治疗冷却标签和默认六秒时长，提交时可按有效规则覆盖。
	UmultiplayerHealCooldownEffect();
};

// 治疗：选择附近同阵营最低血量比例的存活角色；提交前无目标不扣费。
// 扣费通知后会重新选取目标；此时目标消失属于已提交施法，不返还成本／冷却。
UCLASS()
class MULTIPLAYER_API UmultiplayerHealAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明治疗分类和默认效果；治疗直接激活，不接受客户端指定治疗对象。
	UmultiplayerHealAbility();
	// 公共准入通过后查找合法治疗对象，使预览和正式激活共用同一条件。
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 服务器先确认可治疗对象再提交，扣费通知后重新选目标，统一通过 GE 治疗。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};

// 范围技能的默认冷却标签；实际时长在提交时由有效规则覆盖。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置默认范围冷却时长和标签。
public: UmultiplayerAreaCooldownEffect();
};
// 施法锁是跟随能力生命周期的无限 GE，不靠独立倒计时提前解锁。
UCLASS(NotBlueprintable)
class MULTIPLAYER_API UmultiplayerCastingEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置施法标签及表现，具体能力保存句柄并在结束时解除。
public: UmultiplayerCastingEffect();
};
// 同一目标只保留一份眩晕，再次施加刷新时长，不能叠出多份移动锁。
UCLASS()
class MULTIPLAYER_API UmultiplayerStunEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置同目标刷新时长的眩晕及共享禁行标签。
public: UmultiplayerStunEffect();
};

// 起手扣费并启动冷却；延时由 AbilityTask 管理，取消/死亡统一 EndAbility 销毁任务。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	// 声明范围技能分类和可由拥有者主动取消的生命周期规则。
	UmultiplayerAreaAbility();
	// 范围结算归属于该技能；服务器在出手时验证目标，角色不再代管技能专属执行。
	// 无身份的默认入口只供独立结算调用，不创建假关联的结果特效。
	static void ResolveTargets(AmultiplayerCombatCharacter& Character, const FVector& Center,
		const FmultiplayerCombatActivationIdentity& Identity = FmultiplayerCombatActivationIdentity());
protected:
	// 验证并锁定落点，提交后挂施法锁与起手任务；取消不退还已提交成本／冷却。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 提前摘下本次效果句柄，再结束任务并精确解锁，避免同步回调清掉新一轮状态。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 起手完成先标记已出手；服务器按锁定落点结算一次，再进入收招。
	UFUNCTION() void ReleaseArea();
	// 正常收招仅由服务器裁决远端结束，客户端只结束自身表现。
	UFUNCTION() void FinishRecovery();
	FActiveGameplayEffectHandle CastingEffect;
	FVector TargetLocation = FVector::ZeroVector;
	bool bReleased = false;
};
