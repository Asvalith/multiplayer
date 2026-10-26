// Copyright Epic Games, Inc. All Rights Reserved.
// 战斗表现层：Cue 管音画，UMG 订阅状态，HUD 投影范围，动画实例只采样运动与标签。
// 展示可以异步补载与重建，但不能反向提交伤害、资源或技能成功结果。
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
#include "Camera/PlayerCameraManager.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/DecalComponent.h"
#include "Components/Image.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
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
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

namespace
{
	// 资源锚点与缩放在 Feedback 中配置；本函数只消费本次事实快照。
	void ResolveFeedbackTransform(AActor* Target, const FGameplayCueParameters& Parameters,
		const FmultiplayerCombatFeedback& Feedback, FVector& Location, FRotator& Rotation)
	{
		const auto* Character = CastChecked<AmultiplayerCombatCharacter>(Target);
		Location = Target->GetActorLocation(); Rotation = FRotator::ZeroRotator;
		switch (Feedback.Anchor)
		{
		case EMultiplayerFeedbackAnchor::Feet:
			Location -= Character->GetActorUpVector() * Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(); break;
		case EMultiplayerFeedbackAnchor::CueLocation: Location = Parameters.Location; break;
		case EMultiplayerFeedbackAnchor::Muzzle:
			Location = !FVector(Parameters.Normal).IsNearlyZero() ? FVector(Parameters.Location) : Character->GetCombatMuzzleLocation();
			Rotation = !FVector(Parameters.Normal).IsNearlyZero() ? FVector(Parameters.Normal).Rotation() : Character->GetActorRotation(); break;
		default: break;
		}
		Location += Rotation.RotateVector(Feedback.LocalOffset); Rotation += Feedback.RotationOffset;
	}

	// 按角色配置识别近战小兵，避免把它的施法锁画成玩家 R 预告圈。
	bool UsesEnemyMelee(const AmultiplayerCombatCharacter* Character)
	{
		return Character && Character->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& Character->GetAttackAbilityClass()
			&& Character->GetAttackAbilityClass()->IsChildOf(UmultiplayerEnemyMeleeAbility::StaticClass());
	}
}

// ===== 1. GameplayCue：只消费结果，拥有自己的异步加载与音画清理 =====
// Cue 组件按需创建、移除时销毁，不启用常驻 Tick。
AmultiplayerCombatCue::AmultiplayerCombatCue()
{
	bAutoDestroyOnRemove = true;
	bAutoAttachToOwner = false;
	PrimaryActorTick.bCanEverTick = false;
}

// 根据 Cue 类型读取表现配置；治疗特效取施法者配置而非受治疗者配置。
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
	if (GetNetMode() == NM_DedicatedServer) return false;
	if (GameplayCueTag == CombatTags::Cue_Area && bHasExecutionFeedback)
	{
		const auto* Context = FmultiplayerCombatCueContext::Get(Parameters.EffectContext);
		if (!Context) return false;
		if (!AreaFeedbackIdentity.Matches(Context->ActivationIdentity))
		{
			// 新 B 可以接替仍在加载的 A；只有已过期的 A 回调才应被丢弃。
			auto* Character = Cast<AmultiplayerCombatCharacter>(Target);
			if (!Character || !Character->AcceptAreaFeedbackIdentity(Context->ActivationIdentity)) return false;
			StopPresentation();
		}
	}
	if (!bHasExecutionFeedback)
	{
		const auto* SelectedFeedback = GetFeedback(Target, &Parameters);
		if (!SelectedFeedback) return false;
		ExecutionFeedback = *SelectedFeedback;
		bHasExecutionFeedback = true;
		if (GameplayCueTag == CombatTags::Cue_Area)
		{
			AreaFeedbackOwner = Cast<AmultiplayerCombatCharacter>(Target);
			if (const auto* Context = FmultiplayerCombatCueContext::Get(Parameters.EffectContext))
				AreaFeedbackIdentity = Context->ActivationIdentity;
		}
	}
	if (GameplayCueTag == CombatTags::Cue_Area && (!AreaFeedbackOwner.IsValid()
		|| !AreaFeedbackOwner->IsAlive() || AreaFeedbackOwner->IsActorBeingDestroyed()
		|| !AreaFeedbackOwner->AcceptAreaFeedbackIdentity(AreaFeedbackIdentity)))
	{
		K2_EndGameplayCue();
		return false;
	}
	const auto* Feedback = &ExecutionFeedback;
	if (GameplayCueTag == CombatTags::Cue_Heal && !bExecutionHealNotified)
	{
		bExecutionHealNotified = true;
		// 只提示本次确实成功的队友治疗，不给施法者再播放一份假回血粒子。
		if (auto* Caster = Cast<AmultiplayerCombatCharacter>(Parameters.GetInstigator());
			Caster && Caster != Target && Caster->IsLocallyControlled())
			if (!Feedback->SuccessfulCasterMessage.IsEmpty()) Caster->OnCombatMessage.Broadcast(Feedback->SuccessfulCasterMessage);
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
	const auto* Character = CastChecked<AmultiplayerCombatCharacter>(Target);
	FVector Location; FRotator Rotation;
	ResolveFeedbackTransform(Target, Parameters, *Feedback, Location, Rotation);
	// 投射物尾迹由弹体持有；这里的 AttackFeedback 只能配置一次性起手效果。
	if (bUseCascade)
	{
		if (auto* System = Feedback->CascadeEffect.Get())
			if (auto* Component = UGameplayStatics::SpawnEmitterAtLocation(this, System, Location, Rotation, Feedback->Scale))
			{
				if (Feedback->bAttachToTarget)
				{
					Component->SetAbsolute(false, false, true);
					Component->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
				}
				FTimerHandle Cleanup;
				GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
					[Component] { Component->DestroyComponent(); }), Feedback->CascadeCleanupDelay, false);
				if (AreaFeedbackOwner.IsValid()) AreaFeedbackOwner->TrackAreaFeedback(Component, AreaFeedbackIdentity);
			}
	}
	else if (auto* System = Feedback->Effect.Get())
	{
		auto* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, Rotation,
			Feedback->Scale, true, false);
		if (Component)
		{
			if (!Feedback->DurationParameter.IsNone()) Component->SetVariableFloat(Feedback->DurationParameter, Feedback->Duration);
			if (!Feedback->AreaScaleParameter.IsNone()) Component->SetVariableFloat(Feedback->AreaScaleParameter,
				Parameters.RawMagnitude / Character->GetCombatConfig().AreaEffectReferenceRadius);
			if (Feedback->bAttachToTarget)
			{
				// 世界空间尺寸与位置跟随分开，由资产选择是否附着。
				Component->SetAbsolute(false, false, true);
				Component->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
			}
			Component->Activate(true);
			// 素材后续即使被误设为循环，瞬时命中特效也不能无限遗留在场景中。
			FTimerHandle Cleanup;
			GetWorldTimerManager().SetTimer(Cleanup, FTimerDelegate::CreateWeakLambda(Component,
				[Component] { Component->DestroyComponent(); }), Feedback->NiagaraCleanupDelay, false);
			if (AreaFeedbackOwner.IsValid()) AreaFeedbackOwner->TrackAreaFeedback(Component, AreaFeedbackIdentity);
		}
	}
	if (auto* Sound = Feedback->Sound.Get()) UGameplayStatics::PlaySoundAtLocation(this, Sound, Location);
	// 瞬时 Cue 自身无需存活；生成的粒子由 Niagara 自动回收。
	// Cascade 同样自动回收，并保留超时清理，避免误用循环素材留下组件。
	K2_EndGameplayCue();
	return true;
}

