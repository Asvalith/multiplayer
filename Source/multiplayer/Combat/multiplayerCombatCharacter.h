// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Character.h"
#include "multiplayerCombatCharacter.generated.h"

class UAbilitySystemComponent;
class UmultiplayerCombatAttributes;
struct FOnAttributeChangeData;
struct FGameplayTag;

UENUM(BlueprintType)
enum class EMultiplayerCombatTeam : uint8
{
	Players,
	Enemies
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FmultiplayerCombatStatsChanged,
	float, Health, float, MaxHealth, float, Energy, float, MaxEnergy);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FmultiplayerDefenseStateChanged, bool, bIsDefending);

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
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetEnergy() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetMaxEnergy() const;
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsAlive() const;
	// 状态直接读取 GAS 标签；剩余时间供拥有者与服务器查询，远端只复制标签不保证完整效果。
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsDefending() const;
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
	virtual void Jump() override;

	// 仅服务器技能调用；先初始化来源和速度，再开启投射物碰撞。
	bool FireProjectile(float Damage, float Speed);

	// 界面先订阅，再主动读取当前数值；不能假定加入时一定收到变化事件。
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerCombatStatsChanged OnCombatStatsChanged;
	// 预测、服务器拒绝、效果到期与死亡均从同一标签变化恢复表现，不另存一套 UI 防御状态。
	// 标签与血量通知都应刷新界面并读取 IsDefending；死亡状态可能先于防御效果移除到达。
	UPROPERTY(BlueprintAssignable, Category = "Combat") FmultiplayerDefenseStateChanged OnDefenseStateChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void NotifyControllerChanged() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器完成死亡状态提交后，派生敌人停止 AI；客户端表现从复制状态恢复。
	virtual void HandleDeath();
	UFUNCTION(BlueprintImplementableEvent, Category = "Combat") void OnCombatDeath();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UAbilitySystemComponent> AbilitySystem;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat") TObjectPtr<UmultiplayerCombatAttributes> CombatAttributes;
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Combat") EMultiplayerCombatTeam CombatTeam = EMultiplayerCombatTeam::Players;
	UPROPERTY(EditDefaultsOnly, Category = "Combat", meta = (ClampMin = "1")) float InitialMaxHealth = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "Combat", meta = (ClampMin = "0")) float InitialMaxEnergy = 100.0f;
	bool bGrantSupportAbilities = true;

private:
	float GetEffectRemaining(const FGameplayTag& Tag) const;
	void HandleAttributeChanged(const FOnAttributeChangeData& Change);
	void HandleDefenseTagChanged(FGameplayTag Tag, int32 NewCount);
	UFUNCTION() void OnRep_Dead();
	UPROPERTY(ReplicatedUsing = OnRep_Dead) bool bDead = false;
};
