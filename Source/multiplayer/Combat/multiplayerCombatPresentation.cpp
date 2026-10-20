// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatPresentation.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatTargeting.h"
#include "Core/multiplayerCoopGameState.h"
#include "Combat/multiplayerCombatTags.h"
#include "Blueprint/WidgetTree.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/DecalComponent.h"
#include "Components/Image.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/SizeBox.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

namespace
{
	constexpr float CombatTransientEffectLifetime = 2.f;

	// 只为联机验收区分“Cue未抵达”与“抵达后资源/粒子异常”，不改变正式传输或结算。
	void TraceCombatHealCue(const TCHAR* Stage, AActor* Target, const FGameplayTag& Tag,
		const FmultiplayerCombatFeedback* Feedback, const FVector& Location, const USceneComponent* Component = nullptr)
	{
#if !UE_BUILD_SHIPPING
		static const bool bEnabled = []
		{
			FString Mode;
			FParse::Value(FCommandLine::Get(), TEXT("CoopNetTest="), Mode);
			return Mode == TEXT("Combat") || Mode == TEXT("CombatLifecycle");
		}();
		if (!bEnabled || Tag != CombatTags::Cue_Heal) return;
		const UWorld* World = Target ? Target->GetWorld() : nullptr;
		const APawn* Pawn = Cast<APawn>(Target);
		UE_LOG(LogTemp, Display,
			TEXT("COOP_CUE_DIAG cue=Heal stage=%s target=%s role=%d netmode=%d local=%d world=%.3f real=%.3f frame=%llu cascade=%s cascadeLoaded=%d niagara=%s niagaraLoaded=%d location=%s component=%s active=%d registered=%d"),
			Stage, *GetNameSafe(Target), Target ? int32(Target->GetLocalRole()) : -1, World ? int32(World->GetNetMode()) : -1,
			Pawn && Pawn->IsLocallyControlled(), World ? World->GetTimeSeconds() : -1.f, World ? World->GetRealTimeSeconds() : -1.f,
			static_cast<unsigned long long>(GFrameCounter), Feedback ? *Feedback->CascadeEffect.ToSoftObjectPath().ToString() : TEXT("None"),
			Feedback && Feedback->CascadeEffect.IsValid(), Feedback ? *Feedback->Effect.ToSoftObjectPath().ToString() : TEXT("None"),
			Feedback && Feedback->Effect.IsValid(), *Location.ToString(), *GetNameSafe(Component),
			Component && Component->IsActive(), Component && Component->IsRegistered());
#endif
	}

	bool UsesEnemyMelee(const AmultiplayerCombatCharacter* Character)
	{
		return Character && Character->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& Character->GetAttackAbilityClass()
			&& Character->GetAttackAbilityClass()->IsChildOf(UmultiplayerEnemyMeleeAbility::StaticClass());
	}

	// 共享只读名称和冷却查询，HUD 与成长菜单不另存一套技能状态。
	const TCHAR* CombatSkillName(EMultiplayerCombatSkill Skill)
	{
		const TCHAR* Names[] = {TEXT("攻击"), TEXT("防御"), TEXT("治疗"), TEXT("范围"), TEXT("冲刺")};
		return int32(Skill) < UE_ARRAY_COUNT(Names) ? Names[int32(Skill)] : TEXT("未知");
	}
	const TCHAR* CombatSlotKey(int32 Slot)
	{
		const TCHAR* Keys[] = {TEXT("LMB"), TEXT("Q"), TEXT("E"), TEXT("R"), TEXT("Shift")};
		return Slot >= 0 && Slot < UE_ARRAY_COUNT(Keys) ? Keys[Slot] : TEXT("--");
	}
	float CombatSkillCooldown(const AmultiplayerCombatCharacter* Character, EMultiplayerCombatSkill Skill)
	{
		if (!Character) return 0.f;
		switch (Skill)
		{
		case EMultiplayerCombatSkill::Attack: return Character->GetAttackCooldownRemaining();
		case EMultiplayerCombatSkill::Defend: return Character->GetDefenseCooldownRemaining();
		case EMultiplayerCombatSkill::Heal: return Character->GetHealCooldownRemaining();
		case EMultiplayerCombatSkill::Area: return Character->GetAreaCooldownRemaining();
		case EMultiplayerCombatSkill::Dash: return Character->GetDashCooldownRemaining();
		default: return 0.f;
		}
	}
}

AmultiplayerCombatCue::AmultiplayerCombatCue()
{
	bAutoDestroyOnRemove = true;
	bAutoAttachToOwner = false;
	PrimaryActorTick.bCanEverTick = false;
}

const FmultiplayerCombatFeedback* AmultiplayerCombatCue::GetFeedback(AActor* Target, const FGameplayCueParameters* Parameters) const
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(Target);
	if (!Character) return nullptr;
	// 治疗发生在受治疗者身上，但表现定义属于施法者；队友不必保存同一份技能配置。
	if (GameplayCueTag == CombatTags::Cue_Heal && Parameters)
		if (const auto* Caster = Cast<AmultiplayerCombatCharacter>(Parameters->GetInstigator()))
			return &Caster->GetCombatConfig().HealFeedback;
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

// 补载只恢复本次 Cue 的音画；沿用已选表现快照，不能重新治疗、扣费或重复发成功提示。
bool AmultiplayerCombatCue::OnExecute_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	TraceCombatHealCue(bExecutionLoadAttempted ? TEXT("received-after-load") : TEXT("received"), Target, GameplayCueTag,
		bHasExecutionFeedback ? &ExecutionFeedback : GetFeedback(Target, &Parameters), Parameters.Location);
	if (GetNetMode() == NM_DedicatedServer) return false;
	if (!bHasExecutionFeedback)
	{
		const auto* SelectedFeedback = GetFeedback(Target, &Parameters);
		if (!SelectedFeedback)
		{
			TraceCombatHealCue(TEXT("assetmissing"), Target, GameplayCueTag, nullptr, Parameters.Location);
			return false;
		}
		ExecutionFeedback = *SelectedFeedback;
		bHasExecutionFeedback = true;
	}
	const auto* Feedback = &ExecutionFeedback;
	if (GameplayCueTag == CombatTags::Cue_Heal && !bExecutionHealNotified)
	{
		bExecutionHealNotified = true;
		// 只提示本次确实成功的队友治疗，不给施法者再播放一份假回血粒子。
		if (auto* Caster = Cast<AmultiplayerCombatCharacter>(Parameters.GetInstigator());
			Caster && Caster != Target && Caster->IsLocallyControlled())
			Caster->OnCombatMessage.Broadcast(FText::FromString(TEXT("治疗成功：已为标记队友恢复生命")));
	}
	if (GameplayCueTag == CombatTags::Cue_Area && !bExecutionRangeShown)
	{
		bExecutionRangeShown = true;
		if (auto* Character = Cast<AmultiplayerCombatCharacter>(Target); Character && Character->IsLocallyControlled())
			if (auto* Controller = Cast<APlayerController>(Character->GetController()))
				if (auto* HUD = Cast<AmultiplayerCombatHUD>(Controller->GetHUD()))
					HUD->ShowResolvedArea(Parameters.Location, Parameters.RawMagnitude);
	}
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
		TraceCombatHealCue(TEXT("loading"), Target, GameplayCueTag, Feedback, Parameters.Location);
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
	FRotator Rotation = FRotator::ZeroRotator;
	if (GameplayCueTag == CombatTags::Cue_Attack)
	{
		const bool bHasLaunchSnapshot = !FVector(Parameters.Normal).IsNearlyZero();
		Location = bHasLaunchSnapshot ? FVector(Parameters.Location) : Character->GetCombatMuzzleLocation();
		Rotation = bHasLaunchSnapshot ? FVector(Parameters.Normal).Rotation() : Character->GetActorRotation();
	}
	else if (GameplayCueTag == CombatTags::Cue_Heal)
		Location.Z -= Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (bUseCascade ? !Feedback->CascadeEffect.IsValid() : !Feedback->Effect.IsValid())
		TraceCombatHealCue(TEXT("assetmissing"), Target, GameplayCueTag, Feedback, Location);
	// 投射物尾迹由弹体持有；这里的 AttackFeedback 只能配置一次性起手效果。
	if (bUseCascade)
	{
		if (auto* System = Feedback->CascadeEffect.Get())
			if (auto* Component = UGameplayStatics::SpawnEmitterAtLocation(this, System, Location, Rotation))
			{
				TraceCombatHealCue(TEXT("spawned-Cascade"), Target, GameplayCueTag, Feedback, Location, Component);
				FTimerHandle Cleanup;
				GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
					[Component] { Component->DestroyComponent(); }), 5.f, false);
			}
			else TraceCombatHealCue(TEXT("spawnfailed-Cascade"), Target, GameplayCueTag, Feedback, Location);
	}
	else if (auto* System = Feedback->Effect.Get())
	{
		auto* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, Rotation,
			FVector::OneVector, true, false);
		if (Component)
		{
			Component->SetVariableFloat(TEXT("User.Duration"), bGround ? .6f : GameplayCueTag == CombatTags::Cue_Heal ? .8f : .3f);
			if (bGround) Component->SetVariableFloat(TEXT("User.Scale_All"),
				Parameters.RawMagnitude / Character->GetCombatConfig().AreaEffectReferenceRadius);
			if (GameplayCueTag == CombatTags::Cue_Heal)
			{
				// 护体治疗跟随受治疗者，脚底偏移只计算一次；角色缩放不放大粒子。
				Component->SetAbsolute(false, false, true);
				Component->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
			}
			Component->Activate(true);
			TraceCombatHealCue(TEXT("spawned-Niagara"), Target, GameplayCueTag, Feedback, Location, Component);
			// 素材后续即使被误设为循环，瞬时命中特效也不能无限遗留在场景中。
			FTimerHandle Cleanup;
			GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
				[Component] { Component->DestroyComponent(); }), CombatTransientEffectLifetime, false);
		}
		else TraceCombatHealCue(TEXT("spawnfailed-Niagara"), Target, GameplayCueTag, Feedback, Location);
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
		// 小兵借用施法标签暂停追击，但它的近战扇区不是玩家 R 的地面落点圈。
		if (UsesEnemyMelee(Character)) return;
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
// 移除、回收和 EndPlay 共用清理入口；撤销补载并释放持续组件，避免旧 Cue 在异步完成后复活。
void AmultiplayerCombatCue::StopPresentation()
{
	bLoopRequested = false;
	bExecutionLoadAttempted = false;
	bHasExecutionFeedback = false;
	ExecutionFeedback = {};
	bExecutionRangeShown = false;
	bExecutionHealNotified = false;
	bHasMarkerLocation = false;
	if (LoadingAssets) { LoadingAssets->CancelHandle(); LoadingAssets.Reset(); }
	if (LoopEffect) { LoopEffect->DestroyComponent(); LoopEffect = nullptr; }
	if (LoopCascadeEffect) { LoopCascadeEffect->DestroyComponent(); LoopCascadeEffect = nullptr; }
	if (AreaMarker) { AreaMarker->DestroyComponent(); AreaMarker = nullptr; }
}

void UmultiplayerCombatWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (SkillBrushes.IsEmpty())
	{
		UImage* Icons[] = {AttackIcon, DefenseIcon, HealIcon, AreaIcon, DashIcon};
		SkillBrushes.SetNum(5);
		for (int32 Index = 0; Index < 5; ++Index)
		{
			if (Icons[Index]) SkillBrushes[Index] = Icons[Index]->GetBrush();
			else SkillBrushes[Index].DrawAs = ESlateBrushDrawType::NoDrawType;
		}
	}
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
		CombatTags::Cooldown_Heal, CombatTags::Cooldown_Area, CombatTags::Cooldown_Dash };
	if (auto* Previous = BoundProgression.Get()) Previous->OnProgressionChanged.RemoveDynamic(this, &ThisClass::RefreshState);
	BoundProgression.Reset();
	if (auto* PreviousState = BoundGameState.Get())
	{
		PreviousState->OnGameLost.RemoveDynamic(this, &ThisClass::RefreshState);
		PreviousState->OnGameWon.RemoveDynamic(this, &ThisClass::RefreshState);
		PreviousState->OnEncounterChanged.RemoveDynamic(this, &ThisClass::RefreshState);
		PreviousState->OnObjectiveProgressChanged.RemoveDynamic(this, &ThisClass::HandleObjectiveChanged);
	}
	BoundGameState.Reset();
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
		BoundProgression = Current->GetProgressionComponent();
		BoundProgression->OnProgressionChanged.AddUniqueDynamic(this, &ThisClass::RefreshState);
		// 失败是整局状态，不从本 Pawn 是否死亡推导；先订阅，再由下方补读现有快照。
		BoundGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (auto* State = BoundGameState.Get())
		{
			State->OnGameLost.AddUniqueDynamic(this, &ThisClass::RefreshState);
			State->OnGameWon.AddUniqueDynamic(this, &ThisClass::RefreshState);
			State->OnEncounterChanged.AddUniqueDynamic(this, &ThisClass::RefreshState);
			State->OnObjectiveProgressChanged.AddUniqueDynamic(this, &ThisClass::HandleObjectiveChanged);
		}
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
void UmultiplayerCombatWidget::HandleObjectiveChanged(int32 Activated, int32 Required) { RefreshState(); }
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
	const bool bRescuing = Current && Current->GetCoopComponent()->IsRescuing();
	bCasting = Current && (Current->IsCasting() || bRescuing);
	bStunned = Current && Current->IsStunned();
	const ESlateVisibility CastVisibility = bCasting ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (CastingPanel) CastingPanel->SetVisibility(CastVisibility);
	if (CastBar) CastBar->SetVisibility(CastVisibility);
	if (CastText)
	{
		CastText->SetText(bCasting ? FText::FromString(bRescuing ? TEXT("救援引导") : TEXT("范围施法")) : FText::GetEmpty());
		CastText->SetVisibility(CastVisibility);
	}
	RefreshTimers();
}