// 创建或更新持续表现；异步加载回来时仍须确认本次循环尚未结束。
void AmultiplayerCombatCue::StartLoop(AActor* Target, const FGameplayCueParameters& Parameters)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	if (GameplayCueTag == CombatTags::Cue_Casting)
	{
		auto* Character = Cast<AmultiplayerCombatCharacter>(Target);
		if (!Character) return;
		// 小兵借用施法标签暂停追击，但它的近战扇区不是玩家 R 的地面落点圈。
		if (UsesEnemyMelee(Character)) return;
		if (const auto* Context = FmultiplayerCombatCueContext::Get(Parameters.EffectContext))
		{
			const auto Current = Character->GetAreaActivationIdentity();
			if (Character->IsLocallyControlled() && Character->IsCasting() && Current.IsValid()
				&& !Current.Matches(Context->ActivationIdentity)) return;
			if (Context->ActivationIdentity.AuthorityRevision != 0)
			{
				if (!Character->AcceptAreaFeedbackIdentity(Context->ActivationIdentity)) return;
			}
			else if (Character->IsAreaFeedbackCancelled(Context->ActivationIdentity)) return;
			AreaFeedbackIdentity = Context->ActivationIdentity;
		}
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
		const FVector Size(Config.AreaMarkerDepth, Config.AreaRadius, Config.AreaRadius);
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
						if (!bLoopRequested || !Weak.IsValid()) return;
						// 加载的是共享材质，不是旧施法结果；同一 Cue 被新轮次接替时服务当前身份。
						// StartLoop 复核当前取消状态，保留 WhileActive 校正的落点，不重放旧 HitResult。
						FGameplayCueParameters Resume;
						if (AreaFeedbackIdentity.IsValid())
						{
							auto* Context = new FmultiplayerCombatCueContext();
							Context->ActivationIdentity = AreaFeedbackIdentity;
							Resume.EffectContext = FGameplayEffectContextHandle(Context);
						}
						StartLoop(Weak.Get(), Resume);
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
	FVector Location; FRotator Rotation;
	ResolveFeedbackTransform(Target, Parameters, *Feedback, Location, Rotation);
	if (bUseCascade)
	{
		LoopCascadeEffect = UGameplayStatics::SpawnEmitterAtLocation(this, Feedback->CascadeEffect.Get(), Location, Rotation, Feedback->Scale, false);
		if (LoopCascadeEffect && Feedback->bAttachToTarget)
		{
			LoopCascadeEffect->SetAbsolute(false, false, true);
			LoopCascadeEffect->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		}
		return;
	}
	LoopEffect = UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, Feedback->Effect.Get(), Location,
		Rotation, Feedback->Scale, false, false);
	if (!LoopEffect) return;
	if (!Feedback->DurationParameter.IsNone()) LoopEffect->SetVariableFloat(Feedback->DurationParameter, Feedback->LoopDuration);
	// 位置跟随角色，尺寸保留素材的世界大小，避免 Niagara 的绝对位置使附着失效。
	if (Feedback->bAttachToTarget)
	{
		LoopEffect->SetAbsolute(false, false, true);
		LoopEffect->AttachToComponent(Target->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	}
	LoopEffect->Activate(true);
}

// 持续效果首次进入时请求循环表现，可播放一次起始音效。
bool AmultiplayerCombatCue::OnActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	if (GetNetMode() == NM_DedicatedServer) return false;
	bLoopRequested = true;
	StartLoop(Target, Parameters);
	if (const auto* Feedback = GetFeedback(Target))
		if (auto* Sound = Feedback->Sound.Get()) UGameplayStatics::PlaySoundAtLocation(this, Sound, Target->GetActorLocation());
	return true;
}
// 相关性恢复或晚加入时补持续表现，不把它重放为一次新的施法。
bool AmultiplayerCombatCue::WhileActive_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	// 晚加入恢复持续表现，但不重播过去的一次性施法音效。
	bLoopRequested = true;
	StartLoop(Target, Parameters);
	return true;
}
// 对应效果结束后撤销本 Cue 的组件与补载请求。
bool AmultiplayerCombatCue::OnRemove_Implementation(AActor* Target, const FGameplayCueParameters& Parameters)
{
	StopPresentation();
	return true;
}
// 交还引擎 Cue 池前清空表现状态，不让下一次实例复用继承旧组件。
bool AmultiplayerCombatCue::Recycle()
{
	StopPresentation();
	return Super::Recycle();
}
// 退出世界时也执行同一清理，不依赖一定先收到效果移除。
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
	AreaFeedbackOwner.Reset();
	AreaFeedbackIdentity = {};
	bExecutionRangeShown = false;
	bExecutionHealNotified = false;
	bHasMarkerLocation = false;
	if (LoadingAssets) { LoadingAssets->CancelHandle(); LoadingAssets.Reset(); }
	if (LoopEffect) { LoopEffect->DestroyComponent(); LoopEffect = nullptr; }
	if (LoopCascadeEffect) { LoopCascadeEffect->DestroyComponent(); LoopCascadeEffect = nullptr; }
	if (AreaMarker) { AreaMarker->DestroyComponent(); AreaMarker = nullptr; }
}

