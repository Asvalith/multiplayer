// Editor implementation included once inside CombatAssets, after CoopHUDPresentationBuild.inl.
// Explicit update modes edit five project-owned WBP packages; validation modes never save them.
#if !WITH_EDITOR
#error CoopWidgetPresentationBuild.inl requires the editor-only commandlet implementation.
#endif

namespace WidgetPresentation
{
struct FWrite
{
	FName Target; UClass* Owner; FName Function; FName Input; FName Source;
};

void Add(TArray<FWrite>& Out, const TCHAR* Target, UClass* Owner, const TCHAR* Function, const TCHAR* Input, const FString& Source)
{ Out.Add({FName(Target), Owner, FName(Function), FName(Input), FName(*Source)}); }

TArray<FWrite> Writes(UWidgetBlueprint* Widget, UClass* Native)
{
	TArray<FWrite> Out;
	const auto Write = [&](const TCHAR* Target, UClass* Owner, const TCHAR* Function, const TCHAR* Input, const FString& Source)
	{
		// Native bindings are optional across the two authored layouts; never manufacture missing Designer controls.
		if (!*Target || Widget->WidgetTree->FindWidget(FName(Target))) Add(Out, Target, Owner, Function, Input, Source);
	};
	if (Native == UmultiplayerCombatWidget::StaticClass())
	{
		for (const TCHAR* Name : {TEXT("Health"), TEXT("Energy"), TEXT("Cast")})
		{
			Write(*(FString(Name) + TEXT("Bar")), UProgressBar::StaticClass(), TEXT("SetPercent"), TEXT("InPercent"), FString(TEXT("View")) + Name + TEXT("Percent"));
			Write(*(FString(Name) + TEXT("Text")), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), FString(TEXT("View")) + Name + TEXT("Text"));
		}
		for (const TCHAR* Name : {TEXT("State"), TEXT("Progression"), TEXT("Feedback")})
			Write(*(FString(Name) + TEXT("Text")), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), FString(TEXT("View")) + Name + TEXT("Text"));
		for (const TCHAR* Name : {TEXT("CastingPanel"), TEXT("CastBar"), TEXT("CastText")})
			Write(Name, UWidget::StaticClass(), TEXT("SetVisibility"), TEXT("InVisibility"), TEXT("ViewCastVisibility"));
		Write(TEXT("StateText"), UWidget::StaticClass(), TEXT("SetVisibility"), TEXT("InVisibility"), TEXT("ViewStateVisibility"));
		Write(TEXT("FeedbackPanel"), UWidget::StaticClass(), TEXT("SetVisibility"), TEXT("InVisibility"), TEXT("ViewFeedbackVisibility"));
		const TCHAR* Skills[] = {TEXT("Attack"), TEXT("Defense"), TEXT("Heal"), TEXT("Area"), TEXT("Dash")};
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const FString Slot(Skills[Index]), View = TEXT("View") + Slot;
			Write(*(Slot + TEXT("CooldownText")), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), View + TEXT("CooldownText"));
			Write(*(Slot + TEXT("CooldownBar")), UProgressBar::StaticClass(), TEXT("SetPercent"), TEXT("InPercent"), View + TEXT("CooldownPercent"));
			Write(*FString::Printf(TEXT("SkillLabel%d"), Index), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), View + TEXT("Name"));
			Write(*(Slot + TEXT("Icon")), UImage::StaticClass(), TEXT("SetBrush"), TEXT("InBrush"), View + TEXT("Brush"));
			Write(*(Slot + TEXT("Icon")), UImage::StaticClass(), TEXT("SetColorAndOpacity"), TEXT("InColorAndOpacity"), View + TEXT("Tint"));
			Write(*(Slot + TEXT("Slot")), UBorder::StaticClass(), TEXT("SetBrushColor"), TEXT("InBrushColor"), TEXT("SlotBackgroundColor"));
		}
	}
	else if (Native == UmultiplayerCombatProgressionWidget::StaticClass())
	{
		for (const TCHAR* Name : {TEXT("Summary"), TEXT("Skills"), TEXT("SelectedSkill"), TEXT("SelectedSlot"), TEXT("Passives"), TEXT("SelectedPassive"), TEXT("Message")})
			Write(*(FString(TEXT("Progression")) + Name), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), FString(TEXT("View")) + Name);
		for (const TCHAR* Name : {TEXT("LearnSkill"), TEXT("EquipSkill"), TEXT("LearnPassive"), TEXT("EquipPassive"), TEXT("UnequipPassive"), TEXT("SaveProgress"), TEXT("LoadProgress")})
			Write(*(FString(Name) + TEXT("Button")), UWidget::StaticClass(), TEXT("SetIsEnabled"), TEXT("bInIsEnabled"), FString(TEXT("ViewCan")) + Name);
	}
	else
	{
		Write(TEXT("HealthBar"), UProgressBar::StaticClass(), TEXT("SetPercent"), TEXT("InPercent"), TEXT("ViewHealthPercent"));
		Write(TEXT("LerpAttributeBar"), UProgressBar::StaticClass(), TEXT("SetPercent"), TEXT("InPercent"), TEXT("ViewHealthPercent"));
		Write(TEXT("HealthText"), UTextBlock::StaticClass(), TEXT("SetText"), TEXT("InText"), TEXT("ViewHealthText"));
		Write(TEXT(""), UWidget::StaticClass(), TEXT("SetVisibility"), TEXT("InVisibility"), TEXT("ViewVisibility"));
	}
	return Out;
}

