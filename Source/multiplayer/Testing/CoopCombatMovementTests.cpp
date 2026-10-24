// Copyright Epic Games, Inc. All Rights Reserved.

// 回归正式角色出生、会话接纳与视口输入恢复后的真实 CMC 移动。
// 不手动解除移动锁替代业务修复，也不把离屏输入注入称为真人实机验收。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/multiplayerGameMode.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputKeyEventArgs.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"

namespace
{
	// 独立世界使用真实 CombatPlayer 蓝图、GameMode 和 CMC；不手动设 Walking，不冻结移动。
	// 本组验证出生/接纳后的实际位移，不把单世界执行说成网络预测或真人按键验收。
	struct FCombatMovementWorld
	{
		UWorld* World;
		AmultiplayerGameMode* Mode = nullptr;
		UClass* PlayerClass = nullptr;
		ULocalPlayer* InputPlayer = nullptr;
		UGameViewportClient* ViewportClient = nullptr;
		TSharedPtr<SViewport> ViewportWidget;
		TSharedPtr<SOverlay> ViewportOverlay;
		TSharedPtr<FSceneViewport> SceneViewport;
		uint64 PreviousFrame = GFrameCounter;

		// 创建正式 GameMode、角色类和可行走地面，保留真实出生／接纳流程。
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