// 监听拥有者并初始化只读显示快照；图标资源由实际 WBP 默认值提供。
// ===== 2. 战斗 HUD 数据适配：订阅 → 补读快照 → WBP 更新控件 =====
// [亮点] 不把委托当作网络消息；绑定后补读当前值，角色替换/销毁时解绑。
void UmultiplayerCombatWidget::NativeConstruct()
{
	Super::NativeConstruct();
	bPresentationReady = true;
	ClearFeedback();
	if (auto* Controller = GetOwningPlayer())
	{
		Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::BindPawn);
		BindPawn(nullptr, Controller->GetPawn());
	}
	else BindPawn(nullptr, nullptr);
}
// 控件离开视口时解绑控制器、角色和计时器，重建不会累加监听。
void UmultiplayerCombatWidget::NativeDestruct()
{
	if (auto* Controller = GetOwningPlayer()) Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::BindPawn);
	// 解绑先提交最后一份空快照，让真实 WBP 清除退出前的提示；之后再停止向已拆除的控件发布。
	BindPawn(nullptr, nullptr);
	bPresentationReady = false;
	Super::NativeDestruct();
}
// 先释放旧 Pawn/ASC/整局状态的订阅，再绑定新对象并读取当前快照。
// 委托只通知本机；跨网络靠属性和 OnRep。补读快照覆盖晚加入，解绑避免换 Pawn 后重复响应。
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
// Pawn 已销毁时立即解除绑定，等待控制器之后提供新 Pawn。
void UmultiplayerCombatWidget::HandlePawnDestroyed(AActor* DestroyedActor)
{
	BindPawn(nullptr, nullptr);
}
// 目标进度事件只触发重读共享状态，不在 HUD 累加进度。
void UmultiplayerCombatWidget::HandleObjectiveChanged(int32 Activated, int32 Required) { RefreshState(); }
// 按属性事件刷新血条与能量，再更新依赖资源的技能外观。
void UmultiplayerCombatWidget::RefreshStats(float Health, float MaxHealth, float Energy, float MaxEnergy)
{
	ViewHealthPercent = Health / FMath::Max(1.f, MaxHealth);
	ViewEnergyPercent = Energy / FMath::Max(1.f, MaxEnergy);
	ViewHealthText = FText::Format(TemplateText(TEXT("Vitals")), FormatNumber(Health, 0), FormatNumber(MaxHealth, 0));
	ViewEnergyText = FText::Format(TemplateText(TEXT("Vitals")), FormatNumber(Energy, 0), FormatNumber(MaxEnergy, 0));
	RefreshState();
}
// 文案模板由真实 WBP 配置，缺省时显示空文本。
const FText& UmultiplayerCombatWidget::TemplateText(FName Key) const
{
	if (const FText* Value = TextTemplates.Find(Key)) return *Value;
	return FText::GetEmpty();
}

FText UmultiplayerCombatWidget::FormatNumber(float Value, int32 DecimalPlaces) const
{
	FNumberFormattingOptions Format; Format.MinimumFractionalDigits = Format.MaximumFractionalDigits = DecimalPlaces;
	return FText::AsNumber(Value, &Format);
}

void UmultiplayerCombatWidget::RefreshStatusText()
{
	const auto* Current = Character.Get(); const auto* Coop = Current ? Current->GetCoopComponent() : nullptr; const auto* SharedState = BoundGameState.Get();
	{
		TArray<FText> Status;
		if (SharedState && SharedState->HasGameLost()) Status.Add(TemplateText(TEXT("Lost")));
		else if (SharedState && SharedState->HasGameWon()) Status.Add(TemplateText(TEXT("Won")));
		else if (!Current) Status.Add(TemplateText(TEXT("Waiting")));
		else if (!Current->IsAlive()) Status.Add(TemplateText(TEXT("Dead")));
		else if (Coop && Coop->IsDowned()) Status.Add(FText::Format(TemplateText(TEXT("Downed")), FormatNumber(Coop->GetDownedRemaining(), 1)));
		else if (!Current->IsCombatReady()) Status.Add(TemplateText(TEXT("Connecting")));
		else
		{
			if (const auto* State = BoundGameState.Get(); State && State->GetEncounterState().TotalWaves > 0)
			{
				const auto Round = State->GetEncounterState(); const auto& Objective = State->GetObjectiveState();
				const FText WaveLabel = Round.Wave == 0 ? TemplateText(TEXT("WaitingPartner")) : FText::Format(TemplateText(TEXT("Wave")), Round.Wave, Round.TotalWaves, Round.LivingEnemies);
				Status.Add(FText::Format(TemplateText(TEXT("Objectives")), WaveLabel, Objective.ActivatedKeys, Objective.RequiredKeys));
			}
			if (Current->GetShield() > 0) Status.Add(FText::Format(TemplateText(TEXT("Shield")), FormatNumber(Current->GetShield(), 0)));
			if (Current->IsStunned()) Status.Add(TemplateText(TEXT("Stunned")));
			if (Current->IsRooted()) Status.Add(TemplateText(TEXT("Rooted")));
			if (Current->IsBurning()) Status.Add(TemplateText(TEXT("Burning")));
			if (Current->IsDefending()) Status.Add(TemplateText(TEXT("Defending")));
			if (Coop && Coop->IsRescuing()) Status.Add(FText::Format(TemplateText(TEXT("Rescuing")), FormatNumber(Coop->GetRescueProgress() * 100.f, 0)));
			if (Current->GetDashCooldownRemaining() > 0.f) Status.Add(FText::Format(TemplateText(TEXT("DashCooldown")), FormatNumber(Current->GetDashCooldownRemaining(), 1)));
			if (const auto* Progression = BoundProgression.Get(); Progression && (Progression->GetState().Level > 1 || Progression->GetState().SkillPoints != 2))
				Status.Add(FText::Format(TemplateText(TEXT("ShortProgression")), Progression->GetState().Level, Progression->GetState().SkillPoints));
		}
		ViewStateText = FText::Join(TemplateText(TEXT("Separator")), Status);
		ViewStateVisibility = ViewStateText.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible;
	}
	{
		const auto* Progression = BoundProgression.Get();
		ViewProgressionText = Progression ? FText::Format(TemplateText(TEXT("Progression")), Progression->GetState().Level,
			Progression->GetState().Experience, Progression->GetState().Level * 100, Progression->GetState().SkillPoints) : FText::GetEmpty();
	}
}
// 用短时区域显示本次提示，新的消息替换旧消息并重置隐藏时间。
void UmultiplayerCombatWidget::ShowFeedback(FText Message)
{
	ViewFeedbackText = Message;
	ViewFeedbackVisibility = Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible;
	if (bPresentationReady) PresentCombatSnapshot();
	GetWorld()->GetTimerManager().ClearTimer(FeedbackTimer);
	if (!Message.IsEmpty()) GetWorld()->GetTimerManager().SetTimer(FeedbackTimer, this, &ThisClass::ClearFeedback, FeedbackLifetime, false);
}
// 撤销提示计时并清空显示；空文本不会遗留可见背景。
void UmultiplayerCombatWidget::ClearFeedback()
{
	GetWorld()->GetTimerManager().ClearTimer(FeedbackTimer);
	ViewFeedbackText = FText::GetEmpty();
	ViewFeedbackVisibility = ESlateVisibility::Collapsed;
	if (bPresentationReady) PresentCombatSnapshot();
}
// 事件与活动倒计时共用一次快照刷新，不再缓存另一份角色权限或能量。
// 全部计时结束后停掉低频 Timer，闲时仍只响应事件。
void UmultiplayerCombatWidget::RefreshState()
{
	const auto* Current = Character.Get();
	const auto* Coop = Current ? Current->GetCoopComponent() : nullptr;
	const bool bReady = Current && Current->IsCombatReady();
	const bool bRescuing = Coop && Coop->IsRescuing();
	const bool bCasting = Current && (Current->IsCasting() || bRescuing);
	const bool bStunned = Current && Current->IsStunned();
	const float Energy = Current ? Current->GetEnergy() : 0.f;
	ViewCastVisibility = bCasting ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	ViewCastText = bCasting ? TemplateText(bRescuing ? TEXT("CastRescue") : TEXT("CastArea")) : FText::GetEmpty();
	const auto& Config = Current ? Current->GetCombatConfig() : *GetDefault<UmultiplayerCombatConfig>();
	FText* CooldownTexts[] = { &ViewAttackCooldownText, &ViewDefenseCooldownText, &ViewHealCooldownText, &ViewAreaCooldownText, &ViewDashCooldownText };
	float* CooldownPercents[] = { &ViewAttackCooldownPercent, &ViewDefenseCooldownPercent, &ViewHealCooldownPercent, &ViewAreaCooldownPercent, &ViewDashCooldownPercent };
	FText* Names[] = { &ViewAttackName, &ViewDefenseName, &ViewHealName, &ViewAreaName, &ViewDashName };
	FSlateBrush* Brushes[] = { &ViewAttackBrush, &ViewDefenseBrush, &ViewHealBrush, &ViewAreaBrush, &ViewDashBrush };
	FLinearColor* Tints[] = { &ViewAttackTint, &ViewDefenseTint, &ViewHealTint, &ViewAreaTint, &ViewDashTint };
	const auto* Progression = BoundProgression.Get();
	bool bHasActiveTimer = bCasting || (Coop && Coop->IsDowned());
	RefreshStatusText();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const auto Skill = Progression ? Progression->GetSlotSkill(Index) : EMultiplayerCombatSkill(Index);
		const float Remaining = Current ? Current->GetSkillCooldownRemaining(Skill) : 0.f;
		const auto Rule = Current ? Current->GetSkillRule(Skill) : Config.GetSkill(Skill);
		const bool bCoolingDown = Remaining > 0;
		// This snapshot summarizes local resource/state only; it does not grant the skill or query targets.
		const bool bAvailable = bReady && !bStunned && !bCasting && !bCoolingDown
			&& Energy >= Rule.Cost && (!Progression || Progression->GetSkillLevel(Skill) > 0);
		bHasActiveTimer |= bCoolingDown;
		*CooldownTexts[Index] = bCoolingDown ? FormatNumber(Remaining, 1) : FText::GetEmpty();
		*CooldownPercents[Index] = FMath::Clamp(Remaining / FMath::Max(.05f, Rule.Cooldown), 0.f, 1.f);
		*Names[Index] = SkillNames.IsValidIndex(int32(Skill)) ? SkillNames[int32(Skill)] : FText::GetEmpty();
		*Brushes[Index] = SkillIcons.IsValidIndex(int32(Skill)) ? SkillIcons[int32(Skill)] : FSlateBrush();
		*Tints[Index] = bAvailable ? AvailableIconColor : UnavailableIconColor;
	}
	ViewCastPercent = bCasting && Current ? (bRescuing ? Coop->GetRescueProgress() : Current->GetCastProgress()) : 0;
	if (bPresentationReady) PresentCombatSnapshot();
	// Keep the existing event-driven idle behavior: poll only while a countdown or cast is active.
	FTimerManager& Timers = GetWorld()->GetTimerManager();
	if (bHasActiveTimer)
	{
		if (!Timers.IsTimerActive(RefreshTimer)) Timers.SetTimer(RefreshTimer, this, &ThisClass::RefreshState, CountdownRefreshInterval, true);
	}
	else Timers.ClearTimer(RefreshTimer);
}

