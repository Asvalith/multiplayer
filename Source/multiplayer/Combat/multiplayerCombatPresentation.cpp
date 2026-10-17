// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatPresentation.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/Border.h"
#include "Components/DecalComponent.h"
#include "Components/Image.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

AmultiplayerCombatCue::AmultiplayerCombatCue()
{
	bAutoDestroyOnRemove = true;
	bAutoAttachToOwner = false;
	PrimaryActorTick.bCanEverTick = false;
}

const FmultiplayerCombatFeedback* AmultiplayerCombatCue::GetFeedback(AActor* Target) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(Target);
	if (!Character) return nullptr;
	const auto& Rules = Character->GetCombatConfig();
	if (GameplayCueTag == CombatTags::Cue_Attack) return &Rules.AttackFeedback;
	if (GameplayCueTag == CombatTags::Cue_Impact) return &Rules.ImpactFeedback;
	if (GameplayCueTag == CombatTags::Cue_Heal) return &Rules.HealFeedback;
	if (GameplayCueTag == CombatTags::Cue_Defense) return &Rules.DefenseFeedback;
	if (GameplayCueTag == CombatTags::Cue_Casting) return &Rules.CastingFeedback;
	if (GameplayCueTag == CombatTags::Cue_Area) return &Rules.AreaFeedback;
	if (GameplayCueTag == CombatTags::Cue_Stun) return &Rules.StunFeedback;
	return &Rules.FailureFeedback;
}

bool AmultiplayerCombatCue::OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	const auto* Feedback = GetFeedback(Target);
	if (!Feedback || GetNetMode() == NM_DedicatedServer) return false;
	if (!bExecutionLoadAttempted && GameplayCueTag == CombatTags::Cue_Impact)
		CastChecked<AmultiplayerCombatCharacter>(Target)->PlayImpactReaction();
	if (bExecutionLoadAttempted && LoadingAssets && !LoadingAssets->HasLoadCompleted()) return true;
	const bool bUseCascade = !Feedback->CascadeEffect.IsNull();
	TArray<FSoftObjectPath> PendingAssets;
	if (bUseCascade && !Feedback->CascadeEffect.IsValid()) PendingAssets.Add(Feedback->CascadeEffect.ToSoftObjectPath());
	else if (!bUseCascade && !Feedback->Effect.IsNull() && !Feedback->Effect.IsValid()) PendingAssets.Add(Feedback->Effect.ToSoftObjectPath());
	if (!Feedback->Sound.IsNull() && !Feedback->Sound.IsValid()) PendingAssets.Add(Feedback->Sound.ToSoftObjectPath());
	if (!bExecutionLoadAttempted && !PendingAssets.IsEmpty())
	{
		// 正常由角色预载；初次相关漏过预载时只补一次异步请求，失败也不会循环加载。
		bExecutionLoadAttempted = true;
		LoadingAssets = UAssetManager::GetStreamableManager().RequestAsyncLoad(PendingAssets,
			FStreamableDelegate::CreateWeakLambda(this, [this, Weak = TWeakObjectPtr<AActor>(Target), Parameters]
			{
				if (Weak.IsValid()) OnExecute_Implementation(Weak.Get(), Parameters);
				else K2_EndGameplayCue();
			}));
		return true;
	}
	const bool bGround = GameplayCueTag == CombatTags::Cue_Area;
	FVector Location = bGround || GameplayCueTag == CombatTags::Cue_Impact
		? FVector(Parameters.Location) : Target->GetActorLocation();
	const auto* Character = CastChecked<AmultiplayerCombatCharacter>(Target);
	if (GameplayCueTag == CombatTags::Cue_Attack && Character->GetMesh()->GetSkeletalMeshAsset())
		Location = Character->GetMesh()->GetSocketLocation(TEXT("hand_r"));
	else if (GameplayCueTag == CombatTags::Cue_Heal)
		Location.Z -= Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	// 投射物尾迹由弹体持有；这里的 AttackFeedback 只能配置一次性起手效果。
	if (bUseCascade)
	{
		if (auto* System = Feedback->CascadeEffect.Get())
			if (auto* Component = UGameplayStatics::SpawnEmitterAtLocation(this, System, Location))
			{
				FTimerHandle Cleanup;
				GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
					[Component] { Component->DestroyComponent(); }), 5.f, false);
			}
	}
	else if (auto* System = Feedback->Effect.Get())
	{
		auto* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, FRotator::ZeroRotator,
			FVector::OneVector, true, false);
		if (Component)
		{
			Component->SetVariableFloat(TEXT("User.Duration"), bGround ? .6f : .3f);
			if (bGround) Component->SetVariableFloat(TEXT("User.Scale_All"), Parameters.RawMagnitude / 100.f);
			Component->Activate(true);
			// 素材后续即使被误设为循环，瞬时命中特效也不能无限遗留在场景中。
			FTimerHandle Cleanup;
			GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
				[Component] { Component->DestroyComponent(); }), 2.f, false);
		}
	}
	if (auto* Sound = Feedback->Sound.Get()) UGameplayStatics::PlaySoundAtLocation(this, Sound, Location);
	// 瞬时 Cue 自身无需存活；生成的粒子由 Niagara 自动回收。
	// Cascade 同样自动回收，并保留超时清理，避免误用循环素材留下组件。
	K2_EndGameplayCue();
	return true;
}

