#include "Testing/CoopNetTestDriver.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEncounter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Mechanisms/multiplayerCoopGate.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Animation/AnimMontage.h"

namespace
{
	template<class T> T* EscapeActor(UWorld* World, const TCHAR* Name)
	{
		for (TActorIterator<T> It(World); It; ++It) if (It->GetFName() == FName(Name)) return *It;
		return nullptr;
	}
	bool EscapeBool(const UObject* Object, const TCHAR* Name)
	{
		const auto* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), Name) : nullptr;
		return Property && Property->GetPropertyValue_InContainer(Object);
	}
	bool EscapeMoveCommand(const FString& Command)
	{
		return Command == TEXT("EscapeContact") || Command == TEXT("EscapeClear") || Command == TEXT("EscapeSupply") || Command == TEXT("EscapeKeys12")
			|| Command == TEXT("EscapeKeys34") || Command == TEXT("EscapeGate") || Command == TEXT("EscapeExit");
	}
	FString EscapeMovementSnapshot(const AmultiplayerCombatCharacter* Character)
	{
		const auto* Movement=Character->GetCharacterMovement();
		const auto* Controller=Character->GetController();
		return FString::Printf(TEXT("accel=%s lastInput=%s pendingInput=%s maxSpeed=%.1f ignored=%d restricted=%d yaw=%.1f walkableAngle=%.2f walkableZ=%.3f maxStep=%.2f floorBlocking=%d floorWalkable=%d floorActor=%s floorNormal=%s floorDistance=%.2f rootSources=%d rootAnimation=%d"),
			*Movement->GetCurrentAcceleration().ToCompactString(),*Character->GetLastMovementInputVector().ToCompactString(),*Character->GetPendingMovementInputVector().ToCompactString(),Movement->GetMaxSpeed(),
			Controller && Controller->IsMoveInputIgnored(),!Character->IsCombatReady() || Character->IsMovementBlocked() || Character->IsCasting() || Character->GetCoopComponent()->IsRescuing(),Controller ? Controller->GetControlRotation().Yaw : 0.f,
			Movement->GetWalkableFloorAngle(),Movement->GetWalkableFloorZ(),Movement->MaxStepHeight,Movement->CurrentFloor.bBlockingHit,Movement->CurrentFloor.IsWalkableFloor(),
			*GetNameSafe(Movement->CurrentFloor.HitResult.GetActor()),*Movement->CurrentFloor.HitResult.ImpactNormal.ToCompactString(),Movement->CurrentFloor.FloorDist,
			Movement->HasRootMotionSources(),Character->IsPlayingRootMotion());
	}
}

// 这份场景不改HP、Transform、技能配置或目标状态；所有收益/胜负来自原玩法路径。
bool UCoopNetTestDriver::SetEscapeMove(UWorld* World, const FString& Command,
	const FVector& PrimaryGoal, const FVector& PartnerGoal)
{
#if !UE_BUILD_SHIPPING
	UNavigationSystemV1* Navigation = UNavigationSystemV1::GetCurrent(World);
	if (!Navigation) { Assert(TEXT("ServerEscapeNavigation"), false, TEXT("The actual map has no navigation system")); return false; }
	const TPair<ACoopNetTestProbe*, FVector> Moves[] = {{Probe.Get(), PrimaryGoal}, {PartnerProbe.Get(), PartnerGoal}};
	for (const auto& Move : Moves)
	{
		auto* Controller = Move.Key ? Cast<APlayerController>(Move.Key->GetOwner()) : nullptr;
		auto* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (!Pawn) return false;
		TArray<FVector> Points;
		if (Move.Value.Equals(Pawn->GetActorLocation(), 1.f))
		{
			// 已完成自己的领取任务时原地等待，不再追逐已移动到插槽中的展示模型。
			Points.Add(Pawn->GetActorLocation());
		}
		else
		{
			FNavLocation Goal(FVector::ZeroVector);
			const bool bProjected = Navigation->ProjectPointToNavigation(Move.Value, Goal, FVector(140.f, 140.f, 350.f));
			UNavigationPath* Path = bProjected ? UNavigationSystemV1::FindPathToLocationSynchronously(World,
				Pawn->GetActorLocation(), Goal.Location, Pawn) : nullptr;
			if (!Path || !Path->IsValid() || Path->IsPartial())
			{
				Assert(TEXT("ServerEscapeNavigation"), false, FString::Printf(TEXT("No complete actual Recast path for %s: start=%s goal=%s projected=%d projectedGoal=%s"),
					*Command, *Pawn->GetActorLocation().ToCompactString(), *Move.Value.ToCompactString(), bProjected, *Goal.Location.ToCompactString()));
				return false;
			}
			Points = Path->PathPoints;
		}
		Move.Key->EscapeRoute.Command = Command;
		Move.Key->EscapeRoute.Points = MoveTemp(Points);
		++Move.Key->EscapeRoute.Revision;
		Move.Key->ForceNetUpdate();
	}
	EscapePrimaryGoal = PrimaryGoal; EscapePartnerGoal = PartnerGoal;
	EscapeLastRepathAt = FPlatformTime::Seconds();
	if (Phase != Command) SetCommand(Command);
	return true;
#else
	return false;
#endif
}

void UCoopNetTestDriver::SetEscapeWalking(APlayerController* Controller, bool bWalk)
{
	if (bEscapeWalking == bWalk || !Controller) return;
	Controller->InputKey(FInputKeyParams(EKeys::W, bWalk ? IE_Pressed : IE_Released, bWalk ? 1.0 : 0.0));
	bEscapeWalking = bWalk;
}

void UCoopNetTestDriver::ClearEscapeInput()
{
	if (Mode != TEXT("CoopEscape") && !bVisualAcceptance) return;
	for (const auto& Source : EscapeHitSources) if (auto* Capsule=Source.Key.Get())
	{
		Capsule->OnComponentHit.RemoveDynamic(this,&ThisClass::HandleEscapeCapsuleHit);
		Capsule->SetNotifyRigidBodyCollision(Source.Value);
	}
	EscapeHitSources.Reset();
	if (EscapeSpawnObserver.IsValid())
	{
		if (auto* Round = EscapeRoundWorld.Get()) Round->RemoveOnActorSpawnedHandler(EscapeSpawnObserver);
		EscapeSpawnObserver.Reset();
	}
	if (UWorld* World = FindWorld()) if (auto* Controller = World->GetFirstPlayerController())
	{
		SetEscapeWalking(Controller, false);
		if (bEscapeRescueHeld) Controller->InputKey(FInputKeyParams(EKeys::F, IE_Released, 0.0));
		if (bEscapeEnterHeld) Controller->InputKey(FInputKeyParams(EKeys::Enter, IE_Released, 0.0));
	}
	bEscapeRescueHeld = bEscapeEnterHeld = false;
}