const TCHAR* EventName(UClass* Native)
{
	return Native == UmultiplayerCombatWidget::StaticClass() ? TEXT("PresentCombatSnapshot")
		: Native == UmultiplayerCombatProgressionWidget::StaticClass() ? TEXT("PresentProgressionSnapshot") : TEXT("PresentHealthSnapshot");
}

// Buttons issue their native requests from NativeConstruct bindings; these five WBPs only apply snapshots.
// Outside the generated presentation graph, preserve only inert default events and editor comments.
// Use the same scope check for authoring and validation, rather than guessing business behavior from Set* names.
bool HasOnlyPresentationGraphs(UWidgetBlueprint* Widget)
{
	if (!Widget->FunctionGraphs.IsEmpty() || !Widget->DelegateSignatureGraphs.IsEmpty()
		|| !Widget->MacroGraphs.IsEmpty() || !Widget->Bindings.IsEmpty()) return false;
	UEdGraph* Presentation = nullptr;
	for (UEdGraph* Graph : Widget->UbergraphPages) if (Graph && Graph->GetFName() == TEXT("LocalPresentation"))
	{
		if (Presentation) return false;
		Presentation = Graph;
	}
	TArray<UEdGraph*> All;
	Widget->GetAllGraphs(All);
	for (UEdGraph* Graph : All)
	{
		if (!Graph) return false;
		if (Graph == Presentation) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || (!Node->IsA<UK2Node_Event>() && !Node->IsA<UEdGraphNode_Comment>())) return false;
			for (UEdGraphPin* Pin : Node->Pins)
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && !Pin->LinkedTo.IsEmpty()) return false;
		}
	}
	return true;
}

// Only empty default graphs or our previously generated graph are replaced. Unknown authored execution must be reviewed.
bool PrepareGraph(UWidgetBlueprint* Widget)
{
	if (!HasOnlyPresentationGraphs(Widget)) return false;
	for (UEdGraph* Graph : Widget->UbergraphPages)
	{
		const TArray<TObjectPtr<UEdGraphNode>> Previous = Graph->Nodes;
		for (UEdGraphNode* Node : Previous) FBlueprintEditorUtils::RemoveNode(Widget, Node, true);
	}
	return true;
}

UEdGraphPin* Get(UWidgetBlueprint* Widget, UEdGraph* Graph, UClass* Native, FName Name, const FString& NodeName, int32 X, int32 Y, bool bNative)
{
	const FProperty* Property = FindFProperty<FProperty>(bNative ? Native : Widget->GeneratedClass.Get(), Name);
	if (!Property) return nullptr;
	auto* Node = HUDPresentation::Add<UK2Node_VariableGet>(Graph, *NodeName, X, Y);
	Node->SetFromProperty(Property, true, bNative ? Native : Widget->GeneratedClass.Get());
	Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
	return HUDPresentation::Pin(Node, *Name.ToString(), EGPD_Output);
}

