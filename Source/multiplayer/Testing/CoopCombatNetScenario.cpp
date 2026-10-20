// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/CoopNetTestDriver.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/NetDriver.h"
#include "Engine/NetworkObjectList.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Misc/ScopeExit.h"
#include "Animation/AnimInstance.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/UObjectIterator.h"

namespace
{
	struct FCombatPredictionSkill
	{
		TSubclassOf<UGameplayAbility> Ability;
		TSubclassOf<UGameplayEffect> Cooldown;
		TSubclassOf<UGameplayEffect> Cost;
		FGameplayTag CooldownTag;
		float EnergyCost;
	};

	FCombatPredictionSkill PredictionSkill(const FString& Name)
	{
		if (Name == TEXT("Attack")) return { UmultiplayerAttackAbility::StaticClass(), UmultiplayerAttackCooldownEffect::StaticClass(),
			nullptr, CombatTags::Cooldown_Attack, 0.0f };
		if (Name == TEXT("Defense")) return { UmultiplayerDefendAbility::StaticClass(), UmultiplayerDefendCooldownEffect::StaticClass(),
			UmultiplayerDefendCostEffect::StaticClass(), CombatTags::Cooldown_Defend, 20.0f };
		if (Name == TEXT("Area")) return { UmultiplayerAreaAbility::StaticClass(), UmultiplayerAreaCooldownEffect::StaticClass(),
			UmultiplayerAreaCostEffect::StaticClass(), CombatTags::Cooldown_Area, 30.0f };
		return { UmultiplayerHealAbility::StaticClass(), UmultiplayerHealCooldownEffect::StaticClass(),
			UmultiplayerHealCostEffect::StaticClass(), CombatTags::Cooldown_Heal, 25.0f };
	}

	// 每个拒绝场景后都在服务器恢复合法条件；Fresh 组只在主动退出后的新会话执行。
	struct FCombatPredictionCase { const TCHAR* Name; const TCHAR* Skill; const TCHAR* Rejection; };
	const FCombatPredictionCase PredictionCases[] = {
		{ TEXT("AttackCooldown"), TEXT("Attack"), TEXT("Cooldown") },
		{ TEXT("AttackMissingAim"), TEXT("Attack"), TEXT("MissingAim") },
		{ TEXT("DefenseCooldown"), TEXT("Defense"), TEXT("Cooldown") },
		{ TEXT("HealCooldown"), TEXT("Heal"), TEXT("Cooldown") },
		{ TEXT("HealEnergy"), TEXT("Heal"), TEXT("Energy") },
		{ TEXT("AreaCooldown"), TEXT("Area"), TEXT("Cooldown") },
		{ TEXT("AreaEnergy"), TEXT("Area"), TEXT("Energy") },
		{ TEXT("HealNoTarget"), TEXT("Heal"), TEXT("NoTarget") },
		{ TEXT("FreshAttack"), TEXT("Attack"), TEXT("Fresh") },
		{ TEXT("FreshDefense"), TEXT("Defense"), TEXT("Fresh") },
		{ TEXT("FreshHeal"), TEXT("Heal"), TEXT("Fresh") }
	};

	FGameplayTagContainer CombatTransientTags()
	{
		FGameplayTagContainer Tags(CombatTags::State_Defending);
		Tags.AddTag(CombatTags::Cooldown_Attack);
		Tags.AddTag(CombatTags::Cooldown_Defend);
		Tags.AddTag(CombatTags::Cooldown_Heal);
		Tags.AddTag(CombatTags::Cooldown_Area);
		Tags.AddTag(CombatTags::State_Casting);
		Tags.AddTag(CombatTags::State_Stunned);
		return Tags;
	}

	bool HasReceivedPredictionKey(AmultiplayerCombatCharacter* Character, const FString& Skill)
	{
		const FGameplayAbilitySpec* Spec = Character->GetAbilitySystemComponent()->FindAbilitySpecFromClass(PredictionSkill(Skill).Ability);
		const UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		const FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		return Key.IsValidKey() && Key.WasReceived();
	}
	// 场景准备只在服务器施加真实伤害效果，不直接写血量或向业务角色添加测试 RPC。
	bool ApplyCombatTestDamage(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target, float Damage)
	{
		if (!IsValid(Source) || !IsValid(Target)) return false;
		UAbilitySystemComponent* SourceASC = Source->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f,
			SourceASC->MakeEffectContext());
		if (!Spec.IsValid()) return false;
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Damage);
		SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
		return true;
	}

	// 隔离的 AI 权威规则探针，不是玩家网络侧击验收；配置只引用正式资产，Q/资格/伤害均走原生路径。
	bool CheckSideAndRearGuard(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Reference, FString& Details)
	{
		Details = FString::Printf(TEXT("stage=validate source=%s reference=%s sourceAuthority=%d;"),
			*GetNameSafe(Source), *GetNameSafe(Reference), IsValid(Source) && Source->HasAuthority());
		if (!IsValid(Source) || !IsValid(Reference) || !Source->HasAuthority()) return false;
		UWorld* World = Source->GetWorld();
		if (!World || Reference->GetWorld() != World) { Details += TEXT(" invalid or mismatched world;"); return false; }
		const FTransform Transform(FRotator::ZeroRotator, Reference->GetActorLocation() + FVector(0, 3000, 0));
		auto* Guard = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(AmultiplayerCombatCharacter::StaticClass(),
			Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Guard) { Details += TEXT(" stage=spawn deferred guard failed;"); return false; }
		AAIController* Controller = nullptr;
		ON_SCOPE_EXIT
		{
			if (IsValid(Guard)) Guard->Destroy();
			if (IsValid(Controller)) Controller->Destroy();
		};
		Guard->SetActorHiddenInGame(true);
		const auto* ConfigField = FindFProperty<FObjectPropertyBase>(AmultiplayerCombatCharacter::StaticClass(), TEXT("CombatConfig"));
		if (!ConfigField) { Details += TEXT(" stage=config CombatConfig property is missing;"); return false; }
		ConfigField->SetObjectPropertyValue_InContainer(Guard, ConfigField->GetObjectPropertyValue_InContainer(Reference));
		Guard->FinishSpawning(Transform);
		if (!IsValid(Guard) || !Guard->HasActorBegunPlay()) { Details += TEXT(" stage=finish guard did not begin play;"); return false; }
		// 先完成 Actor 初始化再改复制；本同步探针在下一次网络发送前创建并清理。
		Guard->SetReplicates(false);
		Guard->GetCharacterMovement()->DisableMovement();
		Controller = World->SpawnActor<AAIController>();
		if (!Controller) { Details += TEXT(" stage=controller AI controller spawn failed;"); return false; }
		Controller->SetReplicates(false);
		Controller->Possess(Guard);
		// 专服上的 PlayerController 从不本地控制，LocalPredicted Q 会转发给不存在的客户端。
		// AIController 是正常的本地权威控制器，直接通过同一个 Q 入口结算，不改执行策略或资格标签。
		auto* GuardASC = Guard->GetAbilitySystemComponent();
		const float PreviousEnergy = Guard->GetEnergy();
		Guard->Defend();
		const float SourceAttack = Source->GetAbilitySystemComponent()->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute());
		const float GuardArmor = GuardASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetArmorAttribute());
		const bool bConfigMatches = Guard->GetCombatConfigAsset() == Reference->GetCombatConfigAsset();
		Details += FString::Printf(TEXT(" stage=Q authority=%d AIControlled=%d local=%d ready=%d defending=%d configMatch=%d energy=%.0f->%.0f cost=%.0f cooldown=%.3f attack=%.0f armor=%.0f;"),
			Guard->HasAuthority(), Guard->GetController() == Controller && !Guard->IsPlayerControlled(), Guard->IsLocallyControlled(),
			Guard->IsCombatReady(), Guard->IsDefending(), bConfigMatches, PreviousEnergy, Guard->GetEnergy(),
			Guard->GetCombatConfig().Defend.Cost, Guard->GetDefenseCooldownRemaining(), SourceAttack, GuardArmor);
		if (!Guard->HasAuthority() || Guard->GetController() != Controller || !Guard->IsLocallyControlled()
			|| !Guard->IsCombatReady() || !Guard->IsDefending() || !bConfigMatches
			|| Guard->GetEnergy() != PreviousEnergy - Guard->GetCombatConfig().Defend.Cost
			|| Guard->GetDefenseCooldownRemaining() <= 0.f || SourceAttack != 0.f || GuardArmor != 0.f) return false;
		const float SourceYaw = (Source->GetActorLocation() - Guard->GetActorLocation()).Rotation().Yaw;
		bool bPassed = true;
		for (float FacingOffset : {90.f, 180.f})
		{
			Guard->SetActorRotation(FRotator(0, SourceYaw + FacingOffset, 0));
			const float PreviousHealth = Guard->GetHealth(), PreviousShield = Guard->GetShield();
			const bool bApplied = Source->ApplyCombatDamage(Guard, 20.f);
			const float ExpectedShield = FMath::Max(0.f, PreviousShield - 20.f);
			const float ExpectedHealth = PreviousHealth - FMath::Max(0.f, 20.f - PreviousShield);
			bPassed &= bApplied && Guard->IsDefending() && Guard->GetShield() == ExpectedShield && Guard->GetHealth() == ExpectedHealth;
			Details += FString::Printf(TEXT(" %s HP%.0f->%.0f(expected%.0f) shield%.0f->%.0f(expected%.0f);"),
				FacingOffset == 90.f ? TEXT("side") : TEXT("rear"), PreviousHealth, Guard->GetHealth(), ExpectedHealth,
				PreviousShield, Guard->GetShield(), ExpectedShield);
		}
		return bPassed;
	}

	// 正式配置的持续伤害也属于 R 的结果；原生周期为 1 秒、不在施加当帧跳伤害，包含寿命末尾的整周期跳。
	float ExpectedSettledAreaHealth(const AmultiplayerCombatCharacter* Caster)
	{
		const auto& Rules = Caster->GetCombatConfig();
		const auto* Burn = GetDefault<UmultiplayerBurningEffect>();
		const float Period = Burn->Period.GetValueAtLevel(1.f);
		const int32 Ticks = Rules.BurningDamagePerTick > 0.f && Period > 0.f
			? FMath::FloorToInt(Rules.BurningDuration / Period + KINDA_SMALL_NUMBER)
				+ (Burn->bExecutePeriodicEffectOnApplication ? 1 : 0) : 0;
		return 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Amount - Ticks * Rules.BurningDamagePerTick;
	}

	// 在每次网络发送前重置调度，避免后续 GAS/CMC ForceNetUpdate 穿透一次性的延后。
	bool HoldCombatReplicationForFrame(UWorld* World, AmultiplayerCombatCharacter* Character)
	{
		UNetDriver* NetDriver = World->GetNetDriver();
		const TSharedPtr<FNetworkObjectInfo> Info = NetDriver ? NetDriver->GetNetworkObjectList().Find(Character) : nullptr;
		if (!Info.IsValid()) return false;
		Info->NextUpdateTime = TNumericLimits<double>::Max();
		Info->bPendingNetUpdate = false;
		return true;
	}

	// 后两个拒绝场景共用准备步骤；移除的是服务器旧效果，不伪造客户端预测或回滚。
	void ResetCombatDefense(AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		ASC->RemoveActiveEffectsWithGrantedTags(CombatTransientTags());
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.0f);
		Character->ForceNetUpdate();
	}

	// 高空数值夹具也要有真实地面；两端必须查询同一复制碰撞，不再把悬空落点当可释放目标。
	bool AddCombatTargetingGround(UWorld* World, const AmultiplayerCombatCharacter* Character)
	{
		const float Feet = Character->GetActorLocation().Z - Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		auto* Ground = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, Feet - 10.f), FRotator::ZeroRotator);
		auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Ground || !Cube) return false;
		auto* Mesh = Ground->GetStaticMeshComponent();
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(Cube);
		Mesh->SetCollisionProfileName(TEXT("BlockAll"));
		Mesh->SetCanEverAffectNavigation(false);
		Ground->SetActorScale3D(FVector(100, 100, .2f));
		Ground->SetActorHiddenInGame(true);
		Ground->SetReplicates(true);
		Ground->SetReplicateMovement(true);
		Mesh->SetIsReplicated(true);
		Ground->ForceNetUpdate();
		return true;
	}

	bool CombatPreviewsMatchGeometry(AmultiplayerCombatCharacter* Character, AmultiplayerCombatCharacter* Enemy, FString* Details = nullptr)
	{
		const FRotator Aim = FRotator(0, (Enemy->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw, 0);
		const auto Attack = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
		const auto Area = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Aim);
		const auto Defend = Character->GetSkillTargetPreview(EMultiplayerCombatSkill::Defend, &Aim);
		const bool bAttackTarget = Attack.Target.Get() == Enemy;
		const bool bMuzzle = Attack.Origin.Equals(Character->GetCombatMuzzleLocation(&Aim), .1f);
		const bool bProjectileRadius = FMath::IsNearlyEqual(Attack.Radius, CombatTargeting::ProjectileRadius);
		const bool bAreaRadius = FMath::IsNearlyEqual(Area.Radius, Character->GetCombatConfig().AreaRadius);
		const bool bAreaTarget = Area.AffectedTargets.Contains(TWeakObjectPtr<AmultiplayerCombatCharacter>(Enemy));
		const bool bDefenseDirection = (Defend.End - Defend.Origin).GetSafeNormal2D().Equals(Character->GetActorForwardVector().GetSafeNormal2D(), .01f);
		if (Details)
		{
			FString Targets;
			for (auto Weak : Area.AffectedTargets)
				if (const auto* Target = Weak.Get()) Targets += FString::Printf(TEXT(" %s@%s(team%d ready%d)"),
					*Target->GetName(), *Target->GetActorLocation().ToCompactString(), int32(Target->GetCombatTeam()), Target->IsCombatReady());
			*Details = FString::Printf(TEXT("source=%s@%s enemy=%s@%s aim=%s Attack[valid%d targetMatch%d target=%s muzzle%d radius%d(%.2f) origin=%s end=%s] Area[valid%d radius%d(%.2f) targetMatch%d center=%s normal=%s targets=%s] Defense[valid%d direction%d origin=%s end=%s]"),
				*Character->GetName(), *Character->GetActorLocation().ToCompactString(), *Enemy->GetName(), *Enemy->GetActorLocation().ToCompactString(), *Aim.ToCompactString(),
				Attack.bValid, bAttackTarget, *GetNameSafe(Attack.Target.Get()), bMuzzle, bProjectileRadius, Attack.Radius, *Attack.Origin.ToCompactString(), *Attack.End.ToCompactString(),
				Area.bValid, bAreaRadius, Area.Radius, bAreaTarget, *Area.End.ToCompactString(), *Area.Normal.ToCompactString(), *Targets,
				Defend.bValid, bDefenseDirection, *Defend.Origin.ToCompactString(), *Defend.End.ToCompactString());
		}
		return Attack.bValid && bAttackTarget && bMuzzle && bProjectileRadius
			&& Area.bValid && bAreaRadius && bAreaTarget && Defend.bValid && bDefenseDirection;
	}

	bool CombatPreviewFixtureReady(AmultiplayerCombatCharacter* Local, AmultiplayerCombatCharacter* Enemy,
		bool bPartner, FString& Details)
	{
		const auto CapsuleReady = [](const AmultiplayerCombatCharacter* Character)
		{
			const auto* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
			return Character && Character->HasActorBegunPlay() && Character->IsCombatReady() && Character->GetActorEnableCollision()
				&& Capsule && Capsule->IsRegistered() && Capsule->IsPhysicsStateCreated() && Capsule->IsQueryCollisionEnabled()
				&& Capsule->GetCollisionObjectType() == ECC_Pawn
				&& Capsule->GetComponentLocation().Equals(Character->GetActorLocation(), .1f);
		};
		auto* World = Local->GetWorld();
		AmultiplayerCombatCharacter* Other = nullptr;
		for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
			if (*It != Local && It->GetCombatTeam() == EMultiplayerCombatTeam::Players && It->GetPlayerState()) { Other = *It; break; }
		const FVector ExpectedLocal = bPartner ? FVector(100, 300, 4000) : FVector(0, 0, 4000);
		const FVector ExpectedOther = bPartner ? FVector(0, 0, 4000) : FVector(100, 300, 4000);
		const bool bLocalTransform = Local->GetActorLocation().Equals(ExpectedLocal, .1f);
		const bool bOtherTransform = Other && Other->GetActorLocation().Equals(ExpectedOther, .1f);
		const bool bEnemyTransform = Enemy && Enemy->GetActorLocation().Equals(FVector(500, 0, 4000), .1f);
		const bool bTeams = Local->GetCombatTeam() == EMultiplayerCombatTeam::Players && Other
			&& Other->GetCombatTeam() == EMultiplayerCombatTeam::Players && Enemy && Enemy->GetCombatTeam() == EMultiplayerCombatTeam::Enemies;
		const bool bLocalCapsule = CapsuleReady(Local), bOtherCapsule = CapsuleReady(Other), bEnemyCapsule = CapsuleReady(Enemy);
		const float Feet = 4000.f - Local->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		bool bGround = false;
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			const auto* Mesh = It->GetStaticMeshComponent();
			if (It->GetActorLocation().Equals(FVector(0, 0, Feet - 10.f), .1f)
				&& It->GetActorScale3D().Equals(FVector(100, 100, .2f), .001f)
				&& It->HasActorBegunPlay() && Mesh && Mesh->GetStaticMesh() && Mesh->IsRegistered()
				&& Mesh->IsPhysicsStateCreated() && Mesh->IsQueryCollisionEnabled()) { bGround = true; break; }
		}
		Details = FString::Printf(TEXT("FixtureInputs[localBegun%d otherBegun%d enemyBegun%d transforms=%d/%d/%d teams%d capsules=%d/%d/%d ground%d] local=%s@%s(team%d collision%d object%d) other=%s@%s enemy=%s@%s(team%d collision%d object%d)"),
			Local->HasActorBegunPlay(), Other && Other->HasActorBegunPlay(), Enemy && Enemy->HasActorBegunPlay(),
			bLocalTransform, bOtherTransform, bEnemyTransform, bTeams, bLocalCapsule, bOtherCapsule, bEnemyCapsule, bGround,
			*Local->GetName(), *Local->GetActorLocation().ToCompactString(), int32(Local->GetCombatTeam()), int32(Local->GetCapsuleComponent()->GetCollisionEnabled()), int32(Local->GetCapsuleComponent()->GetCollisionObjectType()),
			*GetNameSafe(Other), Other ? *Other->GetActorLocation().ToCompactString() : TEXT("missing"), *GetNameSafe(Enemy), Enemy ? *Enemy->GetActorLocation().ToCompactString() : TEXT("missing"),
			Enemy ? int32(Enemy->GetCombatTeam()) : -1, Enemy ? int32(Enemy->GetCapsuleComponent()->GetCollisionEnabled()) : -1, Enemy ? int32(Enemy->GetCapsuleComponent()->GetCollisionObjectType()) : -1);
		for (const auto* Character : { Local, Other, Enemy })
			if (Character)
			{
				const auto* Capsule = Character->GetCapsuleComponent();
				Details += FString::Printf(TEXT("; %s[begun%d ready%d actorCollision%d registered%d physics%d query%d object%d capsuleAt=%s]"),
					*Character->GetName(), Character->HasActorBegunPlay(), Character->IsCombatReady(), Character->GetActorEnableCollision(),
					Capsule->IsRegistered(), Capsule->IsPhysicsStateCreated(), Capsule->IsQueryCollisionEnabled(), int32(Capsule->GetCollisionObjectType()), *Capsule->GetComponentLocation().ToCompactString());
			}
		return bLocalTransform && bOtherTransform && bEnemyTransform && bTeams && bLocalCapsule && bOtherCapsule && bEnemyCapsule && bGround;
	}

	float TargetingCooldown(const AmultiplayerCombatCharacter* Character, EMultiplayerCombatSkill Skill)
	{
		switch (Skill)
		{
		case EMultiplayerCombatSkill::Defend: return Character->GetDefenseCooldownRemaining();
		case EMultiplayerCombatSkill::Heal: return Character->GetHealCooldownRemaining();
		case EMultiplayerCombatSkill::Area: return Character->GetAreaCooldownRemaining();
		case EMultiplayerCombatSkill::Dash: return Character->GetDashCooldownRemaining();
		default: return Character->GetAttackCooldownRemaining();
		}
	}
}