void UCoopNetTestDriver::BindEscapeHitDiagnostics(AmultiplayerCombatCharacter* Character)
{
#if !UE_BUILD_SHIPPING
	if (!bEscapeMovementDiagnostic || !Character) return;
	auto* Capsule=Character->GetCapsuleComponent();
	if (EscapeHitSources.Contains(Capsule)) return;
	EscapeHitSources.Add(Capsule,Capsule->BodyInstance.bNotifyRigidBodyCollision);
	Capsule->SetNotifyRigidBodyCollision(true);
	Capsule->OnComponentHit.AddUniqueDynamic(this,&ThisClass::HandleEscapeCapsuleHit);
	const auto* Movement=Character->GetCharacterMovement();
	const auto* Navigation=UNavigationSystemV1::GetCurrent(Character->GetWorld());
	const auto* Data=Navigation ? Navigation->GetNavDataForProps(Movement->GetNavAgentPropertiesRef(),Character->GetActorLocation()) : nullptr;
	const auto* Recast=Cast<ARecastNavMesh>(Data);
	if (Data)
	{
		const auto& Config=Data->GetConfig();
		Emit(TEXT("ESCAPE_NAV_AGENT"),FString::Printf(TEXT("pawn=%s nav=%s navRadius=%.2f navHeight=%.2f navStep=%.2f recastStep=%.2f capsuleRadius=%.2f capsuleHeight=%.2f movementRadius=%.2f movementHeight=%.2f movementStep=%.2f"),
			*Character->GetName(),*GetNameSafe(Data),Config.AgentRadius,Config.AgentHeight,Config.AgentStepHeight,
			Recast ? Recast->GetAgentMaxStepHeight(ENavigationDataResolution::Default) : -1.f,Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()*2.f,
			Movement->GetNavAgentPropertiesRef().AgentRadius,Movement->GetNavAgentPropertiesRef().AgentHeight,Movement->MaxStepHeight));
	}
#endif
}

void UCoopNetTestDriver::HandleEscapeCapsuleHit(UPrimitiveComponent* HitComponent,AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,FVector NormalImpulse,const FHitResult& Hit)
{
#if !UE_BUILD_SHIPPING
	const double Time=FPlatformTime::Seconds();
	if (Time-EscapeLastHitAt<1.f) return;
	EscapeLastHitAt=Time;
	auto* Character=Cast<AmultiplayerCombatCharacter>(HitComponent->GetOwner());
	if (!Character) return;
	const auto* Movement=Character->GetCharacterMovement();
	Emit(TEXT("ESCAPE_BLOCKING_HIT"),FString::Printf(TEXT("pawn=%s actor=%s component=%s componentClass=%s normal=%s impactNormal=%s impactPoint=%s hitTime=%.6f penetration=%d depth=%.4f walkable=%d stepUp=%d pos=%s velocity=%s accel=%s maxStep=%.2f gravity=%.2f scale=%s updated=%s plane=%d planeNormal=%s rvo=%d canAffectNav=%d navRelevant=%d boundsOrigin=%s boundsExtent=%s"),
		*Character->GetName(),*GetNameSafe(OtherActor),*GetNameSafe(OtherComponent),*GetNameSafe(OtherComponent ? OtherComponent->GetClass() : nullptr),
		*Hit.Normal.ToCompactString(),*Hit.ImpactNormal.ToCompactString(),*Hit.ImpactPoint.ToCompactString(),Hit.Time,Hit.bStartPenetrating,Hit.PenetrationDepth,
		Movement->IsWalkable(Hit),OtherComponent && OtherComponent->CanCharacterStepUp(Character),*Character->GetActorLocation().ToCompactString(),
		*Character->GetVelocity().ToCompactString(),*Movement->GetCurrentAcceleration().ToCompactString(),Movement->MaxStepHeight,Movement->GetGravityZ(),
		*Character->GetActorScale3D().ToCompactString(),*GetNameSafe(Movement->UpdatedComponent),Movement->bConstrainToPlane,*Movement->GetPlaneConstraintNormal().ToCompactString(),Movement->bUseRVOAvoidance,
		OtherComponent && OtherComponent->CanEverAffectNavigation(),OtherComponent && OtherComponent->IsNavigationRelevant(),
		OtherComponent ? *OtherComponent->Bounds.Origin.ToCompactString() : TEXT("None"),OtherComponent ? *OtherComponent->Bounds.BoxExtent.ToCompactString() : TEXT("None")));
#endif
}

bool UCoopNetTestDriver::CaptureEscapeView(const FString& Name)
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(),TEXT("NullRHI")) || !FApp::CanEverRender())
	{
		if (!SentReceipts.Contains(TEXT("EscapeNonVisual")))
		{
			SentReceipts.Add(TEXT("EscapeNonVisual"));
			Emit(TEXT("NON_VISUAL"),TEXT("No rendered screenshot evidence; all physical gameplay and replicated-state assertions remain required"));
		}
		return true;
	}
	const FString SavedMarker = TEXT("ScreenshotSaved:") + Name;
	if (SentReceipts.Contains(SavedMarker)) return true;
	const double Started = EscapeCaptureWaitStarted.FindOrAdd(Name,FPlatformTime::Seconds());
	if (FPlatformTime::Seconds()-Started > 15.f)
	{
		Assert(TEXT("EscapeScreenshotSaved"),false,TEXT("Current-token screenshot did not produce a nonempty PNG within 15 seconds: ")+Name);
		return false;
	}
	const FString Marker = TEXT("Screenshot:") + Name;
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("CoopEscapeCaptureDir="), Directory) || Directory.IsEmpty())
	{
		Assert(TEXT("EscapeScreenshotDirectory"),false,TEXT("Rendered escape acceptance requires a capture directory"));
		return false;
	}
	// 目录包含本轮唯一 token，重用输出目录时旧 PNG 不能替本轮阶段过关。
	Directory=FPaths::Combine(Directory,Token);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, Name + TEXT(".png"));
	if (SentReceipts.Contains(Marker))
	{
		if (IFileManager::Get().FileSize(*Path) <= 0) return false;
		SentReceipts.Add(SavedMarker);
		Emit(TEXT("SCREENSHOT_SAVED"),Path);
		return true;
	}
	if (!GEngine || !GEngine->GameViewport || FScreenshotRequest::IsScreenshotRequested()) return false;
	FScreenshotRequest::RequestScreenshot(Path, true, false);
	SentReceipts.Add(Marker);
	Emit(TEXT("SCREENSHOT_REQUESTED"), Path);
	return false;