// 只在事件或活动倒计时期间刷新；默认未成长的闲时 HUD 仍保留原来的空状态区。
void UmultiplayerCombatWidget::RefreshStatusText()
{
	const auto* Current = Character.Get();
	const auto* Coop = Current ? Current->GetCoopComponent() : nullptr;
	const auto* SharedState = BoundGameState.Get();
	if (StateText)
	{
		// 沿用原 HUD 的状态栏；多个效果同时显示，不用优先级覆盖把另一种限制藏起来。
		TArray<FString> Status;
		if (SharedState && SharedState->HasGameLost()) Status.Add(TEXT("本局失败 · Enter 重开"));
		else if (SharedState && SharedState->HasGameWon()) Status.Add(TEXT("撤离成功 · Enter 重开"));
		else if (!Current) Status.Add(TEXT("等待角色"));
		else if (!bAlive) Status.Add(TEXT("已死亡 · Enter 重开"));
		else if (Coop && Coop->IsDowned()) Status.Add(FString::Printf(TEXT("倒地 · 等待救援 %.1fs"), Coop->GetDownedRemaining()));
		else if (!bReady) Status.Add(TEXT("正在连接"));
		else
		{
			if (const auto* State = BoundGameState.Get(); State && State->GetEncounterState().TotalWaves > 0)
			{
				const auto Round = State->GetEncounterState();
				const auto& Objective = State->GetObjectiveState();
				const FString WaveLabel = Round.Wave == 0 ? TEXT("等待队友加入")
					: FString::Printf(TEXT("波次 %d/%d · 敌人 %d"), Round.Wave, Round.TotalWaves, Round.LivingEnemies);
				Status.Add(FString::Printf(TEXT("%s · 钥匙 %d/%d\n集齐钥匙 → 双人踩板 → 一起撤离\nShift 冲刺 · F 救援"),
					*WaveLabel,Objective.ActivatedKeys,Objective.RequiredKeys));
			}
			if (Current->GetShield() > 0) Status.Add(FString::Printf(TEXT("护盾 %.0f"), Current->GetShield()));
			if (bStunned) Status.Add(TEXT("眩晕中"));
			if (Current->IsRooted()) Status.Add(TEXT("禁锢中"));
			if (Current->IsBurning()) Status.Add(TEXT("燃烧中"));
			if (Current->IsDefending()) Status.Add(TEXT("防御中"));
			if (Coop && Coop->IsRescuing()) Status.Add(FString::Printf(TEXT("救援中 %.0f%%"), Coop->GetRescueProgress() * 100.f));
			if (Current->GetDashCooldownRemaining() > 0.f) Status.Add(FString::Printf(TEXT("冲刺 %.1fs"), Current->GetDashCooldownRemaining()));
			if (const auto* Progression = BoundProgression.Get(); Progression &&
				(Progression->GetState().Level > 1 || Progression->GetState().SkillPoints != 2))
				Status.Add(FString::Printf(TEXT("Lv.%d · 点数%d · B成长"), Progression->GetState().Level, Progression->GetState().SkillPoints));
		}
		const FText State = FText::FromString(FString::Join(Status, TEXT(" · ")));
		StateText->SetText(State);
		StateText->SetVisibility(State.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (ProgressionText)
	{
		const auto* Progression = BoundProgression.Get();
		ProgressionText->SetText(Progression ? FText::FromString(FString::Printf(TEXT("Lv.%d  XP %d/%d  技能点 %d  [B 成长]"),
			Progression->GetState().Level, Progression->GetState().Experience, Progression->GetState().Level * 100,
			Progression->GetState().SkillPoints)) : FText::GetEmpty());
	}
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
	UTextBlock* Labels[] = { AttackCooldownText, DefenseCooldownText, HealCooldownText, AreaCooldownText, DashCooldownText };
	UProgressBar* Bars[] = { AttackCooldownBar, DefenseCooldownBar, HealCooldownBar, AreaCooldownBar, DashCooldownBar };
	UBorder* Slots[] = { AttackSlot, DefenseSlot, HealSlot, AreaSlot, DashSlot };
	UImage* Icons[] = { AttackIcon, DefenseIcon, HealIcon, AreaIcon, DashIcon };
	const auto* Coop = Current ? Current->GetCoopComponent() : nullptr;
	const auto* Progression = BoundProgression.Get();
	bool bHasActiveTimer = bCasting || (Coop && (Coop->IsDowned() || Coop->IsRescuing()));
	RefreshStatusText();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const auto Skill = Progression ? Progression->GetSlotSkill(Index) : EMultiplayerCombatSkill(Index);
		const float Remaining = CombatSkillCooldown(Current, Skill);
		const auto Rule = Current ? Current->GetSkillRule(Skill) : Config.GetSkill(Skill);
		const bool bCoolingDown = Remaining > 0;
		// 图标仅概括资源与自身状态，不承诺本次目标合法；目标资格由瞄准预览和确认时实时查询。
		const bool bAvailable = bReady && !bStunned && !bCasting && !bCoolingDown
			&& CachedEnergy >= Rule.Cost && (!Progression || Progression->GetSkillLevel(Skill) > 0);
		bHasActiveTimer |= bCoolingDown;
		if (Labels[Index]) Labels[Index]->SetText(bCoolingDown
			? FText::FromString(FString::Printf(TEXT("%.1f"), Remaining)) : FText::GetEmpty());
		if (Bars[Index]) Bars[Index]->SetPercent(FMath::Clamp(Remaining / FMath::Max(.05f, Rule.Cooldown), 0.f, 1.f));
		if (auto* Name = Cast<UTextBlock>(GetWidgetFromName(FName(*FString::Printf(TEXT("SkillLabel%d"), Index)))))
			Name->SetText(FText::FromString(CombatSkillName(Skill)));
		if (Slots[Index])
		{
			// 技能槽只展示状态；不禁用整个容器，避免冷却数字一起变灰而难以辨认。
			Slots[Index]->SetBrushColor(FLinearColor::Transparent);
		}
		// 可用性只改变图标亮度，不重画背景，也不影响冷却数字的可读性。
		if (Icons[Index])
		{
			if (SkillBrushes.IsValidIndex(int32(Skill))) Icons[Index]->SetBrush(SkillBrushes[int32(Skill)]);
			Icons[Index]->SetColorAndOpacity(bAvailable ? FLinearColor::White : FLinearColor(.4f, .4f, .4f, 1.f));
		}
	}
	if (CastBar) CastBar->SetPercent(bCasting && Current
		? (Coop && Coop->IsRescuing() ? Coop->GetRescueProgress() : Current->GetCastProgress()) : 0);
	// 只有活动冷却或读条需要短时更新数字；空闲界面没有常驻轮询，也不重复查询血量。
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	if (bHasActiveTimer)
	{
		if (!Timers.IsTimerActive(RefreshTimer)) Timers.SetTimer(RefreshTimer, this, &ThisClass::RefreshTimers, .1f, true);
	}
	else Timers.ClearTimer(RefreshTimer);
}

// 原生成长面板在 Slate 构建前生成树，不要求迁移或替换原来的 HUD 蓝图。
TSharedRef<SWidget> UmultiplayerCombatProgressionWidget::RebuildWidget()
{
	if (!WidgetTree || !WidgetTree->RootWidget) BuildLayout();
	return Super::RebuildWidget();
}

// 文字大小和控件名称固定，便于真实按钮测试；只影响附加面板自身。
UTextBlock* UmultiplayerCombatProgressionWidget::AddLabel(UVerticalBox* Parent, FName Name, int32 Size)
{
	auto* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
	FSlateFontInfo Font = Label->GetFont(); Font.Size = Size; Label->SetFont(Font);
	Label->SetColorAndOpacity(FSlateColor(FLinearColor(.92f, .94f, .98f)));
	Label->SetAutoWrapText(true); Parent->AddChildToVerticalBox(Label)->SetPadding(FMargin(0, 3)); return Label;
}

// 按钮只绑定窄业务入口，不把服务器快照直接写回客户端组件。
UButton* UmultiplayerCombatProgressionWidget::AddButton(UHorizontalBox* Parent, FName Name, const TCHAR* Label)
{
	auto* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
	auto* Text = WidgetTree->ConstructWidget<UTextBlock>(); Text->SetText(FText::FromString(Label));
	FSlateFontInfo Font = Text->GetFont(); Font.Size = 15; Text->SetFont(Font);
	Button->SetContent(Text); Button->SetBackgroundColor(FLinearColor(.16f, .24f, .34f));
	auto* ButtonSlot = Parent->AddChildToHorizontalBox(Button); ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); ButtonSlot->SetPadding(FMargin(2, 4));
	return Button;
}

