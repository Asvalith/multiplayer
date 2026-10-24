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
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UnrealType.h"

// 扩展能力联机夹具：检验冲刺预测/取消、成长装配与倒地救援的双端契约。
// 只在显式测试中延长观察窗口，正式请求仍走拥有者业务入口，不改正式资产或增加权限后门。

namespace
{
	const FVector ExpansionOrigin(0, 0, 4000);
	// 只延长本场景的可观察窗口，成本/距离仍取生产规则；三端各持瞬态副本，不修改正式资产。
	bool ConfigureExpansion(AmultiplayerCombatCharacter* Character)
	{
		const auto& Current = Character->GetCombatConfig();
		if (Current.GetOuter() == Character) return Current.Dash.Duration == 1.2f && Current.Dash.Cooldown >= 6.f
			&& Current.RescueDuration == 4.f && Current.BleedoutDuration >= 45.f;
		auto* Property = FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("CombatConfig"));
		if (!Property) return false;
		auto* Copy = DuplicateObject<UmultiplayerCombatConfig>(&Current, Character); Copy->SetFlags(RF_Transient);
		Copy->Dash.Duration = 1.2f; Copy->Dash.Cooldown = FMath::Max(6.f, Copy->Dash.Cooldown);
		Copy->RescueDuration = 4.f; Copy->BleedoutDuration = FMath::Max(45.f, Copy->BleedoutDuration);
		Property->SetObjectPropertyValue_InContainer(Character, Copy); return true;
	}
	// 读取真实授予的冲刺主实例，不能通过新建能力对象代替运行中状态。
	UGameplayAbility* DashInstance(AmultiplayerCombatCharacter* Character)
	{
		const auto* Spec = Character->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerDashAbility::StaticClass());
		return Spec ? Spec->GetPrimaryInstance() : nullptr;
	}
	// 能力、状态标签和 CMC 根运动源全部退出才算冲刺收尾完成。
	bool NoDashMotion(AmultiplayerCombatCharacter* Character)
	{
		const auto* Ability = DashInstance(Character);
		return (!Ability || !Ability->IsActive()) && !Character->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing)
			&& !Character->GetCharacterMovement()->HasRootMotionSources();
	}
	// 仅服务器夹具在用例之间重置资源/位置和旧效果，客户端预测断言不走此准备入口。
	void ResetExpansionDash(AmultiplayerCombatCharacter* Character)
	{
		auto* ASC = Character->GetAbilitySystemComponent(); ASC->CancelAllAbilities();
		ASC->RemoveActiveEffects(FGameplayEffectQuery());
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
		Character->SetActorLocation(ExpansionOrigin); Character->ForceNetUpdate();
	}
}