bool Generate(UWidgetBlueprint* Widget, UClass* Native)
{
	if (!PrepareGraph(Widget)) return false;
	UEdGraph* Graph = nullptr;
	for (UEdGraph* Existing : Widget->UbergraphPages) if (Existing->GetFName() == TEXT("LocalPresentation")) Graph = Existing;
	if (!Graph)
	{
		Graph = FBlueprintEditorUtils::CreateNewGraph(Widget, TEXT("LocalPresentation"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Widget, Graph);
	}
	auto* Entry = HUDPresentation::Add<UK2Node_Event>(Graph, TEXT("SnapshotEntry"), -400, 0);
	Entry->EventReference.SetExternalMember(EventName(Native), Native); Entry->bOverrideFunction = true; Entry->PostPlacedNewNode(); Entry->AllocateDefaultPins();
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* Previous = HUDPresentation::Pin(Entry, TEXT("then"), EGPD_Output);
	const TArray<FWrite> Specs = Writes(Widget, Native);
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		const FWrite& Spec = Specs[Index]; const int32 X = Index % 4 * 720, Y = Index / 4 * 260;
		auto* Call = HUDPresentation::Add<UK2Node_CallFunction>(Graph, *FString::Printf(TEXT("Present_%d"), Index), X, Y);
		UFunction* Function = Spec.Owner->FindFunctionByName(Spec.Function); if (!Function) return false;
		Call->SetFromFunction(Function); Call->PostPlacedNewNode(); Call->AllocateDefaultPins();
		Call->NodeComment = Spec.Target.IsNone() ? TEXT("Apply root visibility from the local snapshot") : Spec.Target.ToString() + TEXT(" <- ") + Spec.Source.ToString();
		if (!Schema->TryCreateConnection(Previous, HUDPresentation::Exec(Call))) return false;
		Previous = HUDPresentation::Pin(Call, TEXT("then"), EGPD_Output);
		if (!Spec.Target.IsNone())
		{
			auto* Target = Get(Widget, Graph, Native, Spec.Target, FString::Printf(TEXT("Target_%d"), Index), X - 180, Y + 75, false);
			if (!Target || !Schema->TryCreateConnection(Target, HUDPresentation::Pin(Call, TEXT("self"), EGPD_Input))) return false;
		}
		auto* Value = Get(Widget, Graph, Native, Spec.Source, FString::Printf(TEXT("Snapshot_%d"), Index), X - 180, Y + 145, true);
		if (!Value || !Schema->TryCreateConnection(Value, HUDPresentation::Pin(Call, *Spec.Input.ToString(), EGPD_Input))) return false;
	}
	return Specs.Num() > 0;
}

// Designer stores nested UUserWidget templates separately from their class CDO.
// Migrate that real serialized override too; checking only the leaf class default
// misses an old empty instance override carried by the enclosing WidgetTree.
bool HealthTemplates(UWidgetBlueprint* Widget, bool bApply)
{
	if (!Widget || !Widget->WidgetTree) return false;
	if (Widget->GetOutermost()->GetName() == TEXT("/Game/Combat/Presentation/UI/WBP_ARPGEnemyHealth"))
	{
		const auto* Inner = Cast<UmultiplayerEnemyHealthWidget>(Widget->WidgetTree->FindWidget(TEXT("Health")));
		if (!Inner || Inner->GetClass()->GetPathName() != TEXT("/Game/Combat/Presentation/UI/WBP_ARPGEnemyAttributeBar.WBP_ARPGEnemyAttributeBar_C"))
		{ UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_WIDGET_HEALTH_TEMPLATE wrong actual nested Health class in %s"), *Widget->GetPathName()); return false; }
	}
	bool bValid = true;
	Widget->WidgetTree->ForEachWidget([&](UWidget* Control)
	{
		auto* Health = Cast<UmultiplayerEnemyHealthWidget>(Control); if (!Health) return;
		const auto* Defaults = Health->GetClass()->GetDefaultObject<UmultiplayerEnemyHealthWidget>();
		if (!Defaults || Defaults->HealthTextFormat.IsEmpty()) { bValid = false; return; }
		if (bApply) { Widget->WidgetTree->Modify(); Health->Modify(); Health->HealthTextFormat = Defaults->HealthTextFormat; }
		if (!Health->HealthTextFormat.EqualTo(Defaults->HealthTextFormat))
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_WIDGET_HEALTH_TEMPLATE mismatch template=%s class=%s actual=[%s] classDefault=[%s]"),
				*Health->GetPathName(), *Health->GetClass()->GetPathName(), *Health->HealthTextFormat.ToString(), *Defaults->HealthTextFormat.ToString());
			bValid = false;
		}
	});
	return bValid;
}