// 一个小面板覆盖在游戏上，保留原界面所有组件与位置。
void UmultiplayerCombatProgressionWidget::BuildLayout()
{
	if (!WidgetTree) WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	SetIsFocusable(true);
	auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(); WidgetTree->RootWidget = Canvas;
	auto* Panel = WidgetTree->ConstructWidget<UBorder>(); Panel->SetPadding(FMargin(18));
	Panel->SetBrushColor(FLinearColor(.025f, .035f, .055f, .96f));
	auto* Position = Canvas->AddChildToCanvas(Panel); Position->SetAnchors(FAnchors(.5f, .5f));
	Position->SetAlignment(FVector2D(.5f, .5f)); Position->SetAutoSize(true);
	auto* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(620); Panel->SetContent(Size);
	auto* Lines = WidgetTree->ConstructWidget<UVerticalBox>(); Size->SetContent(Lines);
	AddLabel(Lines, TEXT("ProgressionTitle"), 22)->SetText(FText::FromString(TEXT("成长与技能  [B / Esc 关闭]")));
	SummaryText = AddLabel(Lines, TEXT("ProgressionSummary"), 18);
	SkillsText = AddLabel(Lines, TEXT("ProgressionSkills"));
	SelectedSkillText = AddLabel(Lines, TEXT("ProgressionSelectedSkill"));
	auto* SkillRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(SkillRow);
	AddButton(SkillRow, TEXT("PreviousSkillButton"), TEXT("上个技能"))->OnClicked.AddDynamic(this, &ThisClass::PreviousSkill);
	AddButton(SkillRow, TEXT("NextSkillButton"), TEXT("下个技能"))->OnClicked.AddDynamic(this, &ThisClass::NextSkill);
	LearnSkillButton = AddButton(SkillRow, TEXT("LearnSkillButton"), TEXT("学习 / 升级 (-1点)")); LearnSkillButton->OnClicked.AddDynamic(this, &ThisClass::LearnSkill);
	SlotText = AddLabel(Lines, TEXT("ProgressionSelectedSlot"));
	auto* SlotRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(SlotRow);
	AddButton(SlotRow, TEXT("PreviousSlotButton"), TEXT("上个槽位"))->OnClicked.AddDynamic(this, &ThisClass::PreviousSlot);
	AddButton(SlotRow, TEXT("NextSlotButton"), TEXT("下个槽位"))->OnClicked.AddDynamic(this, &ThisClass::NextSlot);
	EquipSkillButton = AddButton(SlotRow, TEXT("EquipSkillButton"), TEXT("装配所选技能")); EquipSkillButton->OnClicked.AddDynamic(this, &ThisClass::EquipSkill);
	PassivesText = AddLabel(Lines, TEXT("ProgressionPassives")); SelectedPassiveText = AddLabel(Lines, TEXT("ProgressionSelectedPassive"));
	auto* PassiveRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(PassiveRow);
	AddButton(PassiveRow, TEXT("PreviousPassiveButton"), TEXT("上个被动"))->OnClicked.AddDynamic(this, &ThisClass::PreviousPassive);
	AddButton(PassiveRow, TEXT("NextPassiveButton"), TEXT("下个被动"))->OnClicked.AddDynamic(this, &ThisClass::NextPassive);
	LearnPassiveButton = AddButton(PassiveRow, TEXT("LearnPassiveButton"), TEXT("学习被动 (-1点)")); LearnPassiveButton->OnClicked.AddDynamic(this, &ThisClass::LearnPassive);
	auto* EquipRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(EquipRow);
	EquipPassiveButton = AddButton(EquipRow, TEXT("EquipPassiveButton"), TEXT("装配被动")); EquipPassiveButton->OnClicked.AddDynamic(this, &ThisClass::EquipPassive);
	UnequipPassiveButton = AddButton(EquipRow, TEXT("UnequipPassiveButton"), TEXT("卸下被动")); UnequipPassiveButton->OnClicked.AddDynamic(this, &ThisClass::UnequipPassive);
	auto* FileRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Lines->AddChildToVerticalBox(FileRow);
	SaveButton = AddButton(FileRow, TEXT("SaveProgressButton"), TEXT("保存成长 [F5]")); SaveButton->OnClicked.AddDynamic(this, &ThisClass::SaveProgress);
	LoadButton = AddButton(FileRow, TEXT("LoadProgressButton"), TEXT("读取成长 [F9]")); LoadButton->OnClicked.AddDynamic(this, &ThisClass::LoadProgress);
	AddButton(FileRow, TEXT("CloseProgressionButton"), TEXT("关闭"))->OnClicked.AddDynamic(this, &ThisClass::CloseMenu);
	MenuMessage = AddLabel(Lines, TEXT("ProgressionMessage"), 14);
	MenuMessage->SetText(FText::FromString(TEXT("学习每级花1点，技能等级不能超过角色等级。装配已有技能会交换槽位；施法期间不能换装。")));
}

// 打开先监听角色替换，再绑定当前角色并补读快照；真实视口中接管焦点和鼠标。
void UmultiplayerCombatProgressionWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (auto* Controller = GetOwningPlayer())
	{
		Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::BindPawn);
		BindPawn(nullptr, Controller->GetPawn());
		if (GetWorld() && GetWorld()->GetGameViewport())
		{
			FInputModeGameAndUI Mode; Mode.SetWidgetToFocus(TakeWidget()); Mode.SetHideCursorDuringCapture(false);
			Controller->SetInputMode(Mode); Controller->bShowMouseCursor = true; bOwnsInputMode = true; SetKeyboardFocus();
		}
	}
	else BindPawn(nullptr, nullptr);
}

// 关闭先解绑，之后恢复游戏输入；没有轮询计时器需要继续留在后台。
void UmultiplayerCombatProgressionWidget::NativeDestruct()
{
	if (auto* Controller = GetOwningPlayer()) Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::BindPawn);
	BindPawn(nullptr, nullptr);
	if (bOwnsInputMode)
	{
		if (auto* Controller = GetOwningPlayer()) { Controller->SetInputMode(FInputModeGameOnly()); Controller->bShowMouseCursor = false; }
		bOwnsInputMode = false;
	}
	Super::NativeDestruct();
}

// 在子按钮处理之前消费关闭键，避免同一次 B 既关闭菜单又传给角色再次打开。
FReply UmultiplayerCombatProgressionWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	if (Event.GetKey() == EKeys::B || Event.GetKey() == EKeys::Escape) { CloseMenu(); return FReply::Handled(); }
	return Super::NativeOnPreviewKeyDown(Geometry, Event);
}

// 移除附加层会触发正常 UMG 清理，不修改主 HUD 的可见性。
void UmultiplayerCombatProgressionWidget::CloseMenu() { RemoveFromParent(); }

// 角色、成长和 GAS 活动事件都先解绑旧对象，避免旧 Pawn 的通知覆盖新角色菜单。
void UmultiplayerCombatProgressionWidget::BindPawn(APawn* PreviousPawn, APawn* NewPawn)
{
	if (auto* Previous = Character.Get())
	{
		Previous->OnCombatStateChanged.RemoveDynamic(this, &ThisClass::RefreshMenu);
		Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::HandleStats);
		Previous->OnCombatMessage.RemoveDynamic(this, &ThisClass::ShowMessage);
		Previous->OnDestroyed.RemoveDynamic(this, &ThisClass::HandlePawnDestroyed);
	}
	if (auto* Progression = BoundProgression.Get()) Progression->OnProgressionChanged.RemoveDynamic(this, &ThisClass::RefreshMenu);
	if (auto* ASC = BoundAbilitySystem.Get()) { ASC->AbilityActivatedCallbacks.RemoveAll(this); ASC->AbilityEndedCallbacks.RemoveAll(this); }
	BoundProgression.Reset(); BoundAbilitySystem.Reset(); Character = Cast<AmultiplayerCombatCharacter>(NewPawn);
	if (auto* Current = Character.Get())
	{
		Current->OnCombatStateChanged.AddUniqueDynamic(this, &ThisClass::RefreshMenu);
		Current->OnCombatStatsChanged.AddUniqueDynamic(this, &ThisClass::HandleStats);
		Current->OnCombatMessage.AddUniqueDynamic(this, &ThisClass::ShowMessage);
		Current->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandlePawnDestroyed);
		BoundProgression = Current->GetProgressionComponent(); BoundProgression->OnProgressionChanged.AddUniqueDynamic(this, &ThisClass::RefreshMenu);
		BoundAbilitySystem = Current->GetAbilitySystemComponent();
		BoundAbilitySystem->AbilityActivatedCallbacks.AddWeakLambda(this, [this](UGameplayAbility*) { RefreshMenu(); });
		BoundAbilitySystem->AbilityEndedCallbacks.AddWeakLambda(this, [this](UGameplayAbility*) { RefreshMenu(); });
	}
	RefreshMenu();
}

// Pawn 销毁后留空面板等待新 Pawn，不保留旧对象引用或轮询等待器。
void UmultiplayerCombatProgressionWidget::HandlePawnDestroyed(AActor* Actor) { BindPawn(nullptr, nullptr); }
// 属性通知仅用于更新可操作资格，真正数据仍读取成长快照。
void UmultiplayerCombatProgressionWidget::HandleStats(float Health, float MaxHealth, float Energy, float MaxEnergy) { RefreshMenu(); }
// 存取结果由服务器消息进入菜单，客户端不假定异步请求已经成功。
void UmultiplayerCombatProgressionWidget::ShowMessage(FText Message) { if (MenuMessage) MenuMessage->SetText(Message); }

