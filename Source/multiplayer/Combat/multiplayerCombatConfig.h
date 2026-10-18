// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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
	FmultiplayerCombatSkillRule() = default;
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

USTRUCT(BlueprintType)
struct FmultiplayerCombatFeedback
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> Effect;
	// 素材包的 Cascade 与原 Niagara 二选一；配置 Cascade 时优先使用，避免重复播放。
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UParticleSystem> CascadeEffect;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USoundBase> Sound;
};

// 四个技能共用一份资产；服务器和预测端读取相同规则，伤害数值仍只由服务器提交。
UCLASS(BlueprintType)
class MULTIPLAYER_API UmultiplayerCombatConfig : public UDataAsset
{
	GENERATED_BODY()
public:
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "0", ClampMax = "1")) float DefenseDamageMultiplier = .5f;
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
	const FmultiplayerCombatSkillRule& GetSkill(EMultiplayerCombatSkill Skill) const;
	// 仅资产编辑/加载时检查一次；技能执行不反复校验同一份静态配置。
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
	void NormalizeRules();
};