// 打开先监听角色替换，再绑定当前角色并补读快照；真实视口中接管焦点和鼠标。
// ===== 3. 成长菜单：本地选择与业务请求，不持有第二份成长状态 =====
void UmultiplayerCombatProgressionWidget::NativeConstruct()
{
	Super::NativeConstruct();
	bPresentationReady = true;
	// 绑定真实蓝图按钮；重复创建 Slate 时不重复注册请求。
	if (PreviousSkillButton) PreviousSkillButton->OnClicked.AddUniqueDynamic(this, &ThisClass::PreviousSkill);
	if (NextSkillButton) NextSkillButton->OnClicked.AddUniqueDynamic(this, &ThisClass::NextSkill);
	if (LearnSkillButton) LearnSkillButton->OnClicked.AddUniqueDynamic(this, &ThisClass::LearnSkill);
	if (PreviousSlotButton) PreviousSlotButton->OnClicked.AddUniqueDynamic(this, &ThisClass::PreviousSlot);
	if (NextSlotButton) NextSlotButton->OnClicked.AddUniqueDynamic(this, &ThisClass::NextSlot);
	if (EquipSkillButton) EquipSkillButton->OnClicked.AddUniqueDynamic(this, &ThisClass::EquipSkill);
	if (PreviousPassiveButton) PreviousPassiveButton->OnClicked.AddUniqueDynamic(this, &ThisClass::PreviousPassive);
	if (NextPassiveButton) NextPassiveButton->OnClicked.AddUniqueDynamic(this, &ThisClass::NextPassive);
	if (LearnPassiveButton) LearnPassiveButton->OnClicked.AddUniqueDynamic(this, &ThisClass::LearnPassive);
	if (EquipPassiveButton) EquipPassiveButton->OnClicked.AddUniqueDynamic(this, &ThisClass::EquipPassive);
	if (UnequipPassiveButton) UnequipPassiveButton->OnClicked.AddUniqueDynamic(this, &ThisClass::UnequipPassive);
	if (SaveProgressButton) SaveProgressButton->OnClicked.AddUniqueDynamic(this, &ThisClass::SaveProgress);
	if (LoadProgressButton) LoadProgressButton->OnClicked.AddUniqueDynamic(this, &ThisClass::LoadProgress);
	if (CloseProgressionButton) CloseProgressionButton->OnClicked.AddUniqueDynamic(this, &ThisClass::CloseMenu);
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
	bPresentationReady = false;
	if (PreviousSkillButton) PreviousSkillButton->OnClicked.RemoveDynamic(this, &ThisClass::PreviousSkill);
	if (NextSkillButton) NextSkillButton->OnClicked.RemoveDynamic(this, &ThisClass::NextSkill);
	if (LearnSkillButton) LearnSkillButton->OnClicked.RemoveDynamic(this, &ThisClass::LearnSkill);
	if (PreviousSlotButton) PreviousSlotButton->OnClicked.RemoveDynamic(this, &ThisClass::PreviousSlot);
	if (NextSlotButton) NextSlotButton->OnClicked.RemoveDynamic(this, &ThisClass::NextSlot);
	if (EquipSkillButton) EquipSkillButton->OnClicked.RemoveDynamic(this, &ThisClass::EquipSkill);
	if (PreviousPassiveButton) PreviousPassiveButton->OnClicked.RemoveDynamic(this, &ThisClass::PreviousPassive);
	if (NextPassiveButton) NextPassiveButton->OnClicked.RemoveDynamic(this, &ThisClass::NextPassive);
	if (LearnPassiveButton) LearnPassiveButton->OnClicked.RemoveDynamic(this, &ThisClass::LearnPassive);
	if (EquipPassiveButton) EquipPassiveButton->OnClicked.RemoveDynamic(this, &ThisClass::EquipPassive);
	if (UnequipPassiveButton) UnequipPassiveButton->OnClicked.RemoveDynamic(this, &ThisClass::UnequipPassive);
	if (SaveProgressButton) SaveProgressButton->OnClicked.RemoveDynamic(this, &ThisClass::SaveProgress);
	if (LoadProgressButton) LoadProgressButton->OnClicked.RemoveDynamic(this, &ThisClass::LoadProgress);
	if (CloseProgressionButton) CloseProgressionButton->OnClicked.RemoveDynamic(this, &ThisClass::CloseMenu);
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
void UmultiplayerCombatProgressionWidget::ShowMessage(FText Message) { ViewMessage = Message; if (bPresentationReady) PresentProgressionSnapshot(); }

// 文案模板和名称取自真实 WBP，原生只填充快照数值并刷新按钮请求资格。
void UmultiplayerCombatProgressionWidget::RefreshMenu()
{
	const auto* Current = Character.Get(); const auto* Progression = BoundProgression.Get();
	const FmultiplayerCombatProgressionState State = Progression ? Progression->CaptureState() : FmultiplayerCombatProgressionState();
	const bool bHasProfile = Progression && State.ProfileId.IsValid();
	const auto Name = [](const TArray<FText>& Names, int32 Index) { return Names.IsValidIndex(Index) ? Names[Index] : FText::GetEmpty(); };
	ViewSummary = bHasProfile ? FText::Format(SummaryFormat, State.Level, State.Experience, State.Level * 100, State.SkillPoints) : WaitingText;
	TArray<FText> SkillLines;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const auto Skill = EMultiplayerCombatSkill(Index); const int32 Level = Progression ? Progression->GetSkillLevel(Skill) : 0;
		const int32 SkillSlotIndex = State.Slots.IndexOfByKey(Skill);
		SkillLines.Add(FText::Format(SkillLineFormat, Name(SkillNames, Index), Level > 0 ? FText::Format(SkillLevelFormat, Level) : UnlearnedText,
			SkillSlotIndex + 1, Name(SlotKeys, SkillSlotIndex)));
	}
	ViewSkills = FText::Join(FText::FromString(TEXT("\n")), SkillLines);
	const int32 SkillLevel = Progression ? Progression->GetSkillLevel(EMultiplayerCombatSkill(SelectedSkill)) : 0;
	ViewSelectedSkill = FText::Format(SelectedSkillFormat, Name(SkillNames, SelectedSkill),
		SkillLevel > 0 ? FText::Format(SkillLevelFormat, SkillLevel) : UnlockText);
	ViewSelectedSlot = FText::Format(SlotFormat, SelectedSlot + 1, Name(SlotKeys, SelectedSlot), Name(SkillNames, int32(State.Slots[SelectedSlot])));
	ViewPassives = FText::Format(PassivesFormat, State.PassiveLevels[0], State.PassiveLevels[1], Name(PassiveNames, int32(State.EquippedPassive)));
	const int32 PassiveLevel = State.PassiveLevels[SelectedPassive];
	ViewSelectedPassive = FText::Format(SelectedPassiveFormat, Name(PassiveNames, SelectedPassive + 1), PassiveLevel);
	bool bCanChange = bHasProfile && Current && Current->IsCombatReady();
	if (const auto* ASC = BoundAbilitySystem.Get()) for (const auto& Spec : ASC->GetActivatableAbilities()) if (Spec.IsActive()) bCanChange = false;
	ViewCanLearnSkill = bCanChange && SkillLevel < 3 && State.SkillPoints > 0 && State.Level >= SkillLevel + 1;
	ViewCanEquipSkill = bCanChange && SkillLevel > 0;
	ViewCanLearnPassive = bCanChange && PassiveLevel < 3 && State.SkillPoints > 0 && State.Level >= PassiveLevel + 1;
	ViewCanEquipPassive = bCanChange && PassiveLevel > 0;
	ViewCanUnequipPassive = bCanChange && State.EquippedPassive != EMultiplayerCombatPassive::None;
	ViewCanSaveProgress = bHasProfile && Current && Current->IsCombatReady(); ViewCanLoadProgress = bCanChange;
	if (bPresentationReady) PresentProgressionSnapshot();
}

