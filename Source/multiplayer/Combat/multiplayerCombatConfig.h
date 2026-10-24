// Copyright Epic Games, Inc. All Rights Reserved.

// 声明共享的静态技能数值、标签关系与表现资源；不保存本局冷却、余额或效果句柄。
// cpp 提供默认关系、只读查询及加载／编辑时规范化，角色另行计算成长后的有效数值。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "multiplayerCombatConfig.generated.h"

class UNiagaraSystem;
class UMaterialInterface;
class UParticleSystem;
class USoundBase;

UENUM(BlueprintType)
enum class EMultiplayerCombatSkill : uint8 { Attack, Defend, Heal, Area, Dash };

// 配置是只读规则，不保存剩余冷却、已扣资源或正在施法等运行状态。
USTRUCT(BlueprintType)
struct FmultiplayerCombatSkillRule
{
	GENERATED_BODY()
	// 为反射和资产序列化保留字段默认值，不在构造中创建运行时效果。
	FmultiplayerCombatSkillRule() = default;
	// 供默认资产紧凑声明一组规则，参数仍只是配置值而不是本次施法的状态。
	FmultiplayerCombatSkillRule(float InCost, float InCooldown, float InAmount, float InRange,
		float InDuration = 0, float InWindup = 0, float InRecovery = 0)
		: Cost(InCost), Cooldown(InCooldown), Amount(InAmount), Range(InRange), Duration(InDuration),
		Windup(InWindup), Recovery(InRecovery) {}
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float Cost = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.05")) float Cooldown = .5f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float Amount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float Range = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float Duration = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.05")) float Windup = .8f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.05")) float Recovery = .4f;
};

// 只关联可选特效与声音资源；何时播放由 Cue／表现模块决定，不在配置中产生伤害。
USTRUCT(BlueprintType)
struct FmultiplayerCombatFeedback
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> Effect;
	// 素材包的 Cascade 与原 Niagara 二选一；配置 Cascade 时优先使用，避免重复播放。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UParticleSystem> CascadeEffect;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USoundBase> Sound;
};

// 借鉴 Lyra 的关系映射：静态资格、阻断和替换集中配置，任务与效果仍由各能力自己收尾。
USTRUCT(BlueprintType)
struct FmultiplayerAbilityTagRelationship
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FGameplayTag AbilityTag;
	// 准入时拥有者必须具备全部标签；不是从目标身上取状态。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FGameplayTagContainer ActivationRequiredTags;
	// 只禁止新的激活；是否中断已经在途的技能由状态事件另行处理。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FGameplayTagContainer ActivationBlockedTags;
	// 本能力活动期间的能力阻断，结束时只减去自己的引用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FGameplayTagContainer AbilityTagsToBlock;
	// 开始时替换的能力分类；被取消者仍通过自身 EndAbility 清理任务与表现。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FGameplayTagContainer AbilityTagsToCancel;
};

// 五个技能共用一份资产；服务器和预测端读取相同规则，伤害数值仍只由服务器提交。
// 救援只复用下方冲突关系，不共用资源数值。
UCLASS(BlueprintType)
class MULTIPLAYER_API UmultiplayerCombatConfig : public UDataAsset
{
	GENERATED_BODY()
public:
	// 为新资产及未保存关系字段的旧资产建立兼容默认规则，不重写已有地图布局。
	UmultiplayerCombatConfig();
	// 配置在角色生命周期中只读；不能在活动能力持有阻断计数时替换关系表。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Ability Relationships")
	TArray<FmultiplayerAbilityTagRelationship> AbilityRelationships;
	// 汇总父分类和具体能力的拥有者准入要求，仅追加到调用者给出的容器。
	void GetAbilityActivationTagRequirements(const FGameplayTagContainer& AbilityTags,
		FGameplayTagContainer& OutRequired, FGameplayTagContainer& OutBlocked) const;
	// 汇总活动期间阻断及激活时取消的能力分类，引用计数继续由原生 ASC 管理。
	void GetAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags,
		FGameplayTagContainer& OutBlock, FGameplayTagContainer& OutCancel) const;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatSkillRule Attack{0, .5f, 25, 0};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatSkillRule Defend{20, 8, 0, 0, 3};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatSkillRule Heal{25, 6, 30, 600};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatSkillRule Area{30, 5, 35, 650, 1.5f, 5.f / 6.f, 11.f / 6.f};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatSkillRule Dash{15, 3, 0, 450, .35f};
	// 弹反只在 Q 起手短窗口生效，方向和次数由服务器用同一配置复核。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.05")) float ParryWindow = .3f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "-1", ClampMax = "1")) float ParryFrontDot = .5f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0", ClampMax = "8")) int32 MaxProjectileReflections = 2;
	// 有仍能行动的队友才进入倒地；倒地、救援和终局死亡是不同状态。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1")) float BleedoutDuration = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.1")) float RescueDuration = 2.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "50")) float RescueRange = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "10")) float AreaRadius = 220;
	// 接入素材时校准 Scale_All=1 的视觉半径；只缩放表现，不参与命中或伤害判定。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "10")) float AreaEffectReferenceRadius = 400.f;
	// 地面圈只标示同一个技能落点和半径，不用追踪型粒子代替范围边界。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UMaterialInterface> AreaMarkerMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "100")) float ProjectileSpeed = 1600;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> ProjectileEffect;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UParticleSystem> ProjectileCascadeEffect;
	// 新规则可按角色配置启用，默认保留原关卡的技能数值；剩余盾和效果时长不写回配置。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float DefenseShieldAmount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float BurningDamagePerTick = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0.05")) float BurningDuration = 4.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0")) float AreaRootDuration = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback AttackFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback ImpactFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback HealFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback DefenseFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback CastingFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback AreaFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback StunFeedback;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FmultiplayerCombatFeedback FailureFeedback;
	// 读取技能基础规则；等级修正在角色侧完成，不能在调用点写回资产。
	const FmultiplayerCombatSkillRule& GetSkill(EMultiplayerCombatSkill Skill) const;
	// 仅资产编辑/加载时检查一次；技能执行不反复校验同一份静态配置。
	virtual void PostLoad() override;
#if WITH_EDITOR
	// 编辑器修改属性后规范化静态数值，不在每次技能查询中重复清洗配置。
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
	// 只钳制资产数值到本项目支持范围，不授予技能、不调整正在运行的效果。
	void NormalizeRules();
};