// 新场景使用同一真实 DS + 两个 owning Probe；所有技能请求仍由玩家自己的业务入口发起。
void UCoopNetTestDriver::ExpansionServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (Phase.StartsWith(TEXT("Expansion")) && FPlatformTime::Seconds() - PhaseAt > 30)
	{ Assert(TEXT("ServerExpansionPhaseTimeout"), false, TEXT("Stage exceeded 30 seconds: ") + Phase); return; }
	const auto Both = [this](const FString& Name)
	{ return Receipts.Contains(TEXT("PrimaryExpansion") + Name) && Receipts.Contains(TEXT("PartnerExpansion") + Name); };
	const auto PrepareDash = [this](const TCHAR* Case)
	{
		ResetExpansionDash(CombatPrimary.Get()); CombatMovementOrigin = ExpansionOrigin;
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(FString(TEXT("ExpansionDashReady:")) + Case);
	};
	if (Phase == TEXT("ScenarioStart"))
	{
		CombatPrimary = Cast<AmultiplayerCombatCharacter>(PrimaryPlayer(World)->GetPawn());
		CombatPartner = Cast<AmultiplayerCombatCharacter>(PartnerPlayer(World)->GetPawn());
		Assert(TEXT("ServerExpansionFixture"), CombatPrimary.IsValid() && CombatPartner.IsValid()
			&& ConfigureExpansion(CombatPrimary.Get()) && ConfigureExpansion(CombatPartner.Get()), TEXT("Two admitted players and transient dash observation rules"));
		if (bDone) return;
		const auto* State = World->GetGameState<AmultiplayerCoopGameState>();
		Assert(TEXT("ServerExpansionAdmissionBaseline"), CombatPrimary->IsCombatReady() && CombatPartner->IsCombatReady()
			&& CombatPrimary->GetHealth() == 100.f && CombatPartner->GetHealth() == 100.f
			&& CombatPrimary->GetEnergy() == 100.f && CombatPartner->GetEnergy() == 100.f
			&& State && !State->HasGameLost() && !State->HasGameWon(),
			FString::Printf(TEXT("No pre-fixture damage or terminal-state contamination may be reset away: primary[hp%.2f energy%.2f alive%d ready%d downed%d] partner[hp%.2f energy%.2f alive%d ready%d downed%d] state=%d lost%d won%d"),
				CombatPrimary->GetHealth(), CombatPrimary->GetEnergy(), CombatPrimary->IsAlive(), CombatPrimary->IsCombatReady(), CombatPrimary->GetCoopComponent()->IsDowned(),
				CombatPartner->GetHealth(), CombatPartner->GetEnergy(), CombatPartner->IsAlive(), CombatPartner->IsCombatReady(), CombatPartner->GetCoopComponent()->IsDowned(),
				State != nullptr, State && State->HasGameLost(), State && State->HasGameWon()));
		if (bDone) return;
		for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) It->Destroy();
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
		{
			if (auto* AI = Cast<AAIController>(It->GetController())) { AI->UnPossess(); AI->Destroy(); } It->Destroy();
		}
		for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) It->Destroy();
		auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, 3850), FRotator::ZeroRotator);
		auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		Assert(TEXT("ServerExpansionFloor"), Floor && Cube, TEXT("Actual replicated ground supports CMC walking and root-motion collision"));
		if (bDone) return;
		auto* Mesh = Floor->GetStaticMeshComponent(); Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(Cube); Mesh->SetCollisionProfileName(TEXT("BlockAll")); Mesh->SetIsReplicated(true);
		Floor->SetActorScale3D(FVector(100, 100, 1)); Floor->SetReplicates(true); Floor->SetReplicateMovement(true);
		CombatPrimary->GetCoopComponent()->bEnableDowned = true; CombatPartner->GetCoopComponent()->bEnableDowned = true;
		CombatPartner->SetActorLocation(ExpansionOrigin + FVector(0, 500, 0)); CombatPartner->ForceNetUpdate();
		PrepareDash(TEXT("Run"));
	}
	if (Phase.StartsWith(TEXT("ExpansionDashReady:")))
	{
		const FString Case = Phase.RightChop(19);
		if (!Both(TEXT("Dash") + Case + TEXT("Ready"))) return;
		if (Case == TEXT("Run"))
		{
			const auto& Growth = CombatPrimary->GetProgressionComponent()->GetState();
			Assert(TEXT("ServerExpansionDashLearned"), Growth.ActiveLevels[4] == 1 && Growth.SkillPoints == 1
				&& DashInstance(CombatPrimary.Get()), TEXT("Real owned learn RPC unlocked and granted Dash once; repeated request did not spend another point"));
			if (bDone) return;
			if (bVisualAcceptance)
			{
				const auto& PartnerGrowth = CombatPartner->GetProgressionComponent()->GetState();
				Assert(TEXT("ServerExpansionDashPartnerLearned"), PartnerGrowth.ActiveLevels[4] == 1 && PartnerGrowth.SkillPoints == 1
					&& DashInstance(CombatPartner.Get()), TEXT("Visual partner also learned Dash through its own request before capturing its own usable corridor preview"));
				if (bDone) return;
			}
		}
		CombatMovementOrigin = CombatPrimary->GetActorLocation();
		if (Case == TEXT("Reject"))
		{
			Assert(TEXT("ServerExpansionDashRejectFixture"), HoldCombatStateReplication(World, CombatPrimary.Get()),
				TEXT("Established actor property channel is held; GAS activation and rejection RPCs remain real"));
			if (bDone) return;
			CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 0.f);
		}
		SetCommand(TEXT("ExpansionDash:") + Case); return;
	}
	if (Phase == TEXT("ExpansionDash:Run") || Phase == TEXT("ExpansionDash:Root"))
	{
		const bool Root = Phase.EndsWith(TEXT(":Root")); const FString Case = Root ? TEXT("Root") : TEXT("Run");
		auto* Ability = DashInstance(CombatPrimary.Get());
		if (Ability && Ability->IsActive() && Ability->GetCurrentActivationInfo().GetActivationPredictionKey().WasReceived())
		{
			SentReceipts.Add(TEXT("ServerDashStarted:") + Case);
			if (Root && FVector::Dist2D(CombatPrimary->GetActorLocation(), CombatMovementOrigin) > 40
				&& !SentReceipts.Contains(TEXT("ServerDashRootApplied")))
			{
				SentReceipts.Add(TEXT("ServerDashRootApplied")); CombatPrimary->ApplyRoot(2.f);
				Assert(TEXT("ServerExpansionDashRootInterrupt"), CombatPrimary->IsRooted() && !Ability->IsActive(),
					TEXT("Authoritative root interrupted a received-key dash after actual ground displacement"));
			}
		}
		if (bDone || !SentReceipts.Contains(TEXT("ServerDashStarted:") + Case) || !Both(TEXT("Dash") + Case + TEXT("Ended"))) return;
		const float Travel = FVector::Dist2D(CombatPrimary->GetActorLocation(), CombatMovementOrigin);
		Assert(TEXT("ServerExpansionDash") + Case, NoDashMotion(CombatPrimary.Get()) && !CombatPrimary->IsMovementBlocked()
			&& CombatPrimary->GetCharacterMovement()->IsMovingOnGround() && CombatPrimary->GetEnergy() == 85.f
			&& (Root ? Travel > 40 && Travel < CombatPrimary->GetCombatConfig().Dash.Range - 20
				: Travel >= CombatPrimary->GetCombatConfig().Dash.Range * .7f),
			FString::Printf(TEXT("Received owning GAS request; real CMC travel=%.1fcm, energy85, no remaining dash task/source/permission lock"), Travel));
		if (bDone) return;
		PrepareDash(Root ? TEXT("Reject") : TEXT("Root")); return;
	}
	if (Phase == TEXT("ExpansionDash:Reject"))
	{
		if (!Receipts.Contains(TEXT("PrimaryExpansionDashRejectRejected")) || !Receipts.Contains(TEXT("PrimaryExpansionDashRejectLocalRollback"))) return;
		Assert(TEXT("ServerExpansionDashRejected"), CombatPrimary->GetEnergy() == 0.f && NoDashMotion(CombatPrimary.Get())
			&& !CombatPrimary->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Dash)
			&& FVector::Dist2D(CombatPrimary->GetActorLocation(), CombatMovementOrigin) < 50,
			TEXT("Insufficient authority resource refused the real predicted request without an authoritative dash or cooldown"));
		if (bDone) return;
		ReleaseCombatStateReplication(CombatPrimary.Get()); SetCommand(TEXT("ExpansionDashRollback")); return;
	}
	if (Phase == TEXT("ExpansionDashRollback") && Both(TEXT("DashRollback")))
	{
		ResetExpansionDash(CombatPrimary.Get()); CombatPrimary->SetActorRotation(FRotator(0, 180, 0));
		FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(CombatFixtureEnemyClass(),ExpansionOrigin + FVector(-900, 0, 0), FRotator::ZeroRotator, Params);
		Assert(TEXT("ServerExpansionParryFixture"), CombatEnemy.IsValid(), TEXT("Real hostile emitter, not a predicted client projectile"));
		if (bDone) return;
		if (auto* AI = Cast<AAIController>(CombatEnemy->GetController())) { AI->UnPossess(); AI->Destroy(); }
		CombatEnemy->GetCharacterMovement()->DisableMovement(); CombatEnemy->bAlwaysRelevant = true;
		CombatEnemy->ForceNetUpdate(); CombatPrimary->ForceNetUpdate();
		Probe->CombatTarget = CombatEnemy.Get(); PartnerProbe->CombatTarget = CombatEnemy.Get();
		SetCommand(TEXT("ExpansionParryReady")); return;
	}
	if (Phase == TEXT("ExpansionParryReady") && Both(TEXT("ParryReady")))
	{
		// 服务器真实 GE 延长夹具窗口以隔离 Harsh 命令延迟；Q 自己的短窗口另由功能回归覆盖。
		auto* ASC = CombatPrimary->GetAbilitySystemComponent();
		auto Parry = ASC->MakeOutgoingSpec(UmultiplayerParryEffect::StaticClass(), 1.f, ASC->MakeEffectContext()); Parry.Data->SetDuration(2.f, true);
		ASC->ApplyGameplayEffectSpecToSelf(*Parry.Data.Get());
		CombatAimWorld = World; CombatAimTarget = CombatEnemy.Get(); CombatAimDamageCount = 0;
		CombatAimHealthChanged = CombatEnemy->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetHealthAttribute()).AddWeakLambda(this, [this](const FOnAttributeChangeData& Change)
			{ if (Change.NewValue < Change.OldValue) ++CombatAimDamageCount; });
		const FTransform Transform(FRotator::ZeroRotator, CombatEnemy->GetActorLocation() + FVector(75, 0, 0));
		CombatAimProjectile = World->SpawnActorDeferred<AmultiplayerCombatProjectile>(AmultiplayerCombatProjectile::StaticClass(),
			Transform, CombatEnemy.Get(), CombatEnemy.Get(), ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (CombatAimProjectile.IsValid()) { CombatAimProjectile->Initialize(CombatEnemy.Get(), 25.f, 600.f); CombatAimProjectile->FinishSpawning(Transform); }
		Assert(TEXT("ServerExpansionParryLaunch"), CombatPrimary->IsParrying() && CombatAimProjectile.IsValid()
			&& CombatAimProjectile->GetLaunchState().Revision == 1, TEXT("Authority spawned one actual swept projectile into a real timed parry effect"));
		if (bDone) return;
		Probe->Subject = CombatAimProjectile.Get(); PartnerProbe->Subject = CombatAimProjectile.Get();
		SetCommand(TEXT("ExpansionParryFire")); return;
	}
	if (Phase == TEXT("ExpansionParryFire"))
	{
		if (CombatAimProjectile.IsValid() && CombatAimProjectile->GetLaunchState().Revision == 2
			&& !SentReceipts.Contains(TEXT("ServerParryReflected")))
		{
			SentReceipts.Add(TEXT("ServerParryReflected"));
			Assert(TEXT("ServerExpansionParry"), CombatAimProjectile->GetOwner() == CombatPrimary.Get()
				&& CombatAimProjectile->GetInstigator() == CombatPrimary.Get() && FVector(CombatAimProjectile->GetLaunchState().Velocity).X < 0,
				TEXT("Real collision advanced revision and transferred authority damage source/owner/instigator"));
		}
		if (bDone || !Both(TEXT("ParryDamage"))) return;
		Assert(TEXT("ServerExpansionParrySingleDamage"), SentReceipts.Contains(TEXT("ServerParryReflected")) && CombatAimDamageCount == 1
			&& CombatEnemy->GetHealth() == 75.f && CombatPrimary->GetHealth() == 100.f && !CombatAimProjectile.IsValid(),
			TEXT("Returned swept projectile damaged its original source once; both connections saw the new segment and result"));
		if (bDone) return;
		ClearCombatAimFixture(); Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		auto* Progress = CombatPrimary->GetProgressionComponent(); const FGuid Reward = FGuid::NewGuid();
		const bool Awarded = Progress->AwardExperience(100, Reward); const bool Duplicate = Progress->AwardExperience(100, Reward);
		Assert(TEXT("ServerExpansionGrowthAward"), Awarded && !Duplicate && Progress->GetState().Level == 2
			&& Progress->GetState().Experience == 0 && Progress->GetState().SkillPoints == 2 && Progress->GetState().ClaimedRewards.Num() == 1,
			TEXT("Stable server reward id grants one level/point; a repeated reward cannot grant them again"));
		if (bDone) return;
		SetCommand(TEXT("ExpansionGrowthReady")); return;
	}
	if (Phase == TEXT("ExpansionGrowthReady") && Both(TEXT("GrowthReady")))
	{ SetCommand(TEXT("ExpansionGrowthRequests")); return; }
	if (Phase == TEXT("ExpansionGrowthRequests") && Both(TEXT("GrowthApplied")))
	{
		const auto& Growth = CombatPrimary->GetProgressionComponent()->GetState();
		Assert(TEXT("ServerExpansionGrowthRequests"), Growth.Level == 2 && Growth.SkillPoints == 0
			&& Growth.ActiveLevels[3] == 2 && Growth.PassiveLevels[0] == 1 && Growth.Slots[0] == EMultiplayerCombatSkill::Area
			&& Growth.Slots[3] == EMultiplayerCombatSkill::Attack && Growth.EquippedPassive == EMultiplayerCombatPassive::Fortitude
			&& CombatPrimary->GetMaxHealth() == 125.f && CombatPrimary->GetHealth() == 100.f,
			TEXT("Real owned component RPCs learned/equipped, swapped unique slots and refused over-level/overspend; bonuses did not heal"));
		if (bDone) return;
		CombatPrimary->SetActorLocation(ExpansionOrigin); CombatPartner->SetActorLocation(ExpansionOrigin + FVector(150, 0, 0));
		CombatEnemy->ApplyCombatDamage(CombatPrimary.Get(), 1000.f);
		Assert(TEXT("ServerExpansionDowned"), CombatPrimary->GetCoopComponent()->IsDowned() && CombatPrimary->IsAlive()
			&& !CombatPrimary->IsCombatReady() && CombatPrimary->GetHealth() == 1.f && CombatPrimary->GetCoopComponent()->GetDownedRemaining() > 0,
			TEXT("Real lethal GE entered cooperative downed once, with one HP and a running bleedout deadline"));
		if (bDone) return;
		Probe->CombatTarget = CombatPartner.Get(); PartnerProbe->CombatTarget = CombatPartner.Get();
		CombatPrimary->ForceNetUpdate(); CombatPartner->ForceNetUpdate(); SetCommand(TEXT("ExpansionDowned")); return;
	}
	if (Phase == TEXT("ExpansionDowned") && Both(TEXT("Downed")))
	{ SetCommand(TEXT("ExpansionRescueReady:Cancel")); return; }
	if (Phase.StartsWith(TEXT("ExpansionRescueReady:")))
	{
		if (CombatPartner->GetCoopComponent()->IsRescuing())
			SetCommand(TEXT("ExpansionRescueActive:") + Phase.RightChop(21));
		return;
	}
	if (Phase.StartsWith(TEXT("ExpansionRescueActive:")))
	{
		const FString Case = Phase.RightChop(22);
		if (Case == TEXT("Success"))
		{
			if (!Both(TEXT("RescueSuccessDone"))) return;
			const float Health = CombatPrimary->GetHealth();
			Assert(TEXT("ServerExpansionRescueSuccess"), !CombatPrimary->GetCoopComponent()->IsDowned() && CombatPrimary->IsCombatReady()
				&& !CombatPartner->GetCoopComponent()->IsRescuing() && FMath::IsNearlyEqual(Health, CombatPrimary->GetMaxHealth() * .3f)
				&& !CombatPartner->GetCoopComponent()->CompleteRescue() && CombatPrimary->GetHealth() == Health,
				TEXT("One completed server-only rescue restores 30% HP and releases reservation; a late repeated completion cannot heal twice"));
			if (!bDone) { if (CombatEnemy.IsValid()) CombatEnemy->Destroy(); SetCommand(TEXT("Complete")); }
			return;
		}
		if (!Both(TEXT("Rescue") + Case + TEXT("Active"))) return;
		if (Case == TEXT("Cancel")) SetCommand(TEXT("ExpansionRescueCancel"));
		else
		{
			CombatPartner->ApplyStun(2.f);
			Assert(TEXT("ServerExpansionRescueStun"), CombatPartner->IsStunned() && !CombatPartner->GetCoopComponent()->IsRescuing()
				&& CombatPrimary->GetCoopComponent()->IsDowned(), TEXT("Authoritative stun cancels the actual rescue task without reviving its target"));
			if (!bDone) SetCommand(TEXT("ExpansionRescueStun"));
		}
		return;
	}
	if (Phase == TEXT("ExpansionRescueCancel") && Both(TEXT("RescueCancelStopped")))
	{
		Assert(TEXT("ServerExpansionRescueCancel"), !CombatPartner->GetCoopComponent()->IsRescuing()
			&& CombatPrimary->GetCoopComponent()->IsDowned() && CombatPrimary->GetHealth() == 1.f,
			TEXT("Owned cancel released the real reservation and task without completing the downed target"));
		if (!bDone) SetCommand(TEXT("ExpansionRescueReady:Stun")); return;
	}
	if (Phase == TEXT("ExpansionRescueStun") && Both(TEXT("RescueStunStopped")) && !CombatPartner->IsStunned()
		&& !CombatPartner->IsMovementBlocked() && CombatPartner->GetCharacterMovement()->IsMovingOnGround())
	{ SetCommand(TEXT("ExpansionRescueReady:Success")); return; }
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete")) && Receipts.Contains(TEXT("PartnerComplete")))
		Finish(true, TEXT("Dash prediction/rejection/root, reflection, private progression and cooperative rescue passed independent peer checks"));