		// 按生命周期清理本地玩家和离屏视口，再销毁世界，防止输入模式串到下一用例。
		~FCombatMovementWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			if (InputPlayer) InputPlayer->PlayerRemoved();
			if (ViewportClient)
			{
				ViewportClient->RemoveAllViewportWidgets();
				ViewportClient->SetViewportFrame(nullptr);
				SceneViewport.Reset();
				ViewportWidget.Reset();
				ViewportOverlay.Reset();
			}
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 离屏视口仍走引擎的输入模式和分发入口；不创建可见窗口，也不直接改 IgnoreInput。
		void CreateInputViewport()
		{
			FWorldContext& Context = GEngine->GetWorldContextFromWorldChecked(World);
			World->GetGameInstance()->OnWorldChanged(nullptr, World);
			ViewportClient = NewObject<UGameViewportClient>(GEngine);
			Context.GameViewport = ViewportClient;
			ViewportClient->Init(Context, World->GetGameInstance(), false);
			ViewportWidget = SNew(SViewport);
			ViewportOverlay = SNew(SOverlay);
			SceneViewport = MakeShareable(ViewportClient->CreateGameViewport(ViewportWidget));
			ViewportWidget->SetViewportInterface(SceneViewport.ToSharedRef());
			ViewportClient->SetViewportFrame(SceneViewport.Get());
			ViewportClient->SetViewportOverlayWidget(nullptr, ViewportOverlay.ToSharedRef());
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

		// 连续提交移动输入并读取真实水平位移，而不是只断言速度或移动模式标记。
		float WalkDistance(AmultiplayerCombatCharacter* Character, float Seconds = .4f)
		{
			const FVector Before = Character->GetActorLocation();
			Advance(Seconds, Character);
			return FVector::Dist2D(Before, Character->GetActorLocation());
		}

		// 真实 RestartPlayer 经 FinishRestartPlayer/Possess/PlayingState 同步握手，不直接改接纳标志。
		AmultiplayerCombatCharacter* AdmitPlayer(const FVector& Location, bool bWithLocalInput = false)
		{
			if (!Mode || !PlayerClass) return nullptr;
			auto* Start = World->SpawnActor<APlayerStart>(Location, FRotator::ZeroRotator);
			auto* Controller = World->SpawnActor<AmultiplayerCoopPlayerController>();
			if (!Start || !Controller) return nullptr;
			if (bWithLocalInput)
			{
				// 不创建窗口，也不调用私有 Look/Move；由真实本地玩家、保存的映射和输入栈处理按键。
				InputPlayer = NewObject<ULocalPlayer>(GEngine);
				World->GetGameInstance()->AddLocalPlayer(InputPlayer, FPlatformUserId::CreateFromInternalId(0));
				Controller->SetPlayer(InputPlayer);
			}
			Controller->StartSpot = Start;
			Mode->RestartPlayer(Controller);
			if (bWithLocalInput)
			{
				if (auto* Input = InputPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
				{
					FModifyContextOptions Options;
					Options.bForceImmediately = true;
					Input->RequestRebuildControlMappings(Options);
				}
			}
			return Cast<AmultiplayerCombatCharacter>(Controller->GetPawn());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMenuViewportInputTest, "Multiplayer.Combat.MenuViewportInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 菜单留下的 UIOnly 属于跨地图视口状态；必须由实际战斗 HUD 接回输入，不能绕过视口直接调 PC->InputKey。
bool FCoopCombatMenuViewportInputTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	Fixture.CreateInputViewport();
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110), true);
	if (!TestNotNull(TEXT("Viewport probe possesses the actual combat player"), Player)) return false;
	auto* Controller = Cast<APlayerController>(Player->GetController());
	UClass* HUDClass = LoadClass<AHUD>(nullptr, TEXT("/Game/Combat/UI/BP_CombatHUD.BP_CombatHUD_C"));
	if (!TestNotNull(TEXT("Production combat HUD blueprint exists"), HUDClass)
		|| !TestNotNull(TEXT("Local viewport owns enhanced input"), Cast<UEnhancedPlayerInput>(Controller->PlayerInput))) return false;
	const FInputDeviceId InputDevice = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	if (!TestTrue(TEXT("Viewport input device routes to the fixture local player"),
		GEngine->GetLocalPlayerFromInputDevice(Fixture.ViewportClient, InputDevice) == Fixture.InputPlayer)) return false;
	Fixture.Advance(.3f);
	const FVector BeforeMenuInput = Player->GetActorLocation();

	// 与 BP_MenuController 相同的菜单入口，保留它会跨 Travel 遗留的 UIOnly 和鼠标显示状态。
	UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(Controller, nullptr, EMouseLockMode::DoNotLock, false);
	Controller->bShowMouseCursor = true;
	if (!TestTrue(TEXT("Menu UIOnly actually blocks the shared game viewport"), Fixture.ViewportClient->IgnoreInput())) return false;
	Fixture.ViewportClient->InputKey(FInputKeyEventArgs(Fixture.SceneViewport.Get(), InputDevice, EKeys::W, IE_Pressed));
	Fixture.Advance(.4f);
	Fixture.ViewportClient->InputKey(FInputKeyEventArgs(Fixture.SceneViewport.Get(), InputDevice, EKeys::W, IE_Released));
	TestTrue(TEXT("W through the menu viewport cannot move the admitted player"),
		Player->GetActorLocation().Equals(BeforeMenuInput, .1f));
	TestTrue(TEXT("The blocked player remains alive and admitted, not movement-locked by GAS"),
		Player->IsAlive() && Player->IsCombatReady() && !Player->IsStunned() && !Player->IsCasting()
		&& Player->GetCharacterMovement()->MovementMode != MOVE_None);

	// 引擎创建真实 HUD 并执行其原有蓝图 BeginPlay；测试自身不调用 GameOnly 或解锁移动。
	Controller->ClientSetHUD(HUDClass);
	if (!TestNotNull(TEXT("Production HUD was created by ClientSetHUD"), Controller->GetHUD())) return false;
	TestTrue(TEXT("The saved production HUD is used, not the separate native presentation HUD"),
		Controller->GetHUD()->GetClass() == HUDClass);
	TestFalse(TEXT("Combat HUD BeginPlay restores game viewport input after the menu"), Fixture.ViewportClient->IgnoreInput());
	TestFalse(TEXT("Combat HUD hides the menu cursor for gameplay"), Controller->bShowMouseCursor);
	const FVector BeforeGameInput = Player->GetActorLocation();
	Fixture.ViewportClient->InputKey(FInputKeyEventArgs(Fixture.SceneViewport.Get(), InputDevice, EKeys::W, IE_Pressed));
	Fixture.Advance(.4f);
	Fixture.ViewportClient->InputKey(FInputKeyEventArgs(Fixture.SceneViewport.Get(), InputDevice, EKeys::W, IE_Released));
	const float Distance = FVector::Dist2D(BeforeGameInput, Player->GetActorLocation());
	TestTrue(TEXT("W through the restored viewport produces real CharacterMovement displacement"), Distance > 50.f);
	AddInfo(FString::Printf(TEXT("Menu-to-combat viewport W movement: %.2fcm"), Distance));
	return !HasAnyErrors();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCameraFacingTest, "Multiplayer.Combat.CameraFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 走 PC/EnhancedInput 的真实鼠标和按键入口；不能只检查蓝图开关，也不能手调角色旋转来代替技能转身。
bool FCoopCombatCameraFacingTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110), true);
	if (!TestNotNull(TEXT("Camera probe possesses the actual combat player"), Player)) return false;
	auto* Controller = Cast<APlayerController>(Player->GetController());
	UCameraComponent* Camera = Player->FindComponentByClass<UCameraComponent>();
	if (!TestNotNull(TEXT("Local controller owns enhanced input"), Cast<UEnhancedPlayerInput>(Controller->PlayerInput))
		|| !TestNotNull(TEXT("Possession creates the real pawn input component"), Player->InputComponent.Get())
		|| !TestNotNull(TEXT("Actual saved player contains its camera"), Camera)) return false;
	Fixture.Advance(.3f);
	const FVector Start = Player->GetActorLocation();
	const FRotator BodyBeforeLook = Player->GetActorRotation();
	const FRotator ControlBeforeLook = Controller->GetControlRotation();
	const FRotator CameraBeforeLook = Camera->GetComponentRotation();
	Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
	Fixture.Advance(.04f);
	TestTrue(TEXT("Mouse mapping actually changes control rotation"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(ControlBeforeLook.Yaw, Controller->GetControlRotation().Yaw)) > 1.f);
	TestTrue(TEXT("Free camera really follows the mouse"), !CameraBeforeLook.Equals(Camera->GetComponentRotation(), .1f));
	TestTrue(TEXT("Looking without movement does not rotate the body"), BodyBeforeLook.Equals(Player->GetActorRotation(), .1f));
	TestTrue(TEXT("Looking does not translate the capsule"), Player->GetActorLocation().Equals(Start, .1f));

