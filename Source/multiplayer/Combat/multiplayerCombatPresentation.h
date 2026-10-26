// Copyright Epic Games, Inc. All Rights Reserved.
// 表现接口集合：Cue、事件驱动 HUD、动画采样与旧布局兼容；均不拥有权威战斗结算。
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatAbility.h"
#include "GameplayCueNotify_Actor.h"
#include "GameFramework/HUD.h"
#include "multiplayerCombatPresentation.generated.h"

// [职责] 本头聚合现有表现类型，按 Cue、战斗 HUD、成长菜单、存量 Widget 父类、Canvas、动画分段。
// 它们分别持有自己的订阅/组件，不共享一份“表现管理器”，也不拥有权威属性。
// [边界] BlueprintImplementableEvent 只把只读快照交给保存的 WBP；规则请求仍回到角色/组件入口。

class AmultiplayerCombatCharacter;
class AmultiplayerCoopGameState;
class UAbilitySystemComponent;
class UBorder;
class UButton;
class UDecalComponent;
class UImage;
class UFont;
class UNiagaraComponent;
class UParticleSystemComponent;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UHorizontalBox;
class UmultiplayerCombatProgressionComponent;
struct FmultiplayerCombatTargetPreview;
struct FStreamableHandle;

// Cue 只处理本地音画；不生成伤害、不修改标签、不决定技能是否成功。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatCue : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	// 配置移除即清理的 Cue 行为，不启用常驻 Tick。
	AmultiplayerCombatCue();
	// 执行一次性结果音画，异步补载只恢复表现。
	virtual bool OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	// 持续效果首次生效时启动循环表现和起始声音。
	virtual bool OnActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	// 晚加入或相关性恢复时补持续表现，不重放过去的起始声音。
	virtual bool WhileActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	// 效果移除后清理自身循环组件和补载请求。
	virtual bool OnRemove_Implementation(AActor* Target, const FGameplayCueParameters& Parameters) override;
	// 复用前清空旧 Cue 状态，不把引擎 Cue 复用等同于自研 Actor 池。
	virtual bool Recycle() override;
protected:
	// 退出世界也走统一清理，不能依赖一定先收到 OnRemove。
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	// 选择该 Cue 的资源配置；治疗采用实际施法者配置。
	const FmultiplayerCombatFeedback* GetFeedback(AActor* Target, const FGameplayCueParameters* Parameters = nullptr) const;
	// 创建/更新持续组件，异步加载结束时重新检查本次是否仍需要显示。
	void StartLoop(AActor* Target, const FGameplayCueParameters& Parameters);
	// 先撤销循环意图再释放资源，阻止旧加载回调复活表现。
	void StopPresentation();
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> LoopEffect;
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopCascadeEffect;
	UPROPERTY(Transient) TObjectPtr<UDecalComponent> AreaMarker;
	// 异步补载固定本次施法的表现配置，不因施法者断线而改读受治疗者的配置。
	UPROPERTY(Transient) FmultiplayerCombatFeedback ExecutionFeedback;
	// 原生 Context 携带同一次 GA 身份，补载不再另取本地清理代数。
	TWeakObjectPtr<AmultiplayerCombatCharacter> AreaFeedbackOwner;
	FmultiplayerCombatActivationIdentity AreaFeedbackIdentity;
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
public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") TMap<FName, FText> TextTemplates;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") TArray<FText> SkillNames;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style") FLinearColor AvailableIconColor{};
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style") FLinearColor UnavailableIconColor{};
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style") FLinearColor SlotBackgroundColor{};
	// Read-only view data is consumed by the real WBP event graph; no native control setters.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewHealthPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewEnergyPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewCastPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewHealthText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewEnergyText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewCastText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewStateText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewProgressionText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewFeedbackText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") ESlateVisibility ViewCastVisibility = ESlateVisibility::Collapsed;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") ESlateVisibility ViewStateVisibility = ESlateVisibility::Collapsed;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") ESlateVisibility ViewFeedbackVisibility = ESlateVisibility::Collapsed;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewAttackCooldownText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewAttackCooldownPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewAttackName;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FSlateBrush ViewAttackBrush;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FLinearColor ViewAttackTint{};
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewDefenseCooldownText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewDefenseCooldownPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewDefenseName;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FSlateBrush ViewDefenseBrush;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FLinearColor ViewDefenseTint{};
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewHealCooldownText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewHealCooldownPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewHealName;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FSlateBrush ViewHealBrush;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FLinearColor ViewHealTint{};
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewAreaCooldownText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewAreaCooldownPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewAreaName;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FSlateBrush ViewAreaBrush;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FLinearColor ViewAreaTint{};
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewDashCooldownText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewDashCooldownPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewDashName;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FSlateBrush ViewDashBrush;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FLinearColor ViewDashTint{};
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style") TArray<FSlateBrush> SkillIcons;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style", meta = (ClampMin = "0.01")) float FeedbackLifetime = 0;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Style", meta = (ClampMin = "0.01")) float CountdownRefreshInterval = 0;
protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "Combat|UI") void PresentCombatSnapshot();
	// 订阅拥有者 Pawn 变化，绑定当前角色并补读快照。
	virtual void NativeConstruct() override;
	// 控件移除时解绑旧对象和计时器，重建不累积监听。
	virtual void NativeDestruct() override;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> EnergyBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> CastBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> AttackCooldownBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> DefenseCooldownBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealCooldownBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> AreaCooldownBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> DashCooldownBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> AttackSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> DefenseSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> HealSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> AreaSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> DashSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> AttackIcon;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> DefenseIcon;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> HealIcon;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> AreaIcon;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UImage> DashIcon;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> CastingPanel;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UBorder> FeedbackPanel;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealthText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> EnergyText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CastText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StateText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AttackCooldownText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DefenseCooldownText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealCooldownText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AreaCooldownText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DashCooldownText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ProgressionText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FeedbackText;