// 观察真实属性通知与正式资源；准备阶段不写生命/资源，也不自行播放任何治疗 Cue。
void UCoopNetTestDriver::PrepareCombatSelfHealObservation(UWorld* World, AmultiplayerCombatCharacter* Character)
{
	CombatSelfHealWorld = World;
	CombatSelfHealObserved = Character;
	CombatSelfHealHealthChanges = 0;
	bCombatSelfHealExactHealthChange = false;
	CombatSelfHealHealthChanged = Character->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerCombatAttributes::GetHealthAttribute()).AddWeakLambda(this, [this](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue == Change.OldValue) return;
			++CombatSelfHealHealthChanges;
			bCombatSelfHealExactHealthChange = FMath::IsNearlyEqual(Change.OldValue, 90.f)
				&& FMath::IsNearlyEqual(Change.NewValue, 100.f);
		});
	if (bServer) return;
	const auto& Feedback = Character->GetCombatConfig().HealFeedback;
	if (!Feedback.CascadeEffect.IsNull()) Feedback.CascadeEffect.LoadSynchronous();
	else if (!Feedback.Effect.IsNull()) Feedback.Effect.LoadSynchronous();
	CombatSelfHealEffectsBefore.Reset();
	for (TObjectIterator<USceneComponent> It; It; ++It)
		if (It->GetWorld() == World) CombatSelfHealEffectsBefore.Add(*It);
}

// 成功、失败和退出共用收尾；只撤销夹具自己的输入/监听，并恢复临时挪开的队友。
void UCoopNetTestDriver::ClearCombatSelfHealFixture()
{
	if (CombatSelfHealInputTick.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(CombatSelfHealInputTick);
		CombatSelfHealInputTick.Reset();
		if (auto* World = CombatSelfHealWorld.Get())
			if (auto* PC = World->GetFirstPlayerController())
			{
				if (auto* Character = Cast<AmultiplayerCombatCharacter>(PC->GetPawn())) Character->CancelSkillTargeting();
				PC->InputKey(FInputKeyParams(EKeys::E, IE_Released, 0.));
			}
	}
	if (auto* Character = CombatSelfHealObserved.Get())
		Character->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(CombatSelfHealHealthChanged);
	CombatSelfHealHealthChanged.Reset();
	CombatSelfHealEffectsBefore.Reset();
	CombatSelfHealEffect.Reset();
	CombatSelfHealObserved.Reset();
	CombatSelfHealWorld.Reset();
	if (bServer && bCombatSelfHealPartnerMoved && CombatPartner.IsValid())
	{
		CombatPartner->SetActorLocation(CombatSelfHealPartnerOrigin, false, nullptr, ETeleportType::TeleportPhysics);
		CombatPartner->ForceNetUpdate();
	}
	bCombatSelfHealPartnerMoved = false;
}