	const FVector ExpectedMove = FRotator(0, Controller->GetControlRotation().Yaw, 0).Vector();
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Pressed, 1.));
	Fixture.Advance(.4f);
	const FVector Displacement = Player->GetActorLocation() - Start;
	TestTrue(TEXT("Saved W mapping produces real CMC movement"), Displacement.Size2D() > 50.f);
	TestTrue(TEXT("Movement is relative to the free camera heading"),
		FVector::DotProduct(Displacement.GetSafeNormal2D(), ExpectedMove) > .95f);
	TestTrue(TEXT("Moving turns the body toward travel rather than its old heading"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, ExpectedMove.Rotation().Yaw)) < 5.f);
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.));
	Fixture.Advance(.5f);
	Controller->InputKey(FInputKeyParams(EKeys::MouseX, 160., .02f, 1));
	Fixture.Advance(.04f);
	const float AttackYaw = Controller->GetControlRotation().Yaw;
	TestTrue(TEXT("Attack fixture is genuinely aiming away from its body"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, AttackYaw)) > 1.f);
	Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Pressed, 1.));
	Fixture.Advance(.02f);
	Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Released, 0.));
	TestTrue(TEXT("Actual mouse button is accepted by GAS"), Player->GetAttackCooldownRemaining() > 0);
	TestTrue(TEXT("Accepted attack turns the body to this activation aim"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, AttackYaw)) < .1f);
	TestTrue(TEXT("Attack facing remains upright"), FMath::Abs(Player->GetActorRotation().Pitch) < .1f
		&& FMath::Abs(Player->GetActorRotation().Roll) < .1f);

	Controller->InputKey(FInputKeyParams(EKeys::MouseX, -160., .02f, 1));
	Fixture.Advance(.02f);
	const FRotator BeforeRejectedAttack = Player->GetActorRotation();
	const float EnergyBeforeRejectedAttack = Player->GetEnergy();
	Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Pressed, 1.));
	Fixture.Advance(.02f);
	Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Released, 0.));
	TestTrue(TEXT("Attack rejected during cooldown does not turn the body"),
		BeforeRejectedAttack.Equals(Player->GetActorRotation(), .1f));
	TestEqual(TEXT("Rejected attack does not consume additional energy"), Player->GetEnergy(), EnergyBeforeRejectedAttack);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDeathInputTest, "Multiplayer.Combat.DeathInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 死亡退出 Pawn 输入栈，而不是每帧钉住 Transform 或关闭整个 Controller；迟到控制器也不能重新开放尸体输入。
bool FCoopCombatDeathInputTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110), true);
	if (!TestNotNull(TEXT("Death input probe possesses the real combat blueprint"), Player)) return false;
	auto* Controller = Cast<APlayerController>(Player->GetController());
	UCameraComponent* Camera = Player->FindComponentByClass<UCameraComponent>();
	if (!TestNotNull(TEXT("Death input controller is initialized"), Controller->PlayerInput.Get())
		|| !TestNotNull(TEXT("Pawn owns an actual input component"), Player->InputComponent.Get())
		|| !TestNotNull(TEXT("Death input probe contains the real camera"), Camera)) return false;
	Fixture.Advance(.3f);
	const float BeforeLiveLook = Controller->GetControlRotation().Yaw;
	Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
	Fixture.Advance(.04f);
	if (!TestTrue(TEXT("The same mouse input works before death"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(BeforeLiveLook, Controller->GetControlRotation().Yaw)) > 1.f)) return false;

	int32 PawnInputCallbacks = 0;
	int32 ControllerInputCallbacks = 0;
	FInputKeyBinding PawnBinding(FInputChord(EKeys::F9), IE_Pressed);
	PawnBinding.KeyDelegate.GetDelegateForManualSet().BindLambda([&] { ++PawnInputCallbacks; });
	Player->InputComponent->KeyBindings.Add(MoveTemp(PawnBinding));
	FInputKeyBinding ControllerBinding(FInputChord(EKeys::F10), IE_Pressed);
	ControllerBinding.KeyDelegate.GetDelegateForManualSet().BindLambda([&] { ++ControllerInputCallbacks; });
	Controller->InputComponent->KeyBindings.Add(MoveTemp(ControllerBinding));
	Controller->InputKey(FInputKeyParams(EKeys::F9, IE_Pressed, 1.));
	Fixture.Advance(.02f);
	Controller->InputKey(FInputKeyParams(EKeys::F9, IE_Released, 0.));
	TestEqual(TEXT("Pawn sentinel is a working input binding before death"), PawnInputCallbacks, 1);

	const auto Kill = [](AmultiplayerCombatCharacter* Character)
	{
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		auto Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.f);
		ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
	};
	Kill(Player);
	Fixture.Advance(.1f);
	const FVector DeathLocation = Player->GetActorLocation();
	const FRotator DeathBodyRotation = Player->GetActorRotation();
	const FRotator DeathControlRotation = Controller->GetControlRotation();
	const FRotator DeathCameraRotation = Camera->GetComponentRotation();
	const float DeathEnergy = Player->GetEnergy();
	int32 FailedSkillCallbacks = 0;
	const FDelegateHandle FailureHandle = Player->GetAbilitySystemComponent()->AbilityFailedCallbacks.AddLambda(
		[&](const UGameplayAbility*, const FGameplayTagContainer&) { ++FailedSkillCallbacks; });
	const FKey GameplayKeys[] = { EKeys::W, EKeys::SpaceBar, EKeys::LeftMouseButton,
		EKeys::Q, EKeys::E, EKeys::R, EKeys::RightMouseButton, EKeys::F9, EKeys::F10 };
	for (const FKey Key : GameplayKeys) Controller->InputKey(FInputKeyParams(Key, IE_Pressed, 1.));
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
		Controller->InputKey(FInputKeyParams(EKeys::MouseY, 60., .02f, 1));
		Fixture.Advance(.02f);
	}
	for (const FKey Key : GameplayKeys) Controller->InputKey(FInputKeyParams(Key, IE_Released, 0.));
	TestFalse(TEXT("Actual damage has committed persistent death"), Player->IsAlive());
	TestTrue(TEXT("Dead pawn remains possessed for its controller UI"), Controller->GetPawn() == Player);
	TestTrue(TEXT("Dead mouse input no longer changes control rotation"), DeathControlRotation.Equals(Controller->GetControlRotation(), .1f));
	TestTrue(TEXT("Dead mouse input no longer changes the camera"), DeathCameraRotation.Equals(Camera->GetComponentRotation(), .1f));
	TestTrue(TEXT("Dead input cannot translate the capsule"), DeathLocation.Equals(Player->GetActorLocation(), .1f));
	TestTrue(TEXT("Dead input cannot rotate the body"), DeathBodyRotation.Equals(Player->GetActorRotation(), .1f));
	TestEqual(TEXT("Dead pawn is absent from input dispatch, not merely blocked by GAS"), PawnInputCallbacks, 1);
	TestEqual(TEXT("Dead skill keys do not even request GAS activation"), FailedSkillCallbacks, 0);
	TestEqual(TEXT("Controller input continues while pawn gameplay input is disabled"), ControllerInputCallbacks, 1);
	TestEqual(TEXT("Dead input does not spend energy"), Player->GetEnergy(), DeathEnergy);
	Player->GetAbilitySystemComponent()->AbilityFailedCallbacks.Remove(FailureHandle);

	// 状态先到、控制器后到：模拟死亡快照已恢复的 Pawn 再被本地接管，并走客户端 Restart。
	Controller->UnPossess();
	auto* LatePawn = Fixture.World->SpawnActor<AmultiplayerCombatCharacter>(Fixture.PlayerClass,
		FVector(0, 1800, 110), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("A late possession pawn is available"), LatePawn)) return false;
	Kill(LatePawn);
	Controller->Possess(LatePawn);
	LatePawn->PawnClientRestart();
	Fixture.Advance(.1f);
	const FRotator LateControl = Controller->GetControlRotation();
	const FTransform LateTransform = LatePawn->GetActorTransform();
	Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Pressed, 1.));
	Fixture.Advance(.2f);
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.));
	TestTrue(TEXT("Possess and Restart do not restore gameplay input to a dead pawn"),
		LateControl.Equals(Controller->GetControlRotation(), .1f) && LateTransform.Equals(LatePawn->GetActorTransform(), .1f));

	// 只验证输入属于 Pawn 生命周期；不绕过 GameMode 对个人复活/刷新资源的限制。
	Controller->UnPossess();
	auto* FreshPawn = Fixture.World->SpawnActor<AmultiplayerCombatCharacter>(Fixture.PlayerClass,
		FVector(0, -1800, 110), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("A fresh living pawn is available for input isolation"), FreshPawn)) return false;
	Controller->Possess(FreshPawn);
	FreshPawn->PawnClientRestart();
	Fixture.Advance(.3f);
	const FRotator FreshControl = Controller->GetControlRotation();
	const FVector FreshLocation = FreshPawn->GetActorLocation();
	Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
	Fixture.Advance(.04f);
	TestTrue(TEXT("The same controller can look again with a new living pawn"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(FreshControl.Yaw, Controller->GetControlRotation().Yaw)) > 1.f);
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Pressed, 1.));
	Fixture.Advance(.4f);
	Controller->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.));
	TestTrue(TEXT("The same controller can move a new living pawn without manually enabling input"),
		FVector::Dist2D(FreshLocation, FreshPawn->GetActorLocation()) > 50.f);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCameraFacingMovingAttackTest, "Multiplayer.Combat.CameraFacingMovingAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 横移和攻击都走保存的键位；逐帧验证出手朝向快照，不用直接改 Transform 掩盖 CMC 抢回朝向的问题。
