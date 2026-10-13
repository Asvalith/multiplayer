// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/CoopNetTestDriver.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetworkObjectList.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
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

	// 故意留出“服务器已改变、客户端尚未知”的窗口；只延后夹具属性复制，不阻断真实 GAS RPC。
	bool DelayCombatStateReplication(UWorld* World, AmultiplayerCombatCharacter* Character)
	{
		UNetDriver* NetDriver = World->GetNetDriver();
		const TSharedPtr<FNetworkObjectInfo> Info = NetDriver ? NetDriver->GetNetworkObjectList().Find(Character) : nullptr;
		if (!Info.IsValid()) return false;
		Info->NextUpdateTime = World->GetTimeSeconds() + 30.0f;
		Info->bPendingNetUpdate = false;
		return true;
	}

	// 后两个拒绝场景共用准备步骤；移除的是服务器旧效果，不伪造客户端预测或回滚。
	void ResetCombatDefense(AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		FGameplayTagContainer Tags(CombatTags::State_Defending);
		Tags.AddTag(CombatTags::Cooldown_Defend);
		ASC->RemoveActiveEffectsWithGrantedTags(Tags);
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.0f);
		Character->ForceNetUpdate();
	}
}

// 冻结场景移动以隔离战斗同步；施放仍由客户端经过 GAS 的真实请求通道发起。
void UCoopNetTestDriver::CombatServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
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
	else if (Phase == TEXT("CombatAttack") && BothObserved(TEXT("Attack")))
	{
		Assert(TEXT("ServerCombatAttack"), CombatEnemy.IsValid()
			&& FMath::IsNearlyEqual(CombatEnemy->GetHealth(), 55.0f), TEXT("Two immediate requests produced one 25-damage hit"));
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
		if (!BothObserved(TEXT("Defense"))) return;
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
	else if (Phase == TEXT("CombatHeal") && BothObserved(TEXT("Heal")))
	{
		Assert(TEXT("ServerCombatHeal"), CombatPartner.IsValid() && CombatPrimary.IsValid()
			&& FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f)
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 55.0f),
			TEXT("Two immediate heal requests restored teammate 30 health and consumed energy once"));
		if (bDone) return;
		ResetCombatDefense(CombatPrimary.Get());
		Probe->Subject = CombatPrimary.Get();
		PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("CombatResourceReady"));
	}
	else if (Phase == TEXT("CombatResourceReady") && BothObserved(TEXT("ResourceReady")))
	{
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 10.0f);
		Assert(TEXT("ServerCombatResourceFixture"), DelayCombatStateReplication(World, CombatPrimary.Get()),
			TEXT("Server energy is 10; pawn property replication held while Probe delivers the stale-state request"));
		if (!bDone) SetCommand(TEXT("CombatResourceReject"));
	}
	else if (Phase == TEXT("CombatResourceReject"))
	{
		if (Receipts.Contains(TEXT("PrimaryResourcePredicted")) && !SentReceipts.Contains(TEXT("ReleaseResourceReplication")))
		{
			SentReceipts.Add(TEXT("ReleaseResourceReplication"));
			CombatPrimary->ForceNetUpdate();
		}
		if (!BothObserved(TEXT("ResourceRollback")) || !Receipts.Contains(TEXT("PrimaryResourceRejected"))) return;
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
		Assert(TEXT("ServerCombatDeathFixture"), DelayCombatStateReplication(World, CombatPrimary.Get()),
			TEXT("Server pawn is dead; its death replication held while the owning client still sees alive state"));
		if (bDone) return;
		SetCommand(TEXT("CombatPlayerDeath"));
	}
	else if (Phase == TEXT("CombatPlayerDeath"))
	{
		if (Receipts.Contains(TEXT("PrimaryDeathPredicted")) && !SentReceipts.Contains(TEXT("ReleaseDeathReplication")))
		{
			SentReceipts.Add(TEXT("ReleaseDeathReplication"));
			CombatPrimary->ForceNetUpdate();
		}
		if (!BothObserved(TEXT("PlayerDeath")) || !Receipts.Contains(TEXT("PrimaryDeathRejected"))) return;
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		Assert(TEXT("ServerCombatPlayerDeath"), CombatPrimary.IsValid() && !CombatPrimary->IsAlive()
			&& CombatPrimary->GetHealth() == 0.0f && !CombatPrimary->GetActorEnableCollision()
			&& FMath::IsNearlyEqual(CombatPrimary->GetEnergy(), 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& CombatPartner.IsValid() && FMath::IsNearlyEqual(CombatPartner->GetHealth(), 70.0f),
			TEXT("Death rejected stale client defense prediction; energy stayed 100 and defense/cooldown did not survive"));
		if (!bDone) SetCommand(TEXT("Complete"));
	}
	else if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))
		&& Receipts.Contains(TEXT("PartnerComplete")))
	{
		Finish(true, TEXT("Combat authority, local prediction, accepted reconciliation and two real rejection cases passed; no visual acceptance"));
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

	// 当次调用返回前读取预测值并监听真实 Key；不能用后来到达的服务器结果冒充本地预测。
	const auto PredictDefense = [this, LocalCharacter, ASC](const FString& CaseName)
	{
		const bool bStaleReady = LocalCharacter->IsAlive() && FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 100.0f)
			&& !ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend);
		LocalCharacter->Defend();
		FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerDefendAbility::StaticClass());
		UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		bool bDefenseEffectHasKey = false;
		bool bCooldownEffectHasKey = false;
		for (const FActiveGameplayEffectHandle& Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
		{
			const FActiveGameplayEffect* Effect = ASC->GetActiveGameplayEffect(Handle);
			if (Effect == nullptr || Effect->PredictionKey != Key) continue;
			bDefenseEffectHasKey |= Effect->Spec.Def->IsA(UmultiplayerDefendingEffect::StaticClass());
			bCooldownEffectHasKey |= Effect->Spec.Def->IsA(UmultiplayerDefendCooldownEffect::StaticClass());
		}
		const bool bPredicted = bStaleReady && Key.IsLocalClientKey() && Key.WasLocallyGenerated()
			&& FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 80.0f)
			&& ASC->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& bDefenseEffectHasKey && bCooldownEffectHasKey;
		if (Key.IsLocalClientKey())
		{
			Key.NewRejectedDelegate().BindWeakLambda(this, [this, CaseName, KeyValue = Key.Current]
			{
				SendReceipt(TEXT("Primary") + CaseName + TEXT("Rejected"), CaseName != TEXT("Defense"),
					FString::Printf(TEXT("Engine rejected the locally observed PredictionKey=%d"), KeyValue));
			});
			Key.NewCaughtUpDelegate().BindWeakLambda(this, [this, CaseName]
			{
				SentReceipts.Add(TEXT("PredictionCaughtUp:") + CaseName);
			});
		}
		SendReceipt(TEXT("Primary") + CaseName + TEXT("Predicted"), bPredicted,
			FString::Printf(TEXT("Before Defend returned: stale alive/energy100=%d, current energy=%.1f, defense/CD GE key=%d"),
				bStaleReady, LocalCharacter->GetEnergy(), Key.Current));
		// 同帧再次按下也必须被预测冷却/状态拦截，不额外扣费或创建第二次预测。
		if (!bDone)
		{
			LocalCharacter->Defend();
			Assert(TEXT("Primary") + CaseName + TEXT("RepeatedInput"), FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 80.0f),
				TEXT("Immediate repeated defense input did not consume another 20 energy"));
		}
	};

	// 技能列表可能晚于 Probe 到达；等真实授予复制就绪，不伪造已成功的激活。
	if (!bPartner && !SentReceipts.Contains(CommandMarker))
	{
		if (Phase == TEXT("CombatAttack") && ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			PC->SetControlRotation(FRotator::ZeroRotator);
			LocalCharacter->Attack();
			LocalCharacter->Attack();
		}
		else if ((Phase == TEXT("CombatDefense") || Phase == TEXT("CombatResourceReject") || Phase == TEXT("CombatPlayerDeath"))
			&& ASC->FindAbilitySpecFromClass(UmultiplayerDefendAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			PredictDefense(Phase == TEXT("CombatDefense") ? TEXT("Defense")
				: Phase == TEXT("CombatResourceReject") ? TEXT("Resource") : TEXT("Death"));
		}
		else if (Phase == TEXT("CombatHeal") && ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass()))
		{
			SentReceipts.Add(CommandMarker);
			LocalCharacter->Heal();
			LocalCharacter->Heal();
		}
	}
	if (bDone) return;
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
			&& FMath::IsNearlyEqual(Subject->GetEnergy(), 80.0f)
			&& Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending))
			SendReceipt(Prefix + TEXT("Defense"), true, TEXT("Observed reduced damage, energy 80 and replicated defense tag"));
		else if (Phase == TEXT("CombatHeal") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& FMath::IsNearlyEqual(Subject->GetHealth(), 70.0f))
		{
			// 主客户端同时核对自己的资源，Partner 单独确认自己的治疗结果。
			if (bPartner || FMath::IsNearlyEqual(LocalCharacter->GetEnergy(), 55.0f))
				SendReceipt(Prefix + TEXT("Heal"), true, TEXT("Observed teammate health 70; primary also verified energy 55"));
		}
		else if ((Phase == TEXT("CombatResourceReady") || Phase == TEXT("CombatDeathReady")) && Subject->IsAlive()
			&& FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend))
		{
			SendReceipt(Prefix + (Phase == TEXT("CombatResourceReady") ? TEXT("ResourceReady") : TEXT("DeathReady")), true,
				TEXT("Both clients have alive/energy100 and no previous defense/cooldown before server-side divergence"));
		}
		else if (Phase == TEXT("CombatResourceReject") && Subject->IsAlive()
			&& FMath::IsNearlyEqual(Subject->GetEnergy(), 10.0f)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& (bPartner || SentReceipts.Contains(TEXT("PrimaryResourceRejected"))))
		{
			SendReceipt(Prefix + TEXT("ResourceRollback"), true,
				TEXT("After real rejection, authoritative energy10 replaced stale prediction and defense/CD tags are absent"));
		}
		else if (Phase == TEXT("CombatEnemyDeath") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Enemies
			&& !Subject->IsAlive() && Subject->GetHealth() == 0.0f && !Subject->GetActorEnableCollision())
			SendReceipt(Prefix + TEXT("EnemyDeath"), true, TEXT("Observed enemy zero health, dead state and disabled collision"));
		else if (Phase == TEXT("CombatPlayerDeath") && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players
			&& !Subject->IsAlive() && Subject->GetHealth() == 0.0f && !Subject->GetActorEnableCollision()
			&& FMath::IsNearlyEqual(Subject->GetEnergy(), 100.0f)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Defending)
			&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Defend)
			&& (bPartner || SentReceipts.Contains(TEXT("PrimaryDeathRejected"))))
		{
			if (!bPartner && !SentReceipts.Contains(TEXT("DeadSkillsAttempted")))
			{
				SentReceipts.Add(TEXT("DeadSkillsAttempted"));
				LocalCharacter->Attack();
				LocalCharacter->Heal();
			}
			SendReceipt(Prefix + TEXT("PlayerDeath"), true,
				TEXT("Observed authoritative death/energy100 and cleared prediction tags; primary also attempted forbidden skills"));
		}
	}
	if (!bPartner && Phase == TEXT("Complete"))
	{
		SendReceipt(TEXT("Complete"), true, TEXT("Combat state scenario complete; no pixel or smoothness validation"));
		Finish(true);
	}
#endif
}