// 真实输入按帧推进：截图落盘后先右键取消，再松原按键，避免把预览验收变成额外一次施法。
bool UCoopNetTestDriver::CaptureTargetingInput(UWorld* World, const FString& Name, const FKey& Key,
	EMultiplayerCombatSkill Skill, AmultiplayerCombatCharacter* ExpectedTarget)
{
#if !UE_BUILD_SHIPPING
	if (!bVisualAcceptance) return true;
	const FString Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	const FString Completed = Prefix + Name + TEXT("InputDone");
	if (SentReceipts.Contains(Completed)) return true;
	auto* PC = World->GetFirstPlayerController();
	auto* Character = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
	if (!Character || !Character->IsCombatReady() || !ValidateVisualFixturePawn(Character)) return false;
	auto* ASC = Character->GetAbilitySystemComponent();
	const FString Pressed = TEXT("PreviewPressed:") + Name;
	const FString Observed = TEXT("PreviewObserved:") + Name;
	const FString Captured = TEXT("PreviewCaptured:") + Name;
	const FString Cancelled = TEXT("PreviewCancelPressed:") + Name;
	const FString CancelObserved = TEXT("PreviewCancelObserved:") + Name;
	const FString Released = TEXT("PreviewReleased:") + Name;
	const auto ResourcesUnchanged = [this, Character, ASC, Skill]()
	{
		if (!FMath::IsNearlyEqual(Character->GetEnergy(), CombatPreviewInputEnergy)
			|| !FMath::IsNearlyEqual(Character->GetHealth(), CombatPreviewInputHealth)
			|| TargetingCooldown(Character, Skill) > 0.f || Character->IsCasting()) return false;
		for (const auto& Before : CombatPreviewInputKeys)
		{
			bool bFound = false;
			for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
			{
				if (!Spec.Ability || Spec.Ability->GetClass()->GetPathName() != Before.Key) continue;
				const auto* Instance = Spec.GetPrimaryInstance();
				if (Spec.IsActive() || !Instance
					|| Instance->GetCurrentActivationInfo().GetActivationPredictionKey().Current != Before.Value) return false;
				bFound = true;
				break;
			}
			if (!bFound) return false;
		}
		return true;
	};
	if (!SentReceipts.Contains(Pressed))
	{
		for (const auto Required : { EMultiplayerCombatSkill::Attack, EMultiplayerCombatSkill::Defend,
			EMultiplayerCombatSkill::Heal, EMultiplayerCombatSkill::Area })
		{
			const auto* Spec = ASC->FindAbilitySpecFromClass(Character->GetSkillAbilityClass(Required));
			if (!Spec || !Spec->GetPrimaryInstance()) return false;
		}
		CombatPreviewInputName = Name;
		CombatPreviewInputRotation = PC->GetControlRotation();
		CombatPreviewInputEnergy = Character->GetEnergy();
		CombatPreviewInputHealth = Character->GetHealth();
		CombatPreviewInputKeys.Reset();
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
			if (Spec.Ability && Spec.GetPrimaryInstance())
				CombatPreviewInputKeys.Add(Spec.Ability->GetClass()->GetPathName(),
					Spec.GetPrimaryInstance()->GetCurrentActivationInfo().GetActivationPredictionKey().Current);
		const float PreviewYaw = CombatEnemy.IsValid()
			? (CombatEnemy->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw : PC->GetControlRotation().Yaw;
		PC->SetControlRotation(FRotator(-15.f, PreviewYaw, 0));
		PC->InputKey(FInputKeyParams(Key, IE_Pressed, 1.0));
		SentReceipts.Add(Pressed);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	if (GFrameCounter <= CombatPreviewInputFrame || CombatPreviewInputName != Name) return false;
	if (!SentReceipts.Contains(Observed))
	{
		if (!Character->IsTargetingSkill()) return false;
		const auto Preview = Character->GetSkillTargetPreview(Skill);
		const bool bCorrectTarget = !ExpectedTarget || (Preview.bValid && Preview.Target.Get() == ExpectedTarget);
		const bool bCorrectSkill = Character->GetTargetingSkill() == Skill;
		const bool bDashLearned = Character->GetProgressionComponent()->GetSkillLevel(EMultiplayerCombatSkill::Dash) > 0;
		const bool bCorrectGeometry = Skill != EMultiplayerCombatSkill::Dash ? Preview.bValid
			: bDashLearned ? Preview.bValid && FVector::Dist2D(Preview.Origin, Preview.End) > 5.f : Preview.Radius > 0.f;
		FGameplayTagContainer Reasons;
		const bool bDashAvailable = Skill == EMultiplayerCombatSkill::Dash && Character->CheckSkillAvailable(Skill, &Reasons);
		const bool bDashQualification = Skill != EMultiplayerCombatSkill::Dash || (bDashLearned ? bDashAvailable && Reasons.IsEmpty()
			: !bDashAvailable && Reasons.HasTagExact(CombatTags::Failure_NotLearned) && !Character->GetSkillFailureReason(Skill).IsEmpty());
		SendReceipt(Prefix + Name + TEXT("InputObserved"), bCorrectSkill && bCorrectTarget && bCorrectGeometry && bDashQualification && ResourcesUnchanged(),
			TEXT("Actual owning-controller key press entered the matching HUD preview without cost, cooldown or any ability activation"));
		if (bDone) return false;
		SentReceipts.Add(Observed);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	if (!SentReceipts.Contains(Captured))
	{
		if (!Character->IsTargetingSkill() || Character->GetTargetingSkill() != Skill) return false;
		if (!CaptureSkillView(Name, Character, ExpectedTarget)) return false;
		SentReceipts.Add(Captured);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	if (!SentReceipts.Contains(Cancelled))
	{
		PC->InputKey(FInputKeyParams(EKeys::RightMouseButton, IE_Pressed, 1.0));
		SentReceipts.Add(Cancelled);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	if (!SentReceipts.Contains(CancelObserved))
	{
		if (Character->IsTargetingSkill()) return false;
		SendReceipt(Prefix + Name + TEXT("Cancelled"), ResourcesUnchanged(),
			TEXT("Actual right-mouse input cancelled the held preview on a later frame without starting a GAS action"));
		if (bDone) return false;
		PC->InputKey(FInputKeyParams(EKeys::RightMouseButton, IE_Released, 0.0));
		SentReceipts.Add(CancelObserved);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	if (!SentReceipts.Contains(Released))
	{
		PC->InputKey(FInputKeyParams(Key, IE_Released, 0.0));
		SentReceipts.Add(Released);
		CombatPreviewInputFrame = GFrameCounter;
		return false;
	}
	const bool bConserved = !Character->IsTargetingSkill() && ResourcesUnchanged();
	PC->SetControlRotation(CombatPreviewInputRotation);
	SendReceipt(Completed, bConserved,
		TEXT("Current-token preview PNG saved; right-click cancelled before original key release; health/energy, cooldown and real ability prediction keys remained unchanged"));
	return !bDone;
#else
	return false;
#endif
}

// 夹具只隐藏属性，保持已打开的 Actor/ASC RPC 通道；不靠固定秒数自动解除。
bool UCoopNetTestDriver::HoldCombatStateReplication(UWorld* World, AmultiplayerCombatCharacter* Character)
{
	if (!HoldCombatReplicationForFrame(World, Character)) return false;
	CombatReplicationHolds.Add(Character);
	if (!CombatReplicationHoldTick.IsValid())
	{
		// 此回调位于 Actor/GAS/CMC 更新之后、Net TickFlush 之前，覆盖同帧发出的强制更新。
		CombatReplicationHoldTick = FWorldDelegates::OnWorldPostActorTick.AddWeakLambda(this,
			[this](UWorld* TickWorld, ELevelTick, float)
			{
				for (auto Weak : CombatReplicationHolds)
					if (auto* Character = Weak.Get(); Character && Character->GetWorld() == TickWorld)
						HoldCombatReplicationForFrame(TickWorld, Character);
			});
	}
	return true;
}

// 只有收到真实预测、引擎拒绝和本地撤销回执后才由场景释放，随后正常发送当前状态。
void UCoopNetTestDriver::ReleaseCombatStateReplication(AmultiplayerCombatCharacter* Character)
{
	CombatReplicationHolds.Remove(Character);
	if (IsValid(Character)) Character->ForceNetUpdate();
	if (CombatReplicationHolds.IsEmpty()) ClearCombatStateReplicationHolds();
}

// 进程退出或世界清理时撤销测试委托；弱引用不延长 Pawn、World 或资源的寿命。
void UCoopNetTestDriver::ClearCombatStateReplicationHolds()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(CombatReplicationHoldTick);
	CombatReplicationHoldTick.Reset();
	for (auto Weak : CombatReplicationHolds)
		if (auto* Character = Weak.Get()) Character->ForceNetUpdate();
	CombatReplicationHolds.Reset();
}

// 冻结场景移动以隔离战斗同步；施放仍由客户端经过 GAS 的真实请求通道发起。
void UCoopNetTestDriver::CombatServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (CombatPredictionServerTick(World)) return;
	if (Phase == TEXT("ScenarioStart"))
	{
		APlayerController* PrimaryPC = PrimaryPlayer(World);
		APlayerController* PartnerPC = PartnerPlayer(World);
		CombatPrimary = PrimaryPC ? Cast<AmultiplayerCombatCharacter>(PrimaryPC->GetCharacter()) : nullptr;
		CombatPartner = PartnerPC ? Cast<AmultiplayerCombatCharacter>(PartnerPC->GetCharacter()) : nullptr;
		Assert(TEXT("ServerCombatFixture"), CombatPrimary.IsValid() && CombatPartner.IsValid(),
			TEXT("Both actual player pawns support combat; no replacement player fixture"));
		if (bDone) return;
		Emit(TEXT("FIXTURE"), FString::Printf(TEXT("Accepted player baseline: primaryHP=%.2f partnerHP=%.2f"),
			CombatPrimary->GetHealth(), CombatPartner->GetHealth()));
		Assert(TEXT("ServerCombatPlayerBaseline"), CombatPrimary->GetHealth() == 100.f && CombatPartner->GetHealth() == 100.f
			&& CombatPrimary->GetEnergy() == 100.f && CombatPartner->GetEnergy() == 100.f,
			TEXT("Both actual accepted players enter the isolated fixture at health100/energy100; no resource reset hides background damage"));
		if (bDone) return;
		CombatPrimary->GetCharacterMovement()->DisableMovement();
		CombatPartner->GetCharacterMovement()->DisableMovement();
		CombatPrimary->SetActorLocation(FVector(0, 0, 4000));
		CombatPartner->SetActorLocation(FVector(100, 300, 4000));
		Assert(TEXT("ServerCombatTargetingFloor"), AddCombatTargetingGround(World, CombatPrimary.Get()),
			TEXT("Real replicated collision ground supports identical client previews and authority ground targeting"));
		if (bDone) return;
		if (!PrepareVisualFixtureSurface(World,CombatPrimary.Get())) return;
		PrimaryPC->SetControlRotation(FRotator::ZeroRotator);
		PartnerPC->SetControlRotation(FRotator::ZeroRotator);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(CombatFixtureEnemyClass(),FVector(500, 0, 4000), FRotator::ZeroRotator, Params);
		Assert(TEXT("ServerCombatSpawn"), CombatEnemy.IsValid(), TEXT("Server spawned one replicated target"));
		if (bDone) return;
		// 保留原生敌人的 ASC 与碰撞，只停止 AI，避免其额外攻击改变受测数值。
		if (AAIController* AI = Cast<AAIController>(CombatEnemy->GetController()))
		{
			AI->UnPossess();
			AI->Destroy();
		}
		CombatEnemy->GetCharacterMovement()->DisableMovement();
		// Character 出生时可能先贴地调整高度；冻结后明确恢复本夹具声明的位置。
		CombatEnemy->SetActorLocation(FVector(500, 0, 4000), false, nullptr, ETeleportType::TeleportPhysics);
		CombatEnemy->ForceNetUpdate();
		CombatEnemy->bAlwaysRelevant = true;
		if (!ApplyCombatTestDamage(CombatPrimary.Get(), CombatEnemy.Get(), 20.0f))
		{
			Finish(false, TEXT("Could not initialize damaged target"));
			return;
		}
		Probe->Subject = CombatEnemy.Get();
		PartnerProbe->Subject = CombatEnemy.Get();
		Probe->CombatTarget = CombatEnemy.Get();
		PartnerProbe->CombatTarget = CombatEnemy.Get();
		FString AuthorityPreviewDetails;
		const bool bAuthorityPreview = CombatPreviewsMatchGeometry(CombatPrimary.Get(), CombatEnemy.Get(), &AuthorityPreviewDetails);
		Assert(TEXT("ServerCombatTargetPreview"), bAuthorityPreview, AuthorityPreviewDetails);
		if (bDone) return;
		SetCommand(TEXT("CombatInitial"));
	}

	const auto BothObserved = [this](const TCHAR* Suffix)
	{
		return Receipts.Contains(FString(TEXT("Primary")) + Suffix)
			&& Receipts.Contains(FString(TEXT("Partner")) + Suffix);
	};
	if (Phase == TEXT("CombatHeal") && Receipts.Contains(TEXT("PrimaryHealConfirmed"))
		&& !SentReceipts.Contains(TEXT("ServerHealSnapshot")))
	{
		SentReceipts.Add(TEXT("ServerHealSnapshot"));
		Emit(TEXT("FIXTURE"), FString::Printf(TEXT("After real Heal confirmation: primaryHP=%.2f partnerHP=%.2f energy=%.2f distance=%.2f"),
			CombatPrimary->GetHealth(), CombatPartner->GetHealth(), CombatPrimary->GetEnergy(),
			FVector::Distance(CombatPrimary->GetActorLocation(), CombatPartner->GetActorLocation())));
		// 权威结果独立检查，不等待客户端恰好读到期望值才判断业务是否成功。
		Assert(TEXT("ServerCombatHeal"), FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.f)
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 55.f) && HasReceivedPredictionKey(CombatPrimary.Get(), TEXT("Heal")),
			TEXT("Two immediate heal requests restored teammate 30 health and consumed energy once"));
		if (bDone) return;
	}
	const bool bInitialVisualReady = !bVisualAcceptance || (BothObserved(TEXT("TargetingDefenseInputDone"))
		&& BothObserved(TEXT("TargetingAreaInputDone")) && BothObserved(TEXT("TargetingDashLockedInputDone")));
	if (Phase == TEXT("CombatInitial") && BothObserved(TEXT("CurrentState")) && BothObserved(TEXT("TargetPreview")) && bInitialVisualReady)
	{
		Assert(TEXT("ServerCombatCurrentState"), CombatEnemy.IsValid()
			&& FMath::IsNearlyEqual(CombatEnemy->GetHealth(), 80.0f), TEXT("Pre-existing damage is present on both replicas"));
		if (!bDone) SetCommand(TEXT("CombatAttack"));
	}
	else if (Phase == TEXT("CombatAttack") && BothObserved(TEXT("Attack"))
		&& Receipts.Contains(TEXT("PrimaryAttackConfirmed")))
	{
		Assert(TEXT("ServerCombatAttack"), CombatEnemy.IsValid()
			&& FMath::IsNearlyEqual(CombatEnemy->GetHealth(), 55.0f) && HasReceivedPredictionKey(CombatPrimary.Get(), TEXT("Attack")),
			TEXT("Two predicted requests produced one authoritative 25-damage hit, with a real received prediction key"));
		if (bDone) return;
		Probe->Subject = CombatPrimary.Get();
		PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("CombatDefense"));
	}
	else if (Phase == TEXT("CombatDefense"))
	{
		const bool bDefending = CombatPrimary.IsValid()
			&& CombatPrimary->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending);
		if (bDefending && !SentReceipts.Contains(TEXT("ServerDefenseDamage")))
		{
			SentReceipts.Add(TEXT("ServerDefenseDamage"));
			CombatPrimary->SetActorRotation((CombatEnemy->GetActorLocation() - CombatPrimary->GetActorLocation()).Rotation());
			const float PreviousHealth = CombatPrimary->GetHealth(), PreviousShield = CombatPrimary->GetShield();
			Assert(TEXT("ServerCombatFrontalGuard"), PreviousHealth == 100.f
				&& CombatEnemy->ApplyCombatDamage(CombatPrimary.Get(), 20.f)
				&& CombatPrimary->GetHealth() == PreviousHealth && CombatPrimary->GetShield() == PreviousShield,
				TEXT("The actual admitted player's Q fully blocks a real body-front direct attack without losing health or shield"));
			if (bDone) return;
			FString DirectionDetails;
			const bool bDirectionsCorrect = CheckSideAndRearGuard(CombatEnemy.Get(), CombatPrimary.Get(), DirectionDetails);
			Assert(TEXT("ServerCombatSideRearGuard"), bDirectionsCorrect,
				TEXT("Isolated AI-controlled authority rule probe, not player network side/rear acceptance; real Q and unchanged formal config:") + DirectionDetails);
			if (bDone) return;
			// 格挡本身不能当成受伤基线；以真实无方向伤害耗完当前盾并只扣十血，供后续 E90->100。
			// 此准备点只用无 Cue 的真实 GE helper，不让前置命中特效污染 Q 截图；正面攻击验收保持原路径。
			Assert(TEXT("ServerCombatHealDamageBaseline"), ApplyCombatTestDamage(CombatEnemy.Get(), CombatPrimary.Get(), PreviousShield + 10.f)
				&& CombatPrimary->GetHealth() == 90.f && CombatPrimary->GetShield() == 0.f && CombatPrimary->IsDefending(),
				TEXT("Actual undirected shield+10 damage after the proven frontal block establishes health90/shield0 without rewriting health or config"));
			if (bDone) return;
		}
		if (!BothObserved(TEXT("Defense")) || !Receipts.Contains(TEXT("PartnerDefenseResourcesHidden"))) return;
		if (bVisualAcceptance && !BothObserved(TEXT("DefensePoseActive"))) return;
		if (!Receipts.Contains(TEXT("PrimaryDefensePredicted")) || !Receipts.Contains(TEXT("PrimaryDefenseConfirmed"))) return;
		FGameplayAbilitySpec* DefenseSpec = CombatPrimary->GetAbilitySystemComponent()->FindAbilitySpecFromClass(
			UmultiplayerDefendAbility::StaticClass());
		UGameplayAbility* DefenseInstance = DefenseSpec ? DefenseSpec->GetPrimaryInstance() : nullptr;
		const FPredictionKey ServerKey = DefenseInstance
			? DefenseInstance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		Assert(TEXT("ServerCombatDefense"), CombatPrimary.IsValid()
			&& FMath::IsNearlyEqual(CombatPrimary->GetHealth(), 90.0f)
			&& CombatPrimary->GetShield() == 0.f
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 80.0f) && ServerKey.IsValidKey() && ServerKey.WasReceived(),
			FString::Printf(TEXT("Q consumed energy once; frontal damage preserved health/shield, side/rear stayed normal, then actual undirected shield+10 damage prepared health90; received PredictionKey=%d"), ServerKey.Current));
		if (bDone) return;
		if (!ApplyCombatTestDamage(CombatEnemy.Get(), CombatPartner.Get(), 60.0f))
		{
			Finish(false, TEXT("Could not prepare wounded teammate"));
			return;
		}
		Emit(TEXT("FIXTURE"), FString::Printf(TEXT("Before real Heal: primaryHP=%.2f partnerHP=%.2f distance=%.2f"),
			CombatPrimary->GetHealth(), CombatPartner->GetHealth(), FVector::Distance(CombatPrimary->GetActorLocation(), CombatPartner->GetActorLocation())));
		Assert(TEXT("ServerCombatHealTargetReady"), FMath::IsNearlyEqual(CombatPartner->GetHealth(), 40.f),
			TEXT("Real damage prepared the intended wounded teammate at health40 before requesting Heal"));
		if (bDone) return;
		Probe->Subject = CombatPartner.Get();
		PartnerProbe->Subject = CombatPartner.Get();
		SetCommand(bVisualAcceptance ? TEXT("CombatHealPreview") : TEXT("CombatHeal"));
	}
	else if (Phase == TEXT("CombatHealPreview") && BothObserved(TEXT("TargetingHealInputDone")))
	{
		Assert(TEXT("ServerTargetingHealUncommitted"), CombatPrimary->GetHealth() == 90.f && CombatPartner->GetHealth() == 40.f
			&& CombatPrimary->GetEnergy() == 80.f && CombatPartner->GetEnergy() == 100.f
			&& CombatPrimary->GetHealCooldownRemaining() == 0.f && CombatPartner->GetHealCooldownRemaining() == 0.f,
			TEXT("Both real E-key previews were captured and cancelled; server health, energy and healing cooldown remain at the pre-heal baseline"));
		if (!bDone) SetCommand(TEXT("CombatHeal"));
	}
	else if (Phase == TEXT("CombatHeal") && BothObserved(TEXT("Heal"))
		&& SentReceipts.Contains(TEXT("ServerHealSnapshot")))
	{
		if (bVisualAcceptance)
		{
			// 保留真实的 HP90/E55；只挪开仍受伤的队友，等原治疗冷却自然结束后测试自疗。
			CombatSelfHealPartnerOrigin = CombatPartner->GetActorLocation();
			const float Separation = CombatPrimary->GetSkillRule(EMultiplayerCombatSkill::Heal).Range + 150.f;
			const FVector Away = (CombatSelfHealPartnerOrigin - CombatPrimary->GetActorLocation()).GetSafeNormal2D();
			CombatPartner->SetActorLocation(CombatPrimary->GetActorLocation() + Away * Separation, false, nullptr, ETeleportType::TeleportPhysics);
			CombatPartner->ForceNetUpdate();
			bCombatSelfHealPartnerMoved = true;
			const auto* Spec = CombatPrimary->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
			const auto* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
			CombatSelfHealPreviousKey = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey().Current : 0;
			PrepareCombatSelfHealObservation(World, CombatPrimary.Get());
			Probe->Subject = CombatPrimary.Get();
			PartnerProbe->Subject = CombatPrimary.Get();
			SetCommand(TEXT("CombatSelfHealPrepare"));
			return;
		}
		ResetCombatDefense(CombatPrimary.Get());
		Probe->Subject = CombatPrimary.Get();
		PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("PredictPrepare:AttackCooldown"));
	}
	else if (Phase == TEXT("CombatSelfHealPrepare") && BothObserved(TEXT("SelfHealReady")))
	{
		Assert(TEXT("ServerCombatSelfHealReady"), CombatPrimary->GetHealth() == 90.f && CombatPrimary->GetEnergy() == 55.f
			&& CombatPartner->GetHealth() == 70.f && CombatPrimary->GetHealCooldownRemaining() == 0.f
			&& CombatPrimary->GetHealTarget() == CombatPrimary.Get()
			&& FVector::DistSquared(CombatPrimary->GetActorLocation(), CombatPartner->GetActorLocation())
				> FMath::Square(CombatPrimary->GetSkillRule(EMultiplayerCombatSkill::Heal).Range),
			TEXT("Original teammate-heal health90/energy55 preserved; cooldown expired naturally; injured teammate moved outside actual range so only self is eligible"));
		if (!bDone) SetCommand(TEXT("CombatSelfHeal"));
	}
	else if (Phase == TEXT("CombatSelfHeal"))
	{
		const auto* Spec = CombatPrimary->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
		const auto* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		const auto Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		if (!SentReceipts.Contains(TEXT("ServerCombatSelfHealSettlement")) && Key.WasReceived()
			&& Key.Current != CombatSelfHealPreviousKey)
		{
			// 依据服务器自己的新激活与属性通知立即判断，绝不先等客户端报告期望数值。
			int32 CooldownEffects = 0;
			for (auto Handle : CombatPrimary->GetAbilitySystemComponent()->GetActiveEffects(FGameplayEffectQuery()))
				if (const auto* Effect = CombatPrimary->GetAbilitySystemComponent()->GetActiveGameplayEffect(Handle);
					Effect && Effect->Spec.Def->IsA<UmultiplayerHealCooldownEffect>()) ++CooldownEffects;
			SentReceipts.Add(TEXT("ServerCombatSelfHealSettlement"));
			Assert(TEXT("ServerCombatSelfHealSettlement"), CombatPrimary->GetHealth() == 100.f && CombatPrimary->GetEnergy() == 30.f
				&& CombatPartner->GetHealth() == 70.f && CooldownEffects == 1 && CombatPrimary->GetHealCooldownRemaining() > 0.f
				&& CombatSelfHealHealthChanges == 1 && bCombatSelfHealExactHealthChange,
				FString::Printf(TEXT("Actual owned E activation key%d: HP90->%.0f once(count%d exact%d), energy55->%.0f, one Heal CD=%d remaining%.3f; teammate remains70"),
					Key.Current, CombatPrimary->GetHealth(), CombatSelfHealHealthChanges, bCombatSelfHealExactHealthChange,
					CombatPrimary->GetEnergy(), CooldownEffects, CombatPrimary->GetHealCooldownRemaining()));
			if (bDone) return;
		}
		if (!SentReceipts.Contains(TEXT("ServerCombatSelfHealSettlement")) || !BothObserved(TEXT("SelfHealOutcome"))
			|| !BothObserved(TEXT("SelfHealEffectObserved")) || !Receipts.Contains(TEXT("PrimarySelfHealInputReleased"))
			|| !Receipts.Contains(TEXT("PrimarySelfHealPredicted")) || !Receipts.Contains(TEXT("PrimarySelfHealConfirmed"))
			|| !BothObserved(TEXT("VisualSelfHeal"))) return;
		const FVector Restored = CombatSelfHealPartnerOrigin;
		ClearCombatSelfHealFixture();
		Assert(TEXT("ServerCombatSelfHealPartnerRestored"), CombatPartner->GetActorLocation().Equals(Restored, .1f)
			&& CombatPartner->GetHealth() == 70.f, TEXT("Temporary teammate displacement restored before original prediction/rejection phases; no healing eligibility or health was patched"));
		if (bDone) return;
		ResetCombatDefense(CombatPrimary.Get());
		Probe->Subject = CombatPrimary.Get();
		PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("PredictPrepare:AttackCooldown"));
	}
	else if (Phase == TEXT("CombatResourceReady") && BothObserved(TEXT("ResourceReady")))
	{
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 10.0f);
		Assert(TEXT("ServerCombatResourceFixture"), HoldCombatStateReplication(World, CombatPrimary.Get()),
			TEXT("Server energy is 10; pawn property replication held while Probe delivers the stale-state request"));
		if (!bDone) SetCommand(TEXT("CombatResourceReject"));
	}
	else if (Phase == TEXT("CombatResourceReject"))
	{
		if (Receipts.Contains(TEXT("PrimaryResourceLocalRollback")) && !SentReceipts.Contains(TEXT("ReleaseResourceReplication")))
		{
			SentReceipts.Add(TEXT("ReleaseResourceReplication"));
			ReleaseCombatStateReplication(CombatPrimary.Get());
		}
		if (!BothObserved(TEXT("ResourceRollback")) || !Receipts.Contains(TEXT("PrimaryResourceRejected"))
			|| !Receipts.Contains(TEXT("PartnerResourceRollbackResourcesHidden"))) return;
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		Assert(TEXT("ServerCombatResourceRejected"), FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 10.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend),
			TEXT("Server rejected the truly predicted request; no cost, defense or cooldown was committed"));
		if (bDone) return;
		if (!ApplyCombatTestDamage(CombatPrimary.Get(), CombatEnemy.Get(), 1000.0f))
		{
			Finish(false, TEXT("Could not kill target"));
			return;
		}
		Probe->Subject = CombatEnemy.Get();
		PartnerProbe->Subject = CombatEnemy.Get();
		SetCommand(TEXT("CombatEnemyDeath"));
	}
	else if (Phase == TEXT("CombatEnemyDeath") && BothObserved(TEXT("EnemyDeath")))
	{
		Assert(TEXT("ServerCombatEnemyDeath"), CombatEnemy.IsValid() && !CombatEnemy->IsAlive()
			&& CombatEnemy->GetHealth() == 0.0f && !CombatEnemy->GetActorEnableCollision(),
			TEXT("Enemy death committed once; dead target no longer collides"));
		if (bDone) return;
		ResetCombatDefense(CombatPrimary.Get());
		Probe->Subject = CombatPrimary.Get();
		PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("CombatDeathReady"));
	}
	else if (Phase == TEXT("CombatDeathReady") && BothObserved(TEXT("DeathReady")))
	{
		if (!ApplyCombatTestDamage(CombatPartner.Get(), CombatPrimary.Get(), 1000.0f))
		{
			Finish(false, TEXT("Could not kill primary player"));
			return;
		}
		Assert(TEXT("ServerCombatDeathFixture"), HoldCombatStateReplication(World, CombatPrimary.Get()),
			TEXT("Server pawn is dead; its death replication held while the owning client still sees alive state"));
		if (bDone) return;
		SetCommand(TEXT("CombatPlayerDeath"));
	}
	else if (Phase == TEXT("CombatPlayerDeath"))
	{
		if (Receipts.Contains(TEXT("PrimaryDeathLocalRollback")) && !SentReceipts.Contains(TEXT("ReleaseDeathReplication")))
		{
			SentReceipts.Add(TEXT("ReleaseDeathReplication"));
			ReleaseCombatStateReplication(CombatPrimary.Get());
		}
		if (!BothObserved(TEXT("PlayerDeath")) || !Receipts.Contains(TEXT("PrimaryDeathRejected"))
			|| !Receipts.Contains(TEXT("PrimaryDeathAttackRejected")) || !Receipts.Contains(TEXT("PrimaryDeathHealRejected"))) return;
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		Assert(TEXT("ServerCombatPlayerDeath"), CombatPrimary.IsValid() && !CombatPrimary->IsAlive()
			&& CombatPrimary->GetHealth() == 0.0f && !CombatPrimary->GetActorEnableCollision()
			&& !CombatPrimary->InputEnabled() && PrimaryPlayer(World)->InputEnabled()
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Attack) && !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Heal)
			&& CombatPartner.IsValid() && FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f),
			TEXT("Death rejected stale client defense prediction; energy stayed 100 and defense/cooldown did not survive; pawn input disabled while controller input remains enabled"));
		if (bDone) return;
		CombatPreviousController = PrimaryPlayer(World);
		SetCommand(TEXT("PredictFreshLeave"));
	}
	else if (Phase == TEXT("CombatResourceRoutingPrepare"))
	{
		// 交换拥有者验证另一条连接；非默认上限也能检出只收紧 Energy、漏改 MaxEnergy 的情况。
		UAbilitySystemComponent* PartnerASC = CombatPartner->GetAbilitySystemComponent();
		PartnerASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxEnergyAttribute(), 137.0f);
		PartnerASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 37.0f);
		PartnerASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute(), 173.0f);
		CombatPartner->ForceNetUpdate();
		Assert(TEXT("ServerCombatResourceRoutingFixture"), FMath::IsNearlyEqual(CombatPartner->GetEnergy(), 37.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetMaxEnergy(), 137.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetMaxHealth(), 173.0f),
			TEXT("Partner has private energy37/max137 and public health70/max173 on the server"));
		if (bDone) return;
		Probe->Subject = CombatPartner.Get();
		PartnerProbe->Subject = CombatPartner.Get();
		SetCommand(TEXT("CombatResourceRouting"));
	}
	else if (Phase == TEXT("CombatResourceRouting") && BothObserved(TEXT("PublicMaxHealth"))
		&& Receipts.Contains(TEXT("PartnerOwnedResources")) && Receipts.Contains(TEXT("PrimaryPrivateResourcesHidden")))
	{
		Assert(TEXT("ServerCombatResourceRouting"), CombatPartner.IsValid()
			&& FMath::IsNearlyEqual(CombatPartner->GetEnergy(), 37.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetMaxEnergy(), 137.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f)
			&& FMath::IsNearlyEqual(CombatPartner->GetMaxHealth(), 173.0f),
			TEXT("Owner received private resources; non-owner retained energy100/max100 for one second after receiving public max health173"));
		if (!bDone) SetCommand(TEXT("Complete"));
	}
	else if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))
		&& Receipts.Contains(TEXT("PartnerComplete")))
	{
		Finish(true, TEXT("Combat authority, four-skill local prediction, cooldown/resource/target/death rejection with rollback and legal retries, and owner-only resource routing passed; no visual acceptance"));
	}