// 只有事件或选择改变才刷新，等级、装配和按钮资格使用同一个已提交快照。
void UmultiplayerCombatProgressionWidget::RefreshMenu()
{
	if (!SummaryText) return;
	const auto* Current = Character.Get(); const auto* Progression = BoundProgression.Get();
	const FmultiplayerCombatProgressionState State = Progression ? Progression->CaptureState() : FmultiplayerCombatProgressionState();
	const bool bHasProfile = Progression && State.ProfileId.IsValid();
	SummaryText->SetText(bHasProfile ? FText::FromString(FString::Printf(TEXT("Lv.%d    XP %d / %d    技能点 %d"), State.Level, State.Experience, State.Level * 100, State.SkillPoints))
		: FText::FromString(TEXT("等待角色 / 成长数据同步")));
	TArray<FString> SkillLines;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const auto Skill = EMultiplayerCombatSkill(Index); const int32 Level = Progression ? Progression->GetSkillLevel(Skill) : 0;
		const int32 SkillSlotIndex = State.Slots.IndexOfByKey(Skill);
		SkillLines.Add(FString::Printf(TEXT("%s  %s  ·  槽%d [%s]"), CombatSkillName(Skill),
			*(Level > 0 ? FString::Printf(TEXT("Lv.%d"), Level) : FString(TEXT("未学习"))), SkillSlotIndex + 1, CombatSlotKey(SkillSlotIndex)));
	}
	SkillsText->SetText(FText::FromString(FString::Join(SkillLines, TEXT("\n"))));
	const int32 SkillLevel = Progression ? Progression->GetSkillLevel(EMultiplayerCombatSkill(SelectedSkill)) : 0;
	const FString SkillLevelLabel = SkillLevel > 0 ? FString::Printf(TEXT("Lv.%d"), SkillLevel) : TEXT("未学习（可花1点解锁）");
	SelectedSkillText->SetText(FText::FromString(FString::Printf(TEXT("当前技能：%s  %s"), CombatSkillName(EMultiplayerCombatSkill(SelectedSkill)),
		*SkillLevelLabel)));
	SlotText->SetText(FText::FromString(FString::Printf(TEXT("目标槽%d [%s]：%s"), SelectedSlot + 1, CombatSlotKey(SelectedSlot), CombatSkillName(State.Slots[SelectedSlot]))));
	const TCHAR* PassiveNames[] = {TEXT("无"), TEXT("坚韧"), TEXT("强攻")};
	PassivesText->SetText(FText::FromString(FString::Printf(TEXT("被动：坚韧 Lv.%d (+20生命/级)  ·  强攻 Lv.%d (+5攻击/级)\n当前装配：%s"),
		State.PassiveLevels[0], State.PassiveLevels[1], PassiveNames[int32(State.EquippedPassive)])));
	const int32 PassiveLevel = State.PassiveLevels[SelectedPassive];
	SelectedPassiveText->SetText(FText::FromString(FString::Printf(TEXT("选择被动：%s  Lv.%d"), PassiveNames[SelectedPassive + 1], PassiveLevel)));
	bool bCanChange = bHasProfile && Current && Current->IsCombatReady();
	if (const auto* ASC = BoundAbilitySystem.Get()) for (const auto& Spec : ASC->GetActivatableAbilities()) if (Spec.IsActive()) bCanChange = false;
	LearnSkillButton->SetIsEnabled(bCanChange && SkillLevel < 3 && State.SkillPoints > 0 && State.Level >= SkillLevel + 1);
	EquipSkillButton->SetIsEnabled(bCanChange && SkillLevel > 0);
	LearnPassiveButton->SetIsEnabled(bCanChange && PassiveLevel < 3 && State.SkillPoints > 0 && State.Level >= PassiveLevel + 1);
	EquipPassiveButton->SetIsEnabled(bCanChange && PassiveLevel > 0);
	UnequipPassiveButton->SetIsEnabled(bCanChange && State.EquippedPassive != EMultiplayerCombatPassive::None);
	SaveButton->SetIsEnabled(bHasProfile && Current && Current->IsCombatReady()); LoadButton->SetIsEnabled(bCanChange);
}

// 前后选择仅改变本地菜单光标，不向服务器提交成长状态。
void UmultiplayerCombatProgressionWidget::PreviousSkill() { SelectedSkill = (SelectedSkill + 4) % 5; RefreshMenu(); }
void UmultiplayerCombatProgressionWidget::NextSkill() { SelectedSkill = (SelectedSkill + 1) % 5; RefreshMenu(); }
void UmultiplayerCombatProgressionWidget::PreviousSlot() { SelectedSlot = (SelectedSlot + 4) % 5; RefreshMenu(); }
void UmultiplayerCombatProgressionWidget::NextSlot() { SelectedSlot = (SelectedSlot + 1) % 5; RefreshMenu(); }
void UmultiplayerCombatProgressionWidget::PreviousPassive() { SelectedPassive = 1 - SelectedPassive; RefreshMenu(); }
void UmultiplayerCombatProgressionWidget::NextPassive() { SelectedPassive = 1 - SelectedPassive; RefreshMenu(); }
// 学习与装备只申请服务器操作，UI 等待事件更新实际结果。
void UmultiplayerCombatProgressionWidget::LearnSkill() { if (auto* P = BoundProgression.Get()) P->RequestLearnSkill(EMultiplayerCombatSkill(SelectedSkill)); }
void UmultiplayerCombatProgressionWidget::EquipSkill() { if (auto* P = BoundProgression.Get()) P->RequestEquipSkill(SelectedSlot, EMultiplayerCombatSkill(SelectedSkill)); }
void UmultiplayerCombatProgressionWidget::LearnPassive() { if (auto* P = BoundProgression.Get()) P->RequestLearnPassive(EMultiplayerCombatPassive(SelectedPassive + 1)); }
void UmultiplayerCombatProgressionWidget::EquipPassive() { if (auto* P = BoundProgression.Get()) P->RequestEquipPassive(EMultiplayerCombatPassive(SelectedPassive + 1)); }
void UmultiplayerCombatProgressionWidget::UnequipPassive() { if (auto* P = BoundProgression.Get()) P->RequestEquipPassive(EMultiplayerCombatPassive::None); }
// 磁盘请求使用角色已拥有连接的服务器包装，不从客户端直接调用组件存档函数。
void UmultiplayerCombatProgressionWidget::SaveProgress() { if (auto* P = Character.Get()) P->SaveProgress(); }
void UmultiplayerCombatProgressionWidget::LoadProgress() { if (auto* P = Character.Get()) P->LoadProgress(); }

UmultiplayerEnemyHealthWidgetComponent::UmultiplayerEnemyHealthWidgetComponent()
{
	ConfigureWorldDefaults();
	SetIsReplicatedByDefault(false);
}

void UmultiplayerEnemyHealthWidgetComponent::ConfigureWorldDefaults()
{
	SetWidgetSpace(EWidgetSpace::World);
	SetDrawSize(FVector2D(180.f, 30.f));
	SetDrawAtDesiredSize(false);
	SetPivot(FVector2D(.5f, 1.f));
	SetBlendMode(EWidgetBlendMode::Masked);
	SetTwoSided(true);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetWindowFocusable(false);
	bReceiveHardwareInput = false;
	SetCastShadow(false);
	SetVisibleInRayTracing(false);
	SetManuallyRedraw(true);
	SetRedrawTime(0.f);
	SetTickWhenOffscreen(false);
	SetMaterial(0, nullptr); // 使用正常深度检测的引擎 Masked 材质，不穿墙。
}

void UmultiplayerEnemyHealthWidgetComponent::InitWidget()
{
	// PIE 的 DS 世界也不创建 UI，不能只依赖进程级 IsRunningDedicatedServer。
	if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		SetTickMode(ETickMode::Disabled);
		return;
	}
	Super::InitWidget();
}

void UmultiplayerEnemyHealthWidgetComponent::OnRegister()
{
	// Renderer 在 Super::OnRegister 按 Space 创建；先消除旧蓝图的 Screen 覆盖，不能等 BeginPlay 才改。
	ConfigureWorldDefaults();
	Super::OnRegister();
}

void UmultiplayerEnemyHealthWidgetComponent::InitializeForCharacter(AmultiplayerCombatCharacter* NewCharacter)
{
	Character = NewCharacter;
	if (!NewCharacter || NewCharacter->GetNetMode() == NM_DedicatedServer)
	{
		StopPresentation();
		return;
	}
	bPresentationStopped = false;
	// BP 可能存有旧 Screen/绝对变换；这里只纠正本实例，不改原素材布局。
	ConfigureWorldDefaults();
	const FVector Anchor = NewCharacter->GetCapsuleComponent()->GetComponentTransform().TransformPosition(
		FVector(0.f, 0.f, NewCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 28.f));
	AttachToComponent(NewCharacter->GetMesh(), FAttachmentTransformRules::KeepWorldTransform);
	SetAbsolute(false, true, true);
	SetWorldLocation(Anchor);
	// 180x30 像素对应 72x12cm；不受源角色 Mesh 的缩放和旋转影响。
	SetWorldScale3D(FVector(.4f));
	SetTickMode(ETickMode::Enabled);
	SetHiddenInGame(false);
	InitWidget();
	if (auto* Health = Cast<UmultiplayerEnemyHealthWidget>(GetUserWidgetObject()))
	{
		Health->SetPresentationComponent(this);
		Health->BindCharacter(NewCharacter);
	}
	RequestRedraw();
}