private:
	// 旧角色、ASC 与共享状态先解绑，新对象订阅后再初始化数值。
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	// 销毁后清除绑定，等待控制器提供新 Pawn。
	UFUNCTION() void HandlePawnDestroyed(AActor* DestroyedActor);
	// 目标进度事件只触发当前共享快照重读。
	UFUNCTION() void HandleObjectiveChanged(int32 Activated, int32 Required);
	// 属性事件刷新生命/能量及依赖资源的槽位外观。
	UFUNCTION() void RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	// 事件与活动倒计时共用快照刷新，不决定角色玩法状态。
	UFUNCTION() void RefreshState();
	// 显示短时提示，下一条消息替换旧消息。
	UFUNCTION() void ShowFeedback(FText Message);
	// 清除提示与其隐藏计时器。
	void ClearFeedback();
	// 汇总当前整局目标、生命阶段与控制效果说明。
	void RefreshStatusText();
	const FText& TemplateText(FName Key) const;
	FText FormatNumber(float Value, int32 DecimalPlaces) const;
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	TWeakObjectPtr<UmultiplayerCombatProgressionComponent> BoundProgression;
	TWeakObjectPtr<AmultiplayerCoopGameState> BoundGameState;
	FTimerHandle RefreshTimer;
	FTimerHandle FeedbackTimer;
	bool bPresentationReady = false;
};

// B 键打开真实 UMG 蓝图；布局与文案由蓝图保存，原生只订阅快照并发送业务请求。
UCLASS(Abstract, Blueprintable)
class MULTIPLAYER_API UmultiplayerCombatProgressionWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	// 只移除成长面板，保留原 HUD。
	UFUNCTION(BlueprintCallable, Category = "Combat|UI") void CloseMenu();
	// 以下只读表现模板由 WBP 默认值保存；数字来自当前已提交的成长快照。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") TArray<FText> SkillNames;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") TArray<FText> SlotKeys;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") TArray<FText> PassiveNames;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SummaryFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText WaitingText;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SkillLineFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SkillLevelFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText UnlearnedText;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText UnlockText;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SelectedSkillFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SlotFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText PassivesFormat;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText SelectedPassiveFormat;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewSummary;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewSkills;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewSelectedSkill;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewSelectedSlot;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewPassives;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewSelectedPassive;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewMessage;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanLearnSkill = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanEquipSkill = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanLearnPassive = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanEquipPassive = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanUnequipPassive = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanSaveProgress = false;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") bool ViewCanLoadProgress = false;
protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "Combat|UI") void PresentProgressionSnapshot();
	// 订阅角色并取得菜单焦点，界面仍只发业务请求。
	virtual void NativeConstruct() override;
	// 解除监听，并只恢复本面板曾接管的输入模式。
	virtual void NativeDestruct() override;
	// 提前消费 B/Esc 关闭键，避免同次按键又打开面板。
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
	// 解绑旧角色后订阅新角色、成长与能力活动通知。
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	// 角色销毁时留空菜单，等待下一次 Pawn 绑定。
	UFUNCTION() void HandlePawnDestroyed(AActor* Actor);
	// 按事件和本地选择重绘已提交的成长快照。
	UFUNCTION() void RefreshMenu();
	// 属性变化只刷新菜单可操作资格。
	UFUNCTION() void HandleStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	// 显示服务器返回的成长或存档操作结果。
	UFUNCTION() void ShowMessage(FText Message);
	// 本地循环选择上一个技能。
	UFUNCTION() void PreviousSkill();
	// 本地循环选择下一个技能。
	UFUNCTION() void NextSkill();
	// 请求学习/升级所选技能，服务器检查等级与点数。
	UFUNCTION() void LearnSkill();
	// 本地选择上一个目标槽位。
	UFUNCTION() void PreviousSlot();
	// 本地选择下一个目标槽位。
	UFUNCTION() void NextSlot();
	// 请求装配或交换所选技能。
	UFUNCTION() void EquipSkill();
	// 本地选择前一个被动。
	UFUNCTION() void PreviousPassive();
	// 本地选择后一个被动。
	UFUNCTION() void NextPassive();
	// 请求学习所选被动，不直接施加属性效果。
	UFUNCTION() void LearnPassive();
	// 请求装备所选已学习被动。
	UFUNCTION() void EquipPassive();
	// 请求移除当前被动，成长基础效果保留。
	UFUNCTION() void UnequipPassive();
	// 经角色所属连接请求服务器保存成长。
	UFUNCTION() void SaveProgress();
	// 经角色所属连接请求服务器加载成长。
	UFUNCTION() void LoadProgress();
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionSummary;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionSkills;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionSelectedSkill;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionSelectedSlot;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionPassives;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionSelectedPassive;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UTextBlock> ProgressionMessage;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> PreviousSkillButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> NextSkillButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> LearnSkillButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> PreviousSlotButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> NextSlotButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> EquipSkillButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> PreviousPassiveButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> NextPassiveButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> LearnPassiveButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> EquipPassiveButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> UnequipPassiveButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> SaveProgressButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> LoadProgressButton;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget, AllowPrivateAccess = "true")) TObjectPtr<UButton> CloseProgressionButton;
	TWeakObjectPtr<AmultiplayerCombatCharacter> Character;
	TWeakObjectPtr<UmultiplayerCombatProgressionComponent> BoundProgression;
	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	int32 SelectedSkill = 0;
	int32 SelectedSlot = 0;
	int32 SelectedPassive = 0;
	bool bPresentationReady = false;
	bool bOwnsInputMode = false;
};

// 两份存量血条 WBP 仍引用此父类及快照字段，保留供素材读取；当前敌人不创建或绑定这些 Widget。
UCLASS(Abstract, Blueprintable)
class MULTIPLAYER_API UmultiplayerEnemyHealthWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	// 先订阅再补读快照，支持晚加入、重新绑定以及 Widget 重建。
	UFUNCTION(BlueprintCallable, Category = "Combat|UI")
	void BindCharacter(AmultiplayerCombatCharacter* NewCharacter);
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") float ViewHealthPercent = 0;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") FText ViewHealthText;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|UI|Snapshot") ESlateVisibility ViewVisibility = ESlateVisibility::Collapsed;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|UI|Text") FText HealthTextFormat;
protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "Combat|UI") void PresentHealthSnapshot();
	// Slate 重建后恢复弱观察对象的事件订阅。
	virtual void NativeConstruct() override;
	// 解除活动订阅，但保留后续重建所需的弱观察对象。
	virtual void NativeDestruct() override;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UmultiplayerEnemyHealthWidget> Health;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> LerpAttributeBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealthText;