void AmultiplayerCombatCue::StartLoop(AActor* Target, const FGameplayCueParameters& Parameters)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	if (GameplayCueTag == CombatTags::Cue_Casting)
	{
		const auto* Character = Cast<AmultiplayerCombatCharacter>(Target);
		if (!Character) return;
		const auto& Config = Character->GetCombatConfig();
		if (const FHitResult* Hit = Parameters.EffectContext.GetHitResult())
		{
			// 权威快照到达时更新已有标记；异步加载期间也保留最新落点，不能回放旧预测点。
			MarkerLocation = Hit->ImpactPoint;
			MarkerNormal = FVector(Hit->ImpactNormal).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		}
		else if (!bHasMarkerLocation)
			MarkerLocation = Character->GetAreaTarget(&MarkerNormal);
		bHasMarkerLocation = true;
		const FVector Size(32.f, Config.AreaRadius, Config.AreaRadius);
		const FRotator Rotation = (-MarkerNormal).Rotation();
		if (AreaMarker)
		{
			AreaMarker->DecalSize = Size;
			AreaMarker->SetWorldLocationAndRotation(MarkerLocation, Rotation);
			AreaMarker->MarkRenderStateDirty();
			return;
		}
		if (Config.AreaMarkerMaterial.IsNull()) return;
		if (!Config.AreaMarkerMaterial.IsValid())
		{
			if (!LoadingAssets)
				LoadingAssets = UAssetManager::GetStreamableManager().RequestAsyncLoad(Config.AreaMarkerMaterial.ToSoftObjectPath(),
					FStreamableDelegate::CreateWeakLambda(this, [this, Weak = TWeakObjectPtr<AActor>(Target)]
					{
						if (bLoopRequested && Weak.IsValid()) StartLoop(Weak.Get(), FGameplayCueParameters{});
					}));
			return;
		}
		AreaMarker = UGameplayStatics::SpawnDecalAtLocation(this, Config.AreaMarkerMaterial.Get(), Size, MarkerLocation, Rotation);
		return;
	}
	if (LoopEffect || LoopCascadeEffect) return;
	const auto* Feedback = GetFeedback(Target);
	if (!Feedback) return;
	const bool bUseCascade = !Feedback->CascadeEffect.IsNull();
	const FSoftObjectPath EffectPath = bUseCascade ? Feedback->CascadeEffect.ToSoftObjectPath() : Feedback->Effect.ToSoftObjectPath();
	if (EffectPath.IsNull()) return;
	if (!(bUseCascade ? Feedback->CascadeEffect.IsValid() : Feedback->Effect.IsValid()))
	{
		if (LoadingAssets) return;
		// 初次相关时资源可能仍在异步预载；加载完成再恢复持续表现，不靠Tick反复检查。
		LoadingAssets = UAssetManager::GetStreamableManager().RequestAsyncLoad(EffectPath,
			FStreamableDelegate::CreateWeakLambda(this, [this, Weak = TWeakObjectPtr<AActor>(Target), Parameters]
			{
				if (bLoopRequested && Weak.IsValid())
					if (const auto* Loaded = GetFeedback(Weak.Get()); Loaded
						&& (!Loaded->CascadeEffect.IsNull() ? Loaded->CascadeEffect.IsValid() : Loaded->Effect.IsValid()))
						StartLoop(Weak.Get(), Parameters);
			}));
		return;
	}
	FVector Location = Target->GetActorLocation();
	const bool bDefense = GameplayCueTag == CombatTags::Cue_Defense;
	if (bDefense)
	{
		// 护盾素材以脚底为原点，内部已将球心抬高；胶囊半高只在世界坐标中扣除一次。
		const auto* Character = CastChecked<AmultiplayerCombatCharacter>(Target);
		Location -= Character->GetActorUpVector() * Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	}
	if (bUseCascade)
	{
		LoopCascadeEffect = UGameplayStatics::SpawnEmitterAttached(Feedback->CascadeEffect.Get(), Target->GetRootComponent(), NAME_None,
			bDefense ? Location : FVector::ZeroVector, FRotator::ZeroRotator,
			bDefense ? EAttachLocation::KeepWorldPosition : EAttachLocation::SnapToTarget, false);
		return;
	}
	LoopEffect = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, Feedback->Effect.Get(), Location,
		FRotator::ZeroRotator, FVector::OneVector, false, false);
	if (!LoopEffect) return;
	LoopEffect->SetVariableFloat(TEXT("User.Duration"), 120.f);
	// 位置跟随角色，尺寸保留素材的世界大小，避免 Niagara 的绝对位置使附着失效。
	LoopEffect->SetAbsolute(false, false, true);
	LoopEffect->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	LoopEffect->Activate(true);
}

