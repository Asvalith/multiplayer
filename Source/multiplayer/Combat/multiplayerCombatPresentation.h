// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Components/WidgetComponent.h"
#include "Combat/multiplayerCombatConfig.h"
#include "GameplayCueNotify_Actor.h"
#include "GameFramework/HUD.h"
#include "multiplayerCombatPresentation.generated.h"

class AmultiplayerCombatCharacter;
class AmultiplayerCoopGameState;
class UAbilitySystemComponent;
class UBorder;
class UButton;
class UDecalComponent;
class UImage;
class UNiagaraComponent;
class UParticleSystemComponent;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UHorizontalBox;
class UmultiplayerCombatProgressionComponent;
struct FmultiplayerCombatFeedback;
struct FmultiplayerCombatTargetPreview;
struct FStreamableHandle;

// 每端独立的世界空间血条：仅朝向本地镜头，内容按生命事件重绘，不复制展示旋转。
UCLASS()
class MULTIPLAYER_API UmultiplayerEnemyHealthWidgetComponent : public UWidgetComponent
{
	GENERATED_BODY()
public:
	UmultiplayerEnemyHealthWidgetComponent();
	void ConfigureWorldDefaults();
	void InitializeForCharacter(AmultiplayerCombatCharacter* Character);
	void StopPresentation();
	void NotifyContentChanged();
	// 独立于网络与输入的展示入口，也供无窗口测试核对镜头/距离边界。
	void UpdateForView(const FVector& ViewLocation);
	virtual void InitWidget() override;
	uint32 GetContentRevision() const { return ContentRevision; }
protected:
	virtual void OnRegister() override;
	virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	uint32 ContentRevision = 0;
	bool bPresentationStopped = false;
};

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
	const FmultiplayerCombatFeedback* GetFeedback(AActor* Target, const FGameplayCueParameters* Parameters = nullptr) const;
	void StartLoop(AActor* Target, const FGameplayCueParameters& Parameters);
	void StopPresentation();
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> LoopEffect;
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopCascadeEffect;
	UPROPERTY(Transient) TObjectPtr<UDecalComponent> AreaMarker;
	// 异步补载固定本次施法的表现配置，不因施法者断线而改读受治疗者的配置。
	UPROPERTY(Transient) FmultiplayerCombatFeedback ExecutionFeedback;
	TSharedPtr<FStreamableHandle> LoadingAssets;
	FVector MarkerLocation = FVector::ZeroVector;
	FVector MarkerNormal = FVector::UpVector;
	bool bHasMarkerLocation = false;
	bool bLoopRequested = false;
	bool bExecutionLoadAttempted = false;
	bool bHasExecutionFeedback = false;
	bool bExecutionRangeShown = false;
	bool bExecutionHealNotified = false;
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
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> DashCooldownBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> AttackSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> DefenseSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> HealSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> AreaSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UBorder> DashSlot;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> AttackIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> DefenseIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> HealIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> AreaIcon;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UImage> DashIcon;
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
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DashCooldownText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ProgressionText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FeedbackText;
private:
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	UFUNCTION() void HandlePawnDestroyed(AActor* DestroyedActor);
	UFUNCTION() void HandleObjectiveChanged(int32 Activated, int32 Required);
	UFUNCTION() void RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	UFUNCTION() void RefreshState();
	UFUNCTION() void ShowFeedback(FText Message);
	void ClearFeedback();
	void RefreshTimers();
	void RefreshStatusText();
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	TWeakObjectPtr<UmultiplayerCombatProgressionComponent> BoundProgression;
	TWeakObjectPtr<AmultiplayerCoopGameState> BoundGameState;
	UPROPERTY(Transient) TArray<FSlateBrush> SkillBrushes;
	FTimerHandle RefreshTimer;
	FTimerHandle FeedbackTimer;
	float CachedEnergy = 0;
	bool bAlive = false;
	bool bReady = false;
	bool bCasting = false;
	bool bStunned = false;
};