bool Validate(UWidgetBlueprint* Widget, UClass* Native)
{
	const auto Fail = [Widget](const FString& Why)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_WIDGET_INVALID asset=%s reason=%s"), *Widget->GetPathName(), *Why); return false;
	};
	if (!Widget->WidgetTree || !Widget->GeneratedClass || Widget->ParentClass != Native || !Widget->Bindings.IsEmpty()) return Fail(TEXT("ParentTreeOrLegacyBindings"));
	if (!HasOnlyPresentationGraphs(Widget)) return Fail(TEXT("GraphScopeOutsideSnapshot"));
	if (!HealthTemplates(Widget, false)) return Fail(TEXT("NestedHealthTemplateMustMatchActualClassDefault"));
	if (Native == UmultiplayerCombatWidget::StaticClass())
	{
		const auto* Defaults = Cast<UmultiplayerCombatWidget>(Widget->GeneratedClass->GetDefaultObject());
		if (!Defaults || Defaults->SkillIcons.Num() != 5 || Defaults->SkillNames.Num() != 5 || Defaults->FeedbackLifetime <= 0 || Defaults->CountdownRefreshInterval <= 0) return Fail(TEXT("AuthoredIconsNamesAndUITimingRequired"));
		for (const TCHAR* Name : {TEXT("HealthBar"), TEXT("EnergyBar"), TEXT("CastBar"), TEXT("StateText"), TEXT("AttackIcon"), TEXT("DefenseIcon"), TEXT("HealIcon"), TEXT("AreaIcon"), TEXT("DashIcon")})
			if (!Widget->WidgetTree->FindWidget(FName(Name))) return Fail(FString(TEXT("MissingDesignerControl:")) + Name);
	}
	if (Native == UmultiplayerCombatProgressionWidget::StaticClass() && Writes(Widget, Native).Num() != 14) return Fail(TEXT("ProgressionControlsMissing"));
	if (Native == UmultiplayerEnemyHealthWidget::StaticClass() && !Widget->WidgetTree->FindWidget(TEXT("Health")))
	{
		const auto* Defaults = Cast<UmultiplayerEnemyHealthWidget>(Widget->GeneratedClass->GetDefaultObject());
		if (Writes(Widget, Native).Num() != 4 || !Defaults || Defaults->HealthTextFormat.IsEmpty()) return Fail(TEXT("InnerHealthControlsOrTextTemplateMissing"));
	}
	UEdGraph* Graph = nullptr; int32 Events = 0; TArray<UEdGraph*> All; Widget->GetAllGraphs(All);
	for (UEdGraph* Candidate : All) for (UEdGraphNode* Node : Candidate->Nodes)
	{
		if (const auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == EventName(Native)) { ++Events; Graph = Candidate; }
	}
	if (Events != 1 || !Graph || Graph->GetFName() != TEXT("LocalPresentation")) return Fail(TEXT("UniqueSnapshotEventMissing"));
	auto* Entry = FindObject<UK2Node_Event>(Graph, TEXT("SnapshotEntry"));
	UEdGraphPin* Previous = HUDPresentation::Pin(Entry, TEXT("then"), EGPD_Output);
	const TArray<FWrite> Specs = Writes(Widget, Native); int32 ExpectedNodes = 1;
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		const FWrite& Spec = Specs[Index]; ExpectedNodes += Spec.Target.IsNone() ? 2 : 3;
		auto* Call = FindObject<UK2Node_CallFunction>(Graph, *FString::Printf(TEXT("Present_%d"), Index));
		auto* Value = FindObject<UK2Node_VariableGet>(Graph, *FString::Printf(TEXT("Snapshot_%d"), Index));
		if (!Call || Call->GetTargetFunction() != Spec.Owner->FindFunctionByName(Spec.Function) || !Value || !Value->VariableReference.IsSelfContext()
			|| Value->VariableReference.GetMemberName() != Spec.Source || Value->GetPropertyForVariable() != FindFProperty<FProperty>(Native, Spec.Source)) return Fail(FString::Printf(TEXT("WrongSetterOrSnapshot:%d"), Index));
		if (!HUDPresentation::Linked(HUDPresentation::Exec(Call), Previous)
			|| !HUDPresentation::Linked(HUDPresentation::Pin(Call, *Spec.Input.ToString(), EGPD_Input), HUDPresentation::Pin(Value, *Spec.Source.ToString(), EGPD_Output))) return Fail(FString::Printf(TEXT("MissingExecOrDataEdge:%d"), Index));
		if (!Spec.Target.IsNone())
		{
			auto* Target = FindObject<UK2Node_VariableGet>(Graph, *FString::Printf(TEXT("Target_%d"), Index));
			const auto* ObjectProperty = Target ? CastField<FObjectPropertyBase>(Target->GetPropertyForVariable()) : nullptr;
			if (!Target || !Target->VariableReference.IsSelfContext() || Target->VariableReference.GetMemberName() != Spec.Target || !ObjectProperty
				|| !ObjectProperty->PropertyClass->IsChildOf(Spec.Owner) || !HUDPresentation::Linked(HUDPresentation::Pin(Call, TEXT("self"), EGPD_Input), HUDPresentation::Pin(Target, *Spec.Target.ToString(), EGPD_Output))) return Fail(FString::Printf(TEXT("WrongDesignerTarget:%d"), Index));
		}
		else
		{
			const auto* Target = HUDPresentation::Pin(Call, TEXT("self"), EGPD_Input);
			if (Target && (!Target->LinkedTo.IsEmpty() || Target->DefaultObject)) return Fail(TEXT("RootTargetMustBeSelf"));
		}
		Previous = HUDPresentation::Pin(Call, TEXT("then"), EGPD_Output);
	}
	if (!Previous || !Previous->LinkedTo.IsEmpty() || Graph->Nodes.Num() != ExpectedNodes) return Fail(TEXT("UnexpectedNodesOrExecContinuation"));
	const auto* Function = Widget->GeneratedClass->FindFunctionByName(EventName(Native));
	if (!Function || Function->GetOuterUClass() != Widget->GeneratedClass.Get()) return Fail(TEXT("SnapshotEventMustBeImplementedByThisRealWBP"));
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_WIDGET_GRAPH_PASS asset=%s setters=%d nodes=%d bindings=0 actualCompiledEvent=1"), *Widget->GetPathName(), Specs.Num(), Graph->Nodes.Num());
	return true;
}
}