void UmultiplayerEnemyHealthWidgetComponent::StopPresentation()
{
	bPresentationStopped = true;
	SetHiddenInGame(true);
	if (auto* Health = Cast<UmultiplayerEnemyHealthWidget>(GetUserWidgetObject()))
	{
		Health->BindCharacter(nullptr);
		Health->SetPresentationComponent(nullptr);
	}
	SetWidget(nullptr);
	SetTickMode(ETickMode::Disabled);
}

void UmultiplayerEnemyHealthWidgetComponent::NotifyContentChanged()
{
	if (bPresentationStopped) return;
	++ContentRevision;
	if (const auto* Enemy = Character.Get(); !Enemy || !Enemy->IsAlive()) StopPresentation();
	else RequestRedraw();
}

void UmultiplayerEnemyHealthWidgetComponent::UpdateForView(const FVector& ViewLocation)
{
	if (bPresentationStopped) return;
	const auto* Enemy = Character.Get();
	if (!Enemy || !Enemy->IsAlive()) { StopPresentation(); return; }
	const bool bInRange = FVector::DistSquared(ViewLocation, GetComponentLocation()) <= FMath::Square(3000.f);
	const bool bWasHidden = bHiddenInGame;
	SetHiddenInGame(!bInRange);
	if (!bInRange) return;
	FRotator Facing = (ViewLocation - GetComponentLocation()).Rotation();
	Facing.Roll = 0.f;
	SetWorldRotation(Facing);
	if (bWasHidden)
	{
		if (auto* Health = Cast<UmultiplayerEnemyHealthWidget>(GetUserWidgetObject())) Health->RefreshSnapshot();
		RequestRedraw();
	}
}

void UmultiplayerEnemyHealthWidgetComponent::TickComponent(float DeltaSeconds, ELevelTick TickType,
	FActorComponentTickFunction* TickFunction)
{
	if (bPresentationStopped) return;
	if (auto* Viewer = GetWorld()->GetFirstPlayerController(); Viewer && Viewer->IsLocalController())
	{
		FVector Location; FRotator Rotation;
		Viewer->GetPlayerViewPoint(Location, Rotation);
		UpdateForView(Location);
	}
	// 镜头朝向只改组件变换；纯数值未变时，手动重绘不会再次生成 RenderTarget 内容。
	if (!bPresentationStopped && !bHiddenInGame) Super::TickComponent(DeltaSeconds, TickType, TickFunction);
}

void UmultiplayerEnemyHealthWidgetComponent::EndPlay(EEndPlayReason::Type Reason)
{
	StopPresentation();
	Super::EndPlay(Reason);
}

void UmultiplayerEnemyHealthWidget::SetPresentationComponent(UmultiplayerEnemyHealthWidgetComponent* Component)
{
	PresentationComponent = Component;
	if (Health) Health->SetPresentationComponent(Component);
}

void UmultiplayerEnemyHealthWidget::RefreshSnapshot()
{
	if (Health) Health->RefreshSnapshot();
	else if (auto* Enemy = ObservedCharacter.Get()) RefreshHealth(Enemy->GetHealth(), Enemy->GetMaxHealth(), 0.f, 0.f);
}

void UmultiplayerEnemyHealthWidget::BindCharacter(AmultiplayerCombatCharacter* NewCharacter)
{
	if (auto* Previous = BoundCharacter.Get()) Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshHealth);
	BoundCharacter.Reset();
	ObservedCharacter = NewCharacter;
	LastDisplayedHealth = LastDisplayedMaxHealth = -1.f;
	if (Health)
	{
		// 外层只转交观察对象，实际生命事件由内层条形控件订阅一次。
		Health->SetPresentationComponent(PresentationComponent.Get());
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
	LastDisplayedHealth = LastDisplayedMaxHealth = -1.f;
	if (Health) Health->BindCharacter(nullptr);
	// 保留弱观察对象，Slate 重建后重新订阅；敌人 EndPlay 会明确 BindCharacter(nullptr)。
	Super::NativeDestruct();
}

void UmultiplayerEnemyHealthWidget::RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy)
{
	// 角色的能量/护盾也广播 Stats；生命显示未变化时不格式化文字、不请求 RT 重绘。
	if (CurrentHealth == LastDisplayedHealth && MaxHealth == LastDisplayedMaxHealth) return;
	LastDisplayedHealth = CurrentHealth;
	LastDisplayedMaxHealth = MaxHealth;
	const float Percent = FMath::Clamp(CurrentHealth / FMath::Max(1.f, MaxHealth), 0.f, 1.f);
	if (HealthBar) HealthBar->SetPercent(Percent);
	// 保留源布局的底层条形样式，不运行源蓝图的平滑计时器或逐帧查询。
	if (LerpAttributeBar) LerpAttributeBar->SetPercent(Percent);
	if (HealthText) HealthText->SetText(FText::FromString(FString::Printf(TEXT("%.0f / %.0f"), CurrentHealth, MaxHealth)));
	SetVisibility(CurrentHealth > 0 && ObservedCharacter.IsValid() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (auto* Component = PresentationComponent.Get()) Component->NotifyContentChanged();
}

void AmultiplayerCombatHUD::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwningPlayerController() && GetOwningPlayerController()->IsLocalController() && CombatWidgetClass)
	{
		NativeCombatWidget = CreateWidget<UUserWidget>(GetOwningPlayerController(), CombatWidgetClass);
		if (NativeCombatWidget)
		{
			NativeCombatWidget->AddToViewport();
			// 正常地图入口结束连接菜单的 UIOnly 捕获；禁行状态仍由角色/GAS管理，不清除其输入锁。
			auto* Controller = GetOwningPlayerController();
			FInputModeGameOnly InputMode;
			Controller->SetInputMode(InputMode);
			Controller->bShowMouseCursor = false;
		}
	}
}

// Canvas 只投影本地查询结果，不使用开发调试线，也不向服务器发送预览对象。
bool AmultiplayerCombatHUD::ProjectTargetPoint(const FVector& Location, FVector2D& Screen) const
{
	// 复用本帧 Canvas 的视图，范围多段线不重复获取玩家的投影矩阵。
	const FVector Point = Canvas->Project(Location, true);
	if (Point.Z <= 0.f) return false;
	Screen = FVector2D(Point.X, Point.Y);
	return true;
}

bool AmultiplayerCombatHUD::IsRangePointVisible(const FVector& Location)
{
	if (const bool* Cached = RangeVisibilityCache.Find(Location)) return *Cached;
	// 只让场景遮挡范围线，忽略角色；圆环相邻两段共用端点，本帧不重复射线。
	static const FCollisionResponseParams SceneOnly = []
	{
		FCollisionResponseParams Response(ECR_Ignore);
		Response.CollisionResponse.SetResponse(ECC_WorldStatic, ECR_Block);
		Response.CollisionResponse.SetResponse(ECC_WorldDynamic, ECR_Block);
		return Response;
	}();
	FHitResult Hit;
	const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, RangeViewLocation, Location, ECC_Visibility,
		FCollisionQueryParams(SCENE_QUERY_STAT(CombatRangeVisibility), false, PlayerOwner->GetPawn()), SceneOnly);
	// 允许圈线紧贴地表的接触误差，不把贴地端点误判成整段都在地板后面。
	const bool bVisible = !bBlocked || Hit.Distance + 2.f >= FVector::Dist(RangeViewLocation, Location);
	RangeVisibilityCache.Add(Location, bVisible);
	return bVisible;
}

