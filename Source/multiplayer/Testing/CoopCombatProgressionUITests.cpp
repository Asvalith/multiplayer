// Copyright Epic Games, Inc. All Rights Reserved.

// 回归真实成长菜单按钮、事件驱动刷新、角色替换与关闭后的解绑。
// 通过正式组件处理学习和装配，不修改玩家存档，也不把文字断言当成视觉验收。

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/OutputDevice.h"
#include "Slate/SObjectWidget.h"
#include "TimerManager.h"

namespace
{
	// 真实 UMG、ASC 和角色夹具；没有视口输入注入，也不覆盖本机存档或原 HUD 资产。
	struct FCombatMenuWorld
	{
		UWorld* World;
		// 创建供 UMG 与真实角色绑定的独立世界，关闭无关 AI 和导航。
		FCombatMenuWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL()); World->SetBegunPlay(true);
		}
		// 先由调用方正常关闭 Slate，再结束角色与世界。
		~FCombatMenuWorld()
		{
			World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
		}
		// 默认角色使用生产初始化，未学习的冲刺不会被夹具偷偷授予。
		AmultiplayerCombatCharacter* Character()
		{
			auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(AmultiplayerCombatCharacter::StaticClass(),
				FTransform(FVector(0, 0, 100)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled; Pawn->FinishSpawning(FTransform(FVector(0, 0, 100)));
			Pawn->GetCharacterMovement()->DisableMovement(); return Pawn;
		}
	};
	// 使用生产的反射生命周期入口绑定，无需给测试开放一份会绕过真实解绑逻辑的接口。
	void BindCombatMenu(UUserWidget* Widget, APawn* Previous, APawn* Current)
	{
		struct FParameters { APawn* PreviousPawn; APawn* NewPawn; } Parameters{Previous, Current};
		Widget->ProcessEvent(Widget->FindFunctionChecked(TEXT("BindPawn")), &Parameters);
	}
	// 每次读取真实 TextBlock，而不是断言另一份菜单模型。
	FString MenuText(UUserWidget* Widget, FName Name)
	{
		const auto* Label = Cast<UTextBlock>(Widget->GetWidgetFromName(Name)); return Label ? Label->GetText().ToString() : FString();
	}
	// 检查公开 TimerManager 实际列表，覆盖 Pending 和活动计时器而不是只查看某个句柄。
	bool HasMenuTimer(UWorld* World, UUserWidget* Widget)
	{
		GLog->FlushThreadedLogs(); FStringOutputDevice Output; GLog->AddOutputDevice(&Output);
		World->GetTimerManager().ListTimers(); GLog->FlushThreadedLogs(); GLog->RemoveOutputDevice(&Output);
		return Output.Contains(Widget->GetPathName(), ESearchCase::CaseSensitive);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatMenuLearningTest, "Multiplayer.Combat.ProgressionUI.RealButtonsLearnAndEquip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 通过真实按钮把冲刺从零学到一级；界面变化必须对应服务器点数和真实 GAS Spec。
bool FCombatMenuLearningTest::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate is ready for native UMG"), FSlateApplication::IsInitialized())) return false;
	FCombatMenuWorld Fixture; auto* Pawn = Fixture.Character(); auto* Progression = Pawn->GetProgressionComponent();
	auto* Widget = CreateWidget<UmultiplayerCombatProgressionWidget>(Fixture.World, LoadClass<UmultiplayerCombatProgressionWidget>(nullptr, TEXT("/Game/Combat/Presentation/UI/WBP_CombatProgression.WBP_CombatProgression_C")));
	if (!TestNotNull(TEXT("Saved progression WBP is constructible with its authored layout"), Widget)) return false;
	TSharedPtr<SObjectWidget> Slate = StaticCastSharedRef<SObjectWidget>(Widget->TakeWidget());
	const UFunction* SnapshotEvent = Widget->FindFunction(TEXT("PresentProgressionSnapshot"));
	if (!TestNotNull(TEXT("Real progression WBP implements its presentation event"), SnapshotEvent)) return false;
	TestEqual(TEXT("Progression control writes execute in the actual WBP"), SnapshotEvent->GetOuterUClass(), Widget->GetClass());
	BindCombatMenu(Widget, nullptr, Pawn);
	const auto Button = [Widget](const TCHAR* Name) { return Cast<UButton>(Widget->GetWidgetFromName(FName(Name))); };
	const TCHAR* Names[] = {TEXT("NextSkillButton"), TEXT("LearnSkillButton"), TEXT("NextSlotButton"), TEXT("EquipSkillButton"),
		TEXT("LearnPassiveButton"), TEXT("EquipPassiveButton"), TEXT("UnequipPassiveButton"), TEXT("SaveProgressButton"), TEXT("LoadProgressButton"), TEXT("CloseProgressionButton")};
	for (const TCHAR* Name : Names) if (!TestNotNull(Name, Button(Name))) return false;
	TestEqual(TEXT("Dash starts genuinely unlearned"), Progression->GetSkillLevel(EMultiplayerCombatSkill::Dash), 0);
	const int32 InitialAbilityCount = Pawn->GetAbilitySystemComponent()->GetActivatableAbilities().Num();
	TestNull(TEXT("Unlearned dash has no GAS spec to bypass the menu"), Pawn->GetAbilitySystemComponent()->FindAbilitySpecFromClass(Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill::Dash)));
	TestTrue(TEXT("The five-skill list explains that dash is unlearned"), MenuText(Widget, TEXT("ProgressionSkills")).Contains(TEXT("未学习")));
	for (int32 Index = 0; Index < 4; ++Index) Button(TEXT("NextSkillButton"))->OnClicked.Broadcast();
	TestTrue(TEXT("Selection reaches the unlearned dash"), MenuText(Widget, TEXT("ProgressionSelectedSkill")).Contains(TEXT("冲刺")));
	TestTrue(TEXT("Level one can unlock dash with a point"), Button(TEXT("LearnSkillButton"))->GetIsEnabled());
	Button(TEXT("LearnSkillButton"))->OnClicked.Broadcast();
	const auto* Dash = Pawn->GetAbilitySystemComponent()->FindAbilitySpecFromClass(Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill::Dash));
	if (!TestNotNull(TEXT("Learning really grants dash"), Dash)) return false;
	TestEqual(TEXT("Unlocked dash uses GAS level one"), Dash->Level, 1);
	TestEqual(TEXT("The unlock consumes exactly one skill point"), Progression->GetState().SkillPoints, 1);
	TestTrue(TEXT("Point readout refreshes from the progression event immediately"), MenuText(Widget, TEXT("ProgressionSummary")).Contains(TEXT("技能点 1")));
	TestFalse(TEXT("Character level still gates the next skill level"), Button(TEXT("LearnSkillButton"))->GetIsEnabled());
	Button(TEXT("LearnSkillButton"))->OnClicked.Broadcast();
	TestEqual(TEXT("Repeated click cannot grant free levels or spend rejected points"), Progression->GetState().SkillPoints, 1);
	TestEqual(TEXT("Rejected learning does not duplicate the spec or auxiliary rescue ability"), Pawn->GetAbilitySystemComponent()->GetActivatableAbilities().Num(), InitialAbilityCount + 1);
	Button(TEXT("NextSlotButton"))->OnClicked.Broadcast(); Button(TEXT("EquipSkillButton"))->OnClicked.Broadcast();
	TestEqual(TEXT("Equip button changes the actual Q slot to dash"), Progression->GetSlotSkill(1), EMultiplayerCombatSkill::Dash);
	TestEqual(TEXT("Equipping swaps the prior Q skill into the original dash slot"), Progression->GetSlotSkill(4), EMultiplayerCombatSkill::Defend);
	Button(TEXT("LearnPassiveButton"))->OnClicked.Broadcast(); Button(TEXT("EquipPassiveButton"))->OnClicked.Broadcast();
	TestEqual(TEXT("Passive buttons apply a real max-health modifier"), Pawn->GetMaxHealth(), 120.f);
	TestTrue(TEXT("Current passive is reported by the actual menu text"), MenuText(Widget, TEXT("ProgressionPassives")).Contains(TEXT("当前装配：坚韧")));
	Button(TEXT("UnequipPassiveButton"))->OnClicked.Broadcast();
	TestEqual(TEXT("Unequip button removes only its passive bonus"), Pawn->GetMaxHealth(), 100.f);
	TestFalse(TEXT("Idle menu never creates a polling timer"), HasMenuTimer(Fixture.World, Widget));
	Slate->ResetWidget();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatMenuLifecycleTest, "Multiplayer.Combat.ProgressionUI.PawnReplacementAndCloseLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 打开补读、角色替换解绑和关闭清理都经过真实 UMG 生命周期，不把文字测试当成实机画面验收。
