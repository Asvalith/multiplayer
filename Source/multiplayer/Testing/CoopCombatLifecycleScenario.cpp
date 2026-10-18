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
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	const FVector LifecycleOrigin(0, 0, 4000);
	constexpr float LifecycleWindupSeconds = 3.0f;
	const FRotator LifecycleAim(0, 45, 0), LifecycleOldAim(0, -90, 0);
	constexpr float LifecycleAimDistance = 900.0f, LifecycleTargetSpeed = 200.0f;

	// 仅本模式使用角色持有的瞬态配置副本：留出弱网确认窗口，并让冷却覆盖完整前后摇与观察余量。
	// 不修改正式资产或全局 CDO，Recovery 仍采用角色实际配置。
	bool ConfigureLifecycleRules(AmultiplayerCombatCharacter* Character)
	{
		const auto& Current = Character->GetCombatConfig();
		if (Current.GetOuter() == Character) return FMath::IsNearlyEqual(Current.Area.Windup, LifecycleWindupSeconds)
			&& Current.Area.Cooldown >= LifecycleWindupSeconds + Current.Area.Recovery + 1.0f;
		auto* Property = FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("CombatConfig"));
		if (!Property) return false;
		auto* Copy = DuplicateObject<UmultiplayerCombatConfig>(&Current, Character);
		Copy->SetFlags(RF_Transient);
		Copy->Area.Windup = LifecycleWindupSeconds;
		Copy->Area.Cooldown = FMath::Max(Copy->Area.Cooldown, Copy->Area.Windup + Copy->Area.Recovery + 1.0f);
		Property->SetObjectPropertyValue_InContainer(Character, Copy);
		return &Character->GetCombatConfig() == Copy;
	}

	// 只读故障快照：分清请求未到、提前结束、朝向/落点偏移和目标失效，不修改玩法结果。
	FString DescribeLifecycleState(AmultiplayerCombatCharacter* Character, AmultiplayerCombatCharacter* Target)
	{
		if (!Character) return TEXT("Source pawn is unavailable");
		const auto* ASC = Character->GetAbilitySystemComponent();
		const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		const auto* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		const FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		const auto* Controller = Character->GetController();
		return FString::Printf(TEXT("source=%s active=%d key=%d received=%d casting=%d stunned=%d ready=%d energy=%.2f cd=%.3f mode=%d sourceLocation=%s controllerYaw=%.2f currentAim=%s windup=%.2f recovery=%.2f target=%s targetHP=%.2f targetReady=%d targetStunned=%d targetLocation=%s"),
			*Character->GetName(), Instance && Instance->IsActive(), Key.Current, Key.WasReceived(),
			Character->IsCasting(), Character->IsStunned(), Character->IsCombatReady(), Character->GetEnergy(),
			Character->GetAreaCooldownRemaining(), int32(Character->GetCharacterMovement()->MovementMode),
			*Character->GetActorLocation().ToString(), Controller ? Controller->GetControlRotation().Yaw : Character->GetActorRotation().Yaw,
			*Character->GetAreaTarget().ToString(), Character->GetCombatConfig().Area.Windup, Character->GetCombatConfig().Area.Recovery,
			Target ? *Target->GetName() : TEXT("None"), Target ? Target->GetHealth() : -1.0f,
			Target && Target->IsCombatReady(), Target && Target->IsStunned(), Target ? *Target->GetActorLocation().ToString() : TEXT("None"));
	}

	// 本模式只清理服务器夹具；不在客户端删预测效果或改资源来伪造结果。
	void ResetLifecycleCharacter(AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		ASC->CancelAllAbilities();
		ASC->RemoveActiveEffects(FGameplayEffectQuery());
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 100);
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100);
		// 玩家保持业务代码恢复的移动模式；只冻结目标，不能掩盖登录或施法后的禁行残留。
		if (Character->GetCombatTeam() == EMultiplayerCombatTeam::Enemies)
			Character->GetCharacterMovement()->DisableMovement();
		Character->ForceNetUpdate();
	}
}

// 所有观察只存在于测试场景；不向业务 Actor 增加测试 RPC 或发射后门。
void UCoopNetTestDriver::ClearCombatAimFixture()
{
	FWorldDelegates::OnWorldPreActorTick.Remove(CombatAimControlTick);
	CombatAimControlTick.Reset();
	if (UWorld* World = CombatAimWorld.Get()) World->RemoveOnActorSpawnedHandler(CombatAimSpawned);
	CombatAimSpawned.Reset();
	if (auto* Target = CombatAimTarget.Get())
		Target->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(CombatAimHealthChanged);
	CombatAimHealthChanged.Reset();
	CombatAimTarget.Reset(); CombatAimProjectile.Reset(); CombatAimWorld.Reset();
}

// 在第二个客户端进程获准连接前创建已受控敌人，晚加入必须恢复当前标签而非依赖历史事件。
void UCoopNetTestDriver::PrepareLifecycleLateJoin(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) It->Destroy();
	for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
	{
		if (auto* AI = Cast<AAIController>(It->GetController())) { AI->UnPossess(); AI->Destroy(); }
		It->Destroy();
	}
	for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) It->Destroy();
	// 落点由生产代码的射线求得；复制实际地板让两端都能执行正常瞄准，不绕过施法入口。
	auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, 3850), FRotator::ZeroRotator);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Assert(TEXT("ServerLifecycleFloor"), Floor && Cube, TEXT("Replicated floor fixture uses an existing engine mesh"));
	if (bDone) return;
	Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->GetStaticMeshComponent()->SetIsReplicated(true);
	Floor->SetActorScale3D(FVector(100, 100, 1));
	Floor->SetReplicates(true); Floor->SetReplicateMovement(true); Floor->ForceNetUpdate();
	FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CombatEnemy = World->SpawnActor<AmultiplayerCombatEnemy>(CombatFixtureEnemyClass(),LifecycleOrigin + FVector(650, 0, 0), FRotator::ZeroRotator, Params);
	Assert(TEXT("ServerLifecycleLateState"), CombatEnemy.IsValid(), TEXT("Control fixture exists before primary connection"));
	if (bDone) return;
	if (auto* AI = Cast<AAIController>(CombatEnemy->GetController())) { AI->UnPossess(); AI->Destroy(); }
	CombatEnemy->ApplyStun(120);
	CombatEnemy->GetCharacterMovement()->DisableMovement();
	CombatEnemy->ForceNetUpdate();