void AmultiplayerCombatHUD::DrawWorldSegment(const FVector& Start, const FVector& End,
	const FLinearColor& Color, float Thickness)
{
	FVector2D A, B;
	if (ProjectTargetPoint(Start, A) && ProjectTargetPoint(End, B)
		&& IsRangePointVisible(Start) && IsRangePointVisible(End)
		&& (FVector::DistSquared(Start, End) <= FMath::Square(100.f) || IsRangePointVisible((Start + End) * .5f)))
		DrawLine(A.X, A.Y, B.X, B.Y, Color, Thickness);
}

void AmultiplayerCombatHUD::DrawWorldRing(const FVector& Center, const FVector& Normal,
	float Radius, const FLinearColor& Color)
{
	if (Radius <= 0.f) return;
	FVector X, Y;
	Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector).FindBestAxisVectors(X, Y);
	constexpr int32 Segments = 48;
	FVector Previous = Center + X * Radius;
	for (int32 Index = 1; Index <= Segments; ++Index)
	{
		const float Angle = float(Index) * UE_TWO_PI / Segments;
		const FVector Next = Center + (X * FMath::Cos(Angle) + Y * FMath::Sin(Angle)) * Radius;
		DrawWorldSegment(Previous, Next, Color);
		Previous = Next;
	}
}

// 目标由同源范围查询选择；这里只标出哪些对象当前确实满足阵营、距离和遮挡条件。
void AmultiplayerCombatHUD::DrawTargetMarker(const AmultiplayerCombatCharacter* Target,
	const FLinearColor& Color, const FString& Label)
{
	if (!Target || Target == PlayerOwner->GetPawn()) return;
	FVector2D Top, Bottom, Side;
	const auto* Capsule = Target->GetCapsuleComponent();
	const FVector Center = Target->GetActorLocation();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Right = PlayerOwner->PlayerCameraManager
		? PlayerOwner->PlayerCameraManager->GetCameraRotation().RotateVector(FVector::RightVector) : FVector::RightVector;
	if (!ProjectTargetPoint(Center + FVector::UpVector * HalfHeight, Top)
		|| !ProjectTargetPoint(Center - FVector::UpVector * HalfHeight, Bottom)
		|| !ProjectTargetPoint(Center + Right * Capsule->GetScaledCapsuleRadius(), Side)
		|| !IsRangePointVisible(Center)) return;
	const float MidX = (Top.X + Bottom.X) * .5f;
	const float HalfWidth = FMath::Max(8.f, FMath::Abs(Side.X - MidX) + 3.f);
	const float Left = MidX - HalfWidth, RightX = MidX + HalfWidth;
	const float TopY = FMath::Min(Top.Y, Bottom.Y) - 4.f, BottomY = FMath::Max(Top.Y, Bottom.Y) + 3.f;
	const float Corner = FMath::Min(HalfWidth * .6f, 12.f);
	for (float X : {Left, RightX})
	{
		const float Inward = X == Left ? Corner : -Corner;
		DrawLine(X, TopY, X + Inward, TopY, Color, 2.f);
		DrawLine(X, TopY, X, TopY + Corner, Color, 2.f);
		DrawLine(X, BottomY, X + Inward, BottomY, Color, 2.f);
		DrawLine(X, BottomY, X, BottomY - Corner, Color, 2.f);
	}
	if (!Label.IsEmpty())
	{
		float Width, Height;
		GetTextSize(Label, Width, Height, GEngine->GetSmallFont(), 1.f);
		DrawText(Label, FLinearColor::Black, MidX - Width * .5f + 1.f, TopY - Height - 3.f, GEngine->GetSmallFont());
		DrawText(Label, Color, MidX - Width * .5f, TopY - Height - 4.f, GEngine->GetSmallFont());
	}
}

void AmultiplayerCombatHUD::DrawCenteredPrompt(const FString& Text, float Y, const FLinearColor& Color)
{
	if (Text.IsEmpty()) return;
	const float Scale = FMath::Clamp(Canvas->ClipX / 960.f, 1.f, 1.35f);
	float Width, Height;
	GetTextSize(Text, Width, Height, GEngine->GetSmallFont(), Scale);
	const float X = (Canvas->ClipX - Width) * .5f;
	DrawText(Text, FLinearColor::Black, X + 1.f, Y + 1.f, GEngine->GetSmallFont(), Scale);
	DrawText(Text, Color, X, Y, GEngine->GetSmallFont(), Scale);
}

void AmultiplayerCombatHUD::DrawTargetPreview(const FmultiplayerCombatTargetPreview& Preview, const FLinearColor& Color)
{
	const auto* Character = Cast<AmultiplayerCombatCharacter>(PlayerOwner->GetPawn());
	const float HalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	switch (Preview.Skill)
	{
	case EMultiplayerCombatSkill::Attack:
	case EMultiplayerCombatSkill::Defend:
		// 普攻和自身防御使用原界面的准星、技能提示及实际动作，不叠加调试式射线和扇区。
		break;
	case EMultiplayerCombatSkill::Heal:
		if (auto* Target = Preview.Target.Get())
		{
			// 只标记本次治疗对象，不把整个三维搜索球画满屏幕；真实选人规则仍由同源查询决定。
			const auto* Capsule = Target->GetCapsuleComponent();
			const FVector Feet = Target->GetActorLocation() - FVector::UpVector * (Capsule->GetScaledCapsuleHalfHeight() - 3.f);
			DrawWorldRing(Feet, FVector::UpVector, Capsule->GetScaledCapsuleRadius(), Color);
		}
		break;
	case EMultiplayerCombatSkill::Area:
	{
		// 只保留落点的伤害边界，删除角色外围的最大射程圈与连接线。
		// 判定是有限高度的水平圆柱；斜坡法线只供粒子贴地，不能旋转真实半径边界。
		DrawWorldRing(Preview.End + FVector::UpVector * 2.f, FVector::UpVector, Preview.Radius, Color);
		DrawWorldSegment(Preview.End - FVector(14.f, 0.f, 0.f), Preview.End + FVector(14.f, 0.f, 0.f), Color);
		DrawWorldSegment(Preview.End - FVector(0.f, 14.f, 0.f), Preview.End + FVector(0.f, 14.f, 0.f), Color);
		break;
	}
	case EMultiplayerCombatSkill::Dash:
	{
		const FVector Offset = FVector::UpVector * (3.f - HalfHeight);
		const FVector Start = Preview.Origin + Offset, End = Preview.End + Offset;
		const FVector Forward = (End - Start).GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(Forward, FVector::UpVector) * Preview.Radius;
		DrawWorldRing(End, FVector::UpVector, Preview.Radius, Color);
		DrawWorldSegment(End, End - Forward * 45.f + Side * .65f, Color, 2.f);
		DrawWorldSegment(End, End - Forward * 45.f - Side * .65f, Color, 2.f);
		break;
	}
	}
}

void AmultiplayerCombatHUD::ShowResolvedArea(const FVector& Center, float Radius)
{
	ResolvedAreaCenter = Center;
	ResolvedAreaRadius = Radius;
	ResolvedAreaUntil = GetWorld()->GetTimeSeconds() + CombatTransientEffectLifetime;
}