#endif
}

// 拒绝夹具只改变服务器：先证明旧客户端真正预测，再等引擎拒绝与本地回滚，最后恢复合法重试。
bool UCoopNetTestDriver::CombatPredictionServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Phase.StartsWith(TEXT("Predict"))) return false;
	if (Phase == TEXT("PredictFreshLeave"))
	{
		AmultiplayerCoopPlayerController* PC = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		if (PC == nullptr || PC == CombatPreviousController.Get() || !PC->HasAcceptedCombatSession() || PC->GetCharacter() == nullptr) return true;
		CombatPrimary = Cast<AmultiplayerCombatCharacter>(PC->GetCharacter());
		Assert(TEXT("ServerCombatFreshSession"), CombatPrimary.IsValid() && CombatPrimary->IsAlive()
			&& CombatPrimary->GetHealth() == 100.0f && CombatPrimary->GetEnergy() == 100.0f,
			TEXT("Intentional exit cleared the ticket; a new owned controller received a fresh living pawn, not a revived old pawn"));
		if (bDone) return true;
		if (Probe.IsValid()) Probe->Destroy();
		FActorSpawnParameters Params; Params.Owner = PC;
		Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
		if (CombatEnemy.IsValid()) CombatEnemy->Destroy();
		Params.Owner = nullptr;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(CombatFixtureEnemyClass(),FVector(500, 0, 4000), FRotator::ZeroRotator, Params);
		if (!Probe.IsValid() || !CombatEnemy.IsValid()) { Finish(false, TEXT("Could not rebuild fresh-session test probes/target")); return true; }
		if (AAIController* AI = Cast<AAIController>(CombatEnemy->GetController())) { AI->UnPossess(); AI->Destroy(); }
		CombatEnemy->GetCharacterMovement()->DisableMovement();
		CombatEnemy->SetActorLocation(FVector(500, 0, 4000), false, nullptr, ETeleportType::TeleportPhysics);
		CombatEnemy->ForceNetUpdate();
		CombatEnemy->bAlwaysRelevant = true;
		Probe->CombatTarget = CombatEnemy.Get(); PartnerProbe->CombatTarget = CombatEnemy.Get();
		SetCommand(TEXT("PredictPrepare:FreshAttack"));
	}

	int32 Index = INDEX_NONE;
	for (int32 Candidate = 0; Candidate < UE_ARRAY_COUNT(PredictionCases); ++Candidate)
		if (Phase.EndsWith(FString(TEXT(":")) + PredictionCases[Candidate].Name)) { Index = Candidate; break; }
	if (Index == INDEX_NONE) return true;
	const FCombatPredictionCase& Case = PredictionCases[Index];
	const FString Name(Case.Name), Skill(Case.Skill), Reason(Case.Rejection);
	UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
	const auto BothObserved = [this, &Name](const TCHAR* Suffix)
		{ return Receipts.Contains(TEXT("Primary") + Name + Suffix) && Receipts.Contains(TEXT("Partner") + Name + Suffix); };
	const auto PrepareRetry = [this, ASC]()
	{
		ReleaseCombatStateReplication(CombatPrimary.Get());
		ReleaseCombatStateReplication(CombatPartner.Get());
		ASC->CancelAllAbilities();
		ASC->RemoveActiveEffectsWithGrantedTags(CombatTransientTags());
		CombatEnemy->GetAbilitySystemComponent()->RemoveActiveEffectsWithGrantedTags(CombatTransientTags());
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.0f);
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 90.0f);
		CombatPartner->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.0f);
		CombatEnemy->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 100.0f);
		CombatEnemy->GetCharacterMovement()->DisableMovement();
		CombatEnemy->SetActorLocation(FVector(500, 0, 4000));
		CombatPrimary->GetCharacterMovement()->DisableMovement();
		CombatPrimary->SetActorLocation(FVector(0, 0, 4000));
		CombatPrimary->GetController()->SetControlRotation(FRotator::ZeroRotator);
		CombatPrimary->ForceNetUpdate(); CombatPartner->ForceNetUpdate(); CombatEnemy->ForceNetUpdate();
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
	};
	if (Phase.StartsWith(TEXT("PredictPrepare:")))
	{
		PrepareRetry();
		SetCommand(TEXT("PredictReady:") + Name);
	}
	else if (Phase.StartsWith(TEXT("PredictReady:")) && BothObserved(TEXT("Ready")))
	{
		if (Reason == TEXT("Fresh")) { SetCommand(TEXT("PredictRetry:") + Name); return true; }
		if (Reason == TEXT("Cooldown"))
		{
			const FCombatPredictionSkill Definition = PredictionSkill(Skill);
			FGameplayEffectSpec Spec(Definition.Cooldown.GetDefaultObject(), ASC->MakeEffectContext(), 1.0f);
			Spec.SetDuration(30.0f, true); // 保持到请求到达；原攻击0.5秒冷却可能早于Harsh往返到期。
			ASC->ApplyGameplayEffectSpecToSelf(Spec);
		}
		else if (Reason == TEXT("Energy")) ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 10.0f);
		else if (Reason == TEXT("NoTarget"))
		{
			ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 100.0f);
			CombatPartner->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 100.0f);
		}
		Assert(TEXT("Server") + Name + TEXT("Fixture"), HoldCombatStateReplication(World, CombatPrimary.Get())
			&& HoldCombatStateReplication(World, CombatPartner.Get()),
			TEXT("Server-only cooldown/resource/target change is hidden from the stale client; real GAS RPCs remain enabled"));
		if (!bDone) SetCommand(TEXT("PredictAttempt:") + Name);
	}
	else if (Phase.StartsWith(TEXT("PredictAttempt:")))
	{
		if (!Receipts.Contains(TEXT("Primary") + Name + TEXT("Predicted"))
			|| !Receipts.Contains(TEXT("Primary") + Name + TEXT("Rejected"))
			|| !Receipts.Contains(TEXT("Primary") + Name + TEXT("Rollback"))) return true;
		if (Reason == TEXT("MissingAim"))
		{
			if (!SentReceipts.Contains(TEXT("MissingAimSettling")))
			{
				SentReceipts.Add(TEXT("MissingAimSettling"));
				CombatLifecycleAt = FPlatformTime::Seconds();
			}
			// 已收到真正拒绝后再跨过原出手与收招期限，避免只证明当帧没发弹。
			const auto& Attack = CombatPrimary->GetCombatConfig().Attack;
			if (FPlatformTime::Seconds() - CombatLifecycleAt < Attack.Windup + Attack.Recovery + .5f) return true;
			bool HasProjectile = false;
			for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It)
				HasProjectile |= It->GetOwner() == CombatPrimary.Get();
			Assert(TEXT("ServerAttackMissingAimNoLateProjectile"), !HasProjectile && CombatEnemy->GetHealth() == 100,
				TEXT("Missing activation aim was rejected; original attack release/recovery deadlines passed without any authoritative projectile or damage"));
			if (bDone) return true;
		}
		const bool bUnchanged = CombatPrimary->GetEnergy() == (Reason == TEXT("Energy") ? 10.0f : 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& (Reason != TEXT("MissingAim") || (!ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Attack)
				&& CombatEnemy->GetHealth() == 100.0f))
			&& (Skill != TEXT("Area") || (!CombatPrimary->IsCasting() && CombatEnemy->GetHealth() == 100.0f))
			&& (Reason != TEXT("Cooldown") || ASC->HasMatchingGameplayTag(PredictionSkill(Skill).CooldownTag))
			&& (Reason != TEXT("NoTarget") || (CombatPrimary->GetHealth() == 100.0f && CombatPartner->GetHealth() == 100.0f));
		Assert(TEXT("Server") + Name + TEXT("Rejected"), bUnchanged,
			TEXT("Real prediction key was rejected; no second server cost/effect/health settlement; client rolled back before authoritative fixture release"));
		if (bDone) return true;
		PrepareRetry();
		SetCommand(TEXT("PredictRetryReady:") + Name);
	}
	else if (Phase.StartsWith(TEXT("PredictRetryReady:")) && BothObserved(TEXT("RetryReady")))
		SetCommand(TEXT("PredictRetry:") + Name);
	else if (Phase.StartsWith(TEXT("PredictRetry:")))
	{
		if (Skill == TEXT("Defense") && ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !SentReceipts.Contains(TEXT("Damage:") + Name))
		{
			SentReceipts.Add(TEXT("Damage:") + Name);
			CombatPrimary->SetActorRotation((CombatEnemy->GetActorLocation() - CombatPrimary->GetActorLocation()).Rotation());
			const float PreviousHealth = CombatPrimary->GetHealth(), PreviousShield = CombatPrimary->GetShield();
			Assert(TEXT("Server") + Name + TEXT("FrontalGuard"), PreviousHealth == 90.f
				&& CombatEnemy->ApplyCombatDamage(CombatPrimary.Get(), 20.f)
				&& CombatPrimary->GetHealth() == PreviousHealth && CombatPrimary->GetShield() == PreviousShield,
				TEXT("Accepted predicted Q retry blocks the actual body-front direct attack without consuming health or shield"));
			if (bDone) return true;
			// 重试的回血基线同样只结算真实无方向 GE，不额外播放 Impact Cue 遮住防御姿态。
			Assert(TEXT("Server") + Name + TEXT("UndirectedDamage"), ApplyCombatTestDamage(CombatEnemy.Get(), CombatPrimary.Get(), PreviousShield + 10.f)
				&& CombatPrimary->GetHealth() == 80.f && CombatPrimary->GetShield() == 0.f,
				TEXT("Only the subsequent real undirected shield+10 damage changes the strict retry result from health90 to80"));
			if (bDone) return true;
		}
		if (Skill == TEXT("Area"))
		{
			// 先看到本次真实权威接纳与扣费，不能在客户端尚未发起 R 的阶段首帧误判已结算。
			if (!HasReceivedPredictionKey(CombatPrimary.Get(), Skill)
				|| CombatPrimary->GetEnergy() != 100.f - PredictionSkill(Skill).EnergyCost
				|| CombatPrimary->IsCasting() || CombatEnemy->IsBurning()) return true;
			const FString Settled = TEXT("AreaSettled:") + Name;
			if (!SentReceipts.Contains(Settled))
			{
				SentReceipts.Add(Settled);
				const auto* Burn = GetDefault<UmultiplayerBurningEffect>();
				Assert(TEXT("Server") + Name + TEXT("PeriodicContract"), Burn->Period.GetValueAtLevel(1.f) == 1.f
					&& !Burn->bExecutePeriodicEffectOnApplication
					&& ASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()) == 0.f
					&& CombatEnemy->GetAbilitySystemComponent()->GetNumericAttribute(UmultiplayerCombatAttributes::GetArmorAttribute()) == 0.f,
					TEXT("The actual fixture retains native one-second/no-application burn ticks and zero attack/armor bonuses; formal config is unchanged"));
				if (bDone) return true;
				Assert(TEXT("Server") + Name + TEXT("SettledAreaDamage"), CombatEnemy->GetHealth() == ExpectedSettledAreaHealth(CombatPrimary.Get()),
					TEXT("Recovery and real burn expiration completed; health equals one area hit plus every full native periodic tick, with no health rewrite"));
				if (bDone) return true;
			}
		}
		if (!BothObserved(TEXT("RetryOutcome")) || !Receipts.Contains(TEXT("Primary") + Name + TEXT("RetryConfirmed"))) return true;
		const float ExpectedEnergy = 100.0f - PredictionSkill(Skill).EnergyCost;
		Assert(TEXT("Server") + Name + TEXT("Retry"), HasReceivedPredictionKey(CombatPrimary.Get(), Skill)
			&& CombatPrimary->GetEnergy() == ExpectedEnergy
			&& (Skill != TEXT("Attack") || CombatEnemy->GetHealth() == 75.0f)
			&& (Skill != TEXT("Area") || (!CombatPrimary->IsCasting() && !CombatEnemy->IsBurning()
				&& CombatEnemy->GetHealth() == ExpectedSettledAreaHealth(CombatPrimary.Get())))
			&& (Skill != TEXT("Defense") || (CombatPrimary->GetHealth() == 80.0f && CombatPrimary->GetShield() == 0.f))
			&& (Skill != TEXT("Heal") || CombatPartner->GetHealth() == 70.0f),
			TEXT("A fresh predicted double-click retry was accepted once by the server; previous rejection did not leave a stuck cost/cooldown"));
		if (bDone) return true;
		if (Name == TEXT("HealNoTarget"))
		{
			ResetCombatDefense(CombatPrimary.Get());
			SetCommand(TEXT("CombatResourceReady"));
		}
		else if (Index + 1 == UE_ARRAY_COUNT(PredictionCases)) SetCommand(TEXT("CombatResourceRoutingPrepare"));
		else SetCommand(FString(TEXT("PredictPrepare:")) + PredictionCases[Index + 1].Name);
	}