// 前后选择仅改变本地菜单光标，不向服务器提交成长状态。
void UmultiplayerCombatProgressionWidget::PreviousSkill() { SelectedSkill = (SelectedSkill + 4) % 5; RefreshMenu(); }
// 选择下一个技能，只移动本地菜单光标。
void UmultiplayerCombatProgressionWidget::NextSkill() { SelectedSkill = (SelectedSkill + 1) % 5; RefreshMenu(); }
// 选择上一个待装配槽位，不立即交换服务器配装。
void UmultiplayerCombatProgressionWidget::PreviousSlot() { SelectedSlot = (SelectedSlot + 4) % 5; RefreshMenu(); }
// 选择下一个待装配槽位，随后由装配按钮发请求。
void UmultiplayerCombatProgressionWidget::NextSlot() { SelectedSlot = (SelectedSlot + 1) % 5; RefreshMenu(); }
// 在两种被动之间切换本地选择。
void UmultiplayerCombatProgressionWidget::PreviousPassive() { SelectedPassive = 1 - SelectedPassive; RefreshMenu(); }
// 向后循环切换被动选择，不施加或移除效果。
void UmultiplayerCombatProgressionWidget::NextPassive() { SelectedPassive = 1 - SelectedPassive; RefreshMenu(); }
// 学习与装备只申请服务器操作，UI 等待事件更新实际结果。
void UmultiplayerCombatProgressionWidget::LearnSkill() { if (auto* P = BoundProgression.Get()) P->RequestLearnSkill(EMultiplayerCombatSkill(SelectedSkill)); }
// 请求将所选技能装到所选槽，服务器验证并决定交换规则。
void UmultiplayerCombatProgressionWidget::EquipSkill() { if (auto* P = BoundProgression.Get()) P->RequestEquipSkill(SelectedSlot, EMultiplayerCombatSkill(SelectedSkill)); }
// 请求学习或升级所选被动，点数只能由服务器扣除。
void UmultiplayerCombatProgressionWidget::LearnPassive() { if (auto* P = BoundProgression.Get()) P->RequestLearnPassive(EMultiplayerCombatPassive(SelectedPassive + 1)); }
// 请求装配所选已学习被动，不直接改变客户端属性。
void UmultiplayerCombatProgressionWidget::EquipPassive() { if (auto* P = BoundProgression.Get()) P->RequestEquipPassive(EMultiplayerCombatPassive(SelectedPassive + 1)); }
// 请求卸下当前被动；最终上限和资源处理仍由服务器 GAS 完成。
void UmultiplayerCombatProgressionWidget::UnequipPassive() { if (auto* P = BoundProgression.Get()) P->RequestEquipPassive(EMultiplayerCombatPassive::None); }
// 磁盘请求使用角色已拥有连接的服务器包装，不从客户端直接调用组件存档函数。
void UmultiplayerCombatProgressionWidget::SaveProgress() { if (auto* P = Character.Get()) P->SaveProgress(); }
// 经角色所属连接请求读档，不在本地自行替换成长快照。
void UmultiplayerCombatProgressionWidget::LoadProgress() { if (auto* P = Character.Get()) P->LoadProgress(); }

