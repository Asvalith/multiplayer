#include "Testing/CoopNetTestDriver.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
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

namespace
{
	const FVector ResumeLocation(0, 0, 4000);

	bool ApplyReconnectCooldowns(AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		for (TSubclassOf<UGameplayEffect> Class : { UmultiplayerAttackCooldownEffect::StaticClass(),
			UmultiplayerDefendCooldownEffect::StaticClass(), UmultiplayerHealCooldownEffect::StaticClass(), UmultiplayerAreaCooldownEffect::StaticClass() })
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
	FTicketChecks CheckReconnectTickets(UWorld* World)
	{
		FTicketChecks Result;
		auto* Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
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
		CombatPrimary->SetActorLocation(ResumeLocation); CombatPartner->SetActorLocation(FVector(400, 300, 4000));
		CombatPrimary->GetCharacterMovement()->DisableMovement(); CombatPartner->GetCharacterMovement()->DisableMovement();
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 73);
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 37);
		if (!ApplyReconnectCooldowns(CombatPrimary.Get())) { Finish(false, TEXT("Could not apply real reconnect cooldown effects")); return; }
		CombatPrimary->ApplyStun(30);
		ReconnectCooldownAt = FPlatformTime::Seconds();
		FmultiplayerCoopObjectiveState Progress = State->GetObjectiveState(); Progress.ActivatedKeys = 1; Progress.bGameWon = false;
		State->ApplyAuthoritativeState(Progress);
		ReconnectPartnerController = Partner; CombatPreviousController = Primary;
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("ReconnectLiveBefore"));
	}
	if (Phase == TEXT("ReconnectLiveBefore") && Both(TEXT("ReconnectLiveBefore"))) SetCommand(TEXT("ReconnectLiveDrop"));
	if (Phase == TEXT("ReconnectDeadBefore") && Both(TEXT("ReconnectDeadBefore"))) SetCommand(TEXT("ReconnectDeadDrop"));
	if (Phase == TEXT("ReconnectLiveDrop") || Phase == TEXT("ReconnectDeadDrop"))
	{
		auto* PC = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		if (!PC || PC == CombatPreviousController.Get() || !PC->HasAcceptedCombatSession()) return;
		auto* Character = Cast<AmultiplayerCombatCharacter>(PC->GetPawn());
		if (!Character) return;
		const bool Dead = Phase == TEXT("ReconnectDeadDrop");
		Character->GetCharacterMovement()->DisableMovement();
		const bool Common = Character != CombatPrimary.Get() && Character->GetEnergy() == 37
			&& FVector::Dist(Character->GetActorLocation(), ResumeLocation) < 30;
		Assert(Dead ? TEXT("ServerReconnectDead") : TEXT("ServerReconnectLive"), Common
			&& (Dead ? (!Character->IsAlive() && Character->GetHealth() == 0 && !Character->GetActorEnableCollision())
				: (Character->IsCombatReady() && Character->GetHealth() == 73)), TEXT("New admitted controller/pawn restored server resources, life state and position"));
		if (bDone) return;
		if (!Dead)
		{
			const FmultiplayerCombatResumeState Restored = Character->CaptureResumeState();
			const float Expected = 30 - static_cast<float>(FPlatformTime::Seconds() - ReconnectCooldownAt);
			Assert(TEXT("ServerReconnectCooldownElapsed"), Expected > 0 && Restored.AttackCooldown < 29
				&& FMath::Abs(Restored.AttackCooldown - Expected) < .75f && FMath::Abs(Restored.DefenseCooldown - Expected) < .75f
				&& FMath::Abs(Restored.HealCooldown - Expected) < .75f && FMath::Abs(Restored.AreaCooldown - Expected) < .75f,
				FString::Printf(TEXT("All four real cooldowns deduct elapsed online/offline time: expected=%.3f actual=%.3f/%.3f/%.3f/%.3f"),
					Expected, Restored.AttackCooldown, Restored.DefenseCooldown, Restored.HealCooldown, Restored.AreaCooldown));
			Assert(TEXT("ServerReconnectControlElapsed"), Character->IsStunned() && !Character->IsCasting()
				&& FMath::Abs(Restored.StunRemaining - Expected) < .75f,
				TEXT("New pawn restores the actual remaining stun, does not resume a half-cast, and admission does not clear control"));
			if (bDone) return;
		}
		if (Probe.IsValid()) Probe->Destroy();
		FActorSpawnParameters Params; Params.Owner = PC;
		Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
		if (!Probe.IsValid()) { Finish(false, TEXT("Could not replace reconnected owned probe")); return; }
		CombatPrimary = Character; Probe->Subject = Character; PartnerProbe->Subject = Character;
		SetCommand(Dead ? TEXT("ReconnectDeadVerify") : TEXT("ReconnectLiveVerify"));
	}
	if (Phase == TEXT("ReconnectLiveVerify") && Both(TEXT("ReconnectLive")))
	{
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		if (!Damage.IsValid()) { Finish(false, TEXT("Could not prepare real lethal damage")); return; }
		Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000);
		ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
		CombatPreviousController = PrimaryPlayer(World);
		CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("ReconnectDeadBefore"));
	}
	if (Phase == TEXT("ReconnectDeadVerify") && Both(TEXT("ReconnectDead")))
	{
		Assert(TEXT("ServerReconnectPartnerContinuity"), PartnerPlayer(World) && PartnerPlayer(World) == ReconnectPartnerController.Get()
			&& PartnerPlayer(World)->GetPawn() == CombatPartner.Get() && CombatPartner->IsCombatReady()
			&& World->GetNetDriver() && World->GetNetDriver()->ClientConnections.Num() == 2, TEXT("Partner kept the same live controller/pawn while primary rejoined twice"));
		Assert(TEXT("ServerReconnectSharedState"), State && State->GetObjectiveState().ActivatedKeys == 1 && !State->HasGameWon(),
			TEXT("Nondefault shared round progress was not reset by personal reconnect"));
		if (!bDone) SetCommand(TEXT("Complete"));
	}
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete")) && Receipts.Contains(TEXT("PartnerComplete")))
		Finish(true, TEXT("Real network reconnect preserved live/dead personal state and shared round"));