#endif
}

// 客户端不写服务器资源/成长/生命；只发真实业务请求并观察自己的复制状态或原生预测键回执。
void UCoopNetTestDriver::ExpansionClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	auto* PC = Cast<AmultiplayerCoopPlayerController>(World->GetFirstPlayerController());
	auto* Local = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
	if (!PC || !Local || !PC->HasAcceptedCombatSession() || GameInstance->GetConnectionState() != EMultiplayerConnectionState::Connected) return;
	const FString Prefix = bPartner ? TEXT("PartnerExpansion") : TEXT("PrimaryExpansion");
	if (!SentReceipts.Contains(Prefix + TEXT("Rules")))
	{
		SendReceipt(Prefix + TEXT("Rules"), ConfigureExpansion(Local), TEXT("Transient Dash duration1.2/CD>=6, Rescue4s/Bleedout>=45s match the server observation fixture; costs/range unchanged"));
		if (bDone) return;
	}
	auto* Subject = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	if (Subject && Subject->GetCombatTeam() == EMultiplayerCombatTeam::Players)
	{
		if (!ConfigureExpansion(Subject)) { Assert(Prefix + TEXT("RemoteRules"), false, TEXT("Remote player's transient observation config could not be prepared")); return; }
		CombatPrimary = Subject;
	}
	if (auto* OtherPlayer = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get()); OtherPlayer && OtherPlayer->GetCombatTeam() == EMultiplayerCombatTeam::Players)
		if (!ConfigureExpansion(OtherPlayer)) { Assert(Prefix + TEXT("RemoteRules"), false, TEXT("Rescuer's transient observation config could not be prepared")); return; }
	auto* ASC = Local->GetAbilitySystemComponent();
	if (Phase.StartsWith(TEXT("ExpansionDashReady:")))
	{
		const FString Case = Phase.RightChop(19);
		const auto Pending = [this, Local, Subject, ASC, &Case](const TCHAR* Why)
		{
			const FString Marker = TEXT("ExpansionDashPending:") + Case;
			if (SentReceipts.Contains(Marker)) return;
			SentReceipts.Add(Marker);
			const auto& Growth = Local->GetProgressionComponent()->GetState();
			FGameplayTagContainer Tags; ASC->GetOwnedGameplayTags(Tags);
			Emit(TEXT("EXPANSION_DASH_PENDING"), FString::Printf(TEXT("%s case=%s local=%s hp%.2f energy%.2f alive%d ready%d downed%d controlled%d move%d grounded%d dashLevel%d points%d spec%d learnRequested%d activeTags=%s subject=%s subjectReady%d subjectHP%.2f"),
				Why, *Case, *Local->GetName(), Local->GetHealth(), Local->GetEnergy(), Local->IsAlive(), Local->IsCombatReady(), Local->GetCoopComponent()->IsDowned(),
				Local->IsLocallyControlled(), int32(Local->GetCharacterMovement()->MovementMode), Local->GetCharacterMovement()->IsMovingOnGround(),
				Local->GetProgressionComponent()->GetSkillLevel(EMultiplayerCombatSkill::Dash), Growth.SkillPoints, DashInstance(Local) != nullptr,
				SentReceipts.Contains(TEXT("DashLearnRequested")), *Tags.ToStringSimple(), *GetNameSafe(Subject), Subject && Subject->IsCombatReady(), Subject ? Subject->GetHealth() : -1.f));
		};
		if (!Subject || !Subject->IsCombatReady() || !NoDashMotion(Subject) || Subject->IsMovementBlocked()) { Pending(TEXT("Subject is not ready")); return; }
		if (Case == TEXT("Run") && (!bPartner || bVisualAcceptance))
		{
			if (!SentReceipts.Contains(TEXT("DashLearnRequested")))
			{
				SentReceipts.Add(TEXT("DashLearnRequested"));
				Local->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
				Local->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
			}
			const auto& Growth = Local->GetProgressionComponent()->GetState();
			if (Growth.ActiveLevels[4] != 1 || Growth.SkillPoints != 1 || !DashInstance(Local)) { Pending(TEXT("Waiting for owned learn result/spec")); return; }
			SendReceipt(Prefix + TEXT("DashLearned"), true, TEXT("Real owned learn requests returned one unlocked Dash level/spec and one remaining point"));
		}
		if (!bPartner && (Local->GetEnergy() != 100.f || ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Dash)
			|| !Local->GetCharacterMovement()->IsMovingOnGround() || FVector::Dist2D(Local->GetActorLocation(), ExpansionOrigin) > 30
			|| !DashInstance(Local))) { Pending(TEXT("Owner resources, ground or origin not ready")); return; }
		if (bVisualAcceptance && Case == TEXT("Run"))
		{
			if (Local->GetEnergy() != 100.f || ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Dash)
				|| !Local->GetCharacterMovement()->IsMovingOnGround() || !DashInstance(Local)) { Pending(TEXT("Visual owner is not ready")); return; }
			if (!CaptureTargetingInput(World, TEXT("TargetingDashReady"), EKeys::LeftShift, EMultiplayerCombatSkill::Dash)) return;
		}
		SendReceipt(Prefix + TEXT("Dash") + Case + TEXT("Ready"), true, TEXT("Prior state cleared; owning pawn is grounded with real energy100 and no dash/cooldown/task"));
		return;
	}
	if (Phase.StartsWith(TEXT("ExpansionDash:")))
	{
		const FString Case = Phase.RightChop(14);
		const FString KeyCase = TEXT("ExpansionDash") + Case;
		if (!bPartner && !SentReceipts.Contains(TEXT("DashInvoked:") + Case))
		{
			SentReceipts.Add(TEXT("DashInvoked:") + Case); PC->SetControlRotation(FRotator::ZeroRotator);
			CombatMovementOrigin = Local->GetActorLocation(); Local->Dash();
			auto* Ability = DashInstance(Local);
			FPredictionKey Key = Ability ? Ability->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
			bool Cost = false, Cooldown = false;
			for (auto Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
			{
				const auto* Effect = ASC->GetActiveGameplayEffect(Handle); if (!Effect || Effect->PredictionKey != Key) continue;
				Cost |= Effect->Spec.Def->IsA<UmultiplayerEnergyCostEffect>(); Cooldown |= Effect->Spec.Def->IsA<UmultiplayerDashCooldownEffect>();
			}
			if (Key.IsLocalClientKey())
			{
				Key.NewRejectedDelegate().BindWeakLambda(this, [this, Case]
				{ SendReceipt(TEXT("PrimaryExpansionDash") + Case + TEXT("Rejected"), Case == TEXT("Reject"), TEXT("Native engine rejected the actual local dash prediction key")); });
				Key.NewCaughtUpDelegate().BindWeakLambda(this, [this, Case] { SentReceipts.Add(TEXT("DashCaughtUp:") + Case); });
			}
			SendReceipt(TEXT("PrimaryExpansionDash") + Case + TEXT("Predicted"), Key.IsLocalClientKey() && Key.WasLocallyGenerated()
				&& Ability && Ability->IsActive() && Cost && Cooldown && Local->GetEnergy() == 85.f,
				FString::Printf(TEXT("Local call returned with key%d, matching cost/CD and energy85 before any server result"), Key.Current));
			if (bDone) return;
			Local->Dash();
			SendReceipt(TEXT("PrimaryExpansionDash") + Case + TEXT("RepeatedInput"), Local->GetEnergy() == 85.f
				&& Ability->GetCurrentActivationInfo().GetActivationPredictionKey() == Key, TEXT("Repeated same-frame input did not charge or start a second dash"));
		}
		if (!Subject) return;
		if (Case == TEXT("Reject"))
		{
			if (!bPartner && SentReceipts.Contains(TEXT("PrimaryExpansionDashRejectRejected")) && Local->GetEnergy() == 100.f
				&& NoDashMotion(Local) && !ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Dash))
				SendReceipt(TEXT("PrimaryExpansionDashRejectLocalRollback"), true,
					TEXT("Before held property state was released, native rejection removed the local task/source/cost/CD and restored stale energy100"));
			return;
		}
		if (Case == TEXT("Run") && Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing)
			&& CaptureSkillView(TEXT("DashActive"),Subject))
			SendReceipt(Prefix + TEXT("DashRunActive"), true, TEXT("This independent connection observed the active dash state"));
		if (Case == TEXT("Root") && Subject->IsRooted())
			SendReceipt(Prefix + TEXT("DashRootObserved"), true, TEXT("This connection received the authoritative root interrupt"));
		const bool Observed = SentReceipts.Contains(Prefix + (Case == TEXT("Root") ? TEXT("DashRootObserved") : TEXT("DashRunActive")));
		if (Observed && NoDashMotion(Subject) && !Subject->IsMovementBlocked() && Subject->GetCharacterMovement()->IsMovingOnGround()
			&& (bPartner || (Local->GetEnergy() == 85.f && SentReceipts.Contains(TEXT("DashCaughtUp:") + Case)))
			&& (Case == TEXT("Root") || FVector::Dist2D(Subject->GetActorLocation(), ExpansionOrigin) >= Subject->GetCombatConfig().Dash.Range * .7f)
			&& (Case != TEXT("Run") || CaptureSkillView(TEXT("DashEnded"),Subject)))
			SendReceipt(Prefix + TEXT("Dash") + Case + TEXT("Ended"), true,
				TEXT("Observed authority result, then normal walking with no dash/root-motion source or movement lock; owning key caught up without double cost"));
		return;
	}
	if (Phase == TEXT("ExpansionDashRollback") && Subject && NoDashMotion(Subject) && !Subject->IsMovementBlocked()
		&& !Subject->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Dash)
		&& (bPartner || Local->GetEnergy() == 0.f))
		SendReceipt(Prefix + TEXT("DashRollback"), true, TEXT("After release, owner received authority energy0; both connections retain no rejected dash state"));
	if (Phase == TEXT("ExpansionParryReady") && Subject && Probe->CombatTarget)
	{
		if (!bPartner) PC->SetControlRotation(FRotator(0, 180, 0));
		if (Subject->IsCombatReady() && Subject->GetHealth() == 100.f && FVector::Dist2D(Subject->GetActorLocation(), ExpansionOrigin) < 30)
			SendReceipt(Prefix + TEXT("ParryReady"), true, TEXT("Both peers received emitter and grounded defender before the server projectile fixture"));
	}
	if (Phase == TEXT("ExpansionParryFire"))
	{
		auto* Projectile = Cast<AmultiplayerCombatProjectile>(Probe->Subject.Get());
		if (Projectile && Projectile->GetLaunchState().Revision == 2 && Projectile->GetOwner() == CombatPrimary.Get()
			&& FVector(Projectile->GetLaunchState().Velocity).X < 0 && !Projectile->IsReplicatingMovement())
			SendReceipt(Prefix + TEXT("ParryObserved"), true, TEXT("Received new revision/owner/velocity and rebuilt reflected segment without per-frame movement replication"));
		auto* Emitter = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
		if (Emitter && Emitter->GetHealth() == 75.f && SentReceipts.Contains(Prefix + TEXT("ParryObserved")))
			SendReceipt(Prefix + TEXT("ParryDamage"), true, TEXT("This connection observed the original hostile emitter lose the reflected projectile's single 25 damage"));
	}
	if (Phase == TEXT("ExpansionGrowthReady") && Subject && Subject->GetMaxHealth() == 105.f)
	{
		const auto& Growth = Subject->GetProgressionComponent()->GetState();
		if (bPartner ? (Growth.Level == 1 && Growth.SkillPoints == 2 && !Growth.ProfileId.IsValid() && Growth.ClaimedRewards.IsEmpty())
			: (Growth.Level == 2 && Growth.SkillPoints == 2 && Growth.Experience == 0 && Growth.ClaimedRewards.Num() == 1))
			SendReceipt(Prefix + TEXT("GrowthReady"), true, bPartner ? TEXT("Public maxHP105 arrived but remote owner-only growth remained default/private")
				: TEXT("Owner received the authoritative level2 reward once"));
	}
	if (Phase == TEXT("ExpansionGrowthRequests") && Subject)
	{
		if (!bPartner && !SentReceipts.Contains(TEXT("GrowthRequestsSent")))
		{
			SentReceipts.Add(TEXT("GrowthRequestsSent")); auto* Progress = Local->GetProgressionComponent();
			Progress->RequestLearnSkill(EMultiplayerCombatSkill::Area); Progress->RequestLearnSkill(EMultiplayerCombatSkill::Area);
			Progress->RequestEquipSkill(0, EMultiplayerCombatSkill::Area); Progress->RequestEquipSkill(0, EMultiplayerCombatSkill::Area);
			for (int32 Attempt = 0; Attempt < 4; ++Attempt) Progress->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
			Progress->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude); Progress->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
			Progress->RequestLearnPassive(EMultiplayerCombatPassive::Power);
		}
		const auto& Growth = Subject->GetProgressionComponent()->GetState();
		const auto* Area = Subject->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		if (Subject->GetMaxHealth() == 125.f && Subject->GetHealth() == 100.f
			&& (bPartner ? (Growth.Level == 1 && Growth.SkillPoints == 2 && Growth.EquippedPassive == EMultiplayerCombatPassive::None)
				: (Growth.Level == 2 && Growth.SkillPoints == 0 && Growth.ActiveLevels[3] == 2 && Growth.PassiveLevels[0] == 1
					&& Growth.Slots[0] == EMultiplayerCombatSkill::Area && Growth.EquippedPassive == EMultiplayerCombatPassive::Fortitude && Area && Area->Level == 2)))
			SendReceipt(Prefix + TEXT("GrowthApplied"), true, bPartner ? TEXT("Observer sees public passive maxHP125 while private growth/loadout remain hidden")
				: TEXT("Owner received learned/equipped levels, unique swapped slots, zero points and actual GAS spec level; repeated requests never overspent"));
	}
	if (Phase == TEXT("ExpansionDowned") && Subject && Subject->GetCoopComponent()->IsDowned() && Subject->IsAlive()
		&& !Subject->IsCombatReady() && Subject->GetHealth() == 1.f && Subject->GetCoopComponent()->GetDownedRemaining() > 0
		&& CaptureSkillView(TEXT("Downed"),Subject,Probe->CombatTarget.Get()))
		SendReceipt(Prefix + TEXT("Downed"), true, TEXT("This independent peer received downed/current deadline, not final death or a replayed event"));
	if (Phase.StartsWith(TEXT("ExpansionRescueReady:")) && bPartner)
	{
		const FString Case = Phase.RightChop(21);
		if (!SentReceipts.Contains(TEXT("RescueRequested:") + Case) && Local->IsCombatReady() && !Local->IsStunned() && !Local->IsMovementBlocked())
		{
			SentReceipts.Add(TEXT("RescueRequested:") + Case);
			if (bVisualAcceptance) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Pressed,1.0)); bEscapeRescueHeld=true; }
			else Local->Rescue();
			SendReceipt(TEXT("PartnerExpansionRescue") + Case + TEXT("Requested"), true,bVisualAcceptance
				? TEXT("FIXTURE: owning player pressed and holds real F through the original input binding; server still must observe the actual GAS rescue")
				: TEXT("Owning partner invoked the real server-only GAS rescue ability"));
		}
	}
	if (Phase.StartsWith(TEXT("ExpansionRescueActive:")))
	{
		const FString Case = Phase.RightChop(22); auto* Rescuer = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
		if (bVisualAcceptance && bPartner && bEscapeRescueHeld && Rescuer && Rescuer->GetCoopComponent()->IsRescuing()
			&& !PC->IsInputKeyDown(EKeys::F)) { Assert(TEXT("PartnerVisualFInput"),false,TEXT("Rescue input was not held down")); return; }
		if (Rescuer && Rescuer->GetCoopComponent()->IsRescuing())
			if (Case != TEXT("Success") || CaptureSkillView(TEXT("RescueActive"),Subject,Rescuer))
			{
				if (bVisualAcceptance && bPartner && Case == TEXT("Success")) SendReceipt(TEXT("PartnerVisualFInput"),true,TEXT("Real PlayerController F is held while the authoritative rescue reservation and progress are active"));
				SendReceipt(Prefix + TEXT("Rescue") + Case + TEXT("Active"), true, TEXT("This connection observed the real reservation and rescue progress"));
			}
		if (Case == TEXT("Success") && Subject && Rescuer && !Subject->GetCoopComponent()->IsDowned() && Subject->IsCombatReady()
			&& FMath::IsNearlyEqual(Subject->GetHealth(), Subject->GetMaxHealth() * .3f) && !Rescuer->GetCoopComponent()->IsRescuing()
			&& SentReceipts.Contains(Prefix + TEXT("RescueSuccessActive")))
		{
			if (bVisualAcceptance && bEscapeRescueHeld) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Released,0.0)); bEscapeRescueHeld=false; }
			if (CaptureSkillView(TEXT("RescueDone"),Subject,Rescuer)) SendReceipt(Prefix + TEXT("RescueSuccessDone"), true, TEXT("This connection observed one finished rescue, 30%HP and cleared rescue state"));
		}
	}
	if (Phase == TEXT("ExpansionRescueCancel") || Phase == TEXT("ExpansionRescueStun"))
	{
		const bool Cancel = Phase == TEXT("ExpansionRescueCancel"); auto* Rescuer = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
		if (Cancel && bPartner && !SentReceipts.Contains(TEXT("RescueCancelSent")))
		{
			SentReceipts.Add(TEXT("RescueCancelSent"));
			if (bVisualAcceptance) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Released,0.0)); bEscapeRescueHeld=false; }
			else Local->CancelRescue();
			SendReceipt(TEXT("PartnerExpansionRescueCancelSent"), true, TEXT("Owning partner sent real rescue cancellation, without server fixture ending the ability"));
		}
		if (Subject && Rescuer && Subject->GetCoopComponent()->IsDowned() && Subject->GetHealth() == 1.f
			&& !Rescuer->GetCoopComponent()->IsRescuing() && (Cancel || Rescuer->IsStunned()))
		{
			if (bVisualAcceptance && bEscapeRescueHeld) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Released,0.0)); bEscapeRescueHeld=false; }
			SendReceipt(Prefix + (Cancel ? TEXT("RescueCancelStopped") : TEXT("RescueStunStopped")), true,
				TEXT("Cancelled/interrupted rescue cleared replicated reservation and left the target downed, with no partial revive"));
		}
	}
	if (!bPartner && Phase == TEXT("Complete")) { SendReceipt(TEXT("Complete"), true, TEXT("Combat expansion scenario complete; no rendered animation or UI claim")); Finish(true); }
#endif
}