private:
	// 保留存量布局的事件快照适配；不再持有世界组件或申请 RenderTarget 重绘。
	UFUNCTION() void RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy);
	TWeakObjectPtr<AmultiplayerCombatCharacter> ObservedCharacter;
	TWeakObjectPtr<AmultiplayerCombatCharacter> BoundCharacter;
	bool bPresentationReady = false;
	float LastDisplayedHealth = -1.f;
	float LastDisplayedMaxHealth = -1.f;
};

// 本帧只读屏幕几何。原生只产生数据，真正的 DrawLine / DrawText 由唯一 HUD 蓝图执行。
USTRUCT(BlueprintType)
struct FmultiplayerCombatHUDLine
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) float StartScreenX = 0;
	UPROPERTY(BlueprintReadOnly) float StartScreenY = 0;
	UPROPERTY(BlueprintReadOnly) float EndScreenX = 0;
	UPROPERTY(BlueprintReadOnly) float EndScreenY = 0;
	UPROPERTY(BlueprintReadOnly) FLinearColor LineColor{};
	UPROPERTY(BlueprintReadOnly) float LineThickness = 0;
};

USTRUCT(BlueprintType)
struct FmultiplayerCombatHUDText
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) FString Text;
	UPROPERTY(BlueprintReadOnly) FLinearColor TextColor{};
	UPROPERTY(BlueprintReadOnly) float ScreenX = 0;
	UPROPERTY(BlueprintReadOnly) float ScreenY = 0;
	UPROPERTY(BlueprintReadOnly) TObjectPtr<UFont> Font;
	UPROPERTY(BlueprintReadOnly) float Scale = 0;
};

// 样式、文案与装饰几何参数由共享 HUD 蓝图 CDO 保存，不参与命中或权威范围计算。
USTRUCT(BlueprintType)
struct FmultiplayerCombatHUDPresentation
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor ReadyColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor BlockedColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor HintColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor CastingColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor ResolvedColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor DangerColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor ShadowColor{};
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float LineThickness = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerThickness = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float HintOpacity = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float TelegraphCenterOpacity = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float PromptHeightRatio = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MeleePromptHeightRatio = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float DetailOffset = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float PromptReferenceWidth = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float PromptMinScale = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float PromptMaxScale = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerMinHalfWidth = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerSidePadding = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerTopPadding = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerBottomPadding = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerCornerRatio = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerCornerMax = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerLabelGap = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float MarkerTextScale = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float GroundLift = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float TargetGroundLift = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float CrossHalfSize = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float DashArrowLength = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float DashArrowSideScale = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float TelegraphDistance = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) float ResolvedDuration = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 RingSegments = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 TelegraphSegments = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 DecimalPlaces = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FVector2D ShadowOffset = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TObjectPtr<UFont> Font;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FText> SkillNames;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FText> SlotKeys;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText MeleeWindup;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText MeleeDanger;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText ConfirmAttack;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText ConfirmReleaseFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText SkillActionFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText HealSelf;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText HealTeammate;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText HealDetailFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DefenseDetailFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText AreaDetailFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DashDetailFormat;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText Casting;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText Resolved;
	UPROPERTY(EditAnywhere, BlueprintReadOnly) FText Idle;
};