// ===== 4. 存量血条 Widget 父类：只供旧素材读取，当前敌人不创建或绑定 =====

// 先解绑旧观察对象，再由最内层血条订阅一次生命事件并补读快照。
void UmultiplayerEnemyHealthWidget::BindCharacter(AmultiplayerCombatCharacter* NewCharacter)
{
	if (auto* Previous = BoundCharacter.Get()) Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshHealth);
	BoundCharacter.Reset();
	ObservedCharacter = NewCharacter;
	LastDisplayedHealth = LastDisplayedMaxHealth = -1.f;
	if (Health)
	{
		// 外层只转交观察对象，实际生命事件由内层条形控件订阅一次。
		Health->BindCharacter(NewCharacter);
		ViewVisibility = NewCharacter ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
		if (bPresentationReady) PresentHealthSnapshot();
		return;
	}
	if (NewCharacter)
	{
		BoundCharacter = NewCharacter;
		NewCharacter->OnCombatStatsChanged.AddUniqueDynamic(this, &ThisClass::RefreshHealth);
		RefreshHealth(NewCharacter->GetHealth(), NewCharacter->GetMaxHealth(), 0, 0);
	}
	else { ViewVisibility = ESlateVisibility::Collapsed; if (bPresentationReady) PresentHealthSnapshot(); }
}

// Slate 重建后恢复对弱观察对象的订阅。
void UmultiplayerEnemyHealthWidget::NativeConstruct()
{
	Super::NativeConstruct();
	bPresentationReady = true;
	BindCharacter(ObservedCharacter.Get());
}

// 解除活动订阅但保留弱观察意图，以便正常的 Slate 重建恢复。
void UmultiplayerEnemyHealthWidget::NativeDestruct()
{
	bPresentationReady = false;
	if (auto* Previous = BoundCharacter.Get()) Previous->OnCombatStatsChanged.RemoveDynamic(this, &ThisClass::RefreshHealth);
	BoundCharacter.Reset();
	LastDisplayedHealth = LastDisplayedMaxHealth = -1.f;
	if (Health) Health->BindCharacter(nullptr);
	// 仅保留存量素材的 Slate 重建语义；当前敌人没有绑定或展示这套 Widget。
	Super::NativeDestruct();
}

// 保留存量布局的只读快照更新，不再持有世界组件或申请 RenderTarget 重绘。
void UmultiplayerEnemyHealthWidget::RefreshHealth(float CurrentHealth, float MaxHealth, float Energy, float MaxEnergy)
{
	if (CurrentHealth == LastDisplayedHealth && MaxHealth == LastDisplayedMaxHealth) return;
	LastDisplayedHealth = CurrentHealth; LastDisplayedMaxHealth = MaxHealth;
	ViewHealthPercent = FMath::Clamp(CurrentHealth / FMath::Max(1.f, MaxHealth), 0.f, 1.f);
	FNumberFormattingOptions Format; Format.MinimumFractionalDigits = Format.MaximumFractionalDigits = 0;
	ViewHealthText = FText::Format(HealthTextFormat, FText::AsNumber(CurrentHealth, &Format), FText::AsNumber(MaxHealth, &Format));
	ViewVisibility = CurrentHealth > 0 && ObservedCharacter.IsValid() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (bPresentationReady) PresentHealthSnapshot();
}

// ===== 5. Canvas 范围数据：C++ 查询/投影，唯一 HUD 蓝图执行绘制 =====
// [边界] 预览是本地当前结果，不是服务器授权；粒子大小也不代表伤害范围。
// 本地创建一份已配置 HUD 控件并恢复游戏输入，专服不参与 UI 构建。
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

// 缓存本帧端点的场景遮挡查询，避免相邻范围线重复做同一条射线。
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

// 屏幕记录只保存本帧只读结果；这些方法不提交 Canvas 绘制命令。
void AmultiplayerCombatHUD::AppendScreenSegment(float AX, float AY, float BX, float BY, const FLinearColor& Color, float Thickness)
{
	auto& Line = HUDLines.AddDefaulted_GetRef(); Line.StartScreenX = AX; Line.StartScreenY = AY;
	Line.EndScreenX = BX; Line.EndScreenY = BY; Line.LineColor = Color; Line.LineThickness = Thickness;
}

void AmultiplayerCombatHUD::AppendScreenLabel(const FText& Text, float X, float Y, const FLinearColor& Color, float Scale)
{
	const auto Add = [this, &Text, Scale](float PX, float PY, const FLinearColor& Tint)
	{
		auto& Label = HUDTexts.AddDefaulted_GetRef(); Label.Text = Text.ToString(); Label.ScreenX = PX; Label.ScreenY = PY;
		Label.TextColor = Tint; Label.Font = Presentation.Font; Label.Scale = Scale;
	};
	Add(X + Presentation.ShadowOffset.X, Y + Presentation.ShadowOffset.Y, Presentation.ShadowColor); Add(X, Y, Color);
}

FText AmultiplayerCombatHUD::FormatDistance(float Value) const
{
	FNumberFormattingOptions Format; Format.MinimumFractionalDigits = Format.MaximumFractionalDigits = FMath::Clamp(Presentation.DecimalPlaces, 0, 3);
	return FText::AsNumber(Value, &Format);
}

// AHUD::DrawHUD 同步触发 ReceiveDrawHUD，先更新快照以免蓝图绘制上一帧的数据。
void AmultiplayerCombatHUD::DrawHUD()
{
	HUDLines.Reset(); HUDTexts.Reset(); BuildHUDFrame(); if (Canvas) Super::DrawHUD();
}

// 将可见世界线段投影到 Canvas；被场景挡住的边界不穿墙绘制。
void AmultiplayerCombatHUD::AppendWorldSegment(const FVector& Start, const FVector& End,
	const FLinearColor& Color, float Thickness)
{
	FVector2D A, B;
	if (ProjectTargetPoint(Start, A) && ProjectTargetPoint(End, B)
		&& IsRangePointVisible(Start) && IsRangePointVisible(End)
		&& (FVector::DistSquared(Start, End) <= FMath::Square(100.f) || IsRangePointVisible((Start + End) * .5f)))
		AppendScreenSegment(A.X, A.Y, B.X, B.Y, Color, Thickness);
}