#endif
	return true;
}

// 两个客户端只观察自己的副本；服务器夹具尚未释放时先核对真正的预测撤销。
bool UCoopNetTestDriver::CombatPredictionClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Phase.StartsWith(TEXT("Predict"))) return false;
	if (Phase == TEXT("PredictFreshLeave"))
	{
		if (!bPartner && !SentReceipts.Contains(TEXT("FreshTravelRequested")))
		{
			SentReceipts.Add(TEXT("FreshTravelRequested"));
			SendReceipt(TEXT("PrimaryFreshIntentionalExit"), true,
				TEXT("LeaveGame clears the endpoint ticket; subsequent retry must use a new authenticated controller"));
			GameInstance->LeaveGame();
		}
		return true;
	}
	const FCombatPredictionCase* Case = nullptr;
	for (const FCombatPredictionCase& Candidate : PredictionCases)
		if (Phase.EndsWith(FString(TEXT(":")) + Candidate.Name)) { Case = &Candidate; break; }
	if (Case == nullptr) return true;
	const FString Name(Case->Name), Skill(Case->Skill), Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	AmultiplayerCombatCharacter* Primary = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	AmultiplayerCombatCharacter* Local = Cast<AmultiplayerCombatCharacter>(World->GetFirstPlayerController()->GetCharacter());
	AmultiplayerCombatCharacter* Partner = bPartner ? Local : nullptr;
	if (!Partner) for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
		if (*It != Primary && It->GetCombatTeam() == EMultiplayerCombatTeam::Players) { Partner = *It; break; }
	if (!Primary || !Local || !Partner || !CombatEnemy.IsValid()) return true;
	UAbilitySystemComponent* ASC = Local->GetAbilitySystemComponent();
	const auto* Controller = Cast<AmultiplayerCoopPlayerController>(World->GetFirstPlayerController());
	const bool bReady = Primary->IsCombatReady() && Primary->GetHealth() == 90.0f && Partner->GetHealth() == 40.0f
		&& CombatEnemy->GetHealth() == 100.0f && !Primary->GetAbilitySystemComponent()->HasAnyMatchingGameplayTags(CombatTransientTags())
		&& Controller && Controller->HasAcceptedCombatSession() && Local->IsCombatReady()
		&& GameInstance->GetConnectionState() == EMultiplayerConnectionState::Connected
		&& (bPartner || (Local == Primary && Local->GetEnergy() == 100.0f
			&& ASC->FindAbilitySpecFromClass(PredictionSkill(Skill).Ability) != nullptr));
	if ((Phase.StartsWith(TEXT("PredictReady:")) || Phase.StartsWith(TEXT("PredictRetryReady:"))) && bReady)
		SendReceipt(Prefix + Name + (Phase.StartsWith(TEXT("PredictReady:")) ? TEXT("Ready") : TEXT("RetryReady")), true,
			TEXT("Actual client replicas are alive, reset, ready and have no previous cooldown; owner verifies energy100 and skill grant"));
	const bool bRetry = Phase.StartsWith(TEXT("PredictRetry:"));
	const FString KeyCase = bRetry ? Name + TEXT("Retry") : Name;
	if (!bPartner && (bRetry || Phase.StartsWith(TEXT("PredictAttempt:")))
		&& !SentReceipts.Contains(TEXT("Command:") + Phase) && ASC->FindAbilitySpecFromClass(PredictionSkill(Skill).Ability))
	{
		SentReceipts.Add(TEXT("Command:") + Phase);
		World->GetFirstPlayerController()->SetControlRotation(FRotator::ZeroRotator);
		PredictCombatSkill(Local, Skill, KeyCase, 100.0f, !bRetry);
	}
	if (bDone) return true;
	if (!bPartner && Phase.StartsWith(TEXT("PredictAttempt:"))
		&& SentReceipts.Contains(TEXT("Primary") + Name + TEXT("Rejected")) && Local->GetEnergy() == 100.0f
		&& !ASC->HasAnyMatchingGameplayTags(CombatTransientTags()))
	{
		const int32 Key = CombatPredictionKeys.FindRef(Name);
		bool bPredictedEffectSurvived = false;
		for (const FActiveGameplayEffectHandle& Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
		{
			const FActiveGameplayEffect* Effect = ASC->GetActiveGameplayEffect(Handle);
			bPredictedEffectSurvived |= Effect != nullptr && Effect->PredictionKey.Current == Key;
		}
		const bool bMissingAim = FString(Case->Rejection) == TEXT("MissingAim");
		UAnimInstance* Anim = Local->GetMesh()->GetAnimInstance();
		if (bVisualAcceptance && Name == TEXT("DefenseCooldown") && !bPredictedEffectSurvived)
		{
			// 拒绝收包可能早于本帧动画更新；固定等下一次测试 Tick，不轮询姿态直到成功。
			const FString Settled = TEXT("VisualPoseRollbackTick:") + Name;
			if (!SentReceipts.Contains(Settled)) { SentReceipts.Add(Settled); return true; }
			const auto* CombatAnim = Cast<UmultiplayerCombatAnimInstance>(Anim);
			SendReceipt(TEXT("PrimaryDefenseCooldownPoseRollback"), CombatAnim && !CombatAnim->CombatIsDefending && !Local->IsDefending(),
				TEXT("Actual owning animation update exited predicted guard after engine rejection; no manual animation update or fake tag removal"));
			if (bDone) return true;
		}
		if (!bPredictedEffectSurvived && (!bMissingAim || !Anim
			|| !Anim->Montage_IsPlaying(Local->GetActionMontage(EMultiplayerCombatAnimation::Attack))))
			SendReceipt(TEXT("Primary") + Name + TEXT("Rollback"), true,
				TEXT("Engine rejection removed that prediction key's effects and restored local energy100 before server fixture replication was released; rejected attack montage is stopped"));
	}
	if (bRetry)
	{
		const float ExpectedEnergy = 100.0f - PredictionSkill(Skill).EnergyCost;
		if (!bPartner && SentReceipts.Contains(TEXT("PredictionCaughtUp:") + KeyCase) && Local->GetEnergy() == ExpectedEnergy)
			SendReceipt(TEXT("Primary") + KeyCase + TEXT("Confirmed"), true,
				TEXT("Retry key caught up and authoritative energy matches a single consumption"));
		const bool bOutcome = (Skill == TEXT("Attack") && CombatEnemy->GetHealth() == 75.0f)
			|| (Skill == TEXT("Area") && !Primary->IsCasting() && !CombatEnemy->IsBurning()
				&& CombatEnemy->GetHealth() == ExpectedSettledAreaHealth(Primary))
			|| (Skill == TEXT("Defense") && Primary->GetHealth() == 80.0f && Primary->GetShield() == 0.f && Primary->IsDefending())
			|| (Skill == TEXT("Heal") && Partner->GetHealth() == 70.0f);
		if (bOutcome && (bPartner || Local->GetEnergy() == ExpectedEnergy))
			SendReceipt(Prefix + Name + TEXT("RetryOutcome"), true,
				TEXT("Observed the exact authoritative skill result on this independent connection; Area completed recovery and all configured burn ticks before final health was accepted"));
	}
#endif
	return true;
}

