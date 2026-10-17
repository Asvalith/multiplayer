// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Player/multiplayerCoopPlayerController.h"

namespace
{
	// 独立世界使用真实 CombatPlayer 蓝图、GameMode 和 CMC；不手动设 Walking，不冻结移动。
	// 本组验证出生/接纳后的实际位移，不把单世界执行说成网络预测或真人按键验收。
	struct FCombatMovementWorld
	{
		UWorld* World;
		AmultiplayerGameMode* Mode = nullptr;
		UClass* PlayerClass = nullptr;
		uint64 PreviousFrame = GFrameCounter;

		FCombatMovementWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			// 仅供引擎按正常入口创建权威 GameMode；不初始化项目 GI 或订阅全局网络事件。
			UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
			World->SetGameInstance(Instance);
			Context.OwningGameInstance = Instance;
			World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
			World->SetGameMode(FURL());
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
			PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
			if (Mode && PlayerClass) Mode->DefaultPawnClass = PlayerClass;

			AActor* Ground = World->SpawnActor<AActor>();
			UBoxComponent* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(10000, 10000, 10));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0, 0, -10));
		}

		~FCombatMovementWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 每步提交与角色输入相同的 AddMovementInput，再由真实 CMC 处理加速、碰撞和落地。
		void Advance(float Seconds, AmultiplayerCombatCharacter* Moving = nullptr)
		{
			for (float Elapsed = 0; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.02f, Seconds - Elapsed);
				if (Moving) Moving->AddMovementInput(FVector::ForwardVector, 1.f);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Elapsed += Step;
			}
		}

		float WalkDistance(AmultiplayerCombatCharacter* Character, float Seconds = .4f)
		{
			const FVector Before = Character->GetActorLocation();
			Advance(Seconds, Character);
			return FVector::Dist2D(Before, Character->GetActorLocation());
		}

		// 真实 RestartPlayer 经 FinishRestartPlayer/Possess/PlayingState 同步握手，不直接改接纳标志。
		AmultiplayerCombatCharacter* AdmitPlayer(const FVector& Location)
		{
			if (!Mode || !PlayerClass) return nullptr;
			auto* Start = World->SpawnActor<APlayerStart>(Location, FRotator::ZeroRotator);
			auto* Controller = World->SpawnActor<AmultiplayerCoopPlayerController>();
			if (!Start || !Controller) return nullptr;
			Controller->StartSpot = Start;
			Mode->RestartPlayer(Controller);
			return Cast<AmultiplayerCombatCharacter>(Controller->GetPawn());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionMovementTest, "Multiplayer.Combat.AdmissionMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 复现初始化前 MOVE_None 被禁行快照保存的旧故障；Restart 后尚未接纳仍须禁行，接纳后必须真能走。
bool FCoopCombatAdmissionMovementTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	if (!TestNotNull(TEXT("Production game mode initialized"), Fixture.Mode)
		|| !TestNotNull(TEXT("Actual saved combat player blueprint exists"), Fixture.PlayerClass)) return false;
	const FTransform Transform(FRotator::ZeroRotator, FVector(0, -1800, 110));
	auto* Player = Fixture.World->SpawnActorDeferred<AmultiplayerCombatCharacter>(Fixture.PlayerClass,
		Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Unpossessed actual player spawned"), Player)) return false;
	Player->AutoPossessPlayer = EAutoReceiveInput::Disabled;
	Player->AutoPossessAI = EAutoPossessAI::Disabled;
	Player->FinishSpawning(Transform);
	TestEqual(TEXT("Fresh unpossessed player has not received an engine movement mode"),
		int32(Player->GetCharacterMovement()->MovementMode), int32(MOVE_None));
	Player->SetCombatSessionReady(false);
	// 普通控制器不触发合作项目的自动握手，用于观察 Restart 后、服务器接纳前的真实中间阶段。
	auto* PendingController = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Pending player has a real controller"), PendingController)) return false;
	PendingController->Possess(Player);
	TestFalse(TEXT("Possession alone does not admit the pending player"), Player->IsCombatReady());
	TestEqual(TEXT("Restart cannot unlock an unadmitted player"),
		int32(Player->GetCharacterMovement()->MovementMode), int32(MOVE_None));
	TestTrue(TEXT("Pending player cannot move even when input is submitted"), Fixture.WalkDistance(Player) < .1f);
	Player->SetCombatSessionReady(true);
	TestTrue(TEXT("Admission restores the mode established by Character Restart"),
		Player->GetCharacterMovement()->MovementMode != MOVE_None);
	TestTrue(TEXT("Admitted possessed player moves through real CMC input"), Fixture.WalkDistance(Player) > 50.f);

	// 再走完整 GameMode 出生入口，覆盖本地权威握手在 PlayingState 同步接纳的情况。
	auto* Local = Fixture.AdmitPlayer(FVector(0, 0, 110));
	if (!TestNotNull(TEXT("RestartPlayer created the actual combat blueprint"), Local)) return false;
	auto* Controller = Cast<AmultiplayerCoopPlayerController>(Local->GetController());
	TestTrue(TEXT("Production owned handshake accepted the new controller"), Controller && Controller->HasAcceptedCombatSession());
	TestTrue(TEXT("FinishRestartPlayer does not close a synchronously accepted session"), Local->IsCombatReady());
	TestTrue(TEXT("Real GameMode spawn and handshake produce a movable pawn"), Fixture.WalkDistance(Local) > 50.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatControlMovementTest, "Multiplayer.Combat.ControlMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 不只检查标签或 MovementMode：技能结束/取消与眩晕到期后要继续产生位移，死亡后则始终不动。
bool FCoopCombatControlMovementTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	for (int32 Exit = 0; Exit < 3; ++Exit)
	{
		auto* Player = Fixture.AdmitPlayer(FVector(0, Exit * 1800.f, 110));
		if (!TestNotNull(TEXT("Actual admitted player spawned"), Player)) return false;
		Fixture.Advance(.3f);
		TestTrue(TEXT("Newly admitted player can walk before casting"), Fixture.WalkDistance(Player) > 50.f);
		Player->CastArea();
		if (!TestTrue(TEXT("Actual ability entered its casting lock"), Player->IsCasting())) return false;
		TestTrue(TEXT("Casting stops actual movement even with pending input"), Fixture.WalkDistance(Player, .1f) < .1f);
		if (Exit == 0) Player->CancelCast();
		else if (Exit == 1)
			Fixture.Advance(Player->GetCombatConfig().Area.Windup + Player->GetCombatConfig().Area.Recovery + .2f);
		else
		{
			Player->ApplyStun(.3f);
			TestFalse(TEXT("Stun interrupts the active cast"), Player->IsCasting());
			TestTrue(TEXT("Stun keeps the player immobile"), Fixture.WalkDistance(Player, .1f) < .1f);
			Fixture.Advance(.3f);
			TestFalse(TEXT("Real stun effect expires"), Player->IsStunned());
		}
		TestFalse(TEXT("Each lifecycle exit removes casting"), Player->IsCasting());
		TestTrue(TEXT("Lifecycle exit permits actual movement again"), Fixture.WalkDistance(Player) > 50.f);

		UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
		Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.f);
		ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
		TestFalse(TEXT("Real damage commits death"), Player->IsAlive());
		TestTrue(TEXT("Dead player cannot move from queued input"), Fixture.WalkDistance(Player) < .1f);
		// 引擎重试客户端 Restart 也不能把死亡角色重新变成可移动状态。
		static_cast<ACharacter*>(Player)->Restart();
		TestTrue(TEXT("Restart preserves the persistent death lock"), Fixture.WalkDistance(Player) < .1f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatExistingMovementModeTest, "Multiplayer.Combat.ExistingMovementMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 只修复 Restart 初始化边界；普通控制状态结束时仍尊重其他系统已有的禁行或自定义移动模式。
bool FCoopCombatExistingMovementModeTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110));
	if (!TestNotNull(TEXT("Actual admitted player spawned"), Player)) return false;
	Fixture.Advance(.3f);
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	Movement->DisableMovement();
	Player->ApplyStun(.15f);
	Fixture.Advance(.3f);
	TestFalse(TEXT("Explicitly disabled fixture no longer has stun"), Player->IsStunned());
	TestEqual(TEXT("Stun expiry preserves explicitly disabled movement"), int32(Movement->MovementMode), int32(MOVE_None));
	TestTrue(TEXT("Explicit MOVE_None is not globally changed to Walking"), Fixture.WalkDistance(Player) < .1f);

	Movement->SetMovementMode(MOVE_Custom, 7);
	Player->ApplyStun(.15f);
	Fixture.Advance(.3f);
	TestFalse(TEXT("Custom-mode fixture no longer has stun"), Player->IsStunned());
	TestEqual(TEXT("Control expiry restores the prior custom movement mode"), int32(Movement->MovementMode), int32(MOVE_Custom));
	TestEqual(TEXT("Control expiry restores the custom submode"), int32(Movement->CustomMovementMode), 7);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatWalkingPoseTest, "Multiplayer.Combat.WalkingPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实输入必须同时产生位移和腿部姿态变化；只比较组件空间骨骼，不能把角色整体平移当成走路动画。
bool FCoopCombatWalkingPoseTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110));
	if (!TestNotNull(TEXT("Walking probe uses the actual admitted player"), Player)) return false;
	USkeletalMeshComponent* Mesh = Player->GetMesh();
	// 无视口的测试没有最近渲染帧，显式推进真实动画图；不改变保存资产的可见性优化设置。
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	Fixture.Advance(.3f);
	auto* Animation = Cast<UmultiplayerCombatAnimInstance>(Mesh->GetAnimInstance());
	if (!TestNotNull(TEXT("Saved player instantiates the native combat animation parent"), Animation)) return false;
	const FName Bones[] = { TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l"),
		TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r") };
	for (const FName Bone : Bones)
		if (!TestTrue(*FString::Printf(TEXT("Walking mesh contains %s"), *Bone.ToString()), Mesh->GetBoneIndex(Bone) != INDEX_NONE)) return false;
	const auto Pose = [&]
	{
		TArray<FTransform> Result;
		for (const FName Bone : Bones) Result.Add(Mesh->GetSocketTransform(Bone, RTS_Component));
		return Result;
	};
	const TArray<FTransform> Idle = Pose();
	const FVector Start = Player->GetActorLocation();
	const FName Feet[] = { TEXT("foot_l"), TEXT("foot_r") };
	float FootForwardMin[2] = { MAX_flt, MAX_flt }, FootForwardMax[2] = { -MAX_flt, -MAX_flt };
	float WalkAngle = 0, WalkTranslation = 0;
	for (int32 Sample = 0; Sample < 16; ++Sample)
	{
		Fixture.Advance(.04f, Player);
		const TArray<FTransform> Current = Pose();
		for (int32 Bone = 0; Bone < UE_ARRAY_COUNT(Bones); ++Bone)
		{
			WalkAngle = FMath::Max(WalkAngle, float(Idle[Bone].GetRotation().AngularDistance(Current[Bone].GetRotation())));
			WalkTranslation = FMath::Max(WalkTranslation, float(FVector::Distance(Idle[Bone].GetLocation(), Current[Bone].GetLocation())));
		}
		// 膝盖弯曲不代表双脚真的迈步；单独统计足骨沿角色前向的组件空间摆幅。
		const FVector ComponentForward = Mesh->GetComponentQuat().UnrotateVector(Player->GetActorForwardVector());
		for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
		{
			const FVector Position = Mesh->GetSocketTransform(Feet[Foot], RTS_Component).GetLocation();
			const float Forward = FVector::DotProduct(Position, ComponentForward);
			FootForwardMin[Foot] = FMath::Min(FootForwardMin[Foot], Forward);
			FootForwardMax[Foot] = FMath::Max(FootForwardMax[Foot], Forward);
		}
	}
	TestTrue(TEXT("Real CMC input moves the walking player"), FVector::Dist2D(Start, Player->GetActorLocation()) > 50.f);
	TestTrue(TEXT("Actual movement selects the locomotion pose"), Animation->CombatShouldMove);
	TestTrue(TEXT("Walking rotates leg bones instead of retaining the idle pose"), WalkAngle > .15f);
	TestTrue(TEXT("Walking moves component-space leg bones independently of actor translation"), WalkTranslation > 2.f);
	for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
		TestTrue(*FString::Printf(TEXT("%s genuinely swings forward and back instead of being pinned by IK"), *Feet[Foot].ToString()),
			FootForwardMax[Foot] - FootForwardMin[Foot] > 10.f);

	// 远端同步和 AI 路径跟随可能有速度却无输入加速度；保留惯性只用于这个测试实例。
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	Movement->GroundFriction = 0;
	Movement->BrakingDecelerationWalking = 0;
	Movement->bUseSeparateBrakingFriction = true;
	Movement->BrakingFriction = 0;
	Movement->Velocity = FVector(180, 0, 0);
	const TArray<FTransform> BeforeCoast = Pose();
	const FVector BeforeCoastFeet[] = { Mesh->GetSocketTransform(Feet[0], RTS_Component).GetLocation(),
		Mesh->GetSocketTransform(Feet[1], RTS_Component).GetLocation() };
	float CoastFootMovement[2] = {};
	float CoastAngle = 0;
	for (int32 Sample = 0; Sample < 8; ++Sample)
	{
		Fixture.Advance(.04f);
		const TArray<FTransform> Current = Pose();
		for (int32 Bone = 0; Bone < UE_ARRAY_COUNT(Bones); ++Bone)
			CoastAngle = FMath::Max(CoastAngle, float(BeforeCoast[Bone].GetRotation().AngularDistance(Current[Bone].GetRotation())));
		for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
			CoastFootMovement[Foot] = FMath::Max(CoastFootMovement[Foot], float(FVector::Distance(BeforeCoastFeet[Foot],
				Mesh->GetSocketTransform(Feet[Foot], RTS_Component).GetLocation())));
	}
	TestTrue(TEXT("Coasting probe really has no input acceleration"), Movement->GetCurrentAcceleration().IsNearlyZero());
	TestTrue(TEXT("Coasting probe retains actual horizontal speed"), Player->GetVelocity().Size2D() > 100.f);
	TestTrue(TEXT("Velocity without acceleration still selects locomotion after the blueprint update"), Animation->CombatShouldMove);
	TestTrue(TEXT("Velocity-only movement continues changing the real leg pose"), CoastAngle > .05f);
	for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
		TestTrue(*FString::Printf(TEXT("%s keeps moving without input acceleration"), *Feet[Foot].ToString()), CoastFootMovement[Foot] > 3.f);
	Movement->Velocity = FVector::ZeroVector;
	Fixture.Advance(.3f);
	TestFalse(TEXT("Stopping clears the native movement condition"), Animation->CombatShouldMove);
	AddInfo(FString::Printf(TEXT("Walking pose: angle=%.3frad translation=%.2fcm coastAngle=%.3frad "
		"footForwardRange left=%.2fcm right=%.2fcm coastFootMovement left=%.2fcm right=%.2fcm"),
		WalkAngle, WalkTranslation, CoastAngle, FootForwardMax[0] - FootForwardMin[0], FootForwardMax[1] - FootForwardMin[1],
		CoastFootMovement[0], CoastFootMovement[1]));
	return !HasAnyErrors();
}

#endif
