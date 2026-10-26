// 真实 GameMode -> HUD -> 共享蓝图绘制执行链；CPU Canvas 批次不等于最终屏幕像素验收。
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"

namespace
{
// UE5.5 does not export the Canvas batch helpers; inspect only its public queued batch data.
bool HasCPUCanvasBatches(const FCanvas& Canvas)
{
	for (const auto& Element : Canvas.SortedElements)
		for (const auto* Batch : Element.RenderBatchArray) if (Batch) return true;
	return false;
}

struct FCombatHUDDrawWorld
{
	UWorld* World;
	APlayerController* Controller = nullptr;
	ULocalPlayer* InputPlayer = nullptr;
	UGameViewportClient* ViewportClient = nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<SOverlay> ViewportOverlay;
	TSharedPtr<FSceneViewport> SceneViewport;
	FCombatHUDDrawWorld()
	{
		UWorld::InitializationValues Values; Values.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
		World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(World);
		// Production HUD BeginPlay needs a real local owner and viewport before it creates its UMG.
		UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
		World->SetGameInstance(Instance); Context.OwningGameInstance = Instance;
		World->InitializeActorsForPlay(FURL()); World->SetBegunPlay(true);
		Instance->OnWorldChanged(nullptr, World);
		ViewportClient = NewObject<UGameViewportClient>(GEngine);
		Context.GameViewport = ViewportClient; ViewportClient->Init(Context, Instance, false);
		ViewportWidget = SNew(SViewport); ViewportOverlay = SNew(SOverlay);
		SceneViewport = MakeShareable(ViewportClient->CreateGameViewport(ViewportWidget));
		ViewportWidget->SetViewportInterface(SceneViewport.ToSharedRef());
		ViewportClient->SetViewportFrame(SceneViewport.Get());
		ViewportClient->SetViewportOverlayWidget(nullptr, ViewportOverlay.ToSharedRef());
		InputPlayer = NewObject<ULocalPlayer>(GEngine);
		Instance->AddLocalPlayer(InputPlayer, FPlatformUserId::CreateFromInternalId(0));
		Controller = World->SpawnActor<APlayerController>();
		if (Controller) Controller->SetPlayer(InputPlayer);
	}
	~FCombatHUDDrawWorld()
	{
		World->EndPlay(EEndPlayReason::Quit);
		InputPlayer->PlayerRemoved();
		ViewportClient->RemoveAllViewportWidgets(); ViewportClient->SetViewportFrame(nullptr);
		SceneViewport.Reset(); ViewportWidget.Reset(); ViewportOverlay.Reset();
		GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHUDRealBlueprintDrawingTest, "Multiplayer.Combat.HUDPresentation.ActualBlueprintDrawsSnapshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopCombatHUDRealBlueprintDrawingTest::RunTest(const FString&)
{
	FCombatHUDDrawWorld Fixture;
	if (!TestNotNull(TEXT("HUD fixture has a real local player controller"), Fixture.Controller)
		|| !TestTrue(TEXT("HUD owner has a local player before actor BeginPlay"), Fixture.Controller->IsLocalPlayerController())) return false;
	for (const TCHAR* Path : { TEXT("/Game/Combat/BP_CombatGameMode.BP_CombatGameMode_C"), TEXT("/Game/Combat/Escape/BP_EscapeGameMode.BP_EscapeGameMode_C") })
	{
		UClass* ModeClass = LoadClass<AGameModeBase>(nullptr, Path); if (!TestNotNull(TEXT("Actual map GameMode loads"), ModeClass)) return false;
		const auto* Mode = ModeClass->GetDefaultObject<AGameModeBase>();
		FActorSpawnParameters Spawn; Spawn.Owner = Fixture.Controller;
		auto* HUD = Fixture.World->SpawnActor<AmultiplayerCombatHUD>(Mode->HUDClass, Spawn);
		if (!TestNotNull(TEXT("GameMode really creates its configured production HUD"), HUD)) return false;
		TestEqual(TEXT("Production HUD BeginPlay receives its owner before widget and input setup"), HUD->GetOwningPlayerController(), Fixture.Controller);
		UFunction* Draw = HUD->FindFunction(TEXT("ReceiveDrawHUD")); if (!TestNotNull(TEXT("Real HUD has a compiled draw event"), Draw)) return false;
		TestEqual(TEXT("Both actual HUD classes inherit the one shared compiled draw graph"), Draw->GetOuterUClass()->GetPathName(),
			FString(TEXT("/Game/Combat/Presentation/UI/BP_CombatHUDPresentation.BP_CombatHUDPresentation_C")));
		UCanvas* Canvas = NewObject<UCanvas>(HUD);
		struct FDrawParameters { int32 SizeX = 960; int32 SizeY = 540; } Parameters;
		{
			FCanvas LineBatches(nullptr, nullptr, Fixture.World, ERHIFeatureLevel::SM5); LineBatches.SetAllowedModes(0);
			Canvas->Init(960, 540, nullptr, &LineBatches); HUD->SetCanvas(Canvas, nullptr);
			auto& Line = HUD->HUDLines.AddDefaulted_GetRef(); Line.StartScreenX = 10; Line.StartScreenY = 10; Line.EndScreenX = 100; Line.EndScreenY = 40;
			Line.LineColor = HUD->Presentation.ReadyColor; Line.LineThickness = HUD->Presentation.LineThickness;
			TestFalse(TEXT("Empty CPU Canvas has no queued drawing"), HasCPUCanvasBatches(LineBatches));
			HUD->ProcessEvent(Draw, &Parameters);
			TestTrue(TEXT("Real blueprint line loop submits snapshot geometry to Canvas"), HasCPUCanvasBatches(LineBatches));
			HUD->SetCanvas(nullptr, nullptr);
		}
		HUD->HUDLines.Reset();
		{
			FCanvas TextBatches(nullptr, nullptr, Fixture.World, ERHIFeatureLevel::SM5); TextBatches.SetAllowedModes(0);
			Canvas->Init(960, 540, nullptr, &TextBatches); HUD->SetCanvas(Canvas, nullptr);
			auto& Text = HUD->HUDTexts.AddDefaulted_GetRef(); Text.Text = TEXT("HUD snapshot smoke"); Text.ScreenX = 10; Text.ScreenY = 60;
			Text.Font = HUD->Presentation.Font; Text.Scale = HUD->Presentation.MarkerTextScale; Text.TextColor = HUD->Presentation.HintColor;
			HUD->ProcessEvent(Draw, &Parameters);
			TestTrue(TEXT("Empty line array still completes into the real blueprint text loop"), HasCPUCanvasBatches(TextBatches));
			HUD->SetCanvas(nullptr, nullptr);
		}
		{
			FCanvas ResetBatches(nullptr, nullptr, Fixture.World, ERHIFeatureLevel::SM5); ResetBatches.SetAllowedModes(0);
			Canvas->Init(960, 540, nullptr, &ResetBatches); HUD->SetCanvas(Canvas, nullptr);
			static_cast<AHUD*>(HUD)->DrawHUD();
			TestTrue(TEXT("Native frame boundary clears old snapshots when no local pawn exists"), HUD->HUDLines.IsEmpty() && HUD->HUDTexts.IsEmpty());
			TestFalse(TEXT("Old frame commands are not redrawn after snapshot reset"), HasCPUCanvasBatches(ResetBatches));
			HUD->SetCanvas(nullptr, nullptr);
		}
		HUD->Destroy();
	}
	return !HasAnyErrors();
}
#endif