bool FCoopCombatCameraFacingMovingAttackTest::RunTest(const FString& Parameters)
{
	FCombatMovementWorld Fixture;
	auto* Player = Fixture.AdmitPlayer(FVector(0, 0, 110), true);
	if (!TestNotNull(TEXT("Moving attack possesses the saved combat player"), Player)) return false;
	auto* Controller = Cast<APlayerController>(Player->GetController());
	if (!TestNotNull(TEXT("Moving attack has its real local controller"), Controller)
		|| !TestNotNull(TEXT("Moving attack uses enhanced keyboard input"), Cast<UEnhancedPlayerInput>(Controller->PlayerInput))
		|| !TestNotNull(TEXT("Saved player has a timed attack montage"), Player->GetActionMontage(EMultiplayerCombatAnimation::Attack))) return false;
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	const bool bOriginalOrientToMovement = Movement->bOrientRotationToMovement;
	const bool bOriginalControllerDesiredRotation = Movement->bUseControllerDesiredRotation;
	const float AttackDuration = Player->GetCombatConfig().Attack.Windup + Player->GetCombatConfig().Attack.Recovery;
	if (!TestTrue(TEXT("Saved player normally faces travel rather than controller yaw"),
		bOriginalOrientToMovement && !bOriginalControllerDesiredRotation && !Player->bUseControllerRotationYaw)
		|| !TestTrue(TEXT("Saved attack has an observable startup and recovery"), AttackDuration > .15f)) return false;
	Fixture.Advance(.3f);
	const FVector BeforeSideways = Player->GetActorLocation();
	Controller->InputKey(FInputKeyParams(EKeys::D, IE_Pressed, 1.));
	Fixture.Advance(.4f);
	TestTrue(TEXT("Holding the actual D key produces sideways travel"),
		FVector::Dist2D(BeforeSideways, Player->GetActorLocation()) > 50.f);
	TestTrue(TEXT("Sideways travel genuinely faces away from the camera before attacking"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, Controller->GetControlRotation().Yaw)) > 45.f);

	// 同一套真实输入分别经历正常收招、显式取消和眩晕；D 一直按住，退出后应继续正常移动转身。
	for (int32 Exit = 0; Exit < 3; ++Exit)
	{
		const TCHAR* ExitName = Exit == 0 ? TEXT("completion") : Exit == 1 ? TEXT("cancellation") : TEXT("stun");
		Fixture.Advance(Player->GetAttackCooldownRemaining() + .12f);
		const float AttackYaw = Controller->GetControlRotation().Yaw;
		Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Pressed, 1.));
		Fixture.Advance(.02f);
		Controller->InputKey(FInputKeyParams(EKeys::LeftMouseButton, IE_Released, 0.));
		if (!TestTrue(*FString::Printf(TEXT("%s: real LMB starts the ordinary GAS attack"), ExitName),
			ASC->HasMatchingGameplayTag(CombatTags::State_Attacking))) return false;
		TestFalse(*FString::Printf(TEXT("%s: active attack suspends movement-facing only"), ExitName), Movement->bOrientRotationToMovement);
		TestFalse(*FString::Printf(TEXT("%s: active attack also suspends controller-facing"), ExitName), Movement->bUseControllerDesiredRotation);
		TestFalse(*FString::Printf(TEXT("%s: attacking does not apply a movement restriction"), ExitName), Player->IsMovementBlocked());
		float MaxYawError = FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, AttackYaw));
		float AttackTravel = 0.f;
		int32 ActiveSamples = 0;
		Controller->InputKey(FInputKeyParams(EKeys::MouseX, 120., .02f, 1));
		const float ObserveDuration = Exit == 0 ? AttackDuration + .3f : .06f;
		for (float Elapsed = 0.f; Elapsed < ObserveDuration && ASC->HasMatchingGameplayTag(CombatTags::State_Attacking); Elapsed += .02f)
		{
			const FVector BeforeStep = Player->GetActorLocation();
			Fixture.Advance(.02f);
			// 收招当帧允许 CMC 恢复转身，不把恢复后的合法朝向计入攻击阶段。
			if (!ASC->HasMatchingGameplayTag(CombatTags::State_Attacking)) break;
			++ActiveSamples;
			AttackTravel += FVector::Dist2D(BeforeStep, Player->GetActorLocation());
			MaxYawError = FMath::Max(MaxYawError,
				float(FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, AttackYaw))));
		}
		TestTrue(*FString::Printf(TEXT("%s: mouse still turns the free camera during attack"), ExitName),
			FMath::Abs(FMath::FindDeltaAngleDegrees(AttackYaw, Controller->GetControlRotation().Yaw)) > 1.f);
		TestTrue(*FString::Printf(TEXT("%s: several active frames are observed"), ExitName), ActiveSamples >= 2);
		TestTrue(*FString::Printf(TEXT("%s: held D keeps moving during the attack"), ExitName), AttackTravel > (Exit == 0 ? 20.f : 5.f));
		TestTrue(*FString::Printf(TEXT("%s: neither sideways input nor camera movement overwrites this attack yaw"), ExitName), MaxYawError < .1f);
		if (Exit == 1)
		{
			FGameplayTagContainer AttackTags(CombatTags::Ability_Attack);
			ASC->CancelAbilities(&AttackTags);
		}
		else if (Exit == 2)
		{
			Player->ApplyStun(.2f);
			TestTrue(TEXT("Real stun effect interrupts the moving attack"), Player->IsStunned());
		}
		TestFalse(*FString::Printf(TEXT("%s: attack lifecycle releases its owned facing tag"), ExitName),
			ASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
		TestEqual(*FString::Printf(TEXT("%s: original movement-facing policy is restored immediately"), ExitName),
			bool(Movement->bOrientRotationToMovement), bOriginalOrientToMovement);
		TestEqual(*FString::Printf(TEXT("%s: original controller-facing policy is restored immediately"), ExitName),
			bool(Movement->bUseControllerDesiredRotation), bOriginalControllerDesiredRotation);
		Fixture.Advance(AttackDuration + .25f);
		const FVector BeforeResumedTravel = Player->GetActorLocation();
		Fixture.Advance(.25f);
		TestFalse(*FString::Printf(TEXT("%s: old tasks or stun leave no attack or movement lock"), ExitName),
			ASC->HasMatchingGameplayTag(CombatTags::State_Attacking) || Player->IsStunned() || Player->IsMovementBlocked());
		TestTrue(*FString::Printf(TEXT("%s: held keyboard input resumes real travel after recovery"), ExitName),
			FVector::Dist2D(BeforeResumedTravel, Player->GetActorLocation()) > 50.f);
		TestTrue(*FString::Printf(TEXT("%s: recovered CMC again turns the body toward actual travel"), ExitName),
			FMath::Abs(FMath::FindDeltaAngleDegrees(Player->GetActorRotation().Yaw, Player->GetVelocity().Rotation().Yaw)) < 5.f);
		AddInfo(FString::Printf(TEXT("Moving attack %s: activeSamples=%d travel=%.2fcm maxYawError=%.3fdeg"),
			ExitName, ActiveSamples, AttackTravel, MaxYawError));
	}
	Controller->InputKey(FInputKeyParams(EKeys::D, IE_Released, 0.));
	return !HasAnyErrors();
}

#endif