bool AmultiplayerCombatCue::OnActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	if (GetNetMode() == NM_DedicatedServer) return false;
	bLoopRequested = true;
	StartLoop(Target, Parameters);
	if (const auto* Feedback = GetFeedback(Target))
		if (auto* Sound = Feedback->Sound.Get()) UGameplayStatics::PlaySoundAtLocation(this, Sound, Target->GetActorLocation());
	return true;
}
bool AmultiplayerCombatCue::WhileActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	// 晚加入恢复持续表现，但不重播过去的一次性施法音效。
	bLoopRequested = true;
	StartLoop(Target, Parameters);
	return true;
}
bool AmultiplayerCombatCue::OnRemove_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	StopPresentation();
	return true;
}
bool AmultiplayerCombatCue::Recycle()
{
	StopPresentation();
	return Super::Recycle();
}
void AmultiplayerCombatCue::EndPlay(const EEndPlayReason::Type Reason)
{
	StopPresentation();
	Super::EndPlay(Reason);
}
void AmultiplayerCombatCue::StopPresentation()
{
	bLoopRequested = false;
	bExecutionLoadAttempted = false;
	bHasMarkerLocation = false;
	if (LoadingAssets) { LoadingAssets->CancelHandle(); LoadingAssets.Reset(); }
	if (LoopEffect) { LoopEffect->DestroyComponent(); LoopEffect = nullptr; }
	if (LoopCascadeEffect) { LoopCascadeEffect->DestroyComponent(); LoopCascadeEffect = nullptr; }
	if (AreaMarker) { AreaMarker->DestroyComponent(); AreaMarker = nullptr; }
}