// 客户端只显示复制后的起手阶段；判定几何与服务器共用，绝不读取服务端专有 AI 黑板。
void AmultiplayerCombatHUD::DrawEnemyTelegraphs(const AmultiplayerCombatCharacter* LocalCharacter)
{
	bool bInMeleeRange = false;
	const FLinearColor Danger(1.f, .18f, .08f);
	for (TActorIterator<AmultiplayerCombatEnemy> It(GetWorld()); It; ++It)
	{
		const auto* Enemy = *It;
		if (!Enemy->IsCasting() || !Enemy->IsAlive() || !UsesEnemyMelee(Enemy)
			|| FVector::DistSquared(Enemy->GetActorLocation(), LocalCharacter->GetActorLocation()) > FMath::Square(1500.f)) continue;
		const auto Rule = Enemy->GetSkillRule(EMultiplayerCombatSkill::Attack);
		const float TotalDuration = Rule.Windup + Rule.Recovery;
		if (TotalDuration <= 0.f || Rule.Windup <= 0.f
			|| Enemy->GetCastProgress() >= Rule.Windup / TotalDuration) continue;
		FVector2D Screen;
		if (!ProjectTargetPoint(Enemy->GetActorLocation(), Screen) || Screen.X < 0.f || Screen.X > Canvas->ClipX
			|| Screen.Y < 0.f || Screen.Y > Canvas->ClipY || !PlayerOwner->LineOfSightTo(Enemy)) continue;
		// 近战距离是三维球；按当前玩家的高度求水平截面，再投影到小兵脚底，跳起/跨层不会夸大攻击距离。
		const float DeltaZ = LocalCharacter->GetActorLocation().Z - Enemy->GetActorLocation().Z;
		const float RadiusSquared = FMath::Square(Rule.Range) - FMath::Square(DeltaZ);
		if (RadiusSquared <= 0.f) continue;
		const float Radius = FMath::Sqrt(RadiusSquared);
		const FVector Center = Enemy->GetActorLocation() - FVector::UpVector
			* (Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 3.f);
		const FVector Forward = Enemy->GetActorForwardVector().GetSafeNormal2D();
		const float HalfAngle = FMath::RadiansToDegrees(FMath::Acos(CombatTargeting::MeleeFrontDot));
		FVector Previous = Center + Forward.RotateAngleAxis(-HalfAngle, FVector::UpVector) * Radius;
		DrawWorldSegment(Center, Previous, Danger, 2.f);
		for (int32 Index = 1; Index <= 24; ++Index)
		{
			const FVector Next = Center + Forward.RotateAngleAxis(FMath::Lerp(-HalfAngle, HalfAngle, float(Index) / 24.f), FVector::UpVector) * Radius;
			DrawWorldSegment(Previous, Next, Danger, 2.f);
			Previous = Next;
		}
		DrawWorldSegment(Center, Previous, Danger, 2.f);
		DrawWorldSegment(Center, Center + Forward * Radius, Danger.CopyWithNewOpacity(.45f));
		DrawTargetMarker(Enemy, Danger, TEXT("近战起手"));
		bInMeleeRange |= CombatTargeting::CanMeleeHit(*Enemy, *LocalCharacter, true);
	}
	if (bInMeleeRange)
		DrawCenteredPrompt(TEXT("处于近战攻击范围 · 离开红色扇区可躲避"), Canvas->ClipY * .13f, Danger);
}

// 只有瞄准几何跟随相机逐帧更新；生命、能量、状态和技能计时仍走原控件的事件/活动计时器。
void AmultiplayerCombatHUD::DrawHUD()
{
	Super::DrawHUD();
	const auto* Character = PlayerOwner ? Cast<AmultiplayerCombatCharacter>(PlayerOwner->GetPawn()) : nullptr;
	if (!Canvas || !Character || !PlayerOwner->IsLocalController() || !Character->IsLocallyControlled()
		|| PlayerOwner->bShowMouseCursor || !Character->IsAlive()) return;
	FRotator ViewRotation;
	PlayerOwner->GetPlayerViewPoint(RangeViewLocation, ViewRotation);
	RangeVisibilityCache.Reset();
	const FLinearColor Ready(.2f, .9f, 1.f), Blocked(1.f, .25f, .2f), Hint(.9f, .94f, 1.f);
	const bool bTargeting = Character->IsTargetingSkill();
	const bool bAreaCasting = Character->IsCasting();
	const auto Skill = bTargeting ? Character->GetTargetingSkill()
		: bAreaCasting ? EMultiplayerCombatSkill::Area : EMultiplayerCombatSkill::Attack;
	const auto Preview = bTargeting || bAreaCasting ? Character->GetSkillTargetPreview(Skill) : FmultiplayerCombatTargetPreview();
	const FText Failure = bTargeting || bAreaCasting ? Character->GetSkillFailureReason(Skill) : FText::GetEmpty();
	const bool bAvailable = Preview.bValid && Failure.IsEmpty();
	if (bTargeting) ResolvedAreaUntil = 0;
	const bool bShowingResolvedArea = ResolvedAreaUntil > GetWorld()->GetTimeSeconds();
	if (bTargeting || (bAreaCasting && !bShowingResolvedArea))
		DrawTargetPreview(Preview, bAreaCasting ? FLinearColor(1.f, .75f, .18f) : bAvailable ? Ready : Blocked);
	if (bShowingResolvedArea)
		DrawWorldRing(ResolvedAreaCenter + FVector::UpVector * 2.f, FVector::UpVector, ResolvedAreaRadius, FLinearColor(1.f, .65f, .1f));
	DrawEnemyTelegraphs(Character);
	const float PromptY = Canvas->ClipY * .20f;
	if (bTargeting)
	{
		FString InputKey;
		const auto* Progression = Character->GetProgressionComponent();
		for (int32 Slot = 0; Slot < 5; ++Slot)
			if (Progression->GetSlotSkill(Slot) == Skill) { InputKey = CombatSlotKey(Slot); break; }
		const FString Action = InputKey == TEXT("LMB") ? TEXT("左键确认 · 右键取消")
			: FString::Printf(TEXT("松开 %s 释放 · 右键取消"), *InputKey);
		DrawCenteredPrompt(FString::Printf(TEXT("%s · %s"), CombatSkillName(Skill), *Action), PromptY, Hint);
		FString Detail;
		if (Skill == EMultiplayerCombatSkill::Heal)
		{
			const FString Selection = !Failure.IsEmpty() ? Failure.ToString()
				: Preview.Target.Get() == Character ? TEXT("本次治疗自己") : TEXT("本次治疗标记队友，优先最低血量比例");
			Detail = FString::Printf(TEXT("半径 %.1f m 球形 · 必须可见 · %s"), Preview.Radius / 100.f, *Selection);
		}
		else if (!Failure.IsEmpty()) Detail = Failure.ToString();
		else if (!Preview.bValid) Detail = Character->GetSkillFailureText(Skill,
			Skill == EMultiplayerCombatSkill::Area ? CombatTags::Failure_InvalidGround
			: Skill == EMultiplayerCombatSkill::Heal ? CombatTags::Failure_NoHealTarget
			: Skill == EMultiplayerCombatSkill::Dash ? CombatTags::Failure_DashBlocked : CombatTags::Failure_MuzzleBlocked).ToString();
		else if (Skill == EMultiplayerCombatSkill::Defend)
			Detail = FString::Printf(TEXT("正面格挡 · 前 %.1f 秒可弹反正面投射物"), Character->GetCombatConfig().ParryWindow);
		else if (Skill == EMultiplayerCombatSkill::Area)
			Detail = FString::Printf(TEXT("射程 %.1f m · 落点 %.1f m · 伤害半径 %.1f m · %d 个目标"),
				Character->GetSkillRule(Skill).Range / 100.f, FVector::Dist2D(Preview.Origin, Preview.End) / 100.f,
				Preview.Radius / 100.f, Preview.AffectedTargets.Num());
		else if (Skill == EMultiplayerCombatSkill::Dash)
			Detail = FString::Printf(TEXT("预计水平距离 %.1f m · 静态障碍截断；落差/动态障碍可能改变落点"),
				FVector::Dist2D(Preview.Origin, Preview.End) / 100.f);
		DrawCenteredPrompt(Detail, PromptY + 24.f, bAvailable ? Ready : Blocked);
	}
	else if (bAreaCasting) DrawCenteredPrompt(TEXT("施法中 · 右键打断（已提交消耗不返还）"), PromptY, Hint);
	if (bShowingResolvedArea)
		DrawCenteredPrompt(TEXT("黄色圈为已结算范围，粒子消散不代表持续伤害"), PromptY + 24.f, FLinearColor(1.f, .75f, .18f));
	else if (!bTargeting && !bAreaCasting)
		DrawCenteredPrompt(TEXT("Q / E / R / Shift 按住预览、松开释放 · 右键取消 · 左键普攻"), PromptY, Hint.CopyWithNewOpacity(.8f));
}

void AmultiplayerCombatHUD::EndPlay(EEndPlayReason::Type Reason)
{
	if (NativeCombatWidget) { NativeCombatWidget->RemoveFromParent(); NativeCombatWidget = nullptr; }
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
	const auto* CombatCharacter = Cast<AmultiplayerCombatCharacter>(Pawn);
	// 姿态不持有 Montage、不设超时，也不回写标签；高优先级技能与死亡仍由原有动作槽接管。
	CombatIsDefending = CombatCharacter && CombatCharacter->IsCombatReady()
		&& CombatCharacter->IsDefending() && !CombatCharacter->IsStunned();
}