#else
	return false;
#endif
}

bool UCoopNetTestDriver::PrepareVisualFixture(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	UClass* PlayerClass=LoadClass<AmultiplayerCombatCharacter>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	UClass* HUDClass=LoadClass<AHUD>(nullptr,TEXT("/Game/Combat/Escape/BP_EscapeHUD.BP_EscapeHUD_C"));
	UClass* EnemyClass=CombatFixtureEnemyClass();
	auto* FixtureMode=World->GetAuthGameMode();
	const auto* Player=PlayerClass ? PlayerClass->GetDefaultObject<AmultiplayerCombatCharacter>() : nullptr;
	const auto* Rules=Player ? Player->GetCombatConfigAsset() : nullptr;
	const auto* DeathField=Player ? FindFProperty<FObjectPropertyBase>(Player->GetClass(),TEXT("DeathMontage")) : nullptr;
	const auto* Death=DeathField ? Cast<UAnimMontage>(DeathField->GetObjectPropertyValue_InContainer(Player)) : nullptr;
	const bool bReady=FixtureMode && Player && HUDClass && EnemyClass && Rules && Player->GetMesh()->GetAnimClass()
		&& Player->GetActionMontage(EMultiplayerCombatAnimation::Attack) && Player->GetActionMontage(EMultiplayerCombatAnimation::Heal)
		&& Player->GetActionMontage(EMultiplayerCombatAnimation::Area) && Death && !Death->bEnableAutoBlendOut
		&& (!Rules->DefenseFeedback.Effect.IsNull() || !Rules->DefenseFeedback.CascadeEffect.IsNull())
		&& (!Rules->HealFeedback.Effect.IsNull() || !Rules->HealFeedback.CascadeEffect.IsNull()) && !Rules->AreaMarkerMaterial.IsNull()
		&& Rules->Attack.Amount==25.f && Rules->Heal.Amount==30.f && Rules->DefenseDamageMultiplier==.5f
		&& Rules->DefenseShieldAmount==0.f && Rules->BurningDamagePerTick==0.f && Rules->AreaRootDuration==0.f;
	Assert(TEXT("ServerVisualFixture"),bReady,TEXT("Formal player/enemy/HUD BPs, actual animation/VFX/decal and unchanged legacy numeric rules must exist before the first login"));
	if (!bReady) return false;
	// 只覆盖本次 dev World 的模式实例；不改模式 CDO、保存资产或替换已经接纳的玩家。
	FixtureMode->DefaultPawnClass=PlayerClass;
	FixtureMode->HUDClass=HUDClass;
	return true;
#else
	return false;
#endif
}

// Basic Combat 冻结在高空隔离数值，没有物理地板；只补可见表面以观察脚底特效。
// 另外两套夹具已经有真实可见地板，不叠加表面，也不新增碰撞、导航或任何玩法规则。
bool UCoopNetTestDriver::PrepareVisualFixtureSurface(UWorld* World, AmultiplayerCombatCharacter* Character)
{
#if !UE_BUILD_SHIPPING
	if (!bVisualAcceptance || Mode!=TEXT("Combat")) return true;
	auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
	const float Top=Character->GetActorLocation().Z-Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto* Surface=World->SpawnActor<AStaticMeshActor>(FVector(0,0,Top-5.f),FRotator::ZeroRotator);
	if (!Surface || !Cube) { Assert(TEXT("ServerVisualSurface"),false,TEXT("Visible no-collision fixture surface could not be created")); return false; }
	auto* Mesh=Surface->GetStaticMeshComponent();
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetStaticMesh(Cube);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetIsReplicated(true);
	Surface->SetActorScale3D(FVector(100,100,.1f));
	Surface->SetReplicates(true); Surface->SetReplicateMovement(true); Surface->ForceNetUpdate();
	Assert(TEXT("ServerVisualSurface"),Mesh->GetCollisionEnabled()==ECollisionEnabled::NoCollision && !Mesh->CanEverAffectNavigation(),
		TEXT("Visual-only basic-shape ground top matches capsule feet; original frozen movement, physics and GAS numerical fixture are unchanged"));
	return true;
#else
	return false;
#endif
}

UClass* UCoopNetTestDriver::CombatFixtureEnemyClass() const
{
#if !UE_BUILD_SHIPPING
	if (bVisualAcceptance) return LoadClass<AmultiplayerCombatEnemy>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
#endif
	return AmultiplayerCombatEnemy::StaticClass();
}