// B 键打开的附加成长面板，原 HUD 和蓝图布局不变；所有按钮只发服务器业务请求。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatProgressionWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Combat|UI") void CloseMenu();
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
	void BuildLayout();
	UTextBlock* AddLabel(UVerticalBox* Parent, FName Name, int32 Size = 16);
	UButton* AddButton(UHorizontalBox* Parent, FName Name, const TCHAR* Label);
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	UFUNCTION() void HandlePawnDestroyed(AActor* Actor);
	UFUNCTION() void RefreshMenu();
	UFUNCTION() void HandleStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	UFUNCTION() void ShowMessage(FText Message);
	UFUNCTION() void PreviousSkill();
	UFUNCTION() void NextSkill();
	UFUNCTION() void LearnSkill();
	UFUNCTION() void PreviousSlot();
	UFUNCTION() void NextSlot();
	UFUNCTION() void EquipSkill();
	UFUNCTION() void PreviousPassive();
	UFUNCTION() void NextPassive();
	UFUNCTION() void LearnPassive();
	UFUNCTION() void EquipPassive();
	UFUNCTION() void UnequipPassive();
	UFUNCTION() void SaveProgress();
	UFUNCTION() void LoadProgress();
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SummaryText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SkillsText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SelectedSkillText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SlotText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PassivesText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SelectedPassiveText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MenuMessage;
	UPROPERTY(Transient) TObjectPtr<UButton> LearnSkillButton;
	UPROPERTY(Transient) TObjectPtr<UButton> EquipSkillButton;
	UPROPERTY(Transient) TObjectPtr<UButton> LearnPassiveButton;
	UPROPERTY(Transient) TObjectPtr<UButton> EquipPassiveButton;
	UPROPERTY(Transient) TObjectPtr<UButton> UnequipPassiveButton;
	UPROPERTY(Transient) TObjectPtr<UButton> SaveButton;
	UPROPERTY(Transient) TObjectPtr<UButton> LoadButton;
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	TWeakObjectPtr<UmultiplayerCombatProgressionComponent> BoundProgression;
	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	int32 SelectedSkill = 0;
	int32 SelectedSlot = 0;
	int32 SelectedPassive = 0;
	bool bOwnsInputMode = false;
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
	void SetPresentationComponent(UmultiplayerEnemyHealthWidgetComponent* Component);
	void RefreshSnapshot();
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
	TWeakObjectPtr<UmultiplayerEnemyHealthWidgetComponent> PresentationComponent;
	float LastDisplayedHealth = -1.f;
	float LastDisplayedMaxHealth = -1.f;
};

// HUD 只创建/释放本地控件；布局和资产由派生蓝图选择，专服不会创建 UI。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatHUD : public AHUD
{
	GENERATED_BODY()
public:
	// 瞬时结果保留短时精确边界；粒子大小不作为实际伤害范围的依据。
	void ShowResolvedArea(const FVector& Center, float Radius);
protected:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UUserWidget> CombatWidgetClass;
private:
	void DrawTargetPreview(const FmultiplayerCombatTargetPreview& Preview, const FLinearColor& Color);
	bool ProjectTargetPoint(const FVector& Location, FVector2D& Screen) const;
	bool IsRangePointVisible(const FVector& Location);
	void DrawWorldSegment(const FVector& Start, const FVector& End, const FLinearColor& Color, float Thickness = 1.5f);
	void DrawWorldRing(const FVector& Center, const FVector& Normal, float Radius, const FLinearColor& Color);
	void DrawTargetMarker(const AmultiplayerCombatCharacter* Target, const FLinearColor& Color, const FString& Label);
	void DrawEnemyTelegraphs(const AmultiplayerCombatCharacter* LocalCharacter);
	void DrawCenteredPrompt(const FString& Text, float Y, const FLinearColor& Color);
	UPROPERTY(Transient) TObjectPtr<UUserWidget> CombatWidget;
	FVector ResolvedAreaCenter = FVector::ZeroVector;
	float ResolvedAreaRadius = 0.f;
	double ResolvedAreaUntil = 0.0;
	FVector RangeViewLocation = FVector::ZeroVector;
	TMap<FVector, bool> RangeVisibilityCache;
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
