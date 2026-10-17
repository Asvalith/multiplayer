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
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetworkObjectList.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "HAL/PlatformTime.h"

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
		CombatPrimary->GetCharacterMovement()->DisableMovement();
		CombatPartner->GetCharacterMovement()->DisableMovement();
		CombatPrimary->SetActorLocation(FVector(0, 0, 4000));
		CombatPartner->SetActorLocation(FVector(100, 300, 4000));
		PrimaryPC->SetControlRotation(FRotator::ZeroRotator);
		PartnerPC->SetControlRotation(FRotator::ZeroRotator);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(FVector(500, 0, 4000), FRotator::ZeroRotator, Params);
		Assert(TEXT("ServerCombatSpawn"), CombatEnemy.IsValid(), TEXT("Server spawned one replicated target"));
		if (bDone) return;
		// 保留原生敌人的 ASC 与碰撞，只停止 AI，避免其额外攻击改变受测数值。
		if (AAIController* AI = Cast<AAIController>(CombatEnemy->GetController()))
		{
			AI->UnPossess();
			AI->Destroy();
		}
		CombatEnemy->GetCharacterMovement()->DisableMovement();
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
		SetCommand(TEXT("CombatInitial"));
	}

	const auto BothObserved = [this](const TCHAR* Suffix)
	{
		return Receipts.Contains(FString(TEXT("Primary")) + Suffix)
			&& Receipts.Contains(FString(TEXT("Partner")) + Suffix);
	};
	if (Phase == TEXT("CombatInitial") && BothObserved(TEXT("CurrentState")))
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
			if (!ApplyCombatTestDamage(CombatEnemy.Get(), CombatPrimary.Get(), 20.0f))
			{
				Finish(false, TEXT("Could not apply defended damage"));
				return;
			}
		}
		if (!BothObserved(TEXT("Defense")) || !Receipts.Contains(TEXT("PartnerDefenseResourcesHidden"))) return;
		if (!Receipts.Contains(TEXT("PrimaryDefensePredicted")) || !Receipts.Contains(TEXT("PrimaryDefenseConfirmed"))) return;
		FGameplayAbilitySpec* DefenseSpec = CombatPrimary->GetAbilitySystemComponent()->FindAbilitySpecFromClass(
			UmultiplayerDefendAbility::StaticClass());
		UGameplayAbility* DefenseInstance = DefenseSpec ? DefenseSpec->GetPrimaryInstance() : nullptr;
		const FPredictionKey ServerKey = DefenseInstance
			? DefenseInstance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		Assert(TEXT("ServerCombatDefense"), CombatPrimary.IsValid()
			&& FMath::IsNearlyEqual(CombatPrimary->GetHealth(), 90.0f)
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 80.0f) && ServerKey.IsValidKey() && ServerKey.WasReceived(),
			FString::Printf(TEXT("Defense consumed energy once and reduced 20 damage to 10; received PredictionKey=%d"), ServerKey.Current));
		if (bDone) return;
		if (!ApplyCombatTestDamage(CombatEnemy.Get(), CombatPartner.Get(), 60.0f))
		{
			Finish(false, TEXT("Could not prepare wounded teammate"));
			return;
		}
		Probe->Subject = CombatPartner.Get();
		PartnerProbe->Subject = CombatPartner.Get();
		SetCommand(TEXT("CombatHeal"));
	}
	else if (Phase == TEXT("CombatHeal") && BothObserved(TEXT("Heal"))
		&& Receipts.Contains(TEXT("PrimaryHealConfirmed")))
	{
		Assert(TEXT("ServerCombatHeal"), CombatPartner.IsValid() && CombatPrimary.IsValid()
			&& FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f)
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 55.0f) && HasReceivedPredictionKey(CombatPrimary.Get(), TEXT("Heal")),
			TEXT("Two immediate heal requests restored teammate 30 health and consumed energy once"));
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
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Attack) && !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Heal)
			&& CombatPartner.IsValid() && FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f),
			TEXT("Death rejected stale client defense prediction; energy stayed 100 and defense/cooldown did not survive"));
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
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(FVector(500, 0, 4000), FRotator::ZeroRotator, Params);
		if (!Probe.IsValid() || !CombatEnemy.IsValid()) { Finish(false, TEXT("Could not rebuild fresh-session test probes/target")); return true; }
		if (AAIController* AI = Cast<AAIController>(CombatEnemy->GetController())) { AI->UnPossess(); AI->Destroy(); }
		CombatEnemy->GetCharacterMovement()->DisableMovement();
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
		else
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
		const bool bUnchanged = CombatPrimary->GetEnergy() == (Reason == TEXT("Energy") ? 10.0f : 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
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
			ApplyCombatTestDamage(CombatEnemy.Get(), CombatPrimary.Get(), 20.0f);
		}
		if (!BothObserved(TEXT("RetryOutcome")) || !Receipts.Contains(TEXT("Primary") + Name + TEXT("RetryConfirmed"))) return true;
		const float ExpectedEnergy = 100.0f - PredictionSkill(Skill).EnergyCost;
		Assert(TEXT("Server") + Name + TEXT("Retry"), HasReceivedPredictionKey(CombatPrimary.Get(), Skill)
			&& CombatPrimary->GetEnergy() == ExpectedEnergy
			&& (Skill != TEXT("Attack") || CombatEnemy->GetHealth() == 75.0f)
			&& (Skill != TEXT("Area") || (!CombatPrimary->IsCasting()
				&& CombatEnemy->GetHealth() == 100.0f - CombatPrimary->GetCombatConfig().Area.Amount))
			&& (Skill != TEXT("Defense") || CombatPrimary->GetHealth() == 80.0f)
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
		if (!bPredictedEffectSurvived)
			SendReceipt(TEXT("Primary") + Name + TEXT("Rollback"), true,
				TEXT("Engine rejection removed that prediction key's effects and restored local energy100 before server fixture replication was released"));
	}
	if (bRetry)
	{
		const float ExpectedEnergy = 100.0f - PredictionSkill(Skill).EnergyCost;
		if (!bPartner && SentReceipts.Contains(TEXT("PredictionCaughtUp:") + KeyCase) && Local->GetEnergy() == ExpectedEnergy)
			SendReceipt(TEXT("Primary") + KeyCase + TEXT("Confirmed"), true,
				TEXT("Retry key caught up and authoritative energy matches a single consumption"));
		const bool bOutcome = (Skill == TEXT("Attack") && CombatEnemy->GetHealth() == 75.0f)
			|| (Skill == TEXT("Area") && !Primary->IsCasting()
				&& CombatEnemy->GetHealth() == 100.0f - Primary->GetCombatConfig().Area.Amount)
			|| (Skill == TEXT("Defense") && Primary->GetHealth() == 80.0f && Primary->IsDefending())
			|| (Skill == TEXT("Heal") && Partner->GetHealth() == 70.0f);
		if (bOutcome && (bPartner || Local->GetEnergy() == ExpectedEnergy))
			SendReceipt(Prefix + Name + TEXT("RetryOutcome"), true,
				TEXT("Observed the authoritative single skill result on this independent connection; Area also completed recovery"));
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
	const auto Invoke = [Character, &Skill]()
		{ if (Skill == TEXT("Attack")) Character->Attack(); else if (Skill == TEXT("Defense")) Character->Defend();
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
		else if (Phase == TEXT("CombatHeal") && ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass()))
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
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 55.0f))
			SendReceipt(Prefix + TEXT("Attack"), true, TEXT("Observed one attack result: enemy health 55"));
		else if (Phase == TEXT("CombatDefense") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 90.0f)
			&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 80.0f))
			&& Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending))
		{
			SendReceipt(Prefix + TEXT("Defense"), true, bPartner
				? TEXT("Observed reduced damage and replicated defense tag; remote resources remain private")
				: TEXT("Observed reduced damage, own energy80 and replicated defense tag"));
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
			if (bPartner || FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 55.0f))
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
				TEXT("Observed authoritative death and cleared prediction tags; owner also verified energy100 and attempted forbidden skills"));
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
