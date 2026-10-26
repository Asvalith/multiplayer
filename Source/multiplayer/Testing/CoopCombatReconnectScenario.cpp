#include "Testing/CoopNetTestDriver.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Net/Core/Connection/NetCloseResult.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UnrealType.h"

// 本局短线恢复联机夹具：真实断开连接后经票据握手恢复个人状态、效果剩余时间与成长。
// 覆盖同一服务器进程的内存快照，不宣称服务器重启、永久存档或进行中半技能恢复。

namespace
{
	const FVector ResumeLocation(0, 0, 4000);
	constexpr float ReconnectShield = 60.f;
	constexpr float ReconnectBurnDamage = 1.f;
	constexpr float ReconnectEffectDuration = 30.f;
	constexpr float ReconnectDownedDuration = 45.f;

	// 以成长身份标识连接前后是否同一档案，不把不透明会话票据写进日志。
	FString ReconnectProfileMarker(const AmultiplayerCombatCharacter* Character)
	{
		return TEXT("ReconnectProfile:") + Character->GetProgressionComponent()->GetState().ProfileId.ToString(EGuidFormats::Digits);
	}
	// 校验成长状态、属性与真正授予的能力等级一起恢复，避免只恢复界面数字。
	bool HasReconnectGrowth(AmultiplayerCombatCharacter* Character)
	{
		const auto& Growth = Character->GetProgressionComponent()->GetState();
		const auto* Dash = Character->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerDashAbility::StaticClass());
		return Growth.ProfileId.IsValid() && Growth.Level == 2 && Growth.SkillPoints == 1 && Growth.ActiveLevels[4] == 1
			&& Growth.PassiveLevels[0] == 1 && Growth.EquippedPassive == EMultiplayerCombatPassive::Fortitude
			&& Character->GetMaxHealth() == 125.f && Dash && Dash->Level == 1;
	}
	// 新 Pawn 不能继承旧施法、救援或冲刺任务，持续效果恢复与半技能续跑分开验收。
	bool NoReconnectHalfAction(AmultiplayerCombatCharacter* Character)
	{
		return !Character->IsCasting() && !Character->GetCoopComponent()->IsRescuing()
			&& !Character->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing)
			&& !Character->GetCharacterMovement()->HasRootMotionSources();
	}
	// 仅扩展断线夹具的观察窗口；原始资产和 CDO 不变，不能在重连时重置已开始的失血时间。
	bool ConfigureReconnectCoop(AmultiplayerCombatCharacter* Character)
	{
		const auto& Current = Character->GetCombatConfig();
		if (Current.GetOuter() == Character) return Current.BleedoutDuration == ReconnectDownedDuration && Current.RescueDuration == 8.f;
		auto* Property = FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("CombatConfig"));
		if (!Property) return false;
		auto* Copy = DuplicateObject<UmultiplayerCombatConfig>(&Current, Character); Copy->SetFlags(RF_Transient);
		Copy->BleedoutDuration = ReconnectDownedDuration; Copy->RescueDuration = 8.f;
		Property->SetObjectPropertyValue_InContainer(Character, Copy); return true;
	}

	// 唯一周期效果的实际相位由 GE 自己的 Timer 提供，不用整数经过秒数推算首跳。
	const FmultiplayerCombatResumeEffect* FindReconnectBurn(const FmultiplayerCombatResumeState& State)
	{
		const FmultiplayerCombatResumeEffect* Found = nullptr;
		for (const auto& Effect : State.PersistentEffects)
		{
			if (!Effect.Spec.IsValid() || !Effect.Spec.Data->Def || !Effect.Spec.Data->Def->IsA<UmultiplayerBurningEffect>()) continue;
			if (Found) return nullptr;
			Found = &Effect;
		}
		return Found;
	}

	// 服务器准备真实冷却 GE，供断线后验证剩余时间递减，而非恢复时重新免费计时。
	bool ApplyReconnectCooldowns(AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		for (TSubclassOf<UGameplayEffect> Class : { UmultiplayerAttackCooldownEffect::StaticClass(),
			UmultiplayerDefendCooldownEffect::StaticClass(), UmultiplayerHealCooldownEffect::StaticClass(), UmultiplayerAreaCooldownEffect::StaticClass(),
			UmultiplayerDashCooldownEffect::StaticClass() })
		{
			FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(Class, 1, ASC->MakeEffectContext());
			if (!Spec.IsValid()) return false;
			Spec.Data->SetDuration(30, true);
			if (!ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get()).IsValid()) return false;
		}
		return true;
	}

	// 只在服务器同一帧创建并销毁临时夹具；直接测试权威入口，不新增业务测试 RPC，票据不进入日志。
	struct FTicketChecks { bool Active = false, Invalid = false, AckLost = false, RotatedAlias = false; };
	// 用临时权威控制器检验在线互斥、无效票据和回执丢失轮换；不暴露生产测试 RPC。
	FTicketChecks CheckReconnectTickets(UWorld* World)
	{
		FTicketChecks Result;
		auto* Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
		// 只为当前同步用例建立未登录控制器/Pawn，销毁时不得干扰真实玩家登录计数。
		const auto NewController = [World]()
		{
			FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* PC = World->SpawnActor<AmultiplayerCoopPlayerController>(Params);
			// 临时 Controller 从未 PostLogin，不能让销毁时 Logout 扣除两名真实玩家的计数或创建 inactive 玩家。
			if (PC && PC->PlayerState) { PC->PlayerState->Destroy(); PC->SetPlayerState(nullptr); }
			auto* Pawn = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(0, 0, 12000), FRotator::ZeroRotator, Params);
			if (!PC || !Pawn) { if (PC) PC->Destroy(); if (Pawn) Pawn->Destroy(); return static_cast<AmultiplayerCoopPlayerController*>(nullptr); }
			PC->SetPawn(Pawn); Pawn->SetCombatSessionReady(false);
			return PC;
		};
		auto* First = NewController(); auto* Second = NewController();
		if (!Mode || !First || !Second) { if (First) First->Destroy(); if (Second) Second->Destroy(); return Result; }
		FGuid Initial, Rotated, Unused; FText Error; bool Retry = false;
		if (!Mode->ClaimCombatSession(First, FGuid(), Initial, Error, Retry)) { First->Destroy(); Second->Destroy(); return Result; }
		Result.Active = !Mode->ClaimCombatSession(Second, Initial, Unused, Error, Retry) && Retry;
		Result.Invalid = !Mode->ClaimCombatSession(Second, FGuid::NewGuid(), Unused, Error, Retry) && !Retry;
		auto* Pawn = CastChecked<AmultiplayerCombatCharacter>(First->GetPawn());
		Pawn->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 73);
		Pawn->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 37);
		First->Destroy(); // 真实 Destroyed/PawnLeavingGame 必须在销毁 Pawn 前保存快照。
		const bool AcceptedSecond = Mode->ClaimCombatSession(Second, Initial, Rotated, Error, Retry);
		Second->Destroy();
		auto* Third = NewController();
		if (!Third) return Result;
		// 模拟轮换回执未抵达：客户端实际仍只持 Initial，而不是服务器已经签发的 Rotated。
		const bool AcceptedThird = AcceptedSecond && Mode->ClaimCombatSession(Third, Initial, Rotated, Error, Retry);
		Pawn = Cast<AmultiplayerCombatCharacter>(Third->GetPawn());
		Result.AckLost = AcceptedThird && Pawn && Pawn->GetHealth() == 73 && Pawn->GetEnergy() == 37;
		Third->Destroy();
		auto* Fourth = NewController();
		if (!Fourth) return Result;
		const bool AcceptedFourth = AcceptedThird && Mode->ClaimCombatSession(Fourth, Rotated, Unused, Error, Retry);
		Fourth->Destroy();
		auto* Fifth = NewController();
		if (!Fifth) return Result;
		Result.RotatedAlias = AcceptedFourth && !Mode->ClaimCombatSession(Fifth, Initial, Unused, Error, Retry) && !Retry;
		Fifth->Destroy();
		return Result;
	}
}