void UmultiplayerCombatWidget::NativeConstruct()
{
	Super::NativeConstruct();
	ClearFeedback();
	if (auto* Controller = GetOwningPlayer())
	{
		Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::BindPawn);
		BindPawn(nullptr, Controller->GetPawn());
	}
	else BindPawn(nullptr, nullptr);
}
void UmultiplayerCombatWidget::NativeDestruct()
{
	if (auto* Controller = GetOwningPlayer()) Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::BindPawn);
	BindPawn(nullptr, nullptr);
	Super::NativeDestruct();
}
void UmultiplayerCombatWidget::BindPawn(APawn* PreviousPawn, APawn* NewPawn)
{
	const FGameplayTag CooldownTags[] = { CombatTags::Cooldown_Attack, CombatTags::Cooldown_Defend,
		CombatTags::Cooldown_Heal, CombatTags::Cooldown_Area };
	if (auto* Old = Character.Get())
	{
		Old->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshStats);
		Old->OnCombatStateChanged.RemoveDynamic(this, &ThisClass::RefreshState);
		Old->OnCombatMessage.RemoveDynamic(this, &ThisClass::ShowFeedback);
		Old->OnDestroyed.RemoveDynamic(this, &ThisClass::HandlePawnDestroyed);
	}
	if (auto* ASC = BoundAbilitySystem.Get())
	{
		for (FGameplayTag Tag : CooldownTags)
			ASC->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved).RemoveAll(this);
		ASC->OnActiveGameplayEffectAddedDelegateToSelf.RemoveAll(this);
		ASC->OnAnyGameplayEffectRemovedDelegate().RemoveAll(this);
	}
	BoundAbilitySystem.Reset();
	GetWorld()->GetTimerManager().ClearTimer(RefreshTimer);
	ClearFeedback();
	Character = Cast<AmultiplayerCombatCharacter>(NewPawn);
	if (auto* Current = Character.Get())
	{
		// 先完成订阅再读取快照；换 Pawn、晚加入与预测拒绝都走同一条显示链路。
		Current->OnCombatStatsChanged.AddUniqueDynamic(this, &ThisClass::RefreshStats);
		Current->OnCombatStateChanged.AddUniqueDynamic(this, &ThisClass::RefreshState);
		Current->OnCombatMessage.AddUniqueDynamic(this, &ThisClass::ShowFeedback);
		Current->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandlePawnDestroyed);
		auto* ASC = Current->GetAbilitySystemComponent();
		BoundAbilitySystem = ASC;
		for (FGameplayTag Tag : CooldownTags)
			ASC->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved).AddWeakLambda(this,
				[this](FGameplayTag, int32) { RefreshState(); });
		// 权威效果接替预测效果时标签可能不跨零；效果添加/移除也要重取当前冷却。
		ASC->OnActiveGameplayEffectAddedDelegateToSelf.AddWeakLambda(this,
			[this](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { RefreshState(); });
		ASC->OnAnyGameplayEffectRemovedDelegate().AddWeakLambda(this,
			[this](const FActiveGameplayEffect&) { RefreshState(); });
		RefreshStats(Current->GetHealth(), Current->GetMaxHealth(), Current->GetEnergy(), Current->GetMaxEnergy());
	}
	else RefreshStats(0, 1, 0, 1);
}
void UmultiplayerCombatWidget::HandlePawnDestroyed(AActor* DestroyedActor)
{
	BindPawn(nullptr, nullptr);
}
void UmultiplayerCombatWidget::RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy)
{
	CachedEnergy = Energy;
	if (HealthBar) HealthBar->SetPercent(Health / FMath::Max(1.f, MaxHealth));
	if (EnergyBar) EnergyBar->SetPercent(Energy / FMath::Max(1.f, MaxEnergy));
	if (HealthText) HealthText->SetText(FText::FromString(FString::Printf(TEXT("%.0f / %.0f"), Health, MaxHealth)));
	if (EnergyText) EnergyText->SetText(FText::FromString(FString::Printf(TEXT("%.0f / %.0f"), Energy, MaxEnergy)));
	RefreshState();
}
void UmultiplayerCombatWidget::RefreshState()
{
	const auto* Current = Character.Get();
	bAlive = Current && Current->IsAlive();
	bReady = Current && Current->IsCombatReady();
	bCasting = Current && Current->IsCasting();
	bStunned = Current && Current->IsStunned();
	if (StateText)
	{
		const FText State = FText::FromString(!Current ? TEXT("等待角色") : !bAlive ? TEXT("已死亡")
			: !bReady ? TEXT("正在连接") : bStunned ? TEXT("眩晕中")
			: Current->IsDefending() ? TEXT("防御中") : TEXT(""));
		StateText->SetText(State);
		StateText->SetVisibility(State.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	const ESlateVisibility CastVisibility = bCasting ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (CastingPanel) CastingPanel->SetVisibility(CastVisibility);
	if (CastBar) CastBar->SetVisibility(CastVisibility);
	if (CastText)
	{
		CastText->SetText(bCasting ? FText::FromString(TEXT("范围施法")) : FText::GetEmpty());
		CastText->SetVisibility(CastVisibility);
	}
	RefreshTimers();
}
void UmultiplayerCombatWidget::ShowFeedback(FText Message)
{
	if (FeedbackText) FeedbackText->SetText(Message);
	if (FeedbackPanel) FeedbackPanel->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	GetWorld()->GetTimerManager().ClearTimer(FeedbackTimer);
	if (!Message.IsEmpty()) GetWorld()->GetTimerManager().SetTimer(FeedbackTimer, this, &ThisClass::ClearFeedback, 2.f, false);
}
void UmultiplayerCombatWidget::ClearFeedback()
{
	GetWorld()->GetTimerManager().ClearTimer(FeedbackTimer);
	if (FeedbackText) FeedbackText->SetText(FText::GetEmpty());
	if (FeedbackPanel) FeedbackPanel->SetVisibility(ESlateVisibility::Collapsed);
}
void UmultiplayerCombatWidget::RefreshTimers()
{
	const auto* Current = Character.Get();
	const auto& Config = Current ? Current->GetCombatConfig() : *GetDefault<UmultiplayerCombatConfig>();
	const float Remaining[] = { Current ? Current->GetAttackCooldownRemaining() : 0,
		Current ? Current->GetDefenseCooldownRemaining() : 0, Current ? Current->GetHealCooldownRemaining() : 0,
		Current ? Current->GetAreaCooldownRemaining() : 0 };
	const FmultiplayerCombatSkillRule* Rules[] = { &Config.Attack, &Config.Defend, &Config.Heal, &Config.Area };
	UTextBlock* Labels[] = { AttackCooldownText, DefenseCooldownText, HealCooldownText, AreaCooldownText };
	UProgressBar* Bars[] = { AttackCooldownBar, DefenseCooldownBar, HealCooldownBar, AreaCooldownBar };
	UBorder* Slots[] = { AttackSlot, DefenseSlot, HealSlot, AreaSlot };
	UImage* Icons[] = { AttackIcon, DefenseIcon, HealIcon, AreaIcon };
	bool bHasActiveTimer = bCasting;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Remaining); ++Index)
	{
		const bool bCoolingDown = Remaining[Index] > 0;
		const bool bAvailable = bReady && !bStunned && !bCasting && !bCoolingDown && CachedEnergy >= Rules[Index]->Cost;
		bHasActiveTimer |= bCoolingDown;
		if (Labels[Index]) Labels[Index]->SetText(bCoolingDown
			? FText::FromString(FString::Printf(TEXT("%.1f"), Remaining[Index])) : FText::GetEmpty());
		if (Bars[Index]) Bars[Index]->SetPercent(FMath::Clamp(Remaining[Index] / FMath::Max(.05f, Rules[Index]->Cooldown), 0.f, 1.f));
		if (Slots[Index])
		{
			// 技能槽只展示状态；不禁用整个容器，避免冷却数字一起变灰而难以辨认。
			Slots[Index]->SetBrushColor(FLinearColor::Transparent);
		}
		// 可用性只改变图标亮度，不重画背景，也不影响冷却数字的可读性。
		if (Icons[Index]) Icons[Index]->SetColorAndOpacity(bAvailable ? FLinearColor::White : FLinearColor(.4f, .4f, .4f, 1.f));
	}
	if (CastBar) CastBar->SetPercent(bCasting && Current ? Current->GetCastProgress() : 0);
	// 只有活动冷却或读条需要短时更新数字；空闲界面没有常驻轮询，也不重复查询血量。
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	if (bHasActiveTimer)
	{
		if (!Timers.IsTimerActive(RefreshTimer)) Timers.SetTimer(RefreshTimer, this, &ThisClass::RefreshTimers, .1f, true);
	}
	else Timers.ClearTimer(RefreshTimer);
}