#endif
}

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
	if (Phase == TEXT("ReconnectLiveBefore") && Shared && Subject->GetHealth() == 73 && Subject->IsStunned()
		&& (bPartner || (Local->GetEnergy() == 37 && Local->CaptureResumeState().HealCooldown > 0 && Local->GetAreaCooldownRemaining() > 0)))
	{
		CombatPrimary = Subject; CombatPartner = Local; CombatPreviousController = PC; ReconnectPartnerController = PC;
		SendReceipt(Prefix + TEXT("ReconnectLiveBefore"), true, TEXT("Admitted owner and observer received live73, stun, owner37 energy and all four cooldowns"));
	}
	if (Phase == TEXT("ReconnectDeadBefore") && Shared && !Subject->IsAlive() && Subject->GetHealth() == 0
		&& (bPartner || Local->GetEnergy() == 37))
	{
		CombatPreviousController = PC;
		SendReceipt(Prefix + TEXT("ReconnectDeadBefore"), true, TEXT("Death replicated before the second real disconnect; owner energy stayed37"));
	}
	if (!bPartner && (Phase == TEXT("ReconnectLiveDrop") || Phase == TEXT("ReconnectDeadDrop")))
	{
		const FString Name = Phase == TEXT("ReconnectLiveDrop") ? TEXT("PrimaryReconnectLiveDrop") : TEXT("PrimaryReconnectDeadDrop");
		if (SentReceipts.Contains(Name)) return;
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
		if (bPartner)
		{
			SendReceipt(Prefix + (Dead ? TEXT("ReconnectDead") : TEXT("ReconnectLive")),
				PC == ReconnectPartnerController.Get() && Local == CombatPartner.Get() && Local->IsCombatReady()
					&& Subject != CombatPrimary.Get(), TEXT("Same partner controller/pawn stayed connected and observed the replacement primary and shared progress"));
			CombatPrimary = Subject;
			if (!Dead) SendReceipt(TEXT("PartnerReconnectControl"), Subject->IsStunned() && !Subject->IsCasting(),
				TEXT("Still-connected observer received restored control on the replacement pawn, not an old event"));
		}
		else
		{
			if (Local != Subject || Local->GetEnergy() != 37 || FVector::Dist(Local->GetActorLocation(), ResumeLocation) >= 30) return;
			// 握手回执、属性和死亡RepNotify不是跨对象原子提交；先等本端最终状态齐全再判定。
			if (Dead ? (Local->IsCombatReady() || Local->GetActorEnableCollision()) : !Local->IsCombatReady()) return;
			const FmultiplayerCombatResumeState Stats = Local->CaptureResumeState();
			if (!Dead && (Stats.AttackCooldown <= 0 || Stats.DefenseCooldown <= 0 || Stats.HealCooldown <= 0
				|| Stats.AreaCooldown <= 0 || Stats.StunRemaining <= 0)) return;
			SendReceipt(Prefix + (Dead ? TEXT("ReconnectDead") : TEXT("ReconnectLive")), PC != CombatPreviousController.Get()
				&& (Dead ? (!Local->IsCombatReady() && !Local->GetActorEnableCollision()) : Local->IsCombatReady()),
				TEXT("New owned controller completed real GI reconnect/handshake; personal values, death/cooldowns, position and shared progress restored"));
			if (!Dead) SendReceipt(TEXT("PrimaryReconnectControl"), Local->IsStunned() && !Local->IsCasting()
				&& Stats.AreaCooldown < 30 && Stats.StunRemaining < 30,
				TEXT("Reconnected owner received actual Area cooldown and elapsed stun GameplayEffects; unfinished casting stayed stopped"));
		}
	}
	if (!bPartner && Phase == TEXT("Complete")) { SendReceipt(TEXT("Complete"), true, TEXT("Reconnect fixture complete")); Finish(true); }
#endif
}