// DS 准备个人快照和持续效果、关闭真实连接，再核对新 Pawn 恢复、离线结算与终局。
void UCoopNetTestDriver::ReconnectServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	const auto Both = [this](const TCHAR* Suffix)
	{ return Receipts.Contains(FString(TEXT("Primary")) + Suffix) && Receipts.Contains(FString(TEXT("Partner")) + Suffix); };
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (Phase == TEXT("ScenarioStart"))
	{
		auto* Primary = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		auto* Partner = Cast<AmultiplayerCoopPlayerController>(PartnerPlayer(World));
		if (!Primary || !Partner || !Primary->HasAcceptedCombatSession() || !Partner->HasAcceptedCombatSession()) return;
		CombatPrimary = Cast<AmultiplayerCombatCharacter>(Primary->GetPawn());
		CombatPartner = Cast<AmultiplayerCombatCharacter>(Partner->GetPawn());
		Assert(TEXT("ServerReconnectFixture"), CombatPrimary.IsValid() && CombatPartner.IsValid() && State
			&& State->GetObjectiveState().RequiredKeys > 1, TEXT("Two admitted real player pawns and a shared round"));
		if (bDone) return;
		const FTicketChecks Tickets = CheckReconnectTickets(World);
		Assert(TEXT("ServerReconnectTicketActive"), Tickets.Active, TEXT("An active ticket cannot create a second live session"));
		Assert(TEXT("ServerReconnectTicketInvalid"), Tickets.Invalid, TEXT("An unknown nonempty ticket is rejected, not treated as a fresh session"));
		Assert(TEXT("ServerReconnectTicketAckLost"), Tickets.AckLost, TEXT("Lost rotation receipt retains one inactive alias and restores server health/energy"));
		Assert(TEXT("ServerReconnectTicketRotatedAlias"), Tickets.RotatedAlias, TEXT("Using the received current ticket retires the older alias"));
		if (bDone) return;
		for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) It->Destroy();
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
		{
			if (auto* AI = Cast<AAIController>(It->GetController())) { AI->UnPossess(); AI->Destroy(); }
			It->Destroy();
		}
		for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) It->Destroy();
		// 独立来源不参与战斗：自己的一份真实 GE 持续走时，作为从未断线的伤害/周期对照。
		FActorSpawnParameters SourceParams; SourceParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(ResumeLocation + FVector(0, 5000, 0), FRotator::ZeroRotator, SourceParams);
		if (!CombatEnemy.IsValid()) { Finish(false, TEXT("Could not create the persistent-effect reference source")); return; }
		if (auto* AI = Cast<AAIController>(CombatEnemy->GetController())) { AI->UnPossess(); AI->Destroy(); }
		CombatEnemy->GetCharacterMovement()->DisableMovement(); CombatEnemy->SetActorEnableCollision(false);
		CombatPrimary->SetActorLocation(ResumeLocation); CombatPartner->SetActorLocation(FVector(400, 300, 4000));
		CombatPrimary->GetCharacterMovement()->DisableMovement(); CombatPartner->GetCharacterMovement()->DisableMovement();
		ReconnectPartnerController = Partner; CombatPreviousController = Primary;
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		FmultiplayerCoopObjectiveState Progress = State->GetObjectiveState(); Progress.ActivatedKeys = 1; Progress.bGameWon = false;
		State->ApplyAuthoritativeState(Progress);
		Assert(TEXT("ServerReconnectGrowthAward"), CombatPrimary->GetProgressionComponent()->AwardExperience(100, FGuid::NewGuid())
			&& CombatPrimary->GetProgressionComponent()->GetState().Level == 2, TEXT("Server awarded level2 before the owner's real learn/equip RPCs"));
		if (!bDone) SetCommand(TEXT("ReconnectLoadoutReady"));
		return;
	}
	if (Phase == TEXT("ReconnectLoadoutReady") && Both(TEXT("ReconnectLoadoutReady")))
	{
		Assert(TEXT("ServerReconnectGrowthFixture"), HasReconnectGrowth(CombatPrimary.Get()),
			TEXT("Owned learning/equipping produced Dash1, Fortitude1, stable profile and maxHP125 without a fixture GiveAbility"));
		if (bDone) return;
		SentReceipts.Add(ReconnectProfileMarker(CombatPrimary.Get()));
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 73);
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 37);
		if (!ApplyReconnectCooldowns(CombatPrimary.Get())) { Finish(false, TEXT("Could not apply real reconnect cooldown effects")); return; }
		CombatPrimary->ApplyStun(30);
		CombatPrimary->ApplyRoot(ReconnectEffectDuration);
		CombatPrimary->GrantShield(ReconnectShield); CombatEnemy->GrantShield(ReconnectShield);
		UAbilitySystemComponent* SourceASC = CombatEnemy->GetAbilitySystemComponent();
		SourceASC->OnPeriodicGameplayEffectExecuteDelegateOnSelf.AddWeakLambda(this,
			[this](UAbilitySystemComponent* Source, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
			{
				if (bDone || Phase != TEXT("ReconnectLiveDrop") || CombatPrimary.IsValid() || !Source
					|| !Spec.Def || !Spec.Def->IsA<UmultiplayerBurningEffect>()) return;
				const auto* Current = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(Source->GetWorld()));
				if (!Current || !Current->HasAcceptedCombatSession()) SentReceipts.Add(TEXT("ReferenceBurnObservedOffline"));
			});
		auto ReferenceBurn = SourceASC->MakeOutgoingSpec(UmultiplayerBurningEffect::StaticClass(), 1, SourceASC->MakeEffectContext());
		if (!ReferenceBurn.IsValid()) { Finish(false, TEXT("Could not configure the continuous reference burn")); return; }
		ReferenceBurn.Data->SetDuration(ReconnectEffectDuration, true);
		ReferenceBurn.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, ReconnectBurnDamage);
		const auto ReferenceHandle = SourceASC->ApplyGameplayEffectSpecToSelf(*ReferenceBurn.Data.Get());
		const auto PrimaryHandle = CombatEnemy->ApplyBurning(CombatPrimary.Get(), ReconnectBurnDamage, ReconnectEffectDuration);
		Assert(TEXT("ServerReconnectPersistentFixture"), ReferenceHandle.IsValid() && PrimaryHandle.IsValid()
			&& CombatPrimary->IsRooted() && CombatPrimary->IsBurning() && CombatPrimary->GetShield() == ReconnectShield
			&& CombatEnemy->GetShield() == ReconnectShield && !CombatPrimary->IsDefending() && !CombatEnemy->IsDefending()
			&& SourceASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetAttackPowerAttribute()) == 0
			&& SourceASC->GetNumericAttribute(UmultiplayerCombatAttributes::GetArmorAttribute()) == 0
			&& CombatPrimary->GetAbilitySystemComponent()->GetNumericAttribute(UmultiplayerCombatAttributes::GetArmorAttribute()) == 0,
			TEXT("Same-frame real 1s burns share the first Pending tick; independent 60 shields cover all 30 damage, preserving primary health73"));
		if (bDone) return;
		ReconnectCooldownAt = FPlatformTime::Seconds();
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("ReconnectLiveBefore"));
	}
	if (Phase == TEXT("ReconnectLiveBefore") && Both(TEXT("ReconnectLiveBefore"))) SetCommand(TEXT("ReconnectLiveDrop"));
	if (Phase == TEXT("ReconnectDeadBefore") && Both(TEXT("ReconnectDeadBefore"))) SetCommand(TEXT("ReconnectDeadDrop"));
	if (Phase == TEXT("ReconnectDownedReady") && Both(TEXT("ReconnectDownedReady"))) SetCommand(TEXT("ReconnectDownedRescue"));
	if (Phase == TEXT("ReconnectDownedRescue") && CombatPartner->GetCoopComponent()->IsRescuing()) SetCommand(TEXT("ReconnectDownedBefore"));
	if (Phase == TEXT("ReconnectDownedBefore") && Both(TEXT("ReconnectDownedBefore"))) SetCommand(TEXT("ReconnectDownedDrop"));
	if (Phase == TEXT("ReconnectLiveDrop") || Phase == TEXT("ReconnectDeadDrop") || Phase == TEXT("ReconnectDownedDrop"))
	{
		auto* PC = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		if (!PC || PC == CombatPreviousController.Get() || !PC->HasAcceptedCombatSession()) return;
		auto* Character = Cast<AmultiplayerCombatCharacter>(PC->GetPawn());
		if (!Character) return;
		const bool Dead = Phase == TEXT("ReconnectDeadDrop");
		const bool Downed = Phase == TEXT("ReconnectDownedDrop");
		if (Downed && CombatPartner->GetCoopComponent()->IsRescuing()) return; // 等真实旧目标失效后的验证任务收尾，不替业务清理。
		FmultiplayerCombatResumeState Restored;
		FmultiplayerCombatResumeState Reference;
		const FmultiplayerCombatResumeEffect* RestoredBurn = nullptr;
		const FmultiplayerCombatResumeEffect* ReferenceBurn = nullptr;
		if (!Dead && !Downed)
		{
			Restored = Character->CaptureResumeState();
			if (CombatEnemy.IsValid()) Reference = CombatEnemy->CaptureResumeState();
			RestoredBurn = FindReconnectBurn(Restored); ReferenceBurn = FindReconnectBurn(Reference);
			Assert(TEXT("ServerReconnectBurnReference"), CombatEnemy.IsValid() && RestoredBurn && ReferenceBurn,
				TEXT("Reconnected pawn and uninterrupted reference each retain one real Burning GE"));
			if (bDone) return;
			// 核对时避开 Timer 刚到期/尚未推进的一帧，不把正常 Pending 和到期调度误判为多扣或漏扣。
			const float Period = ReferenceBurn->Spec.Data->GetPeriod();
			if (ReferenceBurn->NextTickDelay < .2f || ReferenceBurn->NextTickDelay > Period - .2f) return;
		}
		Character->GetCharacterMovement()->DisableMovement();
		const bool Common = Character != CombatPrimary.Get() && Character->GetEnergy() == 37
			&& FVector::Dist(Character->GetActorLocation(), ResumeLocation) < 30;
		Assert(Dead ? TEXT("ServerReconnectDead") : Downed ? TEXT("ServerReconnectDowned") : TEXT("ServerReconnectLive"), Common
			&& (Dead ? (!Character->IsAlive() && Character->GetHealth() == 0 && !Character->GetActorEnableCollision())
				: Downed ? (Character->IsAlive() && !Character->IsCombatReady() && Character->GetHealth() == 1.f && Character->GetCoopComponent()->IsDowned())
				: (Character->IsCombatReady() && Character->GetHealth() == 73)), TEXT("New admitted controller/pawn restored server resources, life state and position"));
		if (bDone) return;
		Assert(Dead ? TEXT("ServerReconnectDeadGrowth") : Downed ? TEXT("ServerReconnectDownedGrowth") : TEXT("ServerReconnectGrowth"),
			HasReconnectGrowth(Character) && SentReceipts.Contains(ReconnectProfileMarker(Character)) && NoReconnectHalfAction(Character),
			TEXT("Stable profile, level2, learned Dash1 and equipped Fortitude1/max125 survived; no half dash/rescue resumed"));
		if (bDone) return;
		if (Downed)
		{
			const float Expected = ReconnectDownedDuration - float(FPlatformTime::Seconds() - CombatLifecycleAt);
			Assert(TEXT("ServerReconnectDownedElapsed"), Character->GetCoopComponent()->GetDownedRemaining() > 0
				&& Character->GetCoopComponent()->GetDownedRemaining() < ReconnectDownedDuration - 1
				&& FMath::Abs(Character->GetCoopComponent()->GetDownedRemaining() - Expected) < .75f
				&& !CombatPartner->GetCoopComponent()->IsRescuing(),
				FString::Printf(TEXT("Real downed reconnect retained deadline: expected %.3f actual %.3f; old partner rescue reservation/task ended"),
					Expected, Character->GetCoopComponent()->GetDownedRemaining()));
			if (bDone) return;
		}
		else if (!Dead)
		{
			const float Expected = 30 - static_cast<float>(FPlatformTime::Seconds() - ReconnectCooldownAt);
			Assert(TEXT("ServerReconnectCooldownElapsed"), Expected > 0 && Restored.AttackCooldown < 29
				&& FMath::Abs(Restored.AttackCooldown - Expected) < .75f && FMath::Abs(Restored.DefenseCooldown - Expected) < .75f
				&& FMath::Abs(Restored.HealCooldown - Expected) < .75f && FMath::Abs(Restored.AreaCooldown - Expected) < .75f,
				FString::Printf(TEXT("All four real cooldowns deduct elapsed online/offline time: expected=%.3f actual=%.3f/%.3f/%.3f/%.3f"),
					Expected, Restored.AttackCooldown, Restored.DefenseCooldown, Restored.HealCooldown, Restored.AreaCooldown));
			Assert(TEXT("ServerReconnectControlElapsed"), Character->IsStunned() && !Character->IsCasting()
				&& FMath::Abs(Restored.StunRemaining - Expected) < .75f,
				TEXT("New pawn restores the actual remaining stun, does not resume a half-cast, and admission does not clear control"));
			Assert(TEXT("ServerReconnectDashCooldown"), Restored.DashCooldown > 0 && Restored.DashCooldown < 29
				&& FMath::Abs(Restored.DashCooldown - Expected) < .75f && NoReconnectHalfAction(Character),
				TEXT("Actual learned Dash cooldown deducts online/offline time without restoring a dash task or rescuing action"));
			Assert(TEXT("ServerReconnectBurnOffline"), SentReceipts.Contains(TEXT("ReferenceBurnObservedOffline")),
				TEXT("The uninterrupted real reference executed a burn tick after the old pawn disappeared and before a replacement session was admitted"));
			Assert(TEXT("ServerReconnectPersistentElapsed"), Character->IsRooted() && Character->IsBurning()
				&& FMath::Abs(Character->GetRootRemaining() - Expected) < .75f
				&& FMath::Abs(RestoredBurn->Remaining - Expected) < .75f
				&& FMath::Abs(RestoredBurn->Remaining - ReferenceBurn->Remaining) < .1f
				&& Reference.Shield < ReconnectShield && FMath::IsNearlyEqual(Character->GetShield(), Reference.Shield, .01f),
				FString::Printf(TEXT("Root/Burn elapsed without reset; primary shield=%.3f continuous shield=%.3f health73 retained; root=%.3f burn=%.3f expected=%.3f"),
					Character->GetShield(), Reference.Shield, Character->GetRootRemaining(), RestoredBurn->Remaining, Expected));
			Assert(TEXT("ServerReconnectBurnPhase"), FMath::IsNearlyEqual(RestoredBurn->Spec.Data->GetPeriod(), ReferenceBurn->Spec.Data->GetPeriod())
				&& RestoredBurn->NextTickDelay > 0 && FMath::Abs(RestoredBurn->NextTickDelay - ReferenceBurn->NextTickDelay) < .1f,
				FString::Printf(TEXT("Actual GE next-tick delay: restored=%.4f uninterrupted=%.4f; no integer-second first-tick assumption"),
					RestoredBurn->NextTickDelay, ReferenceBurn->NextTickDelay));
			if (bDone) return;
		}
		else
		{
			Assert(TEXT("ServerReconnectDeadPersistent"), Character->GetShield() == 0 && !Character->IsRooted() && !Character->IsBurning()
				&& Character->CaptureResumeState().PersistentEffects.IsEmpty(), TEXT("Dead reconnect restores zero shield and no obsolete Root/Burn effects"));
			if (bDone) return;
		}
		if (Probe.IsValid()) Probe->Destroy();
		FActorSpawnParameters Params; Params.Owner = PC;
		Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
		if (!Probe.IsValid()) { Finish(false, TEXT("Could not replace reconnected owned probe")); return; }
		CombatPrimary = Character; Probe->Subject = Character; PartnerProbe->Subject = Character;
		Probe->CombatTarget = CombatPartner.Get(); PartnerProbe->CombatTarget = CombatPartner.Get();
		SetCommand(Dead ? TEXT("ReconnectDeadVerify") : Downed ? TEXT("ReconnectDownedVerify") : TEXT("ReconnectLiveVerify"));
	}
	if (Phase == TEXT("ReconnectLiveVerify") && Both(TEXT("ReconnectLive"))
		&& Both(TEXT("ReconnectControl")) && Both(TEXT("ReconnectPersistent")) && Both(TEXT("ReconnectGrowth"))
		&& Receipts.Contains(TEXT("PrimaryReconnectDashCooldown")))
	{
		Assert(TEXT("ServerReconnectDownedFixture"), ConfigureReconnectCoop(CombatPrimary.Get()) && ConfigureReconnectCoop(CombatPartner.Get()),
			TEXT("Only transient reconnect copies use bleedout45s and an8s rescue to expose a real interrupted handoff"));
		if (bDone) return;
		CombatPrimary->GetCoopComponent()->bEnableDowned = true;
		CombatPartner->SetActorLocation(ResumeLocation + FVector(150, 0, 0)); CombatPartner->ForceNetUpdate();
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		if (!Damage.IsValid()) { Finish(false, TEXT("Could not prepare real lethal damage")); return; }
		Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000);
		CombatLifecycleAt = FPlatformTime::Seconds();
		ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
		Assert(TEXT("ServerReconnectDownedStarted"), CombatPrimary->GetCoopComponent()->IsDowned() && CombatPrimary->GetHealth() == 1.f,
			TEXT("Real lethal effect entered downed with the still-connected partner available"));
		if (bDone) return;
		CombatPreviousController = PrimaryPlayer(World);
		Probe->CombatTarget = CombatPartner.Get(); PartnerProbe->CombatTarget = CombatPartner.Get();
		CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("ReconnectDownedReady"));
	}
	if (Phase == TEXT("ReconnectDownedVerify") && Both(TEXT("ReconnectDowned")) && Both(TEXT("ReconnectDownedGrowth")))
		SetCommand(TEXT("ReconnectDownedWaitBleedout"));
	if (Phase == TEXT("ReconnectDownedWaitBleedout") && !CombatPrimary->IsAlive())
	{
		const double Elapsed = FPlatformTime::Seconds() - CombatLifecycleAt;
		Assert(TEXT("ServerReconnectDownedBleedout"), !CombatPrimary->GetCoopComponent()->IsDowned() && CombatPrimary->GetHealth() == 0
			&& Elapsed >= ReconnectDownedDuration - .25f && Elapsed < ReconnectDownedDuration + 1.f,
			FString::Printf(TEXT("After actual reconnect, natural bleedout occurred at original %.2fs deadline (not a renewed45s)"), Elapsed));
		if (bDone) return;
		CombatPreviousController = PrimaryPlayer(World); CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("ReconnectDeadBefore"));
	}
	if (Phase == TEXT("ReconnectDeadVerify") && Both(TEXT("ReconnectDead")) && Both(TEXT("ReconnectDeadPersistent")) && Both(TEXT("ReconnectDeadGrowth")))
	{
		Assert(TEXT("ServerReconnectPartnerContinuity"), PartnerPlayer(World) && PartnerPlayer(World) == ReconnectPartnerController.Get()
			&& PartnerPlayer(World)->GetPawn() == CombatPartner.Get() && CombatPartner->IsCombatReady()
			&& World->GetNetDriver() && World->GetNetDriver()->ClientConnections.Num() == 2, TEXT("Partner kept the same live controller/pawn while primary rejoined live, downed and dead"));
		Assert(TEXT("ServerReconnectSharedState"), State && State->GetObjectiveState().ActivatedKeys == 1 && !State->HasGameWon(),
			TEXT("Nondefault shared round progress was not reset by personal reconnect"));
		if (!bDone)
		{
			if (CombatEnemy.IsValid()) CombatEnemy->Destroy();
			CombatEnemy.Reset();
			SetCommand(TEXT("Complete"));
		}
	}
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete")) && Receipts.Contains(TEXT("PartnerComplete")))
		Finish(true, TEXT("Real network reconnect preserved live/dead personal state and shared round"));