void UmultiplayerEnemyHealthWidget::BindCharacter(AmultiplayerCombatCharacter* NewCharacter)
{
	if (auto* Previous = BoundCharacter.Get()) Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshHealth);
	BoundCharacter.Reset();
	ObservedCharacter = NewCharacter;
	if (Health)
	{
		// 外层只转交观察对象，实际生命事件由内层条形控件订阅一次。
		Health->BindCharacter(NewCharacter);
		SetVisibility(NewCharacter ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		return;
	}
	if (NewCharacter)
	{
		BoundCharacter = NewCharacter;
		NewCharacter->OnCombatStatsChanged.AddUniqueDynamic(this, &ThisClass::RefreshHealth);
		RefreshHealth(NewCharacter->GetHealth(), NewCharacter->GetMaxHealth(), 0, 0);
	}
	else SetVisibility(ESlateVisibility::Collapsed);
}

void UmultiplayerEnemyHealthWidget::NativeConstruct()
{
	Super::NativeConstruct();
	BindCharacter(ObservedCharacter.Get());
}

void UmultiplayerEnemyHealthWidget::NativeDestruct()
{
	if (auto* Previous = BoundCharacter.Get()) Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshHealth);
	BoundCharacter.Reset();
	if (Health) Health->BindCharacter(nullptr);
	// 保留弱观察对象，Slate 重建后重新订阅；敌人 EndPlay 会明确 BindCharacter(nullptr)。
	Super::NativeDestruct();
}

