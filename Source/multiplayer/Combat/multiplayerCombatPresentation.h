// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "GameplayCueNotify_Actor.h"
#include "GameFramework/HUD.h"
#include "multiplayerCombatPresentation.generated.h"

class AmultiplayerCombatCharacter;
class UAbilitySystemComponent;
class UBorder;
class UDecalComponent;
class UImage;
class UNiagaraComponent;
class UParticleSystemComponent;
class UProgressBar;
class UTextBlock;
struct FmultiplayerCombatFeedback;
struct FStreamableHandle;

// Cue 只处理本地音画；不生成伤害、不修改标签、不决定技能是否成功。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatCue : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AmultiplayerCombatCue();
	virtual bool OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool OnActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool WhileActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	virtual bool Recycle() override;
protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	const FmultiplayerCombatFeedback* GetFeedback(AActor* Target) const;
	void StartLoop(AActor* Target, const FGameplayCueParameters& Parameters);
	void StopPresentation();
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> LoopEffect;
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopCascadeEffect;
	UPROPERTY(Transient) TObjectPtr<UDecalComponent> AreaMarker;
	TSharedPtr<FStreamableHandle> LoadingAssets;
	FVector MarkerLocation = FVector::ZeroVector;
	FVector MarkerNormal = FVector::UpVector;
	bool bHasMarkerLocation = false;
	bool bLoopRequested = false;
	bool bExecutionLoadAttempted = false;
};

// 蓝图拥有布局；本类只负责订阅角色、初始化快照和低频刷新技能计时，不在 Tick 查询血量。
UCLASS(Abstract, Blueprintable)
class MULTIPLAYER_API UmultiplayerCombatWidget : public UUserWidget
{
	GENERATED_BODY()
protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> EnergyBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> CastBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> AttackCooldownBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> DefenseCooldownBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealCooldownBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> AreaCooldownBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> AttackSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> DefenseSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> HealSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> AreaSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> AttackIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> DefenseIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> HealIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> AreaIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> CastingPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> FeedbackPanel;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealthText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> EnergyText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CastText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StateText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AttackCooldownText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DefenseCooldownText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealCooldownText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AreaCooldownText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FeedbackText;
private:
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	UFUNCTION() void HandlePawnDestroyed(AActor* DestroyedActor);
	UFUNCTION() void RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	UFUNCTION() void RefreshState();
	UFUNCTION() void ShowFeedback(FText Message);
	void ClearFeedback();
	void RefreshTimers();
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	FTimerHandle RefreshTimer;
	FTimerHandle FeedbackTimer;
	float CachedEnergy = 0;
	bool bAlive = false;
	bool bReady = false;
	bool bCasting = false;
	bool bStunned = false;
};

// 敌人头顶条只观察复制后的生命属性；源素材的嵌套布局保留，源属性组件和业务图不参与运行。
UCLASS(Abstract, Blueprintable)
class MULTIPLAYER_API UmultiplayerEnemyHealthWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	// 先订阅再补读快照，支持晚加入、重新绑定以及 Widget 重建。
	UFUNCTION(BlueprintCallable, Category = "Combat|UI")
	void BindCharacter(AmultiplayerCombatCharacter* NewCharacter);
protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UmultiplayerEnemyHealthWidget> Health;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> LerpAttributeBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealthText;
private:
	UFUNCTION() void RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy);
	TWeakObjectPtr<AmultiplayerCombatCharacter> ObservedCharacter;
	TWeakObjectPtr<AmultiplayerCombatCharacter> BoundCharacter;
};

// HUD 只创建/释放本地控件；布局和资产由派生蓝图选择，专服不会创建 UI。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatHUD : public AHUD
{
	GENERATED_BODY()
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UUserWidget> CombatWidgetClass;
private:
	UPROPERTY(Transient) TObjectPtr<UUserWidget> CombatWidget;
};

// 动画只读取 CMC 的实际运动；技能状态、伤害和网络裁决仍由 GAS 维护。
UCLASS(Transient, Blueprintable)
class MULTIPLAYER_API UmultiplayerCombatAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") float CombatMoveSpeed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") float CombatMoveDirection = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") bool CombatShouldMove = false;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") bool CombatIsFalling = false;
};
