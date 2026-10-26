// 离线工具必须先拒绝冲突模式，再加载地图或保存资产；用真实入口回归这个边界。
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Testing/CombatAssetBuildCommandlet.h"
#include "Testing/CoopEscapeBuildCommandlet.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"

// The commandlet implementation is in this module; tests exercise its real validator without a second graph policy.
namespace CombatAssets::WidgetPresentation
{
bool Validate(UWidgetBlueprint* Widget, UClass* Native);
}

namespace
{
bool RejectModesWithoutEditing(FAutomationTestBase& Test, UCommandlet& Commandlet, const TArray<FString>& Modes)
{
	UWorld* OriginalWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TMap<TWeakObjectPtr<UPackage>, bool> OriginalDirtyStates;
	for (TObjectIterator<UPackage> Package; Package; ++Package)
		OriginalDirtyStates.Add(*Package, Package->IsDirty());
	for (const FString& Mode : Modes)
	{
		Test.TestEqual(*FString::Printf(TEXT("Conflicting or missing mode is rejected: %s"), *Mode), Commandlet.Main(Mode), 1);
		Test.TestTrue(TEXT("Rejected authoring does not replace the current editor world"),
			(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr) == OriginalWorld);
	}
	for (const auto& Entry : OriginalDirtyStates)
		if (const UPackage* Package = Entry.Key.Get())
			Test.TestEqual(*FString::Printf(TEXT("Rejected authoring preserves package dirty state: %s"), *Package->GetName()),
				Package->IsDirty(), Entry.Value);
	return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEscapeAuthoringModeBoundaryTest,
	"Multiplayer.Authoring.EscapeModeBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopEscapeAuthoringModeBoundaryTest::RunTest(const FString&)
{
	const TArray<FString> Modes = {
		TEXT("-Audit -RepairLocalInput"), TEXT("-AuditSaved -RepairLocalInput"),
		TEXT("-Audit -RestoreOriginalLayout"), TEXT("-AuditSaved -RestoreOriginalLayout"),
		TEXT("-Audit -Create"), TEXT("-AuditSaved -Create -CourtyardPrototype"),
		TEXT("-Audit -AuditSaved"), TEXT("-CourtyardPrototype"),
		TEXT("-RestoreOriginalLayout -CourtyardPrototype"), TEXT("")
	};
	AddExpectedError(TEXT("Choose one mode:"), EAutomationExpectedErrorFlags::Contains, Modes.Num());
	TStrongObjectPtr<UCoopEscapeBuildCommandlet> Commandlet(NewObject<UCoopEscapeBuildCommandlet>());
	return RejectModesWithoutEditing(*this, *Commandlet.Get(), Modes);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatAuthoringModeBoundaryTest,
	"Multiplayer.Authoring.CombatModeBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCombatAuthoringModeBoundaryTest::RunTest(const FString&)
{
	const TArray<FString> Modes = {
		TEXT("-Validate -Create"), TEXT("-ValidateResourcePresentation -UpdateResourcePresentation"),
		TEXT("-ValidateWidgetPresentation -UpdateWidgetPresentation"), TEXT("-ValidateHUDPresentation -UpdateHUDPresentation"),
		TEXT("-ValidateProgressionUI -UpdateProgressionUI"), TEXT("-ValidateNoFootIK -RemoveFootIK"),
		TEXT("-ValidateCombatAI -UpdateCombatAI"), TEXT("-AuditARPGEnemySource -ConnectARPGEnemySource"),
		TEXT("-ValidateHUDPresentation -RepairSkillPresentation"), TEXT("")
	};
	AddExpectedError(TEXT("Choose exactly one combat asset mode"), EAutomationExpectedErrorFlags::Contains, Modes.Num());
	TStrongObjectPtr<UCombatAssetBuildCommandlet> Commandlet(NewObject<UCombatAssetBuildCommandlet>());
	return RejectModesWithoutEditing(*this, *Commandlet.Get(), Modes);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatWidgetGraphScopeBoundaryTest,
	"Multiplayer.Authoring.WidgetGraphScopeBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCombatWidgetGraphScopeBoundaryTest::RunTest(const FString&)
{
	TStrongObjectPtr<UWidgetBlueprint> Widget(LoadObject<UWidgetBlueprint>(nullptr,
		TEXT("/Game/Combat/Presentation/UI/WBP_CombatProgression.WBP_CombatProgression")));
	if (!TestTrue(TEXT("Saved progression widget and its real class load"), Widget.IsValid() && Widget->GeneratedClass)) return false;
	UClass* Native = UmultiplayerCombatProgressionWidget::StaticClass();
	const auto Validate = [&] { return CombatAssets::WidgetPresentation::Validate(Widget.Get(), Native); };
	if (!TestTrue(TEXT("Saved production widget passes before any in-memory mutation"), Validate())) return false;
	UPackage* Package = Widget->GetOutermost();
	const bool bOriginallyDirty = Package->IsDirty();
	TStrongObjectPtr<UEdGraph> Extra(NewObject<UEdGraph>(Widget.Get(), NAME_None, RF_Transient));
	Extra->Schema = UEdGraphSchema_K2::StaticClass();
	const auto OriginalBindings = Widget->Bindings;
	ON_SCOPE_EXIT
	{
		Widget->UbergraphPages.Remove(Extra.Get());
		Widget->FunctionGraphs.Remove(Extra.Get());
		Widget->DelegateSignatureGraphs.Remove(Extra.Get());
		Widget->MacroGraphs.Remove(Extra.Get());
		Widget->Bindings = OriginalBindings;
	};
	// Directly attach transient graph objects without Modify, compilation or saving; remove each mutation immediately.
	Widget->UbergraphPages.Add(Extra.Get());
	auto* DefaultEvent = NewObject<UK2Node_Event>(Extra.Get(), NAME_None, RF_Transient);
	DefaultEvent->EventReference.SetExternalMember(TEXT("Construct"), UUserWidget::StaticClass());
	DefaultEvent->bOverrideFunction = true;
	DefaultEvent->AllocateDefaultPins();
	Extra->Nodes.Add(DefaultEvent);
	TestTrue(TEXT("An unconnected default event remains valid"), Validate());
	Extra->Nodes.Reset();
	AddExpectedError(TEXT("COMBAT_WIDGET_INVALID"), EAutomationExpectedErrorFlags::Contains, 7);
	auto* Write = NewObject<UK2Node_VariableSet>(Extra.Get(), NAME_None, RF_Transient);
	Extra->Nodes.Add(Write);
	TestFalse(TEXT("An additional variable write is rejected even without a Set-prefixed function"), Validate());
	Extra->Nodes.Reset();
	auto* BusinessCall = NewObject<UK2Node_CallFunction>(Extra.Get(), NAME_None, RF_Transient);
	BusinessCall->SetFromFunction(UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("ApplyDamage")));
	Extra->Nodes.Add(BusinessCall);
	TestFalse(TEXT("A non-Set gameplay call outside the snapshot graph is rejected"), Validate());
	BusinessCall->AllocateDefaultPins();
	UEdGraphPin* Then = DefaultEvent->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* Execute = BusinessCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!TestTrue(TEXT("Mutation has the actual event and gameplay execution pins"), Then && Execute)) return false;
	Extra->Nodes.Add(DefaultEvent);
	Then->MakeLinkTo(Execute);
	TestFalse(TEXT("An additional event execution path cannot bypass snapshot-only validation"), Validate());
	Then->BreakLinkTo(Execute);
	Extra->Nodes.Reset();
	Widget->UbergraphPages.Remove(Extra.Get());
	Widget->FunctionGraphs.Add(Extra.Get());
	TestFalse(TEXT("An additional function graph is rejected"), Validate());
	Widget->FunctionGraphs.Remove(Extra.Get());
	Widget->MacroGraphs.Add(Extra.Get());
	TestFalse(TEXT("An additional macro graph is rejected"), Validate());
	Widget->MacroGraphs.Remove(Extra.Get());
	Widget->DelegateSignatureGraphs.Add(Extra.Get());
	TestFalse(TEXT("An additional delegate graph is rejected"), Validate());
	Widget->DelegateSignatureGraphs.Remove(Extra.Get());
	Widget->Bindings.AddDefaulted();
	TestFalse(TEXT("A legacy Designer binding is rejected"), Validate());
	Widget->Bindings = OriginalBindings;
	TestTrue(TEXT("The original real widget validates again after every mutation is removed"), Validate());
	TestEqual(TEXT("Graph boundary regression never dirties the saved widget package"), Package->IsDirty(), bOriginallyDirty);
	return !HasAnyErrors();
}

#endif