// 按给定平面绘制圆环，尺寸取查询半径，不从粒子包围盒反推。
void AmultiplayerCombatHUD::AppendWorldRing(const FVector& Center, const FVector& Normal,
	float Radius, const FLinearColor& Color)
{
	if (Radius <= 0.f) return;
	FVector X, Y;
	Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector).FindBestAxisVectors(X, Y);
	const int32 Segments = FMath::Max(3, Presentation.RingSegments);
	FVector Previous = Center + X * Radius;
	for (int32 Index = 1; Index <= Segments; ++Index)
	{
		const float Angle = float(Index) * UE_TWO_PI / Segments;
		const FVector Next = Center + (X * FMath::Cos(Angle) + Y * FMath::Sin(Angle)) * Radius;
		AppendWorldSegment(Previous, Next, Color, Presentation.LineThickness);
		Previous = Next;
	}
}

// 目标由同源范围查询选择；这里只标出哪些对象当前确实满足阵营、距离和遮挡条件。
void AmultiplayerCombatHUD::BuildTargetMarker(const AmultiplayerCombatCharacter* Target,
	const FLinearColor& Color, const FText& Label)
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
	const float HalfWidth = FMath::Max(Presentation.MarkerMinHalfWidth, FMath::Abs(Side.X - MidX) + Presentation.MarkerSidePadding);
	const float Left = MidX - HalfWidth, RightX = MidX + HalfWidth;
	const float TopY = FMath::Min(Top.Y, Bottom.Y) - Presentation.MarkerTopPadding, BottomY = FMath::Max(Top.Y, Bottom.Y) + Presentation.MarkerBottomPadding;
	const float Corner = FMath::Min(HalfWidth * Presentation.MarkerCornerRatio, Presentation.MarkerCornerMax);
	for (float X : {Left, RightX})
	{
		const float Inward = X == Left ? Corner : -Corner;
		AppendScreenSegment(X, TopY, X + Inward, TopY, Color, Presentation.MarkerThickness);
		AppendScreenSegment(X, TopY, X, TopY + Corner, Color, Presentation.MarkerThickness);
		AppendScreenSegment(X, BottomY, X + Inward, BottomY, Color, Presentation.MarkerThickness);
		AppendScreenSegment(X, BottomY, X, BottomY - Corner, Color, Presentation.MarkerThickness);
	}
	if (!Label.IsEmpty())
	{
		float Width, Height;
		GetTextSize(Label.ToString(), Width, Height, Presentation.Font, Presentation.MarkerTextScale);
		AppendScreenLabel(Label, MidX - Width * .5f, TopY - Height - Presentation.MarkerLabelGap, Color, Presentation.MarkerTextScale);
	}
}

// 按视口宽度居中显示操作或失败说明，黑色偏移仅用于保证可读性。
void AmultiplayerCombatHUD::AppendCenteredPrompt(const FText& Text, float Y, const FLinearColor& Color)
{
	if (Text.IsEmpty()) return;
	const float Scale = FMath::Clamp(Canvas->ClipX / FMath::Max(1.f, Presentation.PromptReferenceWidth), Presentation.PromptMinScale, Presentation.PromptMaxScale);
	float Width, Height;
	GetTextSize(Text.ToString(), Width, Height, Presentation.Font, Scale);
	const float X = (Canvas->ClipX - Width) * .5f;
	AppendScreenLabel(Text, X, Y, Color, Scale);
}

// 按技能绘制查询几何；普攻/防御不叠调试射线，R 显示真实伤害圆柱的水平边界。
void AmultiplayerCombatHUD::BuildTargetPreview(const FmultiplayerCombatTargetPreview& Preview, const FLinearColor& Color)
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
			const FVector Feet = Target->GetActorLocation() - FVector::UpVector * (Capsule->GetScaledCapsuleHalfHeight() - Presentation.TargetGroundLift);
			AppendWorldRing(Feet, FVector::UpVector, Capsule->GetScaledCapsuleRadius(), Color);
		}
		break;
	case EMultiplayerCombatSkill::Area:
	{
		// 只保留落点的伤害边界，删除角色外围的最大射程圈与连接线。
		// 判定是有限高度的水平圆柱；斜坡法线只供粒子贴地，不能旋转真实半径边界。
		AppendWorldRing(Preview.End + FVector::UpVector * Presentation.GroundLift, FVector::UpVector, Preview.Radius, Color);
		AppendWorldSegment(Preview.End - FVector(Presentation.CrossHalfSize, 0.f, 0.f), Preview.End + FVector(Presentation.CrossHalfSize, 0.f, 0.f), Color, Presentation.LineThickness);
		AppendWorldSegment(Preview.End - FVector(0.f, Presentation.CrossHalfSize, 0.f), Preview.End + FVector(0.f, Presentation.CrossHalfSize, 0.f), Color, Presentation.LineThickness);
		break;
	}
	case EMultiplayerCombatSkill::Dash:
	{
		const FVector Offset = FVector::UpVector * (Presentation.TargetGroundLift - HalfHeight);
		const FVector Start = Preview.Origin + Offset, End = Preview.End + Offset;
		const FVector Forward = (End - Start).GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(Forward, FVector::UpVector) * Preview.Radius;
		AppendWorldRing(End, FVector::UpVector, Preview.Radius, Color);
		AppendWorldSegment(End, End - Forward * Presentation.DashArrowLength + Side * Presentation.DashArrowSideScale, Color, Presentation.MarkerThickness);
		AppendWorldSegment(End, End - Forward * Presentation.DashArrowLength - Side * Presentation.DashArrowSideScale, Color, Presentation.MarkerThickness);
		break;
	}
	}
}

// 暂存已结算范围供短时展示；黄色边界不是持续伤害区域。
void AmultiplayerCombatHUD::ShowResolvedArea(const FVector& Center, float Radius)
{
	ResolvedAreaCenter = Center;
	ResolvedAreaRadius = Radius;
	ResolvedAreaUntil = GetWorld()->GetTimeSeconds() + Presentation.ResolvedDuration;
}