// HUD 管理本地控件和只读帧快照；唯一共享蓝图负责实际绘制，专服不会创建 UI。
// 预览只读取当前状态，不代替服务器的技能接纳、目标复核或伤害结算。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatHUD : public AHUD
{
	GENERATED_BODY()
public:
	// 瞬时结果保留短时精确边界；粒子大小不作为实际伤害范围的依据。
	void ShowResolvedArea(const FVector& Center, float Radius);
	// 右键清除短时结果边界；不改预览配置或实际命中半径。
	void ClearResolvedArea() { ResolvedAreaUntil = 0.0; }
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|HUD") FmultiplayerCombatHUDPresentation Presentation;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|HUD") TArray<FmultiplayerCombatHUDLine> HUDLines;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Combat|HUD") TArray<FmultiplayerCombatHUDText> HUDTexts;
protected:
	// 本地创建一份配置 HUD，专服不创建 UI；Listen Server 的本地主机仍可显示。
	virtual void BeginPlay() override;
	// 先计算当前帧快照，再让 Super 触发真正蓝图 ReceiveDrawHUD；此处不提交绘制。
	virtual void DrawHUD() override;
	// 移除自己创建的控件，防止退出后遗留视口对象。
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UUserWidget> CombatWidgetClass;
private:
	// 按技能种类投影同源查询的几何，不用粒子尺寸表示命中边界。
	void BuildTargetPreview(const FmultiplayerCombatTargetPreview& Preview, const FLinearColor& Color);
	// 复用当前 Canvas 视图，把有效世界点投影到屏幕。
	bool ProjectTargetPoint(const FVector& Location, FVector2D& Screen) const;
	// 缓存本帧场景遮挡结果，圈线相邻端点不重复射线。
	bool IsRangePointVisible(const FVector& Location);
	// 计算不被场景遮挡的屏幕线段数据。
	void AppendWorldSegment(const FVector& Start, const FVector& End, const FLinearColor& Color, float Thickness);
	// 在指定平面按真实半径生成圆环投影数据。
	void AppendWorldRing(const FVector& Center, const FVector& Normal, float Radius, const FLinearColor& Color);
	// 为当前有效目标绘制角标与说明，不代替服务器选目标。
	void BuildTargetMarker(const AmultiplayerCombatCharacter* Target, const FLinearColor& Color, const FText& Label);
	// 从复制后的近战起手状态绘制预警，不读取服务器黑板。
	void BuildEnemyTelegraphs(const AmultiplayerCombatCharacter* LocalCharacter);
	// 计算居中提示的屏幕坐标，实际字体绘制由蓝图消费快照。
	void AppendCenteredPrompt(const FText& Text, float Y, const FLinearColor& Color);
	void AppendScreenSegment(float AX, float AY, float BX, float BY, const FLinearColor& Color, float Thickness);
	// 添加一条本帧文字数据，不调用 Canvas 绘制，也不修改提示来源的业务状态。
	void AppendScreenLabel(const FText& Text, float X, float Y, const FLinearColor& Color, float Scale);
	// 每帧重建几何快照；生命/能量文本仍由属性事件更新，不跟随此函数轮询。
	void BuildHUDFrame();
	// 只按保存的单位和格式展示距离，不改变技能配置使用的厘米单位。
	FText FormatDistance(float Value) const;
	UPROPERTY(Transient) TObjectPtr<UUserWidget> NativeCombatWidget;
	FVector ResolvedAreaCenter = FVector::ZeroVector;
	float ResolvedAreaRadius = 0.f;
	double ResolvedAreaUntil = 0.0;
	FVector RangeViewLocation = FVector::ZeroVector;
	TMap<FVector, bool> RangeVisibilityCache;
};

// 动画只读取 CMC 的实际运动与 GAS 防御状态；技能状态、伤害和网络裁决仍由 GAS 维护。
UCLASS(Transient, Blueprintable)
class MULTIPLAYER_API UmultiplayerCombatAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	// 采样 CMC 速度和 GAS 防御状态供动画图读取，不回写玩法状态。
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") float CombatMoveSpeed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") float CombatMoveDirection = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") bool CombatShouldMove = false;
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") bool CombatIsFalling = false;
	// 防御是持续效果而不是持续激活的 Ability；预测接管、拒绝和到期都读取同一标签快照。
	UPROPERTY(BlueprintReadOnly, Category = "Combat Animation") bool CombatIsDefending = false;
};