#endif
}

// 施法与瞄准阶段通过真实所属玩家 GAS 请求执行；阶段转换等双连接回执，不靠服务器结果替代客户端观察。
void UCoopNetTestDriver::LifecycleServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (Phase.StartsWith(TEXT("Lifecycle")) && FPlatformTime::Seconds() - PhaseAt > 15)
	{
		Assert(TEXT("ServerLifecyclePhaseTimeout"), false, FString::Printf(TEXT("Stage %s exceeded 15 seconds. %s"),
			*Phase, *DescribeLifecycleState(CombatPrimary.Get(), CombatEnemy.Get())));
		return;
	}
	const auto Both = [this](const FString& Suffix)
	{ return Receipts.Contains(TEXT("Primary") + Suffix) && Receipts.Contains(TEXT("Partner") + Suffix); };
	const auto PrepareMovement = [this](const FString& Case)
	{
		// 只将夹具移回宽地板；不设置 Walking、不添加输入、不替业务代码解锁。
		CombatPrimary->SetActorLocation(LifecycleOrigin);
		CombatPartner->SetActorLocation(LifecycleOrigin + FVector(0, 500, 0));
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("LifecycleMoveReady:") + Case);
	};
	const auto Prepare = [this, World](const TCHAR* Case)
	{
		for (auto* Character : {CombatPrimary.Get(), CombatPartner.Get(), static_cast<AmultiplayerCombatCharacter*>(CombatEnemy.Get())})
			ResetLifecycleCharacter(Character);
		CombatPrimary->SetActorLocation(LifecycleOrigin);
		CombatPartner->SetActorLocation(LifecycleOrigin + FVector(0, 500, 0));
		CombatEnemy->SetActorLocation(LifecycleOrigin + FVector(CombatPrimary->GetCombatConfig().Area.Range, 0, 0));
		PrimaryPlayer(World)->SetControlRotation(FRotator::ZeroRotator);
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		Probe->CombatTarget = CombatEnemy.Get(); PartnerProbe->CombatTarget = CombatEnemy.Get();
		SetCommand(FString(TEXT("LifecycleReady:")) + Case);
	};
	if (Phase == TEXT("ScenarioStart"))
	{
		CombatPrimary = Cast<AmultiplayerCombatCharacter>(PrimaryPlayer(World)->GetPawn());
		CombatPartner = Cast<AmultiplayerCombatCharacter>(PartnerPlayer(World)->GetPawn());
		Assert(TEXT("ServerLifecycleFixture"), CombatPrimary.IsValid() && CombatPartner.IsValid() && CombatEnemy.IsValid()
			&& CombatEnemy->IsStunned(), TEXT("Two admitted players and the enemy controlled before primary joined"));
		if (bDone) return;
		Assert(TEXT("ServerLifecycleRules"), ConfigureLifecycleRules(CombatPrimary.Get())
			&& ConfigureLifecycleRules(CombatPartner.Get()) && ConfigureLifecycleRules(CombatEnemy.Get()),
			TEXT("Only transient per-character config copies use Area.Windup=3 seconds and cooldown>=windup+actual recovery+1 second; no asset or CDO modified"));
		if (bDone) return;
		Probe->Subject = CombatEnemy.Get(); PartnerProbe->Subject = CombatEnemy.Get();
		SetCommand(TEXT("LifecycleLateState"));
	}
	if (Phase == TEXT("LifecycleLateState") && Both(TEXT("LifecycleLateState"))) PrepareMovement(TEXT("Join"));
	if (Phase.StartsWith(TEXT("LifecycleMoveReady:")))
	{
		const FString Case = Phase.RightChop(19);
		if (Both(TEXT("Movement") + Case + TEXT("Ready")))
		{
			CombatMovementOrigin = CombatPrimary->GetActorLocation();
			CombatPartnerMovementOrigin = CombatPartner->GetActorLocation();
			SetCommand(TEXT("LifecycleMove:") + Case);
		}
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleMove:")))
	{
		const FString Case = Phase.RightChop(14);
		if (!Both(TEXT("Movement") + Case)) return;
		const float PrimaryDistance = CombatPrimary->GetActorLocation().X - CombatMovementOrigin.X;
		const float PartnerDistance = CombatPartner->GetActorLocation().X - CombatPartnerMovementOrigin.X;
		Assert(TEXT("ServerMovement") + Case, PrimaryDistance >= 150 && PartnerDistance >= 150,
			FString::Printf(TEXT("Actual owning-client W input moved both authoritative pawns: primary %.1fcm, partner %.1fcm; no SetWalking or direct movement input"),
				PrimaryDistance, PartnerDistance));
		if (bDone) return;
		if (Case == TEXT("Join")) Prepare(TEXT("Release"));
		else if (Case == TEXT("Release")) Prepare(TEXT("Cancel"));
		else if (Case == TEXT("Cancel")) Prepare(TEXT("ConfirmedCancel"));
		else if (Case == TEXT("ConfirmedCancel")) Prepare(TEXT("Stun"));
		else SetCommand(TEXT("LifecycleAimPrepare:Snapshot"));
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleAimPrepare:")))
	{
		const FString Case = Phase.RightChop(20);
		ClearCombatAimFixture();
		for (auto* Character : {CombatPrimary.Get(), static_cast<AmultiplayerCombatCharacter*>(CombatEnemy.Get())})
			ResetLifecycleCharacter(Character);
		CombatPrimary->SetActorLocation(LifecycleOrigin);
		CombatPartner->SetActorLocation(LifecycleOrigin + FVector(-500, 0, 0));
		const FVector Right = FRotationMatrix(LifecycleAim).GetUnitAxis(EAxis::Y);
		const float Lead = Case == TEXT("Moving")
			? LifecycleAimDistance / CombatPrimary->GetCombatConfig().ProjectileSpeed * LifecycleTargetSpeed : 0;
		CombatAimTargetOrigin = LifecycleOrigin + LifecycleAim.Vector() * LifecycleAimDistance - Right * Lead;
		CombatEnemy->SetActorLocation(CombatAimTargetOrigin);
		CombatEnemy->ForceNetUpdate(); CombatPrimary->ForceNetUpdate(); CombatPartner->ForceNetUpdate();
		const auto AimPreview = CombatPrimary->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &LifecycleAim);
		CombatAimExpectedDirection = (AimPreview.End - AimPreview.Origin).GetSafeNormal();
		CombatAimWorld = World; CombatAimTarget = CombatEnemy.Get();
		CombatAimProjectileCount = CombatAimDamageCount = 0; CombatAimHitTime = 0;
		CombatAimHealthChanged = CombatEnemy->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetHealthAttribute()).AddWeakLambda(this, [this, World](const FOnAttributeChangeData& Change)
			{
				if (Change.NewValue < Change.OldValue) { ++CombatAimDamageCount; CombatAimHitTime = World->GetTimeSeconds(); }
			});
		// 持续保持故意错误的服务器朝向，不让另一路 CMC 朝向包碰巧补正测试结果。
		PrimaryPlayer(World)->SetControlRotation(LifecycleOldAim);
		CombatAimControlTick = FWorldDelegates::OnWorldPreActorTick.AddWeakLambda(this,
			[this, World](UWorld* TickWorld, ELevelTick, float)
			{
				if (TickWorld == World)
					if (APlayerController* PC = PrimaryPlayer(World)) PC->SetControlRotation(LifecycleOldAim);
			});
		CombatAimSpawned = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateWeakLambda(this,
			[this, World, Case, Right](AActor* Actor)
			{
				auto* Projectile = Cast<AmultiplayerCombatProjectile>(Actor);
				if (!Projectile || Actor->GetOwner() != CombatPrimary.Get()) return;
				CombatAimProjectile = Projectile; ++CombatAimProjectileCount;
				const float ServerYaw = PrimaryPlayer(World)->GetControlRotation().Yaw;
				Assert(TEXT("ServerAim") + Case + TEXT("Direction"), CombatAimProjectileCount == 1
					&& FMath::Abs(FMath::FindDeltaAngleDegrees(ServerYaw, LifecycleOldAim.Yaw)) < 1
					&& Actor->GetActorForwardVector().Equals(CombatAimExpectedDirection, .001f),
					FString::Printf(TEXT("Actual spawn converged to the per-activation preview while server controller remained %.2f degrees; count=%d"),
						ServerYaw, CombatAimProjectileCount));
				if (bDone || Case != TEXT("Moving")) return;
				// 仅在真实出弹后启动恒速横移，消除测试命令网络延迟对交点的影响。
				// CMC 真正积分位置；运动期间不瞬移、不直接调用命中或伤害函数。
				auto* Movement = CombatEnemy->GetCharacterMovement();
				Movement->bRunPhysicsWithNoController = true;
				Movement->bForceMaxAccel = true;
				Movement->MaxWalkSpeed = LifecycleTargetSpeed;
				Movement->SetMovementMode(MOVE_Walking);
				Movement->Velocity = Right * LifecycleTargetSpeed;
			}));
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		Probe->CombatTarget = CombatEnemy.Get(); PartnerProbe->CombatTarget = CombatEnemy.Get();
		SetCommand(TEXT("LifecycleAimReady:") + Case);
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleAimReady:")))
	{
		const FString Case = Phase.RightChop(18);
		if (Both(TEXT("Aim") + Case + TEXT("Ready"))) SetCommand(TEXT("LifecycleAimFire:") + Case);
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleAimFire:")))
	{
		const FString Case = Phase.RightChop(17);
		if (CombatAimProjectile.IsValid() && CombatAimProjectile->GetLaunchState().bInitialized
			&& !SentReceipts.Contains(TEXT("Launch:") + Case))
		{
			SentReceipts.Add(TEXT("Launch:") + Case);
			const auto& Launch = CombatAimProjectile->GetLaunchState();
			Assert(TEXT("ServerAim") + Case + TEXT("Launch"), FVector(Launch.Velocity).GetSafeNormal().Equals(CombatAimExpectedDirection, .001f)
				&& FMath::IsNearlyEqual(FVector(Launch.Velocity).Size(), CombatPrimary->GetCombatConfig().ProjectileSpeed, 1),
				TEXT("Initialized authoritative launch state contains the captured preview convergence direction and configured speed"));
			if (bDone) return;
		}
		if (CombatAimDamageCount == 0) return;
		if (Case == TEXT("Moving") && !SentReceipts.Contains(TEXT("MovementStopped:Moving")))
		{
			SentReceipts.Add(TEXT("MovementStopped:Moving"));
			const float Travel = FVector::DotProduct(CombatEnemy->GetActorLocation() - CombatAimTargetOrigin,
				FRotationMatrix(LifecycleAim).GetUnitAxis(EAxis::Y));
			Assert(TEXT("ServerAimMovingCMC"), Travel > CombatEnemy->GetCapsuleComponent()->GetScaledCapsuleRadius() * 2
				&& CombatEnemy->GetCharacterMovement()->MovementMode == MOVE_Walking,
				FString::Printf(TEXT("CMC moved the target laterally %.2fcm before the real hit; no runtime teleport or historical rewind"), Travel));
			CombatEnemy->GetCharacterMovement()->DisableMovement(); CombatEnemy->ForceNetUpdate();
			if (bDone) return;
		}
		if (!Both(TEXT("Aim") + Case + TEXT("Outcome")) || !Receipts.Contains(TEXT("PrimaryAim") + Case + TEXT("Predicted"))
			|| !SentReceipts.Contains(TEXT("Launch:") + Case) || World->GetTimeSeconds() - CombatAimHitTime < .5f) return;
		const auto* AttackSpec = CombatPrimary->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass());
		const auto* Attack = AttackSpec ? AttackSpec->GetPrimaryInstance() : nullptr;
		Assert(TEXT("ServerAim") + Case + TEXT("SingleDamage"), CombatAimProjectileCount == 1 && CombatAimDamageCount == 1
			&& Attack && Attack->GetCurrentActivationInfo().GetActivationPredictionKey().WasReceived()
			&& CombatPrimary->GetEnergy() == 100 - CombatPrimary->GetCombatConfig().Attack.Cost
			&& CombatEnemy->GetHealth() == 100 - CombatPrimary->GetCombatConfig().Attack.Amount,
			TEXT("One owned predicted activation created one real projectile and one authoritative damage settlement; both independent clients observed it"));
		if (bDone) return;
		ClearCombatAimFixture();
		SetCommand(Case == TEXT("Snapshot") ? TEXT("LifecycleAimPrepare:Moving") : TEXT("Complete"));
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleReady:")))
	{
		const FString Case = Phase.RightChop(15);
		if (Both(TEXT("Area") + Case + TEXT("Ready"))) SetCommand(TEXT("Lifecycle") + Case);
		return;
	}
	if (Phase == TEXT("LifecycleRelease") || Phase == TEXT("LifecycleCancel")
		|| Phase == TEXT("LifecycleConfirmedCancel") || Phase == TEXT("LifecycleStun"))
	{
		const FString Case = Phase.RightChop(9);
		UAbilitySystemComponent* ASC = CombatPrimary->GetAbilitySystemComponent();
		if (Phase == TEXT("LifecycleStun") && CombatPrimary->IsCasting())
		{
			if (bVisualAcceptance && !Both(TEXT("VisualAreaStunCasting"))) return;
			CombatPrimary->ApplyStun(5);
			Assert(TEXT("ServerAreaStunInterrupt"), CombatPrimary->IsStunned() && !CombatPrimary->IsCasting(),
				TEXT("Server control effect interrupted the actual active ability before release"));
			if (bDone) return;
			CombatLifecycleAt = FPlatformTime::Seconds();
			SetCommand(TEXT("LifecycleStunObserve"));
			return;
		}
		if (Case == TEXT("Release") && CombatEnemy->GetHealth() < 100 - CombatPrimary->GetCombatConfig().Area.Amount)
		{ Assert(TEXT("ServerAreaSingleDamage"), false, TEXT("Area produced more than one settlement")); return; }
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		const UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		const bool ReceivedKey = Instance && Instance->GetCurrentActivationInfo().GetActivationPredictionKey().WasReceived();
		if (Instance && Instance->IsActive() && !SentReceipts.Contains(TEXT("ActiveState:") + Case))
		{
			SentReceipts.Add(TEXT("ActiveState:") + Case);
			Emit(TEXT("DIAGNOSTIC"), TEXT("First observed server active ability: ") + DescribeLifecycleState(CombatPrimary.Get(), CombatEnemy.Get()));
		}
		if (Case == TEXT("Release") && ReceivedKey && !CombatPrimary->IsCasting()
			&& (!bVisualAcceptance || Both(TEXT("VisualAreaReleaseCasting")))
			&& (!bVisualAcceptance || Both(TEXT("AreaReleaseEffectObserved")))
			&& CombatEnemy->GetHealth() == 100 - CombatPrimary->GetCombatConfig().Area.Amount)
		{
			Assert(TEXT("ServerAreaSingleDamage"), CombatPrimary->GetEnergy() == 100 - CombatPrimary->GetCombatConfig().Area.Cost,
				TEXT("Received client prediction key; one authoritative damage and one resource commitment"));
			SetCommand(TEXT("LifecycleVerify:Release"));
		}
		else if ((Case == TEXT("Cancel") || Case == TEXT("ConfirmedCancel"))
			&& Receipts.Contains(TEXT("PrimaryArea") + Case + TEXT("Sent")) && ReceivedKey && !CombatPrimary->IsCasting())
		{
			const float Remaining = CombatPrimary->GetAreaCooldownRemaining();
			const auto& Rule = CombatPrimary->GetCombatConfig().Area;
			if (!SentReceipts.Contains(TEXT("CooldownRetained:") + Case))
			{
				SentReceipts.Add(TEXT("CooldownRetained:") + Case);
				Assert(TEXT("ServerArea") + Case + TEXT("CooldownRetained"), Remaining > 0,
					TEXT("Server ended casting without deleting the committed Area cooldown"));
				if (bDone) return;
			}
			// 从真实冷却的剩余时间反推提交时刻，不能用命令发出时刻冒充服务器收到请求的时刻。
			if (Rule.Cooldown - Remaining < Rule.Windup + Rule.Recovery + .2f) return;
			Assert(TEXT("ServerArea") + Case + TEXT("NoDamage"), CombatEnemy->GetHealth() == 100
				&& CombatPrimary->GetEnergy() == 100 - CombatPrimary->GetCombatConfig().Area.Cost,
				TEXT("Owned GAS cancellation reached the server; original release deadline passed without damage or refund"));
			if (!bDone) SetCommand(TEXT("LifecycleVerify:") + Case);
		}
	}
	if (Phase == TEXT("LifecycleStunObserve") && Both(TEXT("AreaStunObserved"))
		&& FPlatformTime::Seconds() - CombatLifecycleAt > CombatPrimary->GetCombatConfig().Area.Windup + .5f)
	{
		Assert(TEXT("ServerAreaStunNoDamage"), !CombatPrimary->IsCasting() && CombatEnemy->GetHealth() == 100,
			TEXT("The cancelled server release timer did not execute after stun"));
		if (!bDone) SetCommand(TEXT("LifecycleVerify:Stun"));
	}
	if (Phase.StartsWith(TEXT("LifecycleVerify:")))
	{
		const FString Case = Phase.RightChop(16);
		if (!Both(TEXT("Area") + Case + TEXT("Outcome"))) return;
		PrepareMovement(Case);
	}
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete")) && Receipts.Contains(TEXT("PartnerComplete")))
		Finish(true, TEXT("Real W/CMC movement, Area lifecycle/late state and per-activation Attack aim passed; static and CMC-lateral targets received one damage observed by both clients; not historical rewind or visual acceptance"));
#endif
}