// 观察同步调用栈内的真实预测GE；不在客户端改属性、不直接移除效果来伪造回滚。
void UCoopNetTestDriver::PredictCombatSkill(AmultiplayerCombatCharacter* Character, const FString& Skill,
	const FString& CaseName, float EnergyBefore, bool bExpectRejection)
{
#if !UE_BUILD_SHIPPING
	UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
	const FCombatPredictionSkill Definition = PredictionSkill(Skill);
	TMap<TWeakObjectPtr<AmultiplayerCombatCharacter>, float> HealthBefore;
	int32 ProjectilesBefore = 0;
	for (TActorIterator<AmultiplayerCombatCharacter> It(Character->GetWorld()); It; ++It) HealthBefore.Add(*It, It->GetHealth());
	for (TActorIterator<AmultiplayerCombatProjectile> It(Character->GetWorld()); It; ++It) ++ProjectilesBefore;
	const auto Invoke = [Character, ASC, &Skill, &CaseName]()
		{ if (CaseName == TEXT("AttackMissingAim")) ASC->TryActivateAbilityByClass(UmultiplayerAttackAbility::StaticClass());
			else if (Skill == TEXT("Attack")) Character->Attack(); else if (Skill == TEXT("Defense")) Character->Defend();
			else if (Skill == TEXT("Area")) Character->CastArea(); else Character->Heal(); };
	const bool bStaleReady = Character->IsCombatReady() && FMath::IsNearlyEqual(Character->GetEnergy(), EnergyBefore)
		&& !ASC->HasMatchingGameplayTag(Definition.CooldownTag);
	Invoke();
	FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(Definition.Ability);
	UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
	FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
	bool bCooldownHasKey = false, bCostHasKey = Definition.Cost == nullptr, bDefenseHasKey = Skill != TEXT("Defense"),
		bCastingHasKey = Skill != TEXT("Area");
	for (const FActiveGameplayEffectHandle& Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
	{
		const FActiveGameplayEffect* Effect = ASC->GetActiveGameplayEffect(Handle);
		if (Effect == nullptr || Effect->PredictionKey != Key) continue;
		bCooldownHasKey |= Effect->Spec.Def->IsA(Definition.Cooldown);
		bCostHasKey |= Definition.Cost != nullptr && Effect->Spec.Def->IsA(Definition.Cost);
		bDefenseHasKey |= Effect->Spec.Def->IsA(UmultiplayerDefendingEffect::StaticClass());
		bCastingHasKey |= Effect->Spec.Def->IsA(UmultiplayerCastingEffect::StaticClass());
	}
	bool bNoClientSettlement = true;
	for (const auto& Previous : HealthBefore) bNoClientSettlement &= Previous.Key.IsValid() && Previous.Key->GetHealth() == Previous.Value;
	int32 ProjectilesAfter = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Character->GetWorld()); It; ++It) ++ProjectilesAfter;
	bNoClientSettlement &= ProjectilesAfter == ProjectilesBefore;
	const float ExpectedEnergy = EnergyBefore - Definition.EnergyCost;
	const bool bPredicted = bStaleReady && Key.IsLocalClientKey() && Key.WasLocallyGenerated()
		&& bCooldownHasKey && bCostHasKey && bDefenseHasKey && bCastingHasKey && bNoClientSettlement
		&& ASC->HasMatchingGameplayTag(Definition.CooldownTag) && FMath::IsNearlyEqual(Character->GetEnergy(), ExpectedEnergy);
	CombatPredictionKeys.Add(CaseName, Key.Current);
	if (Key.IsLocalClientKey())
	{
		Key.NewRejectedDelegate().BindWeakLambda(this, [this, CaseName, bExpectRejection, KeyValue = Key.Current]
		{
			SendReceipt(TEXT("Primary") + CaseName + TEXT("Rejected"), bExpectRejection,
				FString::Printf(TEXT("Engine rejected the locally observed PredictionKey=%d"), KeyValue));
		});
		Key.NewCaughtUpDelegate().BindWeakLambda(this, [this, CaseName]
			{ SentReceipts.Add(TEXT("PredictionCaughtUp:") + CaseName); });
	}
	SendReceipt(TEXT("Primary") + CaseName + TEXT("Predicted"), bPredicted,
		FString::Printf(TEXT("Before %s returned: ready=%d local=%d policy=%d key=%d energy=%.1f, matching predicted cost/CD GE=%d/%d; no local projectile or health settlement=%d"),
			*Skill, bStaleReady, Character->IsLocallyControlled(), Spec ? static_cast<int32>(Spec->Ability->GetNetExecutionPolicy()) : -1,
			Key.Current, Character->GetEnergy(), bCooldownHasKey, bCostHasKey, bNoClientSettlement));
	if (!bDone)
	{
		// 同帧再次按下也必须被预测冷却/状态拦截，不额外扣费或创建第二次预测。
		Invoke();
		SendReceipt(TEXT("Primary") + CaseName + TEXT("RepeatedInput"), FMath::IsNearlyEqual(Character->GetEnergy(), ExpectedEnergy)
			&& Instance->GetCurrentActivationInfo().GetActivationPredictionKey() == Key,
			TEXT("Immediate repeated input was blocked without a second resource charge or prediction key"));
	}