bool UCoopNetTestDriver::ValidateVisualFixturePawn(AmultiplayerCombatCharacter* Character)
{
#if !UE_BUILD_SHIPPING
	if (!bVisualAcceptance) return true;
	const FString Receipt=(bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+TEXT("PresentationReady");
	if (SentReceipts.Contains(Receipt)) return true;
	const auto* Controller=Character ? Cast<APlayerController>(Character->GetController()) : nullptr;
	if (!Controller || !Controller->GetHUD()) return false;
	const bool bReady=Character->GetClass()->GetPathName()==TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C")
		&& Controller->GetHUD()->GetClass()->GetPathName()==TEXT("/Game/Combat/Escape/BP_EscapeHUD.BP_EscapeHUD_C")
		&& Character->GetMesh()->GetAnimInstance() && Character->GetCombatConfigAsset()
		&& Character->GetActionMontage(EMultiplayerCombatAnimation::Attack) && Character->GetActionMontage(EMultiplayerCombatAnimation::Area);
	SendReceipt(Receipt,bReady,TEXT("Actual admitted owning pawn uses the formal player BP, live animation instance and real Escape HUD; not a native empty presentation fixture"));
	return bReady;
#else
	return false;
#endif
}

// 专项仍使用本地玩家原相机，只以正常控制朝向把观察对象放进画面；不替换 ViewTarget/FOV。
bool UCoopNetTestDriver::CaptureSkillView(const FString& Name, AActor* Subject, AActor* Other)
{
	if (!bVisualAcceptance) return true;
	// 原状态条件满足后才开始等待，保留首帧的真实数值证据；截图观察稳定动画姿态而非通知到达的第一帧。
	const double SettleSeconds=Name==TEXT("Downed") ? .8 : (Name==TEXT("RescueDone") ? .35 : 0.);
	if (SettleSeconds>0.)
	{
		const FString Key=TEXT("VisualSettle:")+Name;
		if (!EscapeCaptureWaitStarted.Contains(Key))
		{
			auto* Character=Cast<AmultiplayerCombatCharacter>(Subject);
			const bool bDowned=Name==TEXT("Downed");
			const bool bState=Character && Character->IsAlive() && (bDowned
				? Character->GetCoopComponent()->IsDowned() && !Character->IsCombatReady() && Character->GetHealth()==1.f
				: !Character->GetCoopComponent()->IsDowned() && Character->IsCombatReady()
					&& FMath::IsNearlyEqual(Character->GetHealth(),Character->GetMaxHealth()*.3f));
			SendReceipt((bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+Name+TEXT("StateBeforeSettle"),bState,
				TEXT("Original authoritative life/HP conditions observed before waiting solely for the presentation blend"));
			EscapeCaptureWaitStarted.Add(Key,FPlatformTime::Seconds());
		}
		if (FPlatformTime::Seconds()-EscapeCaptureWaitStarted[Key]<SettleSeconds) return false;
	}
	if (UWorld* World=FindWorld()) if (auto* PC=World->GetFirstPlayerController())
		if (auto* Local=PC->GetPawn(); Local && Subject && (Subject!=Local || Other))
		{
			const FVector Focus=Other ? (Subject->GetActorLocation()+Other->GetActorLocation())*.5f : Subject->GetActorLocation();
			if (FVector::DistSquared2D(Focus,Local->GetActorLocation())>1.f)
				PC->SetControlRotation((Focus-Local->GetActorLocation()).Rotation());
		}
	if (auto* World=FindWorld())
		if (!ValidateVisualFixturePawn(Cast<AmultiplayerCombatCharacter>(World->GetFirstPlayerController()->GetPawn()))) return false;
	if (!CaptureEscapeView(TEXT("FIXTURE-")+Mode+TEXT("-")+Name)) return false;
	SendReceipt((bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+Name,true,
		TEXT("FIXTURE: actual current-token PNG saved; underlying gameplay/state assertions remain independent"));
	return true;
}

void UCoopNetTestDriver::EscapeServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	auto* Primary = Cast<AmultiplayerCombatCharacter>(PrimaryPlayer(World) ? PrimaryPlayer(World)->GetPawn() : nullptr);
	auto* Partner = Cast<AmultiplayerCombatCharacter>(PartnerPlayer(World) ? PartnerPlayer(World)->GetPawn() : nullptr);
	if (!State || !Primary || !Partner) return;
	BindEscapeHitDiagnostics(Primary); BindEscapeHitDiagnostics(Partner);
	const auto KeyGoal = [World](const TCHAR* Name, const AmultiplayerCombatCharacter* Player)
	{
		const auto* Key = EscapeActor<AmultiplayerCoopKey>(World,Name);
		return EscapeBool(Key,TEXT("bInstalled")) ? Player->GetActorLocation() : Key->GetActorLocation();
	};
	const auto Both = [this](const TCHAR* Name)
	{
		return Receipts.Contains(FString(TEXT("PrimaryEscape")) + Name) && Receipts.Contains(FString(TEXT("PartnerEscape")) + Name);
	};
	if (Phase == TEXT("ScenarioStart"))
	{
		Assert(TEXT("ServerEscapeMap"), World->GetMapName().Contains(TEXT("CoopEscape"))
			&& EscapeActor<AmultiplayerCombatEncounter>(World, TEXT("Escape_Encounter"))
			&& EscapeActor<AmultiplayerCoopKey>(World, TEXT("Escape_Key_1"))
			&& EscapeActor<AmultiplayerCoopKey>(World, TEXT("Escape_Key_4"))
			&& EscapeActor<AActor>(World,TEXT("Escape_Exit")), TEXT("Actual saved escape map, not spawned substitute objectives"));
		if (bDone) return;
		Assert(TEXT("ServerEscapeAdmission"), Primary->IsCombatReady() && Partner->IsCombatReady()
			&& State->GetObjectiveState().ActivatedKeys == 0 && State->GetObjectiveState().RequiredKeys == 4,
			TEXT("Both admitted production pawns start at a fresh 0/4 objective"));
		EscapeRoundWorld = World;
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It) ++EscapeEnemyBirths;
		EscapeSpawnObserver = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateWeakLambda(this,
			[this](AActor* Actor) { if (Actor->IsA<AmultiplayerCombatEnemy>()) ++EscapeEnemyBirths; }));
		EscapePrimaryOrigin = Primary->GetActorLocation(); EscapePartnerOrigin = Partner->GetActorLocation();
		EscapePrimaryMinHealth = Primary->GetHealth(); EscapePartnerMinHealth = Partner->GetHealth();
		if (bEscapeMovementDiagnostic)
		{
			EscapeEmptySince=FPlatformTime::Seconds();
			SetEscapeMove(World,TEXT("EscapeKeys12"),Primary->GetActorLocation(),EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_2"))->GetActorLocation());
			return;
		}
		SetCommand(TEXT("EscapePrepare"));
		return;
	}
	if (Phase == TEXT("EscapeRestart") && EscapeRoundWorld.Get() != World)
	{
		auto* PrimarySession = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		auto* PartnerSession = Cast<AmultiplayerCoopPlayerController>(PartnerPlayer(World));
		if (!PrimarySession || !PartnerSession || !PrimarySession->HasAcceptedCombatSession() || !PartnerSession->HasAcceptedCombatSession()) return;
		if (!Probe.IsValid()) { FActorSpawnParameters Params; Params.Owner = PrimarySession; Probe = World->SpawnActor<ACoopNetTestProbe>(Params); }
		if (!PartnerProbe.IsValid()) return;
		Assert(TEXT("ServerEscapeRestarted"), State->GetObjectiveState().ActivatedKeys == 0 && State->GetObjectiveState().RequiredKeys == 4
			&& !State->HasGameWon() && !State->HasGameLost()
			&& !EscapeBool(EscapeActor<AmultiplayerCoopKey>(World, TEXT("Escape_Key_1")), TEXT("bInstalled"))
			&& !EscapeBool(EscapeActor<AmultiplayerCoopGate>(World, TEXT("Escape_Gate")), TEXT("bGateOpen")),
			TEXT("Enter requested production RestartGame travel; new World has fresh keys, closed gate and clear result"));
		if (!bDone) SetCommand(TEXT("EscapeRestarted"));
		return;
	}
	if (Phase == TEXT("EscapeRestarted"))
	{
		if (Both(TEXT("Restarted"))) { Probe->Stage = TEXT("Complete"); Probe->ForceNetUpdate(); Finish(true, TEXT("Actual playable escape loop and production restart completed")); }
		return;
	}
	if (Phase == TEXT("EscapeRestart")) return;
	EscapePrimaryMinHealth = FMath::Min(EscapePrimaryMinHealth, Primary->GetHealth());
	EscapePartnerMinHealth = FMath::Min(EscapePartnerMinHealth, Partner->GetHealth());
	if (FPlatformTime::Seconds() - EscapeLastSampleAt >= 5.f)
	{
		EscapeLastSampleAt = FPlatformTime::Seconds();
		int32 Pending = 0;
		for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) Pending += It->GetPendingSpawnCount();
		const auto Encounter = State->GetEncounterState();
		FString EnemySnapshot;
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
			if (It->IsAlive()) EnemySnapshot += FString::Printf(TEXT(" %s:hp=%.0f@%s"), *It->GetName(), It->GetHealth(), *It->GetActorLocation().ToCompactString());
		Emit(TEXT("ESCAPE_PROGRESS"), FString::Printf(TEXT("wave=%d/%d living=%d pending=%d births=%d primary[hp=%.0f shield=%.0f energy=%.0f mode=%d pos=%s] partner[hp=%.0f shield=%.0f energy=%.0f mode=%d pos=%s] enemies=%s"),
			Encounter.Wave, Encounter.TotalWaves, Encounter.LivingEnemies, Pending, EscapeEnemyBirths,
			Primary->GetHealth(), Primary->GetShield(), Primary->GetEnergy(), int32(Primary->GetCharacterMovement()->MovementMode), *Primary->GetActorLocation().ToCompactString(),
			Partner->GetHealth(), Partner->GetShield(), Partner->GetEnergy(), int32(Partner->GetCharacterMovement()->MovementMode), *Partner->GetActorLocation().ToCompactString(), *EnemySnapshot));
		Emit(TEXT("ESCAPE_MOVEMENT"),TEXT("primary[")+EscapeMovementSnapshot(Primary)+TEXT("] partner[")+EscapeMovementSnapshot(Partner)+TEXT("]"));
	}
	if (State->HasGameLost())
	{
		Assert(TEXT("ServerEscapeSurvival"), false, TEXT("The real encounter defeated both players before escape; no fixture revival or health reset is permitted")); return;
	}
	if (bEscapeMovementDiagnostic)
	{
		if (FPlatformTime::Seconds()-EscapeEmptySince>30.f)
		{
			Emit(TEXT("DIAGNOSTIC_ONLY"),TEXT("Finished 30 seconds of actual movement/contact logging; ordinary escape acceptance was not executed"));
			Finish(false,TEXT("Diagnostic-only capture completed, not an escape acceptance pass"));
			return;
		}
		if (Phase==TEXT("EscapeKeys12") && FVector::Dist2D(Partner->GetActorLocation(),EscapePartnerGoal)<90.f)
			SetEscapeMove(World,TEXT("EscapeKeys34"),Primary->GetActorLocation(),EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_4"))->GetActorLocation());
		else if (FPlatformTime::Seconds()-EscapeLastRepathAt>3.f) SetEscapeMove(World,Phase,Primary->GetActorLocation(),EscapePartnerGoal);
		return;
	}
	if (Phase == TEXT("EscapePrepare"))
	{
		if (!Both(TEXT("Ready")) || !Both(TEXT("DashLearned")) || !Both(TEXT("DashUsed"))) return;
		Assert(TEXT("ServerEscapeLearned"), Primary->GetProgressionComponent()->GetState().ActiveLevels[4] == 1
			&& Partner->GetProgressionComponent()->GetState().ActiveLevels[4] == 1, TEXT("Both owning learn RPCs granted the real Dash spec; no server GiveAbility shortcut"));
		SetEscapeMove(World, TEXT("EscapeContact"), FVector(-2250,-2800,180), FVector(-1950,-2800,180));
		return;
	}
	if (Phase == TEXT("EscapeContact") && Both(TEXT("DamageObserved")))
	{
		SetEscapeMove(World, TEXT("EscapeClear"), EscapePrimaryGoal, EscapePartnerGoal);
		return;
	}
	if (Phase == TEXT("EscapeClear"))
	{
		const auto Encounter = State->GetEncounterState();
		int32 Pending = 0;
		for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) if (!It->IsActorBeingDestroyed()) Pending += It->GetPendingSpawnCount();
		if (Encounter.Wave == 3 && Encounter.LivingEnemies == 0 && Pending == 0 && Both(TEXT("WavesCleared")))
		{
			Assert(TEXT("ServerEscapeWaves"), EscapeEnemyBirths == 12,
				FString::Printf(TEXT("Actual three waves reached 3/3, births=%d/12, pending=0, living=0; no fixture damage"), EscapeEnemyBirths));
			Assert(TEXT("ServerEscapeDamage"), EscapePrimaryMinHealth < 100.f || EscapePartnerMinHealth < 100.f,
				FString::Printf(TEXT("Real enemy attack health minima: %.1f / %.1f"), EscapePrimaryMinHealth, EscapePartnerMinHealth));
			if (bDone) return;
			auto* Left = EscapeActor<AmultiplayerCombatSupply>(World, TEXT("Escape_Supply_1"));
			auto* Right = EscapeActor<AmultiplayerCombatSupply>(World, TEXT("Escape_Supply_3"));
			if (!Left || !Right) { Assert(TEXT("ServerEscapeSupplyMap"), false); return; }
			SetEscapeMove(World, TEXT("EscapeSupply"), Left->GetActorLocation(), Right->GetActorLocation());
		}
	}
	else if (Phase == TEXT("EscapeSupply") && Both(TEXT("Supply")))
	{
		auto* Left = EscapeActor<AmultiplayerCombatSupply>(World, TEXT("Escape_Supply_1"));
		auto* Right = EscapeActor<AmultiplayerCombatSupply>(World, TEXT("Escape_Supply_3"));
		Assert(TEXT("ServerEscapeSupplies"), Left && Right && Left->IsConsumed() && Right->IsConsumed(), TEXT("Both depleted players physically overlapped and consumed two actual supplies"));
		if (!bDone) SetEscapeMove(World, TEXT("EscapeKeys12"), KeyGoal(TEXT("Escape_Key_1"),Primary), KeyGoal(TEXT("Escape_Key_2"),Partner));
	}
	else if (Phase == TEXT("EscapeKeys12") && State->GetObjectiveState().ActivatedKeys >= 2 && Both(TEXT("Keys12")))
	{
		Assert(TEXT("ServerEscapeKeys12"), EscapeBool(EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_1")),TEXT("bInstalled"))
			&& EscapeBool(EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_2")),TEXT("bInstalled")),
			TEXT("Both assigned first-pair keys were physically installed; incidental pickups on earlier routes are retained"));
		if (bDone) return;
		SetEscapeMove(World, TEXT("EscapeKeys34"), KeyGoal(TEXT("Escape_Key_3"),Primary), KeyGoal(TEXT("Escape_Key_4"),Partner));
	}
	else if (Phase == TEXT("EscapeKeys34") && State->GetObjectiveState().ActivatedKeys == 4 && Both(TEXT("Keys34")))
	{
		Assert(TEXT("ServerEscapeKeys34"), true, TEXT("All four pre-existing keys are physically installed; no objective setter was used"));
		SetEscapeMove(World, TEXT("EscapeGate"), EscapeActor<AmultiplayerPressurePlate>(World,TEXT("Escape_Plate_1"))->GetActorLocation(), EscapeActor<AmultiplayerPressurePlate>(World,TEXT("Escape_Plate_2"))->GetActorLocation());
	}
	else if (Phase == TEXT("EscapeGate"))
	{
		auto* Left=EscapeActor<AmultiplayerPressurePlate>(World,TEXT("Escape_Plate_1"));
		auto* Right=EscapeActor<AmultiplayerPressurePlate>(World,TEXT("Escape_Plate_2"));
		const bool bOpen=EscapeBool(EscapeActor<AmultiplayerCoopGate>(World,TEXT("Escape_Gate")),TEXT("bGateOpen"));
		if (bOpen && Left->IsPlateActive() && Right->IsPlateActive() && !SentReceipts.Contains(TEXT("ServerEscapeGate")))
		{
			TArray<ACharacter*> Occupants;
			Left->GetOccupyingCharacters(Occupants); Right->GetOccupyingCharacters(Occupants);
			// 在真实激活的时点取证；锁存门允许玩家之后离板，不能等异步截图回执后再要求旧条件仍保持。
			if (Occupants.Contains(Primary) && Occupants.Contains(Partner))
			{
				SentReceipts.Add(TEXT("ServerEscapeGate"));
				Assert(TEXT("ServerEscapeGate"),true,TEXT("Authority simultaneously observed both actual plates active, both different admitted pawns occupying them, and the production gate open"));
			}
		}
		if (bOpen && SentReceipts.Contains(TEXT("ServerEscapeGate")) && Both(TEXT("Gate")))
		{
			const FVector Exit=EscapeActor<AActor>(World,TEXT("Escape_Exit"))->GetActorLocation();
			SetEscapeMove(World,TEXT("EscapeExit"),Exit+FVector(-100,0,0),Exit+FVector(100,0,0));
		}
	}
	else if (Phase == TEXT("EscapeExit") && State->HasGameWon())
	{
		Assert(TEXT("ServerEscapeWon"), State->IsObjectiveComplete() && FVector::Dist2D(Primary->GetActorLocation(),EscapePrimaryOrigin)>1000.f
			&& FVector::Dist2D(Partner->GetActorLocation(),EscapePartnerOrigin)>1000.f, TEXT("Both CMC-controlled pawns physically reached the real WinArea after keys and cooperation"));
		if (!bDone) SetCommand(TEXT("EscapeWon"));
	}
	else if (Phase == TEXT("EscapeWon") && Both(TEXT("Won"))) { SetCommand(TEXT("EscapeRestart")); return; }
	if (!bDone && EscapeMoveCommand(Phase) && FPlatformTime::Seconds()-EscapeLastRepathAt>3.f)
		SetEscapeMove(World, Phase, EscapePrimaryGoal, EscapePartnerGoal);
#endif
}

void UCoopNetTestDriver::EscapeClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	auto* PC = World->GetFirstPlayerController();
	auto* Local = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (!Local || !State) return;
	BindEscapeHitDiagnostics(Local);
	const FString Prefix = bPartner ? TEXT("PartnerEscape") : TEXT("PrimaryEscape");
	const FString Command = Probe->Stage;
	const double Now = FPlatformTime::Seconds();
	if (Phase != Command) SetPhase(Command);
	if (Now - EscapeLastSampleAt >= 5.f)
	{
		EscapeLastSampleAt = Now;
		AmultiplayerCombatEnemy* Nearest = nullptr;
		float Distance = MAX_flt;
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
		{
			const float CandidateDistance = FVector::Dist2D(Local->GetActorLocation(), It->GetActorLocation());
			if (It->IsAlive() && CandidateDistance < Distance) { Nearest = *It; Distance = CandidateDistance; }
		}
		const FVector Goal = Probe->EscapeRoute.Points.IsValidIndex(EscapeRoutePoint) ? Probe->EscapeRoute.Points[EscapeRoutePoint] : FVector::ZeroVector;
		Emit(TEXT("ESCAPE_PROGRESS"), FString::Printf(TEXT("command=%s ready=%d route=%d:%d/%d routeCommand=%s goal=%s W=%d mode=%d velocity=%s hp=%.0f energy=%.0f pos=%s nearest=%s distance=%.0f LOS=%d"),
			*Command, Local->IsCombatReady(), Probe->EscapeRoute.Revision, EscapeRoutePoint, Probe->EscapeRoute.Points.Num(), *Probe->EscapeRoute.Command, *Goal.ToCompactString(), bEscapeWalking,
			int32(Local->GetCharacterMovement()->MovementMode), *Local->GetVelocity().ToCompactString(), Local->GetHealth(), Local->GetEnergy(), *Local->GetActorLocation().ToCompactString(),
			*GetNameSafe(Nearest), Distance, Nearest && PC->LineOfSightTo(Nearest)));
		Emit(TEXT("ESCAPE_MOVEMENT"),FString::Printf(TEXT("keyW=%d rawW=%.2f %s"),PC->IsInputKeyDown(EKeys::W),PC->GetInputAnalogKeyState(EKeys::W),*EscapeMovementSnapshot(Local)));
	}
	if (bEscapeEnterHeld) { PC->InputKey(FInputKeyParams(EKeys::Enter, IE_Released, 0.0)); bEscapeEnterHeld=false; }
	if (Command == TEXT("Complete")) { if (!bPartner) { SendReceipt(TEXT("Complete"), true, TEXT("Actual playable escape loop and restart completed")); Finish(true); } return; }
	if (Command == TEXT("EscapePrepare"))
	{
		if (!Local->IsCombatReady()) return;
		if (CaptureEscapeView(TEXT("01-Ready")))
			SendReceipt(Prefix+TEXT("Ready"), true, TEXT("Production pawn, ASC, input mapping and objective snapshot are present"));
		if (!SentReceipts.Contains(TEXT("EscapeLearnRequested")))
		{
			SentReceipts.Add(TEXT("EscapeLearnRequested")); Local->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
		}
		if (Local->GetProgressionComponent()->GetState().ActiveLevels[4] == 1)
		{
			SendReceipt(Prefix+TEXT("DashLearned"), true, TEXT("Owning replicated loadout contains learned Dash"));
			if (!SentReceipts.Contains(Prefix+TEXT("DashUsed")))
			{
				PC->SetControlRotation(FRotator(0,-90,0)); Local->Dash();
				if (Local->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing))
					SendReceipt(Prefix+TEXT("DashUsed"), true, TEXT("Learned Dash actually predicted movement and consumed its configured resource"));
			}
		}
		return;
	}
	if (Command == TEXT("EscapeWon") || Command == TEXT("EscapeRestart"))
	{
		ClearEscapeInput();
		if (State->HasGameWon() && CaptureEscapeView(TEXT("07-Victory"))) SendReceipt(Prefix+TEXT("Won"), true, TEXT("Independent client sees the real replicated win state"));
		if (Command == TEXT("EscapeRestart") && !bPartner && !SentReceipts.Contains(TEXT("EscapeEnterRequested")))
		{
			SentReceipts.Add(TEXT("EscapeEnterRequested")); PC->InputKey(FInputKeyParams(EKeys::Enter, IE_Pressed, 1.0)); bEscapeEnterHeld=true;
		}
		return;
	}
	if (Command == TEXT("EscapeRestarted"))
	{
		ClearEscapeInput();
		if (!State->HasGameWon() && !State->HasGameLost() && State->GetObjectiveState().ActivatedKeys==0
			&& State->GetObjectiveState().RequiredKeys==4 && Local->IsCombatReady())
		{
			if (CaptureEscapeView(TEXT("08-Restarted"))) SendReceipt(Prefix+TEXT("Restarted"), true, TEXT("New owning pawn and 0/4 shared state arrived after actual restart travel"));
		}
		return;
	}
	if (!EscapeMoveCommand(Command)) return;
	if (Command==TEXT("EscapeGate"))
	{
		const auto* OwnPlate=EscapeActor<AmultiplayerPressurePlate>(World,bPartner ? TEXT("Escape_Plate_2") : TEXT("Escape_Plate_1"));
		const bool bOpen=EscapeBool(EscapeActor<AmultiplayerCoopGate>(World,TEXT("Escape_Gate")),TEXT("bGateOpen"));
		if (bOpen || (OwnPlate && OwnPlate->IsPlateActive()))
		{
			SetEscapeWalking(PC,false);
			if (bOpen && CaptureEscapeView(TEXT("06-CooperativeGate")))
				SendReceipt(Prefix+TEXT("Gate"),true,TEXT("Client stopped on its assigned real plate and received the production gate's latched open state"));
			return;
		}
	}
	if (EscapeLocalCommand != Command || EscapeRouteRevision != Probe->EscapeRoute.Revision)
	{
		if (Probe->EscapeRoute.Command != Command) { SetEscapeWalking(PC,false); return; }
		EscapeLocalCommand=Command; EscapeRouteRevision=Probe->EscapeRoute.Revision; EscapeRoutePoint=0;
	}
	if (Command == TEXT("EscapeContact") && !SentReceipts.Contains(Prefix+TEXT("DamageObserved")))
	{
		for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
			if (!Local->IsHostileTo(*It) && It->GetHealth() < It->GetMaxHealth())
			{
				SendReceipt(Prefix+TEXT("DamageObserved"), true, FString::Printf(TEXT("Real enemy attack reduced %s to %.1f/%.1f HP; this independent client received the health state before any defense or return fire"),
					*It->GetName(), It->GetHealth(), It->GetMaxHealth()));
				break;
			}
	}
	if (!Local->IsCombatReady()) { SetEscapeWalking(PC,false); CaptureEscapeView(TEXT("02-Downed")); return; }
	// 真正有队友倒地才尝试救援，不为验收直接制造伤害或复活。
	AmultiplayerCombatCharacter* Downed = nullptr;
	for (TActorIterator<AmultiplayerCombatCharacter> It(World);It;++It)
		if (*It!=Local && !Local->IsHostileTo(*It) && It->GetCoopComponent()->IsDowned()) { Downed=*It; break; }
	if (Downed)
	{
		const FVector Direction=(Downed->GetActorLocation()-Local->GetActorLocation()).GetSafeNormal2D();
		PC->SetControlRotation(Direction.Rotation());
		const bool bNear=FVector::Dist2D(Downed->GetActorLocation(),Local->GetActorLocation())<180.f;
		SetEscapeWalking(PC,!bNear);
		if(bNear && !bEscapeRescueHeld) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Pressed,1.0)); bEscapeRescueHeld=true; CaptureEscapeView(TEXT("03-Rescue")); }
		return;
	}
	if (bEscapeRescueHeld) { PC->InputKey(FInputKeyParams(EKeys::F,IE_Released,0.0)); bEscapeRescueHeld=false; }
	AmultiplayerCombatEnemy* Target=nullptr;
	float LowestHealth=MAX_flt, Distance=MAX_flt;
	for(TActorIterator<AmultiplayerCombatEnemy> It(World);It;++It)
	{
		if (bEscapeMovementDiagnostic) break;
		const float CandidateDistance=FVector::Dist2D(Local->GetActorLocation(),It->GetActorLocation());
		if(!It->IsCombatReady() || CandidateDistance>1100.f || !PC->LineOfSightTo(*It)) continue;
		if(It->GetHealth()<LowestHealth || (It->GetHealth()==LowestHealth && CandidateDistance<Distance))
		{Target=*It;LowestHealth=It->GetHealth();Distance=CandidateDistance;}
	}
	// 首次接敌只停手等待真实伤害；两端均收到生命变化后，服务器才允许进入正常防御与集火阶段。
	if (Command == TEXT("EscapeContact"))
	{
		if (Target && EscapeFirstEnemyAt == 0) EscapeFirstEnemyAt = Now;
		if (EscapeFirstEnemyAt > 0 && Now-EscapeFirstEnemyAt > 15.f && !SentReceipts.Contains(Prefix+TEXT("DamageObserved")))
		{
			SetEscapeWalking(PC,false);
			SendReceipt(Prefix+TEXT("DamageObserved"), false, FString::Printf(TEXT("AI produced no actual life damage within 15 seconds of first contact while players held fire and defense: target=%s distance=%.0f targetCasting=%d localHP=%.0f shield=%.0f"),
				*GetNameSafe(Target), Distance, Target && Target->IsCasting(), Local->GetHealth(), Local->GetShield()));
			return;
		}
		if (Target)
		{
			SetEscapeWalking(PC,false);
			PC->SetControlRotation((Target->GetActorLocation()-Local->GetActorLocation()).Rotation());
			CaptureEscapeView(TEXT("02-FirstContact"));
			return;
		}
	}
	if(Target)
	{
		SetEscapeWalking(PC,false);
		const float Travel=Distance/Local->GetCombatConfig().ProjectileSpeed + Local->GetCombatConfig().Attack.Windup;
		PC->SetControlRotation((Target->GetActorLocation()+Target->GetVelocity()*Travel-Local->GetActorLocation()).Rotation());
		if(Local->GetHealth()<Local->GetMaxHealth()*.65f && Local->GetEnergy()>=Local->GetCombatConfig().Heal.Cost
			&& Local->GetHealCooldownRemaining()<=0.f && Now-EscapeLastHealAt>1.f)
		{Local->Heal(); EscapeLastHealAt=Now;}
		else if(Distance<500.f && Local->GetEnergy()>=Local->GetCombatConfig().Defend.Cost
			&& Local->GetDefenseCooldownRemaining()<=0.f && Now-EscapeLastDefenseAt>1.f)
		{Local->Defend(); EscapeLastDefenseAt=Now;}
		else if(Local->GetAttackCooldownRemaining()<=0.f && Now-EscapeLastAttackAt>.5f)
		{Local->Attack(); EscapeLastAttackAt=Now;}
		CaptureEscapeView(TEXT("02-Combat"));
		return;
	}
	const auto& Route=Probe->EscapeRoute.Points;
	while(EscapeRoutePoint<Route.Num() && FVector::Dist2D(Local->GetActorLocation(),Route[EscapeRoutePoint])<65.f) ++EscapeRoutePoint;
	if(EscapeRoutePoint<Route.Num())
	{
		PC->SetControlRotation((Route[EscapeRoutePoint]-Local->GetActorLocation()).GetSafeNormal2D().Rotation());
		SetEscapeWalking(PC,true);
	}
	else SetEscapeWalking(PC,false);
	if(Command==TEXT("EscapeClear") && State->GetEncounterState().Wave==3 && State->GetEncounterState().LivingEnemies==0)
	{
		if(EscapeEmptySince==0) EscapeEmptySince=Now;
		if(Now-EscapeEmptySince>3.f && CaptureEscapeView(TEXT("03-WavesCleared"))) SendReceipt(Prefix+TEXT("WavesCleared"),true,TEXT("Client sees actual 3/3 wave with a stable empty interval longer than staggered spawn spacing"));
	}
	else EscapeEmptySince=0;
	if(Command==TEXT("EscapeSupply"))
	{
		auto* Supply=EscapeActor<AmultiplayerCombatSupply>(World,bPartner?TEXT("Escape_Supply_3"):TEXT("Escape_Supply_1"));
		if(Supply && Supply->IsConsumed() && CaptureEscapeView(TEXT("04-Supply"))) SendReceipt(Prefix+TEXT("Supply"),true,TEXT("Actual pickup Consumed snapshot arrived after physical movement"));
	}
	if(Command==TEXT("EscapeKeys12") && State->GetObjectiveState().ActivatedKeys>=2
		&& EscapeBool(EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_1")),TEXT("bInstalled"))
		&& EscapeBool(EscapeActor<AmultiplayerCoopKey>(World,TEXT("Escape_Key_2")),TEXT("bInstalled")))
	{if(CaptureEscapeView(TEXT("05-Keys12"))) SendReceipt(Prefix+TEXT("Keys12"),true,TEXT("Client independently sees both first-pair key installation snapshots and shared progress"));}
	if(Command==TEXT("EscapeKeys34") && State->GetObjectiveState().ActivatedKeys==4)
	{if(CaptureEscapeView(TEXT("05-Keys34"))) SendReceipt(Prefix+TEXT("Keys34"),true,TEXT("Client independently sees production objective 4/4"));}
#endif
}