bool UpdateWidgetPresentation(bool bValidateOnly)
{
	const TCHAR* Paths[] = {TEXT("/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals"), TEXT("/Game/Combat/Escape/WBP_EscapeCombatVisuals"),
		TEXT("/Game/Combat/Presentation/UI/WBP_CombatProgression"), TEXT("/Game/Combat/Presentation/UI/WBP_ARPGEnemyAttributeBar"), TEXT("/Game/Combat/Presentation/UI/WBP_ARPGEnemyHealth")};
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/WidgetPresentation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!bValidateOnly && !IFileManager::Get().MakeDirectory(*Backup, true)) return false;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Paths); ++Index)
	{
		auto* Widget = Load<UWidgetBlueprint>(Paths[Index]); if (!Widget || !Widget->WidgetTree || !Widget->GeneratedClass) return false;
		UClass* Native = Index < 2 ? UmultiplayerCombatWidget::StaticClass() : Index == 2 ? UmultiplayerCombatProgressionWidget::StaticClass() : UmultiplayerEnemyHealthWidget::StaticClass();
		if (Widget->ParentClass != Native) return false;
		if (!bValidateOnly)
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(Paths[Index], FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().Copy(*(Backup / FPaths::GetCleanFilename(Filename)), *Filename) != COPY_OK) return false;
			Widget->Modify();
			Widget->WidgetTree->ForEachWidget([](UWidget* Control) { Control->bIsVariable = true; });
			if (!Compile(Widget)) return false; // expose actual Designer variables before linking getters
			if (!WidgetPresentation::Generate(Widget, Native) || !Compile(Widget)) return false;
			if (auto* Defaults = Cast<UmultiplayerCombatWidget>(Widget->GeneratedClass->GetDefaultObject()))
			{
				Defaults->Modify(); Defaults->SkillIcons.Reset();
				if (Defaults->FeedbackLifetime == 0) Defaults->FeedbackLifetime = 2.f;
				if (Defaults->CountdownRefreshInterval == 0) Defaults->CountdownRefreshInterval = .1f;
				for (const TCHAR* Name : {TEXT("AttackIcon"), TEXT("DefenseIcon"), TEXT("HealIcon"), TEXT("AreaIcon"), TEXT("DashIcon")})
				{
					const auto* Icon = Cast<UImage>(Widget->WidgetTree->FindWidget(FName(Name))); if (!Icon) return false;
					Defaults->SkillIcons.Add(Icon->GetBrush());
				}
			}
			if (auto* Defaults = Cast<UmultiplayerEnemyHealthWidget>(Widget->GeneratedClass->GetDefaultObject()); Defaults && Defaults->HealthTextFormat.IsEmpty())
			{ Defaults->Modify(); Defaults->HealthTextFormat = NSLOCTEXT("CombatWorldHealth", "Vitals", "{0} / {1}"); }
			if (!WidgetPresentation::HealthTemplates(Widget, true) || !WidgetPresentation::Validate(Widget, Native) || !Save(Widget)) return false;
		}
		else if (!WidgetPresentation::Validate(Widget, Native)) return false;
	}
	UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_WIDGET_PRESENTATION mode=%s packages=%d backup=%s nativeControlWrites=0 visualPreview=NOT_RUN"), bValidateOnly ? TEXT("Validate") : TEXT("Update"), UE_ARRAY_COUNT(Paths), bValidateOnly ? TEXT("None") : *Backup);
	return true;
}