#endif
}

// 每个进程独立读取复制结果；只有 Primary 发起技能，Partner 验证另一个客户端的观察。
void UCoopNetTestDriver::CombatClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	APlayerController* PC = World->GetFirstPlayerController();
	AmultiplayerCombatCharacter* LocalCharacter = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetCharacter()) : nullptr;
	AmultiplayerCombatCharacter* Subject = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	if (LocalCharacter == nullptr) return;
	LocalCharacter->GetCharacterMovement()->DisableMovement();
	const FString Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	const FString CommandMarker = TEXT("Command:") + Phase;
	UAbilitySystemComponent* ASC = LocalCharacter->GetAbilitySystemComponent();
	if (CombatSelfHealObserved.IsValid() && Phase != TEXT("CombatSelfHealPrepare") && Phase != TEXT("CombatSelfHeal"))
		ClearCombatSelfHealFixture();
	// Probe 和技能列表可能先于接纳回执或角色就绪属性到达，初始握手必须等本地真实可施法。
	if (Phase == TEXT("CombatInitial"))
	{
		const auto* Controller = Cast<AmultiplayerCoopPlayerController>(PC);
		if (!Controller || !Controller->HasAcceptedCombatSession() || !LocalCharacter->IsCombatReady()
			|| !LocalCharacter->IsLocallyControlled() || GameInstance->GetConnectionState() != EMultiplayerConnectionState::Connected)
			return;
		if (!bPartner)
			for (const FString& Skill : { FString(TEXT("Attack")), FString(TEXT("Defense")), FString(TEXT("Heal")) })
			{
				const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(PredictionSkill(Skill).Ability);
				if (!Spec || !Spec->GetPrimaryInstance()) return;
			}
	}
	CombatEnemy = Cast<AmultiplayerCombatEnemy>(Probe->CombatTarget.Get());
	if (Phase == TEXT("CombatInitial") && CombatEnemy.IsValid() && FMath::IsNearlyEqual(CombatEnemy->GetHealth(), 80.f)
		&& !SentReceipts.Contains(Prefix + TEXT("TargetPreview")))
	{
		FString FixtureDetails;
		if (!CombatPreviewFixtureReady(LocalCharacter, CombatEnemy.Get(), bPartner, FixtureDetails))
		{
			FWorldDelegates::OnWorldPostActorTick.Remove(CombatPreviewFixtureTick);
			CombatPreviewFixtureTick.Reset();
			CombatPreviewFixtureWorldTicks = 0;
			if (!SentReceipts.Contains(TEXT("PreviewFixtureInputsPending")))
			{
				Emit(TEXT("FIXTURE_PENDING"), FixtureDetails);
				SentReceipts.Add(TEXT("PreviewFixtureInputsPending"));
			}
			return;
		}
		// 只门控独立的复制输入与真实世界帧，不轮询“预览是否通过”来掩盖算法错误。
		if (!CombatPreviewFixtureTick.IsValid())
		{
			FString BeforeTickGeometry;
			CombatPreviewsMatchGeometry(LocalCharacter, CombatEnemy.Get(), &BeforeTickGeometry);
			Emit(TEXT("FIXTURE_BEFORE_WORLD_TICKS"), FixtureDetails + TEXT("; ") + BeforeTickGeometry);
			CombatPreviewFixtureWorldTicks = 0;
			CombatPreviewFixtureTick = FWorldDelegates::OnWorldPostActorTick.AddWeakLambda(this,
				[this, PendingWorld = TWeakObjectPtr<UWorld>(World)](UWorld* TickWorld, ELevelTick, float)
				{
					if (TickWorld == PendingWorld.Get()) CombatPreviewFixtureWorldTicks = FMath::Min(2, CombatPreviewFixtureWorldTicks + 1);
				});
			Emit(TEXT("FIXTURE_INPUTS_READY"), FixtureDetails);
			return;
		}
		if (CombatPreviewFixtureWorldTicks < 2) return;
		FWorldDelegates::OnWorldPostActorTick.Remove(CombatPreviewFixtureTick);
		CombatPreviewFixtureTick.Reset();
		FString GeometryDetails;
		const bool bGeometry = CombatPreviewsMatchGeometry(LocalCharacter, CombatEnemy.Get(), &GeometryDetails);
		SendReceipt(Prefix + TEXT("TargetPreview"), bGeometry,
			FixtureDetails + TEXT("; WorldPostActorTicks=2; ") + GeometryDetails);
	}
	if (bVisualAcceptance && Phase == TEXT("CombatInitial"))
	{
		if (!SentReceipts.Contains(Prefix + TEXT("TargetPreview"))) return;
		if (!CaptureTargetingInput(World, TEXT("TargetingDefense"), EKeys::Q, EMultiplayerCombatSkill::Defend)) return;
		if (!CaptureTargetingInput(World, TEXT("TargetingArea"), EKeys::R, EMultiplayerCombatSkill::Area)) return;
		if (!CaptureTargetingInput(World, TEXT("TargetingDashLocked"), EKeys::LeftShift, EMultiplayerCombatSkill::Dash)) return;
	}
	if (bVisualAcceptance && Phase == TEXT("CombatHealPreview"))
	{
		if (!Subject || !FMath::IsNearlyEqual(Subject->GetHealth(), 40.f)) return;
		const auto Heal = LocalCharacter->GetSkillTargetPreview(EMultiplayerCombatSkill::Heal);
		if (!Heal.bValid || Heal.Target.Get() != Subject) return;
		CaptureTargetingInput(World, TEXT("TargetingHeal"), EKeys::E, EMultiplayerCombatSkill::Heal, Subject);
		return;
	}
	if (Phase == TEXT("CombatHeal") && Subject && Subject->GetHealth() < 90.f
		&& !SentReceipts.Contains(Prefix + TEXT("HealPreview")))
	{
		const auto Heal = LocalCharacter->GetSkillTargetPreview(EMultiplayerCombatSkill::Heal);
		if (Heal.bValid && Heal.Target.Get() == Subject)
			SendReceipt(Prefix + TEXT("HealPreview"), LocalCharacter->GetHealTarget() == Subject,
				TEXT("This connection highlights the actual wounded teammate selected by the authoritative healing rules, not a separate HUD selection"));
	}
	// 所有阶段持续检查，避免在阶段回执之后才到达的远端资源泄漏被忽略。
	for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
	{
		if (*It == LocalCharacter || It->GetCombatTeam() != EMultiplayerCombatTeam::Players) continue;
		if (!FMath::IsNearlyEqual(It->GetEnergy(), 100.0f) || !FMath::IsNearlyEqual(It->GetMaxEnergy(), 100.0f))
		{
			SendReceipt(Prefix + TEXT("PrivateResourceLeak"), false,
				FString::Printf(TEXT("Non-owned pawn %s exposed energy%.1f/max%.1f; both must retain local defaults100"),
					*It->GetName(), It->GetEnergy(), It->GetMaxEnergy()));
			return;
		}
	}
	if (bVisualAcceptance && (Phase == TEXT("CombatSelfHealPrepare") || Phase == TEXT("CombatSelfHeal")))
	{
		if (!Subject || Subject->GetCombatTeam() != EMultiplayerCombatTeam::Players || !Subject->IsCombatReady()) return;
		AmultiplayerCombatCharacter* Teammate = bPartner ? LocalCharacter : nullptr;
		if (!Teammate)
			for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
				if (*It != Subject && It->GetCombatTeam() == EMultiplayerCombatTeam::Players && It->GetPlayerState()) { Teammate = *It; break; }
		if (!Teammate) return;
		auto* SourceASC = Subject->GetAbilitySystemComponent();
		if (Phase == TEXT("CombatSelfHealPrepare"))
		{
			if (SentReceipts.Contains(Prefix + TEXT("SelfHealReady"))) return;
			const bool Ready = Subject->GetHealth() == 90.f && Teammate->GetHealth() == 70.f
				&& !SourceASC->HasMatchingGameplayTag(CombatTags::Cooldown_Heal)
				&& Subject->GetHealTarget() == Subject && FVector::DistSquared(Subject->GetActorLocation(), Teammate->GetActorLocation())
					> FMath::Square(Subject->GetSkillRule(EMultiplayerCombatSkill::Heal).Range)
				&& (bPartner || (Subject == LocalCharacter && LocalCharacter->GetEnergy() == 55.f
					&& ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass())
					&& LocalCharacter->GetProgressionComponent()->GetSlotSkill(2) == EMultiplayerCombatSkill::Heal));
			if (!Ready) return;
			PrepareCombatSelfHealObservation(World, Subject);
			const auto& Feedback = Subject->GetCombatConfig().HealFeedback;
			SendReceipt(Prefix + TEXT("SelfHealReady"), !Feedback.CascadeEffect.IsNull() ? Feedback.CascadeEffect.IsValid() : Feedback.Effect.IsValid(),
				TEXT("Actual replicas preserve health90/teammate70; self is the sole eligible heal target after natural cooldown expiry; owner retains energy55; formal Heal asset preloaded and old particle components snapshotted"));
			return;
		}
		if (!bPartner && !SentReceipts.Contains(CommandMarker))
		{
			SentReceipts.Add(CommandMarker);
			CombatSelfHealInputStep = 0;
			CombatSelfHealInputTick = FWorldDelegates::OnWorldPostActorTick.AddWeakLambda(this,
				[this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakCaster = TWeakObjectPtr<AmultiplayerCombatCharacter>(LocalCharacter)]
				(UWorld* TickWorld, ELevelTick, float)
				{
					if (TickWorld != WeakWorld.Get() || bDone) return;
					auto* Caster = WeakCaster.Get();
					auto* Controller = Caster ? Cast<APlayerController>(Caster->GetController()) : nullptr;
					if (!Caster || !Controller) { Assert(TEXT("PrimarySelfHealInputLifetime"), false, TEXT("Self-heal pawn/controller disappeared during actual E input")); return; }
					auto* CasterASC = Caster->GetAbilitySystemComponent();
					if (CombatSelfHealInputStep == 0)
					{
						const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Heal);
						SendReceipt(TEXT("PrimarySelfHealInputPreview"), Controller->IsInputKeyDown(EKeys::E)
							&& Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Heal
							&& Preview.bValid && Preview.Target.Get() == Caster && Caster->GetHealth() == 90.f
							&& Caster->GetEnergy() == 55.f && Caster->GetHealCooldownRemaining() == 0.f,
							TEXT("Actual E-down entered self-targeted healing preview at unchanged health90/energy55, before any commitment"));
						if (bDone) return;
						Controller->InputKey(FInputKeyParams(EKeys::E, IE_Released, 0.));
						CombatSelfHealInputStep = 1;
						return;
					}
					const auto* Spec = CasterASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
					const auto* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
					FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
					bool KeyedCost = false, KeyedCooldown = false;
					for (auto Handle : CasterASC->GetActiveEffects(FGameplayEffectQuery()))
					{
						const auto* Effect = CasterASC->GetActiveGameplayEffect(Handle);
						if (!Effect || Effect->PredictionKey != Key) continue;
						KeyedCost |= Effect->Spec.Def->IsA<UmultiplayerHealCostEffect>();
						KeyedCooldown |= Effect->Spec.Def->IsA<UmultiplayerHealCooldownEffect>();
					}
					SendReceipt(TEXT("PrimarySelfHealPredicted"), Key.IsLocalClientKey() && Key.WasLocallyGenerated()
						&& KeyedCost && KeyedCooldown && Caster->GetEnergy() == 30.f && Caster->GetHealth() == 90.f,
						FString::Printf(TEXT("Actual E-up frame before a later network receive: key%d keyed-cost%d keyed-CD%d energy55->%.0f; health remains authoritative90, not locally healed"),
							Key.Current, KeyedCost, KeyedCooldown, Caster->GetEnergy()));
					if (bDone) return;
					CombatPredictionKeys.Add(TEXT("SelfHeal"), Key.Current);
					Key.NewCaughtUpDelegate().BindWeakLambda(this, [this] { SentReceipts.Add(TEXT("SelfHealCaughtUp")); });
					SendReceipt(TEXT("PrimarySelfHealInputReleased"), !Controller->IsInputKeyDown(EKeys::E) && !Caster->IsTargetingSkill(),
						TEXT("Actual E key released and self preview ended through production input binding; no direct Heal injection"));
					FWorldDelegates::OnWorldPostActorTick.Remove(CombatSelfHealInputTick);
					CombatSelfHealInputTick.Reset();
				});
			PC->InputKey(FInputKeyParams(EKeys::E, IE_Pressed, 1.));
		}
		if (!SentReceipts.Contains(Prefix + TEXT("SelfHealEffectObserved")))
		{
			const auto& Feedback = Subject->GetCombatConfig().HealFeedback;
			const FVector Expected = Subject->GetActorLocation() - FVector::UpVector * Subject->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const auto Observe = [&](USceneComponent* Component, const TCHAR* Kind)
			{
				if (SentReceipts.Contains(Prefix + TEXT("SelfHealEffectObserved")) || Component->GetWorld() != World
					|| !Component->IsRegistered() || !Component->IsActive() || CombatSelfHealEffectsBefore.Contains(Component)) return;
				SendReceipt(Prefix + TEXT("SelfHealEffectObserved"), Component->GetComponentLocation().Equals(Expected, 1.f),
					FString::Printf(TEXT("New actual active %s Heal component=%s selected-formal-asset=1 old-snapshot=0 at-own-feet=%s expected=%s; independent of HP/PNG checks"),
						Kind, *Component->GetPathName(), *Component->GetComponentLocation().ToString(), *Expected.ToString()));
				if (!bDone)
				{
					CombatSelfHealEffect = Component;
					// 短粒子与属性复制可能先后抵达；活跃时立即请求画面，生命与预测确认仍分别验收。
					SentReceipts.Add(Prefix + TEXT("SelfHealCaptureStarted"));
					CaptureSkillView(TEXT("SelfHeal"), Subject);
				}
			};
			for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
				if (!Feedback.CascadeEffect.IsNull() && It->Template == Feedback.CascadeEffect.Get()) Observe(*It, TEXT("Cascade"));
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
				if (Feedback.CascadeEffect.IsNull() && !Feedback.Effect.IsNull() && It->GetAsset() == Feedback.Effect.Get()) Observe(*It, TEXT("Niagara"));
		}
		if (SentReceipts.Contains(Prefix + TEXT("SelfHealCaptureStarted"))
			&& !SentReceipts.Contains(Prefix + TEXT("VisualSelfHeal"))) CaptureSkillView(TEXT("SelfHeal"), Subject);
		if (CombatSelfHealHealthChanges > 1)
		{
			SendReceipt(Prefix + TEXT("SelfHealOutcome"), false, TEXT("This independent connection observed more than one HP change from one self-heal"));
			return;
		}
		if (!bPartner && SentReceipts.Contains(TEXT("SelfHealCaughtUp")))
		{
			int32 CooldownEffects = 0;
			for (auto Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
				if (const auto* Effect = ASC->GetActiveGameplayEffect(Handle);
					Effect && Effect->Spec.Def->IsA<UmultiplayerHealCooldownEffect>()) ++CooldownEffects;
			SendReceipt(TEXT("PrimarySelfHealConfirmed"), LocalCharacter->GetEnergy() == 30.f && CooldownEffects == 1,
				TEXT("Original self-heal prediction key caught up with energy30 and exactly one real authoritative healing cooldown"));
		}
		if (Subject->GetHealth() == 100.f && Teammate->GetHealth() == 70.f && CombatSelfHealHealthChanges == 1
			&& bCombatSelfHealExactHealthChange && (bPartner || LocalCharacter->GetEnergy() == 30.f))
		{
			SendReceipt(Prefix + TEXT("SelfHealOutcome"), true, TEXT("This independent client observed one real replicated HP90->100 self-heal; other player stayed70; owner also verifies energy55->30"));
		}
		return;
	}
	if (CombatPredictionClientTick(World)) return;

	// 当次调用返回前读取预测值并监听真实 Key；不能用后来到达的服务器结果冒充本地预测。
	const auto PredictDefense = [this, LocalCharacter](const FString& CaseName)
	{
		PredictCombatSkill(LocalCharacter, TEXT("Defense"), CaseName, 100.0f, CaseName != TEXT("Defense"));
	};

	// 技能列表可能晚于 Probe 到达；等真实授予复制就绪，不伪造已成功的激活。
	if (!bPartner && !SentReceipts.Contains(CommandMarker))
	{
		if (Phase == TEXT("CombatAttack") && ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			PC->SetControlRotation(FRotator::ZeroRotator);
			PredictCombatSkill(LocalCharacter, TEXT("Attack"), TEXT("Attack"), 100.0f, false);
		}
		else if ((Phase == TEXT("CombatDefense") || Phase == TEXT("CombatResourceReject") || Phase == TEXT("CombatPlayerDeath"))
			&& ASC->FindAbilitySpecFromClass(UmultiplayerDefendAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			PredictDefense(Phase == TEXT("CombatDefense") ? TEXT("Defense")
				: Phase == TEXT("CombatResourceReject") ? TEXT("Resource") : TEXT("Death"));
			if (Phase == TEXT("CombatPlayerDeath"))
			{
				PredictCombatSkill(LocalCharacter, TEXT("Attack"), TEXT("DeathAttack"), 80.0f, true);
				PredictCombatSkill(LocalCharacter, TEXT("Heal"), TEXT("DeathHeal"), 80.0f, true);
			}
		}
		else if (Phase == TEXT("CombatHeal") && SentReceipts.Contains(TEXT("PrimaryHealPreview"))
			&& ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			PredictCombatSkill(LocalCharacter, TEXT("Heal"), TEXT("Heal"), 80.0f, false);
		}
	}
	if (bDone) return;
	// 先观察引擎撤销预测，再允许服务器恢复属性复制；不能把覆盖到来的服务器值冒充回滚。
	if (!bPartner && (Phase == TEXT("CombatResourceReject") || Phase == TEXT("CombatPlayerDeath"))
		&& LocalCharacter->IsAlive() && LocalCharacter->GetEnergy() == 100.0f
		&& !ASC->HasAnyMatchingGameplayTags(CombatTransientTags()))
	{
		const bool bDeath = Phase == TEXT("CombatPlayerDeath");
		if (SentReceipts.Contains(bDeath ? TEXT("PrimaryDeathRejected") : TEXT("PrimaryResourceRejected"))
			&& (!bDeath || (SentReceipts.Contains(TEXT("PrimaryDeathAttackRejected")) && SentReceipts.Contains(TEXT("PrimaryDeathHealRejected")))))
			SendReceipt(bDeath ? TEXT("PrimaryDeathLocalRollback") : TEXT("PrimaryResourceLocalRollback"), true,
				TEXT("Rejected predictions restored stale local energy100 and removed all cost/CD/defense before authority replication was released"));
	}
	if (!bPartner && ((Phase == TEXT("CombatAttack") && SentReceipts.Contains(TEXT("PredictionCaughtUp:Attack")))
		|| (Phase == TEXT("CombatHeal") && SentReceipts.Contains(TEXT("PredictionCaughtUp:Heal")))))
	{
		const FString CaseName = Phase == TEXT("CombatAttack") ? TEXT("Attack") : TEXT("Heal");
		const float ExpectedEnergy = Phase == TEXT("CombatAttack") ? 100.0f : 55.0f;
		if (FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), ExpectedEnergy))
			SendReceipt(TEXT("Primary") + CaseName + TEXT("Confirmed"), true,
				TEXT("Real prediction key caught up; authoritative energy remained correct without a second cost"));
	}
	if (!bPartner && Phase == TEXT("CombatDefense") && SentReceipts.Contains(TEXT("PredictionCaughtUp:Defense")))
	{
		SendReceipt(TEXT("PrimaryDefenseConfirmed"), FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 80.0f)
			&& ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend),
			TEXT("PredictionKey caught up; authoritative reconciliation kept energy80, without double cost"));
	}

	// Subject 与数值可能分批就绪，满足当前阶段期望后才能回执；未到达不会默认为成功。
	if (Subject != nullptr)
	{
		if (Phase == TEXT("CombatInitial") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 80.0f))
			SendReceipt(Prefix + TEXT("CurrentState"), true, TEXT("Received current enemy health 80 without replaying its damage event"));
		else if (Phase == TEXT("CombatAttack") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 55.0f) && CaptureSkillView(TEXT("BasicAttack"),Subject))
			SendReceipt(Prefix + TEXT("Attack"), true, TEXT("Observed one attack result: enemy health 55"));
		else if (Phase == TEXT("CombatDefense") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 90.0f)
			&& Subject->GetShield() == 0.f
			&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 80.0f))
			&& Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending))
		{
			if (bVisualAcceptance)
			{
				const FString Seen = Prefix + TEXT("VisualDefenseFirstSeen");
				if (!SentReceipts.Contains(Seen)) { SentReceipts.Add(Seen); return; }
				const auto* CombatAnim = Cast<UmultiplayerCombatAnimInstance>(Subject->GetMesh()->GetAnimInstance());
				SendReceipt(Prefix + TEXT("DefensePoseActive"), CombatAnim && CombatAnim->CombatIsDefending,
					TEXT("Actual owner/remote AnimInstance entered guard after a natural animation update with the replicated defense state"));
				if (bDone) return;
			}
			if (CaptureSkillView(TEXT("BasicDefense"),Subject)) SendReceipt(Prefix + TEXT("Defense"), true, bPartner
				? TEXT("Observed health90/shield0 after the server-verified frontal block and explicit undirected damage baseline; defense replicated and remote resources remain private")
				: TEXT("Observed health90/shield0 after the server-verified frontal block and explicit undirected damage baseline, own energy80 and replicated defense tag"));
			if (bPartner)
				SendReceipt(TEXT("PartnerDefenseResourcesHidden"), Subject != LocalCharacter
					&& FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f)
					&& FMath::IsNearlyEqual(Subject->GetMaxEnergy(), 100.0f),
					TEXT("Primary defense changed public health/tag while its remote energy/max remained local defaults100"));
		}
		else if (Phase == TEXT("CombatHeal") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 70.0f))
		{
			// 主客户端同时核对自己的资源，Partner 单独确认自己的治疗结果。
			if ((bPartner || FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 55.0f)) && CaptureSkillView(TEXT("BasicHeal"),Subject))
				SendReceipt(Prefix + TEXT("Heal"), true, TEXT("Observed teammate health 70; primary also verified energy 55"));
		}
		else if ((Phase == TEXT("CombatResourceReady") || Phase == TEXT("CombatDeathReady")) && Subject->IsAlive()
			&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f))
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Attack)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Heal))
		{
			SendReceipt(Prefix + (Phase == TEXT("CombatResourceReady") ? TEXT("ResourceReady") : TEXT("DeathReady")), true,
				TEXT("Both clients see alive with no previous defense/cooldown; only owner verifies authoritative energy100"));
		}
		else if (Phase == TEXT("CombatResourceReject") && Subject->IsAlive()
			&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 10.0f))
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& (bPartner || SentReceipts.Contains(TEXT("PrimaryResourceRejected"))))
		{
			SendReceipt(Prefix + TEXT("ResourceRollback"), true,
				bPartner ? TEXT("Remote defense/CD tags are absent; private authoritative energy is not required on this connection")
				: TEXT("After real rejection, authoritative energy10 replaced stale prediction and defense/CD tags are absent"));
			if (bPartner)
				SendReceipt(TEXT("PartnerResourceRollbackResourcesHidden"), Subject != LocalCharacter
					&& FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f)
					&& FMath::IsNearlyEqual(Subject->GetMaxEnergy(), 100.0f),
					TEXT("Primary private energy10 was not forwarded; non-owner energy/max remained local defaults100"));
		}
		else if (Phase == TEXT("CombatEnemyDeath") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& !Subject->IsAlive() && Subject->GetHealth() == 0.0f && !Subject->GetActorEnableCollision())
			SendReceipt(Prefix + TEXT("EnemyDeath"), true, TEXT("Observed enemy zero health, dead state and disabled collision"));
		else if (Phase == TEXT("CombatPlayerDeath") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& !Subject->IsAlive() && Subject->GetHealth() == 0.0f && !Subject->GetActorEnableCollision()
			&& !Subject->InputEnabled() && PC->InputEnabled()
			&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f))
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Attack)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Heal)
			&& (bPartner || SentReceipts.Contains(TEXT("PrimaryDeathRejected"))))
		{
			if (!bPartner && !SentReceipts.Contains(TEXT("DeadSkillsAttempted")))
			{
				SentReceipts.Add(TEXT("DeadSkillsAttempted"));
				LocalCharacter->Attack();
				LocalCharacter->Heal();
			}
			SendReceipt(Prefix + TEXT("PlayerDeath"), true,
				TEXT("Observed authoritative death and cleared prediction tags; owner also verified energy100 and attempted forbidden skills; pawn input disabled without disabling controller UI input"));
		}
		else if (Phase == TEXT("CombatResourceRouting") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& (bPartner ? Subject == LocalCharacter : Subject != LocalCharacter)
			&& Subject->IsAlive() && FMath::IsNearlyEqual(Subject->GetHealth(), 70.0f)
			&& FMath::IsNearlyEqual(Subject->GetMaxHealth(), 173.0f))
		{
			// 公共属性已经到达才开始计时，排除整只 Pawn 尚未复制造成的假阴性。
			if (!SentReceipts.Contains(Prefix + TEXT("PublicMaxHealth")))
			{
				PhaseAt = FPlatformTime::Seconds();
				SendReceipt(Prefix + TEXT("PublicMaxHealth"), true, TEXT("Received public partner health70/max173 on this connection"));
			}
			if (bPartner && FMath::IsNearlyEqual(Subject->GetEnergy(), 37.0f)
				&& FMath::IsNearlyEqual(Subject->GetMaxEnergy(), 137.0f))
				SendReceipt(TEXT("PartnerOwnedResources"), true, TEXT("Owning connection received authoritative partner energy37/max137"));
			else if (!bPartner && FPlatformTime::Seconds() - PhaseAt >= 1.0)
				SendReceipt(TEXT("PrimaryPrivateResourcesHidden"), FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f)
					&& FMath::IsNearlyEqual(Subject->GetMaxEnergy(), 100.0f),
					TEXT("For one second after public max health173 arrived, non-owner energy/max stayed at local defaults100"));
		}
	}
	if (!bPartner && Phase == TEXT("Complete"))
	{
		SendReceipt(TEXT("Complete"), true, TEXT("Combat state scenario complete; no pixel or smoothness validation"));
		Finish(true);
	}
#endif
}