#endif
}

// 客户端经历实际断线/重连握手后独立核对恢复；不上传血量、冷却或效果作为权威来源。
void UCoopNetTestDriver::ReconnectClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	auto* PC = Cast<AmultiplayerCoopPlayerController>(World->GetFirstPlayerController());
	auto* Local = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
	auto* Subject = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (!PC || !Local || !Subject || !State || !PC->HasAcceptedCombatSession()
		|| GameInstance->GetConnectionState() != EMultiplayerConnectionState::Connected) return;
	Local->GetCharacterMovement()->DisableMovement();
	const bool Shared = State->GetObjectiveState().ActivatedKeys == 1 && !State->HasGameWon();
	const FString Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	if (Phase == TEXT("ReconnectLoadoutReady") && Shared)
	{
		if (!bPartner && !SentReceipts.Contains(TEXT("ReconnectLoadoutRequested")))
		{
			const auto& Growth = Local->GetProgressionComponent()->GetState();
			if (Growth.Level != 2 || Growth.SkillPoints != 3) return;
			SentReceipts.Add(TEXT("ReconnectLoadoutRequested"));
			Local->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
			Local->GetProgressionComponent()->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
			Local->GetProgressionComponent()->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
		}
		if (Subject->GetMaxHealth() != 125.f || (!bPartner && !HasReconnectGrowth(Local))) return;
		if (!bPartner) SentReceipts.Add(ReconnectProfileMarker(Local));
		SendReceipt(Prefix + TEXT("ReconnectLoadoutReady"), true, bPartner
			? TEXT("Observer received public maxHP125, without relying on remote private progression data")
			: TEXT("Owner learned Dash1/equipped Fortitude1 through real component RPC and recorded its stable server profile"));
	}
	if (Phase == TEXT("ReconnectLiveBefore") && Shared && Subject->GetHealth() == 73 && Subject->IsStunned()
		&& Subject->IsRooted() && Subject->IsBurning() && Subject->GetShield() > 0 && Subject->GetShield() <= ReconnectShield
		&& Subject->GetMaxHealth() == 125.f
		&& (bPartner || (Local->GetEnergy() == 37 && Local->CaptureResumeState().HealCooldown > 0
			&& Local->GetAreaCooldownRemaining() > 0 && Local->GetDashCooldownRemaining() > 0 && HasReconnectGrowth(Local))))
	{
		CombatPrimary = Subject; CombatPartner = Local; CombatPreviousController = PC; ReconnectPartnerController = PC;
		SendReceipt(Prefix + TEXT("ReconnectLiveBefore"), true, TEXT("Admitted owner and observer received live73, shield, Root/Burn/stun, owner37 energy and all four cooldowns"));
	}
	if (Phase == TEXT("ReconnectDeadBefore") && Shared && !Subject->IsAlive() && Subject->GetHealth() == 0
		&& Subject->GetShield() == 0 && !Subject->IsRooted() && !Subject->IsBurning()
		&& (bPartner || Local->GetEnergy() == 37))
	{
		CombatPreviousController = PC;
		SendReceipt(Prefix + TEXT("ReconnectDeadBefore"), true, TEXT("Natural bleedout death replicated before the final real disconnect; owner energy stayed37"));
	}
	if (Phase == TEXT("ReconnectDownedReady") && Shared && Subject->GetCoopComponent()->IsDowned() && Subject->GetHealth() == 1.f)
	{
		if (!ConfigureReconnectCoop(Local) || !ConfigureReconnectCoop(Subject))
		{ Assert(Prefix + TEXT("ReconnectDownedRules"), false, TEXT("Could not create transient downed/rescue observation rules")); return; }
		SendReceipt(Prefix + TEXT("ReconnectDownedReady"), true, TEXT("Both peers see actual downed state and its current deadline before starting old rescue"));
	}
	if (Phase == TEXT("ReconnectDownedRescue") && bPartner && !SentReceipts.Contains(TEXT("ReconnectOldRescueRequested")))
	{
		SentReceipts.Add(TEXT("ReconnectOldRescueRequested")); Local->Rescue();
		SendReceipt(TEXT("PartnerReconnectOldRescueRequested"), true, TEXT("Connected partner invoked the real server-only rescue before primary disconnect"));
	}
	if (Phase == TEXT("ReconnectDownedBefore"))
	{
		auto* Rescuer = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
		if (Shared && Subject->GetCoopComponent()->IsDowned() && Subject->GetHealth() == 1.f && Rescuer && Rescuer->GetCoopComponent()->IsRescuing())
		{
			CombatPreviousController = PC;
			SendReceipt(Prefix + TEXT("ReconnectDownedBefore"), true, TEXT("This connection saw downed plus a real unfinished partner rescue before the network drop"));
		}
	}
	if (!bPartner && (Phase == TEXT("ReconnectLiveDrop") || Phase == TEXT("ReconnectDeadDrop") || Phase == TEXT("ReconnectDownedDrop")))
	{
		const FString Name = Phase == TEXT("ReconnectLiveDrop") ? TEXT("PrimaryReconnectLiveDrop")
			: Phase == TEXT("ReconnectDownedDrop") ? TEXT("PrimaryReconnectDownedDrop") : TEXT("PrimaryReconnectDeadDrop");
		if (SentReceipts.Contains(Name)) return;
		// 等待生产 GI 的低频 Timer 从真实 ACK 累计数据得到样本；不由夹具注入 RTT 或触发采样。
		const auto& Timing = GameInstance->GetConnectionTiming();
		if (Timing.SampleCount == 0) return;
		const FString TimingName = Phase == TEXT("ReconnectLiveDrop") ? TEXT("PrimaryReconnectTimingLive")
			: Phase == TEXT("ReconnectDownedDrop") ? TEXT("PrimaryReconnectTimingDowned") : TEXT("PrimaryReconnectTimingDead");
		SendReceipt(TimingName, Timing.SmoothedRTT > 0 && Timing.RTTVariation >= 0
			&& FMath::IsFinite(Timing.GetRTOSeconds()) && Timing.GetRTOSeconds() >= .25 && Timing.GetRTOSeconds() <= 5.,
			FString::Printf(TEXT("Production ACK-batch sampler: samples=%d SRTT=%.6fs variation=%.6fs RTO=%.6fs; no fixture RTT injection"),
				Timing.SampleCount, Timing.SmoothedRTT, Timing.RTTVariation, Timing.GetRTOSeconds()));
		if (bDone) return;
		SentReceipts.Add(Name);
		UNetDriver* Net = World->GetNetDriver();
		Assert(Name, Net && Net->ServerConnection && GEngine, TEXT("Closing the real server connection; GI, not the fixture, owns retry scheduling"));
		if (bDone) return;
		Probe.Reset();
		Net->ServerConnection->Close(ENetCloseResult::ConnectionLost);
		// Close只关闭socket状态；通过真实引擎错误入口执行其断线清理/返回地图和GI订阅，不直接伪造GI阶段。
		GEngine->BroadcastNetworkFailure(World, Net, ENetworkFailure::ConnectionLost, TEXT("CoopTest intentional real connection close"));
		return;
	}
	if (Phase == TEXT("ReconnectLiveVerify") || Phase == TEXT("ReconnectDeadVerify"))
	{
		const bool Dead = Phase == TEXT("ReconnectDeadVerify");
		if (!Shared || Subject->IsAlive() == Dead || Subject->GetHealth() != (Dead ? 0 : 73)
			|| (!Dead && !Subject->IsStunned()) || (Dead && Subject->IsStunned())) return;
		if (Dead ? (Subject->GetShield() != 0 || Subject->IsRooted() || Subject->IsBurning())
			: (!Subject->IsRooted() || !Subject->IsBurning() || Subject->GetShield() <= 0 || Subject->GetShield() >= ReconnectShield)) return;
		if (Subject->GetMaxHealth() != 125.f || !NoReconnectHalfAction(Subject)) return;
		if (!bPartner && (!HasReconnectGrowth(Local) || !SentReceipts.Contains(ReconnectProfileMarker(Local)))) return;
		if (bPartner)
		{
			SendReceipt(Prefix + (Dead ? TEXT("ReconnectDead") : TEXT("ReconnectLive")),
				PC == ReconnectPartnerController.Get() && Local == CombatPartner.Get() && Local->IsCombatReady()
					&& Subject != CombatPrimary.Get(), TEXT("Same partner controller/pawn stayed connected and observed the replacement primary and shared progress"));
			CombatPrimary = Subject;
			if (!Dead) SendReceipt(TEXT("PartnerReconnectControl"), Subject->IsStunned() && !Subject->IsCasting(),
				TEXT("Still-connected observer received restored control on the replacement pawn, not an old event"));
			SendReceipt(Dead ? TEXT("PartnerReconnectDeadPersistent") : TEXT("PartnerReconnectPersistent"), true,
				Dead ? TEXT("Observer received zero shield and cleared Root/Burn after dead reconnect")
					: TEXT("Observer received public shield and Root/Burn tags on the replacement pawn; no remote GE-duration assumption"));
			SendReceipt(Dead ? TEXT("PartnerReconnectDeadGrowth") : TEXT("PartnerReconnectGrowth"), true,
				TEXT("Observer received public maxHP125 and no resumed dash/rescue, without querying another owner's private growth snapshot"));
		}
		else
		{
			if (Local != Subject || Local->GetEnergy() != 37 || FVector::Dist(Local->GetActorLocation(), ResumeLocation) >= 30) return;
			// 握手回执、属性和死亡RepNotify不是跨对象原子提交；先等本端最终状态齐全再判定。
			if (Dead ? (Local->IsCombatReady() || Local->GetActorEnableCollision()) : !Local->IsCombatReady()) return;
			const FmultiplayerCombatResumeState Stats = Local->CaptureResumeState();
			if (!Dead && (Stats.AttackCooldown <= 0 || Stats.DefenseCooldown <= 0 || Stats.HealCooldown <= 0
				|| Stats.AreaCooldown <= 0 || Stats.StunRemaining <= 0 || Stats.DashCooldown <= 0
				|| Local->GetRootRemaining() <= 0 || Local->GetBurnRemaining() <= 0)) return;
			SendReceipt(Prefix + (Dead ? TEXT("ReconnectDead") : TEXT("ReconnectLive")), PC != CombatPreviousController.Get()
				&& (Dead ? (!Local->IsCombatReady() && !Local->GetActorEnableCollision()) : Local->IsCombatReady()),
				TEXT("New owned controller completed real GI reconnect/handshake; personal values, death/cooldowns, position and shared progress restored"));
			if (!Dead) SendReceipt(TEXT("PrimaryReconnectControl"), Local->IsStunned() && !Local->IsCasting()
				&& Stats.AreaCooldown < 30 && Stats.StunRemaining < 30,
				TEXT("Reconnected owner received actual Area cooldown and elapsed stun GameplayEffects; unfinished casting stayed stopped"));
			SendReceipt(Dead ? TEXT("PrimaryReconnectDeadPersistent") : TEXT("PrimaryReconnectPersistent"),
				Dead ? Stats.PersistentEffects.IsEmpty() : Local->GetRootRemaining() < ReconnectEffectDuration && Local->GetBurnRemaining() < ReconnectEffectDuration,
				Dead ? TEXT("Owner received zero shield and no restored Root/Burn after death")
					: TEXT("Owner received consumed shield and elapsed Root/Burn GameplayEffects through replication, not the reference actor's delayed network values"));
			SendReceipt(Dead ? TEXT("PrimaryReconnectDeadGrowth") : TEXT("PrimaryReconnectGrowth"), true,
				TEXT("Owning replica retained the same profile, level2, Dash1 spec, equipped Fortitude1 and public max125 without restoring half actions"));
			if (!Dead) SendReceipt(TEXT("PrimaryReconnectDashCooldown"), Stats.DashCooldown > 0 && Stats.DashCooldown < 30,
				TEXT("Owner received actual elapsed Dash cooldown GE, not a renewed cooldown or running dash task"));
		}
	}
	if (Phase == TEXT("ReconnectDownedVerify"))
	{
		auto* Rescuer = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
		if (!Shared || !Subject->GetCoopComponent()->IsDowned() || !Subject->IsAlive() || Subject->IsCombatReady()
			|| Subject->GetHealth() != 1.f || Subject->GetMaxHealth() != 125.f || !NoReconnectHalfAction(Subject)
			|| !Rescuer || Rescuer->GetCoopComponent()->IsRescuing() || Subject->GetCoopComponent()->GetDownedRemaining() <= 0
			|| Subject->GetCoopComponent()->GetDownedRemaining() >= ReconnectDownedDuration - 1) return;
		if (bPartner)
		{
			SendReceipt(TEXT("PartnerReconnectDowned"), PC == ReconnectPartnerController.Get() && Local == CombatPartner.Get()
				&& Local->IsCombatReady() && Subject != CombatPrimary.Get(), TEXT("Same partner saw replacement downed pawn and cleared old rescue; remaining deadline was not reset"));
			CombatPrimary = Subject;
			SendReceipt(TEXT("PartnerReconnectDownedGrowth"), true, TEXT("Observer received public max125 after downed reconnect, without reading private growth"));
		}
		else if (Local == Subject && Local->GetEnergy() == 37 && HasReconnectGrowth(Local)
			&& SentReceipts.Contains(ReconnectProfileMarker(Local)) && PC != CombatPreviousController.Get())
		{
			SendReceipt(TEXT("PrimaryReconnectDowned"), true, TEXT("New owned controller restored current downed deadline/HP1/energy37 without half rescue or dash"));
			SendReceipt(TEXT("PrimaryReconnectDownedGrowth"), true, TEXT("Owning downed replica retained stable profile, level2, Dash1 and equipped Fortitude1"));
		}
	}
	if (!bPartner && Phase == TEXT("Complete")) { SendReceipt(TEXT("Complete"), true, TEXT("Reconnect fixture complete")); Finish(true); }
#endif
}