void UmultiplayerEnemyHealthWidget::RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy)
{
	const float Percent = FMath::Clamp(CurrentHealth / FMath::Max(1.f, MaxHealth), 0.f, 1.f);
	if (HealthBar) HealthBar->SetPercent(Percent);
	// 保留源布局的底层条形样式，不运行源蓝图的平滑计时器或逐帧查询。
	if (LerpAttributeBar) LerpAttributeBar->SetPercent(Percent);
	if (HealthText) HealthText->SetText(FText::FromString(FString::Printf(TEXT("%.0f / %.0f"), CurrentHealth, MaxHealth)));
	SetVisibility(CurrentHealth > 0 && ObservedCharacter.IsValid() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void AmultiplayerCombatHUD::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwningPlayerController() && GetOwningPlayerController()->IsLocalController() && CombatWidgetClass)
	{
		CombatWidget = CreateWidget<UUserWidget>(GetOwningPlayerController(), CombatWidgetClass);
		if (CombatWidget) CombatWidget->AddToViewport();
	}
}
void AmultiplayerCombatHUD::EndPlay(EEndPlayReason::Type Reason)
{
	if (CombatWidget) { CombatWidget->RemoveFromParent(); CombatWidget = nullptr; }
	Super::EndPlay(Reason);
}

// 在动画更新开始时采样角色速度，动画图直接读取结果，不反向修改角色移动。
void UmultiplayerCombatAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	const APawn* Pawn = TryGetPawnOwner();
	const FVector Velocity = Pawn ? Pawn->GetVelocity() : FVector::ZeroVector;
	CombatMoveSpeed = Velocity.Size2D();
	const FVector LocalVelocity = Pawn ? Pawn->GetActorRotation().UnrotateVector(Velocity) : FVector::ZeroVector;
	CombatMoveDirection = CombatMoveSpeed > 1.f
		? FMath::RadiansToDegrees(FMath::Atan2(LocalVelocity.Y, LocalVelocity.X)) : 0.f;
	// AI 路径跟随和远端角色可能只有速度、没有输入加速度；腿部状态不能依赖本地输入是否存在。
	CombatShouldMove = CombatMoveSpeed > 3.f;
	const ACharacter* Character = Cast<ACharacter>(Pawn);
	CombatIsFalling = Character && Character->GetCharacterMovement()->IsFalling();
}