bool FCombatMenuLifecycleTest::RunTest(const FString&)
{
	if (!FSlateApplication::IsInitialized()) return false;
	FCombatMenuWorld Fixture; auto* Old = Fixture.Character(); auto* Current = Fixture.Character();
	Old->GetProgressionComponent()->AwardExperience(100, FGuid::NewGuid());
	auto* Widget = CreateWidget<UmultiplayerCombatProgressionWidget>(Fixture.World, LoadClass<UmultiplayerCombatProgressionWidget>(nullptr, TEXT("/Game/Combat/Presentation/UI/WBP_CombatProgression.WBP_CombatProgression_C")));
	if (!TestNotNull(TEXT("Real saved progression WBP loads for lifecycle checks"), Widget)) return false;
	TSharedPtr<SObjectWidget> Slate = StaticCastSharedRef<SObjectWidget>(Widget->TakeWidget());
	BindCombatMenu(Widget, nullptr, Old);
	TestTrue(TEXT("Opening reads existing level without waiting for another reward"), MenuText(Widget, TEXT("ProgressionSummary")).Contains(TEXT("Lv.2")));
	BindCombatMenu(Widget, Old, Current);
	const FString CurrentSummary = MenuText(Widget, TEXT("ProgressionSummary"));
	TestTrue(TEXT("Replacement immediately reads its own level"), CurrentSummary.Contains(TEXT("Lv.1")));
	TestFalse(TEXT("Old progression no longer holds the menu delegate"), Old->GetProgressionComponent()->OnProgressionChanged.GetAllObjects().Contains(Widget));
	TestTrue(TEXT("New progression owns exactly the live subscription"), Current->GetProgressionComponent()->OnProgressionChanged.GetAllObjects().Contains(Widget));
	Old->GetProgressionComponent()->AwardExperience(200, FGuid::NewGuid());
	TestEqual(TEXT("Old pawn rewards cannot overwrite replacement UI"), MenuText(Widget, TEXT("ProgressionSummary")), CurrentSummary);
	Current->GetProgressionComponent()->AwardExperience(100, FGuid::NewGuid());
	TestTrue(TEXT("Current pawn rewards still update the menu through events"), MenuText(Widget, TEXT("ProgressionSummary")).Contains(TEXT("Lv.2")));
	Slate->ResetWidget();
	TestNull(TEXT("Normal Slate cleanup invokes menu destruction"), Slate->GetWidgetObject());
	TestFalse(TEXT("Closing unbinds progression events"), Current->GetProgressionComponent()->OnProgressionChanged.GetAllObjects().Contains(Widget));
	TestFalse(TEXT("Closing unbinds combat-state events"), Current->OnCombatStateChanged.GetAllObjects().Contains(Widget));
	const FString Detached = MenuText(Widget, TEXT("ProgressionSummary"));
	Current->GetProgressionComponent()->AwardExperience(200, FGuid::NewGuid());
	TestEqual(TEXT("Closed menu does not keep refreshing in the background"), MenuText(Widget, TEXT("ProgressionSummary")), Detached);
	TestFalse(TEXT("Closed menu leaves no widget timer"), HasMenuTimer(Fixture.World, Widget));
	return !HasAnyErrors();
}
#endif