// Release 走正式 R 按键/松开并在输入帧读取预测；其余注入分支保留同调用栈的边界覆盖。
void UCoopNetTestDriver::LifecycleClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	auto* PC = Cast<AmultiplayerCoopPlayerController>(World->GetFirstPlayerController());
	auto* Local = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
	auto* Subject = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	auto* Target = Cast<AmultiplayerCombatCharacter>(Probe->CombatTarget.Get());
	if (!Local || !Subject || !PC->HasAcceptedCombatSession()) return;
	const FString Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	if (Phase.StartsWith(TEXT("Lifecycle")) && FPlatformTime::Seconds() - PhaseAt > 15)
	{
		Assert(Prefix + TEXT("LifecyclePhaseTimeout"), false, FString::Printf(TEXT("Stage %s exceeded 15 seconds. %s"),
			*Phase, *DescribeLifecycleState(Subject, Target)));
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleMoveReady:")))
	{
		const FString Case = Phase.RightChop(19);
		const FVector Expected = LifecycleOrigin + (bPartner ? FVector(0, 500, 0) : FVector::ZeroVector);
		// 眩晕必须由真实效果到期解除；不在测试里删效果或强制恢复移动。
		if (Local->IsCombatReady() && !Local->IsCasting() && !Local->IsStunned()
			&& FVector::Dist(Local->GetActorLocation(), Expected) < 30)
		{
			PC->SetControlRotation(FRotator::ZeroRotator);
			SendReceipt(Prefix + TEXT("Movement") + Case + TEXT("Ready"), true,
				TEXT("Admitted live pawn is at the replicated floor fixture with no casting/stun; movement mode is not modified by the test"));
		}
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleMove:")))
	{
		const FString Case = Phase.RightChop(14), Marker = TEXT("Command:") + Phase;
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!SentReceipts.Contains(Marker))
		{
			Assert(Prefix + TEXT("ViewportInput") + Case, Viewport && !Viewport->IgnoreInput() && !PC->bShowMouseCursor,
				TEXT("Combat HUD restored game viewport input after the connection menu; no test-side input mode reset"));
			if (bDone) return;
			SentReceipts.Add(Marker);
			CombatMovementOrigin = Local->GetActorLocation();
			CombatMovementAt = FPlatformTime::Seconds();
			bCombatMovementInputHeld = true;
			// 经 PC/PlayerInput/EnhancedInput 的真实 W 映射，再由角色 Move 和 CMC 驱动。
			// 从视口入口提交，UIOnly 残留必须使测试失败，不能绕过视口直接喂给 PC。
			Viewport->InputKey(FInputKeyEventArgs(Viewport->Viewport,
				IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC->GetLocalPlayer()->GetPlatformUserId()),
				EKeys::W, IE_Pressed));
		}
		if (PC->IsInputKeyDown(EKeys::W)) SentReceipts.Add(TEXT("MovementKeyDown:") + Case);
		if (bCombatMovementInputHeld && FPlatformTime::Seconds() - CombatMovementAt >= 1.25)
		{
			Viewport->InputKey(FInputKeyEventArgs(Viewport->Viewport,
				IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC->GetLocalPlayer()->GetPlatformUserId()),
				EKeys::W, IE_Released));
			bCombatMovementInputHeld = false;
			CombatMovementAt = FPlatformTime::Seconds();
		}
		if (!bCombatMovementInputHeld && FPlatformTime::Seconds() - CombatMovementAt >= .5)
		{
			const float Distance = Local->GetActorLocation().X - CombatMovementOrigin.X;
			SendReceipt(Prefix + TEXT("Movement") + Case,
				SentReceipts.Contains(TEXT("MovementKeyDown:") + Case) && Distance >= 150,
				FString::Printf(TEXT("Pressed/released real W for 1.25s; local forward travel %.1fcm, movement mode %d; no teleport or SetWalking during input"),
					Distance, int32(Local->GetCharacterMovement()->MovementMode)));
		}
		return;
	}
	if (Phase == TEXT("LifecycleLateState"))
	{
		if (Subject->IsStunned() && Subject->IsAlive()) SendReceipt(Prefix + TEXT("LifecycleLateState"), true,
			bPartner ? TEXT("Observer sees current enemy stun") : TEXT("New connection received a pre-existing stun without its application event"));
		return;
	}
	if (!Target) return;
	UAbilitySystemComponent* ASC = Local->GetAbilitySystemComponent();
	if (Phase.StartsWith(TEXT("LifecycleAimReady:")))
	{
		const FString Case = Phase.RightChop(18);
		const FVector Right = FRotationMatrix(LifecycleAim).GetUnitAxis(EAxis::Y);
		const float Lead = Case == TEXT("Moving")
			? LifecycleAimDistance / Subject->GetCombatConfig().ProjectileSpeed * LifecycleTargetSpeed : 0;
		const FVector ExpectedTarget = LifecycleOrigin + LifecycleAim.Vector() * LifecycleAimDistance - Right * Lead;
		const bool Ready = Subject->IsCombatReady() && Target->IsCombatReady() && Target->GetHealth() == 100
			&& !Subject->IsCasting() && !Subject->IsStunned() && !Target->IsStunned()
			&& FVector::Dist(Subject->GetActorLocation(), LifecycleOrigin) < 30
			&& FVector::Dist2D(Target->GetActorLocation(), ExpectedTarget) < 30
			&& (bPartner || (Local == Subject && Local->GetAttackCooldownRemaining() == 0
				&& ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass())));
		if (Ready && !SentReceipts.Contains(Prefix + TEXT("Aim") + Case + TEXT("Ready")))
		{
			ClearCombatAimFixture();
			CombatAimWorld = World; CombatAimTarget = Target; CombatAimDamageCount = 0;
			CombatAimHealthChanged = Target->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
				UmultiplayerCombatAttributes::GetHealthAttribute()).AddWeakLambda(this, [this](const FOnAttributeChangeData& Change)
				{ if (Change.NewValue < Change.OldValue) ++CombatAimDamageCount; });
			SendReceipt(Prefix + TEXT("Aim") + Case + TEXT("Ready"), true,
				TEXT("This connection received the complete target fixture and subscribed to its actual replicated health before attack"));
		}
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleAimFire:")))
	{
		const FString Case = Phase.RightChop(17), Marker = TEXT("Command:") + Phase;
		if (!bPartner && !SentReceipts.Contains(Marker))
		{
			SentReceipts.Add(Marker);
			const float HealthBefore = Target->GetHealth();
			PC->SetControlRotation(LifecycleAim);
			Local->Attack();
			// 同一调用栈立刻转开，正常 CMC 只会在之后发送这个不同的当前方向。
			PC->SetControlRotation(LifecycleOldAim);
			const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass());
			UGameplayAbility* Ability = Spec ? Spec->GetPrimaryInstance() : nullptr;
			FPredictionKey Key = Ability ? Ability->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
			bool KeyedCooldown = false;
			for (auto Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
			{
				const auto* Effect = ASC->GetActiveGameplayEffect(Handle);
				KeyedCooldown |= Effect && Effect->PredictionKey == Key
					&& Effect->Spec.Def->IsA<UmultiplayerAttackCooldownEffect>();
			}
			SendReceipt(TEXT("PrimaryAim") + Case + TEXT("Predicted"), Key.IsLocalClientKey() && Key.WasLocallyGenerated()
				&& KeyedCooldown && Target->GetHealth() == HealthBefore,
				TEXT("Owned Attack predicted its keyed cooldown at 45-degree aim; immediate -90-degree turn did not cause client-side damage"));
			if (bDone) return;
			Key.NewCaughtUpDelegate().BindWeakLambda(this, [this, Case] { SentReceipts.Add(TEXT("AimCaughtUp:") + Case); });
		}
		const float ExpectedHealth = 100 - Subject->GetCombatConfig().Attack.Amount;
		if (CombatAimDamageCount > 1 || Target->GetHealth() < ExpectedHealth)
		{
			SendReceipt(Prefix + TEXT("Aim") + Case + TEXT("Outcome"), false,
				TEXT("This connection observed repeated damage from one activation"));
			return;
		}
		if (Target->GetHealth() == ExpectedHealth && CombatAimDamageCount == 1
			&& (bPartner || SentReceipts.Contains(TEXT("AimCaughtUp:") + Case)))
			SendReceipt(Prefix + TEXT("Aim") + Case + TEXT("Outcome"), true,
				TEXT("This independent connection observed exactly one replicated damage transition; owner also confirmed the original prediction key"));
		return;
	}
	if (Phase.StartsWith(TEXT("LifecycleReady:")))
	{
		const FString Case = Phase.RightChop(15);
		if (!ConfigureLifecycleRules(Local) || !ConfigureLifecycleRules(Subject) || !ConfigureLifecycleRules(Target))
		{ SendReceipt(Prefix + TEXT("LifecycleRules"), false, TEXT("Could not create per-character transient casting rules")); return; }
		SendReceipt(Prefix + TEXT("LifecycleRules"), true,
			TEXT("This process uses per-character transient rules: Area.Windup=3 seconds, cooldown>=windup+actual recovery+1 second; assets and CDO remain unchanged"));
		FHitResult Floor; FCollisionQueryParams Query(SCENE_QUERY_STAT(LifecycleReadyFloor), false, Local);
		const bool FloorReady = World->LineTraceSingleByObjectType(Floor, LifecycleOrigin + FVector(0, 0, 30),
			LifecycleOrigin - FVector(0, 0, 300), FCollisionObjectQueryParams(ECC_WorldStatic), Query);
		const bool Ready = FloorReady && Subject->IsCombatReady() && FVector::Dist(Subject->GetActorLocation(), LifecycleOrigin) < 25
			&& Target->IsAlive() && Target->GetHealth() == 100
			&& !Subject->IsCasting() && !Subject->IsStunned() && !Target->IsStunned()
			&& (bPartner || (Local == Subject && Local->GetEnergy() == 100 && Local->GetAreaCooldownRemaining() == 0
				&& ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass())));
		if (Ready)
		{
			if (Case == TEXT("Release") && bVisualAcceptance && !SentReceipts.Contains(Prefix + TEXT("AreaReleaseReady")))
			{
				// 只预载正式配置，不自己触发 Cue；后续必须观察本次真实执行新生成的粒子组件。
				const auto& Feedback = Subject->GetCombatConfig().AreaFeedback;
				if (!Feedback.CascadeEffect.IsNull()) Feedback.CascadeEffect.LoadSynchronous();
				else if (!Feedback.Effect.IsNull()) Feedback.Effect.LoadSynchronous();
				CombatAreaEffectsBefore.Reset();
				for (TObjectIterator<USceneComponent> It; It; ++It)
					if (It->GetWorld() == World && It->IsRegistered()) CombatAreaEffectsBefore.Add(*It);
				bCombatAreaReleaseCenter = false;
			}
			SendReceipt(Prefix + TEXT("Area") + Case + TEXT("Ready"), true, TEXT("Actual replicas, skill grant, resource/cooldown and replicated aiming floor are ready"));
		}
		return;
	}
	if (bVisualAcceptance && (Phase == TEXT("LifecycleRelease") || Phase == TEXT("LifecycleVerify:Release")))
	{
		if (Subject->IsCasting())
		{
			const auto Locked = Subject->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
			if (Locked.bValid) { CombatAreaReleaseCenter = Locked.End; bCombatAreaReleaseCenter = true; }
		}
		if (bCombatAreaReleaseCenter && !SentReceipts.Contains(Prefix + TEXT("AreaReleaseEffectObserved")))
		{
			const auto& Feedback = Subject->GetCombatConfig().AreaFeedback;
			const auto Observe = [&](USceneComponent* Component, const TCHAR* Kind)
			{
				if (SentReceipts.Contains(Prefix + TEXT("AreaReleaseEffectObserved"))
					|| Component->GetWorld() != World || !Component->IsRegistered() || !Component->IsActive()
					|| CombatAreaEffectsBefore.Contains(Component)) return;
				SendReceipt(Prefix + TEXT("AreaReleaseEffectObserved"), Component->GetComponentLocation().Equals(CombatAreaReleaseCenter, 1.f),
					FString::Printf(TEXT("Actual %s Cue particle component=%s asset-selected=1 active=%d expected-locked-ground=%s actual=%s; not a screenshot/file-existence check"),
						Kind, *Component->GetPathName(), Component->IsActive(), *CombatAreaReleaseCenter.ToString(), *Component->GetComponentLocation().ToString()));
			};
			for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
				if (!Feedback.CascadeEffect.IsNull() && It->Template == Feedback.CascadeEffect.Get()) Observe(*It, TEXT("Cascade"));
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
				if (Feedback.CascadeEffect.IsNull() && !Feedback.Effect.IsNull() && It->GetAsset() == Feedback.Effect.Get()) Observe(*It, TEXT("Niagara"));
		}
	}
	if (!bPartner && (Phase == TEXT("LifecycleRelease") || Phase == TEXT("LifecycleCancel")
		|| Phase == TEXT("LifecycleConfirmedCancel") || Phase == TEXT("LifecycleStun")))
	{
		const FString Case = Phase.RightChop(9), Marker = TEXT("Command:") + Phase;
		if (!SentReceipts.Contains(Marker))
		{
			SentReceipts.Add(Marker); PC->SetControlRotation(FRotator::ZeroRotator);
			const float Before = Local->GetEnergy(), HealthBefore = Target->GetHealth();
			if (Case == TEXT("Release"))
			{
				// PostActorTick 在本帧输入执行之后；下一帧 NetTickDispatch 之前读取，不能等权威结果冒充本地预测。
				CombatAreaInputStep = 0;
				CombatAreaInputTick = FWorldDelegates::OnWorldPostActorTick.AddWeakLambda(this,
					[this, World, WeakLocal = TWeakObjectPtr<AmultiplayerCombatCharacter>(Local),
						WeakTarget = TWeakObjectPtr<AmultiplayerCombatCharacter>(Target), Before, HealthBefore](UWorld* TickWorld, ELevelTick, float)
					{
						if (TickWorld != World || bDone) return;
						auto* Caster = WeakLocal.Get();
						auto* Victim = WeakTarget.Get();
						auto* Controller = Caster ? Cast<APlayerController>(Caster->GetController()) : nullptr;
						if (!Caster || !Victim || !Controller) { Assert(TEXT("PrimaryAreaReleaseInputLifetime"), false, TEXT("Real input fixture disappeared during R press/release")); return; }
						auto* SourceASC = Caster->GetAbilitySystemComponent();
						const float Cost = Caster->GetCombatConfig().Area.Cost;
						if (CombatAreaInputStep == 0)
						{
							const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
							SendReceipt(TEXT("PrimaryAreaReleaseInputPreview"), Controller->IsInputKeyDown(EKeys::R)
								&& Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Area
								&& Preview.bValid
								&& !Caster->IsCasting() && Caster->GetEnergy() == Before && Caster->GetAreaCooldownRemaining() == 0,
								FString::Printf(TEXT("Real R preview: key-down=%d targeting=%d skill=%d valid-floor=%d center=%s casting=%d energy=%.2f before=%.2f cooldown=%.3f pawn=%s input=%s"),
									Controller->IsInputKeyDown(EKeys::R), Caster->IsTargetingSkill(), int32(Caster->GetTargetingSkill()), Preview.bValid,
									*Preview.End.ToString(), Caster->IsCasting(), Caster->GetEnergy(), Before, Caster->GetAreaCooldownRemaining(),
									*Caster->GetClass()->GetPathName(), *GetNameSafe(Controller->PlayerInput)));
							if (bDone) return;
							Controller->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.));
							CombatAreaInputStep = 1;
							return;
						}
						if (CombatAreaInputStep == 1)
						{
							const auto* Spec = SourceASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
							UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
							FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
							bool KeyedCooldown = false, KeyedCost = false;
							for (const auto Handle : SourceASC->GetActiveEffects(FGameplayEffectQuery()))
							{
								const auto* Effect = SourceASC->GetActiveGameplayEffect(Handle);
								if (!Effect || Effect->PredictionKey != Key) continue;
								KeyedCooldown |= Effect->Spec.Def->IsA<UmultiplayerAreaCooldownEffect>();
								KeyedCost |= Effect->Spec.Def->IsA<UmultiplayerAreaCostEffect>();
							}
							SendReceipt(TEXT("PrimaryAreaReleasePredicted"), Caster->IsCasting() && Key.IsLocalClientKey()
								&& Key.WasLocallyGenerated() && KeyedCooldown && KeyedCost
								&& Caster->GetEnergy() == Before - Cost && Victim->GetHealth() == HealthBefore,
								FString::Printf(TEXT("Real R key-up frame before later network receive: casting=%d key=%d local=%d generated=%d keyed-CD=%d keyed-cost=%d energy=%.2f expected=%.2f targetHP=%.2f beforeHP=%.2f; %s"),
									Caster->IsCasting(), Key.Current, Key.IsLocalClientKey(), Key.WasLocallyGenerated(), KeyedCooldown, KeyedCost,
									Caster->GetEnergy(), Before - Cost, Victim->GetHealth(), HealthBefore, *DescribeLifecycleState(Caster, Victim)));
							if (bDone) return;
							CombatPredictionKeys.Add(TEXT("LifecycleRelease"), Key.Current);
							Key.NewCaughtUpDelegate().BindWeakLambda(this, [this] { SentReceipts.Add(TEXT("AreaCaughtUp:Release")); });
							SendReceipt(TEXT("PrimaryAreaReleaseInputReleased"), !Controller->IsInputKeyDown(EKeys::R) && !Caster->IsTargetingSkill(),
								TEXT("R is physically released and editable preview ended without LMB confirmation or direct CastArea injection"));
							Controller->InputKey(FInputKeyParams(EKeys::R, IE_Pressed, 1.));
							CombatAreaInputStep = 2;
							return;
						}
						if (CombatAreaInputStep == 2)
						{
							Controller->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.));
							CombatAreaInputStep = 3;
							return;
						}
						const auto* ActiveSpec = SourceASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
						const auto* ActiveAbility = ActiveSpec ? ActiveSpec->GetPrimaryInstance() : nullptr;
						SendReceipt(TEXT("PrimaryAreaReleaseRepeatedInput"), !Controller->IsInputKeyDown(EKeys::R)
							&& !Caster->IsTargetingSkill() && Caster->IsCasting() && Caster->GetEnergy() == Before - Cost
							&& ActiveAbility && ActiveAbility->GetCurrentActivationInfo().GetActivationPredictionKey().Current == CombatPredictionKeys.FindRef(TEXT("LifecycleRelease")),
							TEXT("A second genuine R press/release during the same active cast did not create a second commitment"));
						FWorldDelegates::OnWorldPostActorTick.Remove(CombatAreaInputTick);
						CombatAreaInputTick.Reset();
					});
				PC->InputKey(FInputKeyParams(EKeys::R, IE_Pressed, 1.));
			}
			else
			{
				Local->CastArea();
				const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
				UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
				FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
				bool HasKeyedCooldown = false, HasKeyedCost = false;
				for (const FActiveGameplayEffectHandle& Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
				{
					const FActiveGameplayEffect* Effect = ASC->GetActiveGameplayEffect(Handle);
					if (!Effect || Effect->PredictionKey != Key) continue;
					HasKeyedCooldown |= Effect->Spec.Def->IsA(UmultiplayerAreaCooldownEffect::StaticClass());
					HasKeyedCost |= Effect->Spec.Def->IsA(UmultiplayerAreaCostEffect::StaticClass());
				}
				SendReceipt(TEXT("PrimaryArea") + Case + TEXT("Predicted"), Local->IsCasting() && Key.IsLocalClientKey()
					&& Key.WasLocallyGenerated() && HasKeyedCooldown && HasKeyedCost
					&& Local->GetEnergy() == Before - Local->GetCombatConfig().Area.Cost && Target->GetHealth() == HealthBefore,
					TEXT("Before CastArea returned: local casting, real keyed cost/CD, one resource change and no client damage"));
				Key.NewCaughtUpDelegate().BindWeakLambda(this, [this, Case] { SentReceipts.Add(TEXT("AreaCaughtUp:") + Case); });
				Local->CastArea();
				SendReceipt(TEXT("PrimaryArea") + Case + TEXT("RepeatedInput"), Local->GetEnergy() == Before - Local->GetCombatConfig().Area.Cost,
					TEXT("Immediate repeated input did not consume a second cost"));
				if (Case == TEXT("Cancel"))
				{
					Local->CancelCast();
					SendReceipt(TEXT("PrimaryAreaCancelSent"), !Local->IsCasting() && Local->GetAreaCooldownRemaining() > 0
						&& Local->GetEnergy() == Before - Local->GetCombatConfig().Area.Cost,
						TEXT("Local cancel ended casting, retained one cost/CD, and sent normal GAS end/cancel notification"));
				}
			}
		}
	}
	if (!bPartner && Phase == TEXT("LifecycleConfirmedCancel")
		&& SentReceipts.Contains(TEXT("AreaCaughtUp:ConfirmedCancel")) && !SentReceipts.Contains(TEXT("PrimaryAreaConfirmedCancelSent")))
	{
		const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		const UGameplayAbility* Instance = Spec ? Spec->GetPrimaryInstance() : nullptr;
		const FPredictionKey Key = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : FPredictionKey();
		bool HasAuthoritativeCasting = false;
		for (auto Handle : ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::State_Casting))))
		{
			const auto* Effect = ASC->GetActiveGameplayEffect(Handle);
			HasAuthoritativeCasting |= Effect && Effect->PredictionKey == Key && Effect->PredictionKey.WasReceived()
				&& Effect->Spec.Def->IsA<UmultiplayerCastingEffect>();
		}
		if (HasAuthoritativeCasting && Local->IsCasting())
		{
			SendReceipt(TEXT("PrimaryAreaConfirmedCancelAuthorityObserved"), true,
				TEXT("Real prediction key caught up and a received server Casting GE with that same key replaced the prediction"));
			Local->CancelCast();
			SendReceipt(TEXT("PrimaryAreaConfirmedCancelSent"), !Local->IsCasting() && Local->GetAreaCooldownRemaining() > 0
				&& Local->GetEnergy() == 100 - Local->GetCombatConfig().Area.Cost,
				TEXT("Cancelling after authority handoff cleared local casting immediately without refunding cost or deleting cooldown"));
		}
	}
	if ((Phase == TEXT("LifecycleRelease") || Phase == TEXT("LifecycleStun")) && Subject->IsCasting())
		CaptureSkillView(Phase == TEXT("LifecycleRelease") ? TEXT("AreaReleaseCasting") : TEXT("AreaStunCasting"),Subject,Target);
	if (Phase == TEXT("LifecycleStunObserve") && Subject->IsStunned() && !Subject->IsCasting())
	{
		if (CaptureSkillView(TEXT("AreaStunStopped"),Subject,Target)) SendReceipt(Prefix + TEXT("AreaStunObserved"), true, TEXT("This connection received server stun and cast interruption state"));
	}
	if (Phase.StartsWith(TEXT("LifecycleVerify:")))
	{
		const FString Case = Phase.RightChop(16);
		const float ExpectedHealth = Case == TEXT("Release") ? 100 - Local->GetCombatConfig().Area.Amount : 100;
		const bool Outcome = !Subject->IsCasting() && Target->GetHealth() == ExpectedHealth
			&& (bPartner || (SentReceipts.Contains(TEXT("AreaCaughtUp:") + Case)
				&& Local->GetEnergy() == 100 - Local->GetCombatConfig().Area.Cost));
		if (Outcome && (Case != TEXT("Release") || CaptureSkillView(TEXT("AreaReleaseResult"),Subject,Target))) SendReceipt(Prefix + TEXT("Area") + Case + TEXT("Outcome"), true,
			TEXT("Independent connection observed final health/cast state; owner additionally confirmed key catch-up and one cost"));
	}
	if (!bPartner && Phase == TEXT("Complete")) { SendReceipt(TEXT("Complete"), true, TEXT("Area lifecycle and Attack aiming scenario complete; not a visual quality or historical rewind test")); Finish(true); }
#endif
}
