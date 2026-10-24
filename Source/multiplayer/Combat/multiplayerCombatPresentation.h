// Copyright Epic Games, Inc. All Rights Reserved.
// 表现接口集合：世界血条、Cue、事件驱动 HUD 与动画采样；均不拥有权威战斗结算。
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
struct FmultiplayerCombatTargetPreview;
struct FStreamableHandle;

// 每端独立的世界空间血条：仅朝向本地镜头，内容按生命事件重绘，不复制展示旋转。
UCLASS()
class MULTIPLAYER_API UmultiplayerEnemyHealthWidgetComponent : public UWidgetComponent
{
	GENERATED_BODY()
public:
	// 配置各端独立的世界血条，禁止复制 UI 组件。
	UmultiplayerEnemyHealthWidgetComponent();
	// 统一深度遮挡、尺寸和手动重绘规则，纠正旧蓝图的屏幕空间覆盖。
	void ConfigureWorldDefaults();
	// 设置头顶锚点并把实际角色交给内层血条订阅。
	void InitializeForCharacter(AmultiplayerCombatCharacter* Character);
	// 停止绘制并释放观察关系；死亡和退出共用。
	void StopPresentation();
	// 内容变化时才请求 RenderTarget 重绘。
	void NotifyContentChanged();
	// 独立于网络与输入的展示入口，也供无窗口测试核对镜头/距离边界。
	void UpdateForView(const FVector& ViewLocation);
	// 专服跳过 UMG 创建，渲染端交给原生 Widget 初始化。
	virtual void InitWidget() override;
	// 返回实际内容变化次数，供检查无变化时是否仍重复请求重绘。
	uint32 GetContentRevision() const { return ContentRevision; }
protected:
	// 在 Renderer 创建前落实世界空间设置。
	virtual void OnRegister() override;
	// 更新镜头朝向、距离和存活筛选，不逐帧重建数值内容。
	virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	// 世界退出也必须解除血条展示与订阅。
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
	// 订阅拥有者 Pawn 变化，绑定当前角色并补读快照。
	virtual void NativeConstruct() override;
	// 控件移除时解绑旧对象和计时器，重建不累积监听。
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
	// 旧角色、ASC 与共享状态先解绑，新对象订阅后再初始化数值。
	UFUNCTION() void BindPawn(APawn* PreviousPawn, APawn* NewPawn);
	// 销毁后清除绑定，等待控制器提供新 Pawn。
	UFUNCTION() void HandlePawnDestroyed(AActor* DestroyedActor);
	// 目标进度事件只触发当前共享快照重读。
	UFUNCTION() void HandleObjectiveChanged(int32 Activated, int32 Required);
	// 属性事件刷新生命/能量及依赖资源的槽位外观。
	UFUNCTION() void RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy);
	// 重建当前状态区与读条显示，不决定角色玩法状态。
	UFUNCTION() void RefreshState();
	// 显示短时提示，下一条消息替换旧消息。
	UFUNCTION() void ShowFeedback(FText Message);
	// 清除提示与其隐藏计时器。
	void ClearFeedback();
	// 只在存在活动冷却/读条时维持低频数字刷新。
	void RefreshTimers();
	// 汇总当前整局目标、生命阶段与控制效果说明。
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
	// 只移除成长面板，保留原 HUD。
	UFUNCTION(BlueprintCallable, Category = "Combat|UI") void CloseMenu();
protected:
	// Slate 创建前按需构造附加面板的控件树。
	virtual TSharedRef<SWidget> RebuildWidget() override;
	// 订阅角色并取得菜单焦点，界面仍只发业务请求。
	virtual void NativeConstruct() override;
	// 解除监听，并只恢复本面板曾接管的输入模式。
	virtual void NativeDestruct() override;
	// 提前消费 B/Esc 关闭键，避免同次按键又打开面板。
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
	// 一次建立菜单布局与按钮事件，不修改原 HUD 蓝图。
	void BuildLayout();
	// 生成统一文字样式和稳定控件名称。
	UTextBlock* AddLabel(UVerticalBox* Parent, FName Name, int32 Size = 16);
	// 生成按钮外观，业务动作由调用者绑定。
	UButton* AddButton(UHorizontalBox* Parent, FName Name, const TCHAR* Label);
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
	// 向内层素材血条转交唯一的重绘通知接收器。
	void SetPresentationComponent(UmultiplayerEnemyHealthWidgetComponent* Component);
	// 刚可见或重建时主动补读生命快照。
	void RefreshSnapshot();
protected:
	// Slate 重建后恢复弱观察对象的事件订阅。
	virtual void NativeConstruct() override;
	// 解除活动订阅，但保留后续重建所需的弱观察对象。
	virtual void NativeDestruct() override;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UmultiplayerEnemyHealthWidget> Health;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UProgressBar> LerpAttributeBar;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> HealthText;
private:
	// 生命显示变化才改文字/进度并申请重绘，能量通知不会反复刷新相同血条。
	UFUNCTION() void RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy);
	TWeakObjectPtr<AmultiplayerCombatCharacter> ObservedCharacter;
	TWeakObjectPtr<AmultiplayerCombatCharacter> BoundCharacter;
	TWeakObjectPtr<UmultiplayerEnemyHealthWidgetComponent> PresentationComponent;
	float LastDisplayedHealth = -1.f;
	float LastDisplayedMaxHealth = -1.f;
};

// HUD 管理本地控件并投影瞄准/敌人预警；布局和资产由派生蓝图选择，专服不会创建 UI。
// 预览只读取当前状态，不代替服务器的技能接纳、目标复核或伤害结算。
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerCombatHUD : public AHUD
{
	GENERATED_BODY()
public:
	// 瞬时结果保留短时精确边界；粒子大小不作为实际伤害范围的依据。
	void ShowResolvedArea(const FVector& Center, float Radius);
protected:
	// 本地创建一份配置 HUD，专服不创建 UI；Listen Server 的本地主机仍可显示。
	virtual void BeginPlay() override;
	// 每帧投影预览几何、资格说明与敌人预警，不逐帧重建 UMG 数值内容。
	virtual void DrawHUD() override;
	// 移除自己创建的控件，防止退出后遗留视口对象。
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UUserWidget> CombatWidgetClass;
private:
	// 按技能种类投影同源查询的几何，不用粒子尺寸表示命中边界。
	void DrawTargetPreview(const FmultiplayerCombatTargetPreview& Preview, const FLinearColor& Color);
	// 复用当前 Canvas 视图，把有效世界点投影到屏幕。
	bool ProjectTargetPoint(const FVector& Location, FVector2D& Screen) const;
	// 缓存本帧场景遮挡结果，圈线相邻端点不重复射线。
	bool IsRangePointVisible(const FVector& Location);
	// 绘制不被场景遮挡的世界线段。
	void DrawWorldSegment(const FVector& Start, const FVector& End, const FLinearColor& Color, float Thickness = 1.5f);
	// 在指定平面按真实半径分段绘制圆环。
	void DrawWorldRing(const FVector& Center, const FVector& Normal, float Radius, const FLinearColor& Color);
	// 为当前有效目标绘制角标与说明，不代替服务器选目标。
	void DrawTargetMarker(const AmultiplayerCombatCharacter* Target, const FLinearColor& Color, const FString& Label);
	// 从复制后的近战起手状态绘制预警，不读取服务器黑板。
	void DrawEnemyTelegraphs(const AmultiplayerCombatCharacter* LocalCharacter);
	// 在视口中部绘制操作或失败提示。
	void DrawCenteredPrompt(const FString& Text, float Y, const FLinearColor& Color);
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