// 客户端只显示复制后的起手阶段；判定几何与服务器共用，绝不读取服务端专有 AI 黑板。
void AmultiplayerCombatHUD::BuildEnemyTelegraphs(const AmultiplayerCombatCharacter* LocalCharacter)
{
	bool bInMeleeRange = false;
	const FLinearColor Danger = Presentation.DangerColor;
	for (TActorIterator<AmultiplayerCombatEnemy> It(GetWorld()); It; ++It)
	{
		const auto* Enemy = *It;
		if (!Enemy->IsCasting() || !Enemy->IsAlive() || !UsesEnemyMelee(Enemy)
			|| FVector::DistSquared(Enemy->GetActorLocation(), LocalCharacter->GetActorLocation()) > FMath::Square(Presentation.TelegraphDistance)) continue;
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
			* (Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - Presentation.TargetGroundLift);
		const FVector Forward = Enemy->GetActorForwardVector().GetSafeNormal2D();
		const float HalfAngle = FMath::RadiansToDegrees(FMath::Acos(CombatTargeting::MeleeFrontDot));
		FVector Previous = Center + Forward.RotateAngleAxis(-HalfAngle, FVector::UpVector) * Radius;
		AppendWorldSegment(Center, Previous, Danger, Presentation.MarkerThickness);
		const int32 Segments = FMath::Max(1, Presentation.TelegraphSegments);
		for (int32 Index = 1; Index <= Segments; ++Index)
		{
			const FVector Next = Center + Forward.RotateAngleAxis(FMath::Lerp(-HalfAngle, HalfAngle, float(Index) / Segments), FVector::UpVector) * Radius;
			AppendWorldSegment(Previous, Next, Danger, Presentation.MarkerThickness);
			Previous = Next;
		}
		AppendWorldSegment(Center, Previous, Danger, Presentation.MarkerThickness);
		AppendWorldSegment(Center, Center + Forward * Radius, Danger.CopyWithNewOpacity(Presentation.TelegraphCenterOpacity), Presentation.LineThickness);
		BuildTargetMarker(Enemy, Danger, Presentation.MeleeWindup);
		bInMeleeRange |= CombatTargeting::CanMeleeHit(*Enemy, *LocalCharacter, true);
	}
	if (bInMeleeRange)
		AppendCenteredPrompt(Presentation.MeleeDanger, Canvas->ClipY * Presentation.MeleePromptHeightRatio, Danger);
}

// Canvas 每帧查询预览资格并绘制瞄准几何和敌人预警；UMG 数值与计时仍走事件/活动计时器。
void AmultiplayerCombatHUD::BuildHUDFrame()
{
	const auto* Character = PlayerOwner ? Cast<AmultiplayerCombatCharacter>(PlayerOwner->GetPawn()) : nullptr;
	if (!Canvas || !Character || !PlayerOwner->IsLocalController() || !Character->IsLocallyControlled()
		|| PlayerOwner->bShowMouseCursor || !Character->IsAlive()) return;
	FRotator ViewRotation;
	PlayerOwner->GetPlayerViewPoint(RangeViewLocation, ViewRotation);
	RangeVisibilityCache.Reset();
	const FLinearColor Ready = Presentation.ReadyColor, Blocked = Presentation.BlockedColor, Hint = Presentation.HintColor;
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
		BuildTargetPreview(Preview, bAreaCasting ? Presentation.CastingColor : bAvailable ? Ready : Blocked);
	if (bShowingResolvedArea)
		AppendWorldRing(ResolvedAreaCenter + FVector::UpVector * Presentation.GroundLift, FVector::UpVector, ResolvedAreaRadius, Presentation.ResolvedColor);
	BuildEnemyTelegraphs(Character);
	const float PromptY = Canvas->ClipY * Presentation.PromptHeightRatio;
	if (bTargeting)
	{
		int32 SlotIndex = INDEX_NONE;
		const auto* Progression = Character->GetProgressionComponent();
		for (int32 Slot = 0; Slot < 5; ++Slot)
			if (Progression->GetSlotSkill(Slot) == Skill) { SlotIndex = Slot; break; }
		const FText InputKey = Presentation.SlotKeys.IsValidIndex(SlotIndex) ? Presentation.SlotKeys[SlotIndex] : FText::GetEmpty();
		const FText SkillName = Presentation.SkillNames.IsValidIndex(int32(Skill)) ? Presentation.SkillNames[int32(Skill)] : FText::GetEmpty();
		const FText Action = SlotIndex == 0 ? Presentation.ConfirmAttack : FText::Format(Presentation.ConfirmReleaseFormat, InputKey);
		AppendCenteredPrompt(FText::Format(Presentation.SkillActionFormat, SkillName, Action), PromptY, Hint);
		FText Detail;
		if (Skill == EMultiplayerCombatSkill::Heal)
		{
			const FText Selection = !Failure.IsEmpty() ? Failure : Preview.Target.Get() == Character ? Presentation.HealSelf : Presentation.HealTeammate;
			Detail = FText::Format(Presentation.HealDetailFormat, FormatDistance(Preview.Radius / 100.f), Selection);
		}
		else if (!Failure.IsEmpty()) Detail = Failure;
		else if (!Preview.bValid) Detail = Character->GetSkillFailureText(Skill,
			Skill == EMultiplayerCombatSkill::Area ? CombatTags::Failure_InvalidGround
			: Skill == EMultiplayerCombatSkill::Heal ? CombatTags::Failure_NoHealTarget
			: Skill == EMultiplayerCombatSkill::Dash ? CombatTags::Failure_DashBlocked : CombatTags::Failure_MuzzleBlocked);
		else if (Skill == EMultiplayerCombatSkill::Defend)
			Detail = FText::Format(Presentation.DefenseDetailFormat, FormatDistance(Character->GetCombatConfig().ParryWindow));
		else if (Skill == EMultiplayerCombatSkill::Area)
			Detail = FText::Format(Presentation.AreaDetailFormat, FormatDistance(Character->GetSkillRule(Skill).Range / 100.f),
				FormatDistance(FVector::Dist2D(Preview.Origin, Preview.End) / 100.f), FormatDistance(Preview.Radius / 100.f), Preview.AffectedTargets.Num());
		else if (Skill == EMultiplayerCombatSkill::Dash)
			Detail = FText::Format(Presentation.DashDetailFormat, FormatDistance(FVector::Dist2D(Preview.Origin, Preview.End) / 100.f));
		AppendCenteredPrompt(Detail, PromptY + Presentation.DetailOffset, bAvailable ? Ready : Blocked);
	}
	else if (bAreaCasting) AppendCenteredPrompt(Presentation.Casting, PromptY, Hint);
	if (bShowingResolvedArea)
		AppendCenteredPrompt(Presentation.Resolved, PromptY + Presentation.DetailOffset, Presentation.CastingColor);
	else if (!bTargeting && !bAreaCasting)
		AppendCenteredPrompt(Presentation.Idle, PromptY, Hint.CopyWithNewOpacity(Presentation.HintOpacity));
}

// HUD 退出时移除自己创建的控件，不留下第二份视口界面。
void AmultiplayerCombatHUD::EndPlay(EEndPlayReason::Type Reason)
{
	if (NativeCombatWidget) { NativeCombatWidget->RemoveFromParent(); NativeCombatWidget = nullptr; }
	Super::EndPlay(Reason);
}

// ===== 6. 动画采样：读取 CMC 与 GAS，不写角色 Transform 或战斗结果 =====
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
