// Editor implementation included once inside CombatAssets by CombatAssetBuildCommandlet.cpp.
// Only the shared presentation Blueprint owns the DrawHUD graph; this file never runs at module startup.
#if !WITH_EDITOR
#error CoopHUDPresentationBuild.inl requires the editor-only commandlet implementation.
#endif

namespace HUDPresentation
{
UEdGraphPin* Pin(UEdGraphNode* Node, const TCHAR* Name, EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;
	for (UEdGraphPin* Value : Node->Pins)
		if (Value && Value->Direction == Direction && Value->PinName.ToString().Replace(TEXT(" "), TEXT("")) == Name) return Value;
	return nullptr;
}

UEdGraphPin* Exec(UEdGraphNode* Node)
{
	UEdGraphPin* Result = nullptr;
	if (Node) for (UEdGraphPin* Value : Node->Pins) if (Value->Direction == EGPD_Input && Value->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{ if (Result) return nullptr; Result = Value; }
	return Result;
}

bool Linked(const UEdGraphPin* A, const UEdGraphPin* B)
{ return A && B && A->LinkedTo.Num() == 1 && A->LinkedTo.Contains(B); }

template<class T> T* Add(UEdGraph* Graph, const TCHAR* Name, int32 X, int32 Y)
{
	auto* Node = NewObject<T>(Graph, FName(Name), RF_Transactional); Graph->AddNode(Node, false, false);
	Node->CreateNewGuid(); Node->NodePosX = X; Node->NodePosY = Y; return Node;
}

bool GenerateGraph(UBlueprint* Blueprint)
{
	auto* Macros = Load<UBlueprint>(TEXT("/Engine/EditorBlueprintResources/StandardMacros")); if (!Macros) return false;
	UEdGraph* ForEach = nullptr; for (UEdGraph* Graph : Macros->MacroGraphs) if (Graph->GetFName() == TEXT("ForEachLoop")) ForEach = Graph;
	if (!ForEach) return false;
	UEdGraph* Graph = nullptr; for (UEdGraph* Candidate : Blueprint->UbergraphPages) if (Candidate->GetFName() == TEXT("CombatHUDFrame")) Graph = Candidate;
	if (!Graph)
	{
		Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("CombatHUDFrame"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	}
	TArray<UEdGraphNode*> Previous; for (UEdGraphNode* Node : Graph->Nodes) Previous.Add(Node);
	for (UEdGraphNode* Node : Previous) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	auto* Event = Add<UK2Node_Event>(Graph, TEXT("CombatFrame_DrawHUD"), 0, 0);
	Event->EventReference.SetExternalMember(TEXT("ReceiveDrawHUD"), AHUD::StaticClass()); Event->bOverrideFunction = true; Event->PostPlacedNewNode(); Event->AllocateDefaultPins();
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	const auto Link = [Schema](UEdGraphPin* A, UEdGraphPin* B) { return A && B && Schema->TryCreateConnection(A, B); };
	UK2Node_MacroInstance* Loops[2] = {};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const bool bLine = Index == 0; const FString Prefix = bLine ? TEXT("CombatFrame_Lines") : TEXT("CombatFrame_Texts");
		const TCHAR* Field = bLine ? TEXT("HUDLines") : TEXT("HUDTexts");
		UScriptStruct* Type = bLine ? FmultiplayerCombatHUDLine::StaticStruct() : FmultiplayerCombatHUDText::StaticStruct();
		auto* Getter = Add<UK2Node_VariableGet>(Graph, *(Prefix + TEXT("_Snapshot")), 80, Index * 450 + 160);
		const FArrayProperty* SnapshotProperty = FindFProperty<FArrayProperty>(AmultiplayerCombatHUD::StaticClass(), Field);
		if (!SnapshotProperty) return false;
		// Native inherited fields are read on this HUD, not an unconnected external object target.
		Getter->SetFromProperty(SnapshotProperty, true, AmultiplayerCombatHUD::StaticClass()); Getter->PostPlacedNewNode(); Getter->AllocateDefaultPins();
		auto* Loop = Add<UK2Node_MacroInstance>(Graph, *(Prefix + TEXT("_Loop")), 380, Index * 450); Loops[Index] = Loop;
		Loop->SetMacroGraph(ForEach); Loop->PostPlacedNewNode(); Loop->AllocateDefaultPins();
		// Array connection may reconstruct wildcard macro pins: reacquire every later pin.
		if (!Link(Pin(Getter, Field, EGPD_Output), Pin(Loop, TEXT("Array"), EGPD_Input))) return false;
		auto* Break = Add<UK2Node_BreakStruct>(Graph, *(Prefix + TEXT("_Item")), 720, Index * 450 + 180);
		Break->StructType = Type; Break->PostPlacedNewNode(); Break->AllocateDefaultPins();
		if (!Link(Pin(Loop, TEXT("ArrayElement"), EGPD_Output), Break->FindPin(Type->GetFName(), EGPD_Input))) return false;
		auto* Call = Add<UK2Node_CallFunction>(Graph, *(Prefix + TEXT("_Draw")), 1050, Index * 450);
		UFunction* Function = AHUD::StaticClass()->FindFunctionByName(bLine ? TEXT("DrawLine") : TEXT("DrawText")); if (!Function) return false;
		Call->SetFromFunction(Function); Call->PostPlacedNewNode(); Call->AllocateDefaultPins();
		if (!Link(Pin(Loop, TEXT("LoopBody"), EGPD_Output), Exec(Call))) return false;
		for (TFieldIterator<FProperty> Property(Type); Property; ++Property)
			if (!Link(Pin(Break, *Property->GetName(), EGPD_Output), Pin(Call, *Property->GetName(), EGPD_Input))) return false;
	}
	return Link(Pin(Event, TEXT("then"), EGPD_Output), Exec(Loops[0])) && Link(Pin(Loops[0], TEXT("Completed"), EGPD_Output), Exec(Loops[1]));
}

bool ValidateGraph(UBlueprint* Blueprint)
{
	const auto Fail = [Blueprint](const TCHAR* Reason)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_GRAPH_INVALID asset=%s reason=%s"), *GetPathNameSafe(Blueprint), Reason);
		return false;
	};
	UEdGraph* Graph = nullptr; TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs); int32 Events = 0;
	for (UEdGraph* Candidate : Graphs)
		for (UEdGraphNode* Node : Candidate->Nodes)
		{
			if (const auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == TEXT("ReceiveDrawHUD")) { ++Events; Graph = Candidate; }
			if (const auto* Call = Cast<UK2Node_CallFunction>(Node); Call && Call->FunctionReference.GetMemberName().ToString().StartsWith(TEXT("Draw"))
				&& Candidate->GetFName() != TEXT("CombatHUDFrame")) return Fail(TEXT("DrawCallOutsideSharedGraph"));
		}
	if (Events != 1 || !Graph || Graph->GetFName() != TEXT("CombatHUDFrame") || Graph->Nodes.Num() != 9)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_GRAPH_LAYOUT events=%d graph=%s nodes=%d"), Events, *GetNameSafe(Graph), Graph ? Graph->Nodes.Num() : -1);
		return Fail(TEXT("ExpectedOneDrawEventAndNineNodes"));
	}
	auto* Event = FindObject<UK2Node_Event>(Graph, TEXT("CombatFrame_DrawHUD")); UK2Node_MacroInstance* Loops[2] = {};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const bool bLine = Index == 0; const FString Prefix = bLine ? TEXT("CombatFrame_Lines") : TEXT("CombatFrame_Texts");
		const TCHAR* Field = bLine ? TEXT("HUDLines") : TEXT("HUDTexts");
		UScriptStruct* Type = bLine ? FmultiplayerCombatHUDLine::StaticStruct() : FmultiplayerCombatHUDText::StaticStruct();
		auto* Getter = FindObject<UK2Node_VariableGet>(Graph, *(Prefix + TEXT("_Snapshot")));
		auto* Loop = FindObject<UK2Node_MacroInstance>(Graph, *(Prefix + TEXT("_Loop"))); Loops[Index] = Loop;
		auto* Break = FindObject<UK2Node_BreakStruct>(Graph, *(Prefix + TEXT("_Item")));
		auto* Call = FindObject<UK2Node_CallFunction>(Graph, *(Prefix + TEXT("_Draw")));
		const UEdGraph* Macro = Loop ? Loop->GetMacroGraph() : nullptr;
		if (!Getter || !Loop || !Break || !Call)
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_GRAPH_NODES prefix=%s getter=%d loop=%d break=%d call=%d"), *Prefix, Getter != nullptr, Loop != nullptr, Break != nullptr, Call != nullptr);
			return Fail(TEXT("MissingAuthoredNode"));
		}
		if (!Getter->VariableReference.IsSelfContext() || Getter->VariableReference.GetMemberName() != Field
			|| Getter->GetPropertyForVariable() != FindFProperty<FArrayProperty>(AmultiplayerCombatHUD::StaticClass(), Field))
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_GRAPH_GETTER field=%s self=%d member=%s nativeProperty=%d"), Field, Getter->VariableReference.IsSelfContext(), *Getter->VariableReference.GetMemberName().ToString(), Getter->GetPropertyForVariable() == FindFProperty<FArrayProperty>(AmultiplayerCombatHUD::StaticClass(), Field));
			return Fail(TEXT("SnapshotMustReadNativeSelfArray"));
		}
		if (!Macro || Macro->GetFName() != TEXT("ForEachLoop") || Macro->GetOutermost()->GetName() != TEXT("/Engine/EditorBlueprintResources/StandardMacros")) return Fail(TEXT("ExpectedEngineForEachMacro"));
		if (Break->StructType != Type) return Fail(TEXT("WrongSnapshotStruct"));
		if (Call->GetTargetFunction() != AHUD::StaticClass()->FindFunctionByName(bLine ? TEXT("DrawLine") : TEXT("DrawText"))) return Fail(TEXT("WrongNativeDrawFunction"));
		if (!Linked(Pin(Loop, TEXT("Array"), EGPD_Input), Pin(Getter, Field, EGPD_Output))
			|| !Linked(Break->FindPin(Type->GetFName(), EGPD_Input), Pin(Loop, TEXT("ArrayElement"), EGPD_Output))
			|| !Linked(Pin(Loop, TEXT("LoopBody"), EGPD_Output), Exec(Call))) return Fail(TEXT("SnapshotLoopOrDrawExecEdge"));
		for (TFieldIterator<FProperty> Property(Type); Property; ++Property)
			if (!Linked(Pin(Call, *Property->GetName(), EGPD_Input), Pin(Break, *Property->GetName(), EGPD_Output)))
			{
				UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_GRAPH_DATA prefix=%s field=%s"), *Prefix, *Property->GetName());
				return Fail(TEXT("SnapshotFieldNotConnectedToDrawInput"));
			}
		const auto* Target = Pin(Call, TEXT("self"), EGPD_Input); if (Target && (!Target->LinkedTo.IsEmpty() || Target->DefaultObject)) return Fail(TEXT("DrawTargetMustBeSelf"));
		const auto* Then = Pin(Call, TEXT("then"), EGPD_Output); if (Then && !Then->LinkedTo.IsEmpty()) return Fail(TEXT("UnexpectedDrawContinuation"));
		if (!bLine)
		{
			const auto* ScalePosition = Pin(Call, TEXT("bScalePosition"), EGPD_Input);
			if (!ScalePosition || !ScalePosition->LinkedTo.IsEmpty() || ScalePosition->DefaultValue.ToBool()) return Fail(TEXT("DrawTextPositionMustNotScale"));
		}
	}
	if (!Event || !Linked(Pin(Event, TEXT("then"), EGPD_Output), Exec(Loops[0])) || !Linked(Pin(Loops[0], TEXT("Completed"), EGPD_Output), Exec(Loops[1]))) return Fail(TEXT("DrawEventAndLoopCompletionExecEdges"));
	return true;
}

void Configure(AmultiplayerCombatHUD& HUD)
{
	auto& Style = HUD.Presentation;
	Style.ReadyColor = FLinearColor(.2f, .9f, 1.f);
	Style.BlockedColor = FLinearColor(1.f, .25f, .2f);
	Style.HintColor = FLinearColor(.9f, .94f, 1.f);
	Style.CastingColor = FLinearColor(1.f, .75f, .18f);
	Style.ResolvedColor = FLinearColor(1.f, .65f, .1f);
	Style.DangerColor = FLinearColor(1.f, .18f, .08f);
	Style.ShadowColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
	Style.LineThickness = 1.5f;
	Style.MarkerThickness = 2.f;
	Style.HintOpacity = .8f;
	Style.TelegraphCenterOpacity = .45f;
	Style.PromptHeightRatio = .20f;
	Style.MeleePromptHeightRatio = .13f;
	Style.DetailOffset = 24.f;
	Style.PromptReferenceWidth = 960.f;
	Style.PromptMinScale = 1.f;
	Style.PromptMaxScale = 1.35f;
	Style.MarkerMinHalfWidth = 8.f;
	Style.MarkerSidePadding = 3.f;
	Style.MarkerTopPadding = 4.f;
	Style.MarkerBottomPadding = 3.f;
	Style.MarkerCornerRatio = .6f;
	Style.MarkerCornerMax = 12.f;
	Style.MarkerLabelGap = 4.f;
	Style.MarkerTextScale = 1.f;
	Style.GroundLift = 2.f;
	Style.TargetGroundLift = 3.f;
	Style.CrossHalfSize = 14.f;
	Style.DashArrowLength = 45.f;
	Style.DashArrowSideScale = .65f;
	Style.TelegraphDistance = 1500.f;
	Style.ResolvedDuration = 2.f;
	Style.RingSegments = 48;
	Style.TelegraphSegments = 24;
	Style.DecimalPlaces = 1;
	Style.ShadowOffset = FVector2D(1.f, 1.f);
	// Engine SmallFont may be RF_Transient; persist the font asset named by BaseEngine.ini instead.
	Style.Font = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
	Style.SkillNames = { NSLOCTEXT("CombatHUD", "Attack", "攻击"), NSLOCTEXT("CombatHUD", "Defend", "防御"), NSLOCTEXT("CombatHUD", "Heal", "治疗"), NSLOCTEXT("CombatHUD", "Area", "范围"), NSLOCTEXT("CombatHUD", "Dash", "冲刺") };
	Style.SlotKeys = { FText::FromString(TEXT("LMB")), FText::FromString(TEXT("Q")), FText::FromString(TEXT("E")), FText::FromString(TEXT("R")), FText::FromString(TEXT("Shift")) };
	Style.MeleeWindup = NSLOCTEXT("CombatHUD", "MeleeWindup", "近战起手");
	Style.MeleeDanger = NSLOCTEXT("CombatHUD", "MeleeDanger", "处于近战攻击范围 · 离开红色扇区可躲避");
	Style.ConfirmAttack = NSLOCTEXT("CombatHUD", "ConfirmAttack", "左键确认 · 右键取消");
	Style.ConfirmReleaseFormat = NSLOCTEXT("CombatHUD", "ConfirmReleaseFormat", "松开 {0} 释放 · 右键取消");
	Style.SkillActionFormat = NSLOCTEXT("CombatHUD", "SkillActionFormat", "{0} · {1}");
	Style.HealSelf = NSLOCTEXT("CombatHUD", "HealSelf", "本次治疗自己");
	Style.HealTeammate = NSLOCTEXT("CombatHUD", "HealTeammate", "本次治疗标记队友，优先最低血量比例");
	Style.HealDetailFormat = NSLOCTEXT("CombatHUD", "HealDetailFormat", "半径 {0} m 球形 · 必须可见 · {1}");
	Style.DefenseDetailFormat = NSLOCTEXT("CombatHUD", "DefenseDetailFormat", "正面格挡 · 前 {0} 秒可弹反正面投射物");
	Style.AreaDetailFormat = NSLOCTEXT("CombatHUD", "AreaDetailFormat", "射程 {0} m · 落点 {1} m · 伤害半径 {2} m · {3} 个目标");
	Style.DashDetailFormat = NSLOCTEXT("CombatHUD", "DashDetailFormat", "预计水平距离 {0} m · 静态障碍截断；落差/动态障碍可能改变落点");
	Style.Casting = NSLOCTEXT("CombatHUD", "Casting", "施法中 · 右键打断（已提交消耗不返还）");
	Style.Resolved = NSLOCTEXT("CombatHUD", "Resolved", "黄色圈为已结算范围，粒子消散不代表持续伤害");
	Style.Idle = NSLOCTEXT("CombatHUD", "Idle", "Q / E / R / Shift 按住预览、松开释放 · 右键取消 · 左键普攻");
}
}

// Existing real HUD widgets keep their trees and lifecycle graphs; only authored text/style defaults change.
bool UpdateHUDWidgetText(bool bValidateOnly)
{
	for (const TCHAR* Path : { TEXT("/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals"), TEXT("/Game/Combat/Escape/WBP_EscapeCombatVisuals") })
	{
		auto* Widget = Load<UWidgetBlueprint>(Path); if (!Widget || !Widget->GeneratedClass || Widget->ParentClass != UmultiplayerCombatWidget::StaticClass()) return false;
		auto* Defaults = Cast<UmultiplayerCombatWidget>(Widget->GeneratedClass->GetDefaultObject()); if (!Defaults) return false;
		if (!bValidateOnly)
		{
			Widget->Modify(); Defaults->Modify(); Defaults->TextTemplates.Reset();
			Defaults->TextTemplates.Add(TEXT("Vitals"), NSLOCTEXT("CombatHUDWidget", "Vitals", "{0} / {1}"));
			Defaults->TextTemplates.Add(TEXT("CastRescue"), NSLOCTEXT("CombatHUDWidget", "CastRescue", "救援引导"));
			Defaults->TextTemplates.Add(TEXT("CastArea"), NSLOCTEXT("CombatHUDWidget", "CastArea", "范围施法"));
			Defaults->TextTemplates.Add(TEXT("Lost"), NSLOCTEXT("CombatHUDWidget", "Lost", "本局失败 · Enter 重开"));
			Defaults->TextTemplates.Add(TEXT("Won"), NSLOCTEXT("CombatHUDWidget", "Won", "撤离成功 · Enter 重开"));
			Defaults->TextTemplates.Add(TEXT("Waiting"), NSLOCTEXT("CombatHUDWidget", "Waiting", "等待角色"));
			Defaults->TextTemplates.Add(TEXT("Dead"), NSLOCTEXT("CombatHUDWidget", "Dead", "已死亡 · Enter 重开"));
			Defaults->TextTemplates.Add(TEXT("Downed"), NSLOCTEXT("CombatHUDWidget", "Downed", "倒地 · 等待救援 {0}s"));
			Defaults->TextTemplates.Add(TEXT("Connecting"), NSLOCTEXT("CombatHUDWidget", "Connecting", "正在连接"));
			Defaults->TextTemplates.Add(TEXT("WaitingPartner"), NSLOCTEXT("CombatHUDWidget", "WaitingPartner", "等待队友加入"));
			Defaults->TextTemplates.Add(TEXT("Wave"), NSLOCTEXT("CombatHUDWidget", "Wave", "波次 {0}/{1} · 敌人 {2}"));
			Defaults->TextTemplates.Add(TEXT("Objectives"), NSLOCTEXT("CombatHUDWidget", "Objectives", "{0} · 钥匙 {1}/{2}\n集齐钥匙 → 双人踩板 → 一起撤离\nShift 冲刺 · F 救援"));
			Defaults->TextTemplates.Add(TEXT("Shield"), NSLOCTEXT("CombatHUDWidget", "Shield", "护盾 {0}"));
			Defaults->TextTemplates.Add(TEXT("Stunned"), NSLOCTEXT("CombatHUDWidget", "Stunned", "眩晕中"));
			Defaults->TextTemplates.Add(TEXT("Rooted"), NSLOCTEXT("CombatHUDWidget", "Rooted", "禁锢中"));
			Defaults->TextTemplates.Add(TEXT("Burning"), NSLOCTEXT("CombatHUDWidget", "Burning", "燃烧中"));
			Defaults->TextTemplates.Add(TEXT("Defending"), NSLOCTEXT("CombatHUDWidget", "Defending", "防御中"));
			Defaults->TextTemplates.Add(TEXT("Rescuing"), NSLOCTEXT("CombatHUDWidget", "Rescuing", "救援中 {0}%"));
			Defaults->TextTemplates.Add(TEXT("DashCooldown"), NSLOCTEXT("CombatHUDWidget", "DashCooldown", "冲刺 {0}s"));
			Defaults->TextTemplates.Add(TEXT("ShortProgression"), NSLOCTEXT("CombatHUDWidget", "ShortProgression", "Lv.{0} · 点数{1} · B成长"));
			Defaults->TextTemplates.Add(TEXT("Progression"), NSLOCTEXT("CombatHUDWidget", "Progression", "Lv.{0}  XP {1}/{2}  技能点 {3}  [B 成长]"));
			Defaults->TextTemplates.Add(TEXT("Separator"), NSLOCTEXT("CombatHUDWidget", "Separator", " · "));
			Defaults->SkillNames = { NSLOCTEXT("CombatHUD", "Attack", "攻击"), NSLOCTEXT("CombatHUD", "Defend", "防御"), NSLOCTEXT("CombatHUD", "Heal", "治疗"), NSLOCTEXT("CombatHUD", "Area", "范围"), NSLOCTEXT("CombatHUD", "Dash", "冲刺") };
			Defaults->AvailableIconColor = FLinearColor::White; Defaults->UnavailableIconColor = FLinearColor(.4f, .4f, .4f, 1.f); Defaults->SlotBackgroundColor = FLinearColor::Transparent;
			if (!Save(Widget)) return false;
		}
		if (Defaults->SkillNames.Num() != 5) return false;
		for (const TCHAR* Key : { TEXT("Vitals"), TEXT("CastRescue"), TEXT("CastArea"), TEXT("Lost"), TEXT("Won"), TEXT("Waiting"), TEXT("Dead"), TEXT("Downed"), TEXT("Connecting"), TEXT("WaitingPartner"), TEXT("Wave"), TEXT("Objectives"), TEXT("Shield"), TEXT("Stunned"), TEXT("Rooted"), TEXT("Burning"), TEXT("Defending"), TEXT("Rescuing"), TEXT("DashCooldown"), TEXT("ShortProgression"), TEXT("Progression"), TEXT("Separator") })
		{ const FText* Value = Defaults->TextTemplates.Find(FName(Key)); if (!Value || Value->IsEmpty()) return false; }
	}
	return true;
}

bool UpdateHUDPresentation(bool bValidateOnly)
{
	const FString SharedPath = Folder / TEXT("UI/BP_CombatHUDPresentation");
	auto* Shared = Load<UBlueprint>(SharedPath); const bool bCreated = !Shared;
	if (!Shared && !bValidateOnly)
		Shared = Keep(FKismetEditorUtilities::CreateBlueprint(AmultiplayerCombatHUD::StaticClass(), CreatePackage(*SharedPath), TEXT("BP_CombatHUDPresentation"),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CombatAssetBuild")));
	if (!Shared || Shared->ParentClass != AmultiplayerCombatHUD::StaticClass())
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID shared=%s parent=%s expectedNativeParent=1"), *GetPathNameSafe(Shared), Shared ? *GetPathNameSafe(Shared->ParentClass) : TEXT("None"));
		return false;
	}
	if (!bValidateOnly)
	{
		Shared->Modify(); if (!HUDPresentation::GenerateGraph(Shared) || !HUDPresentation::ValidateGraph(Shared) || !Compile(Shared)) return false;
		auto* Defaults = Cast<AmultiplayerCombatHUD>(Shared->GeneratedClass->GetDefaultObject()); if (!Defaults) return false;
		Defaults->Modify(); HUDPresentation::Configure(*Defaults); if (bCreated) FAssetRegistryModule::AssetCreated(Shared); if (!Save(Shared)) return false;
	}
	if (!Shared->GeneratedClass)
	{
		UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID shared=%s reason=MissingGeneratedClass"), *Shared->GetPathName());
		return false;
	}
	if (!HUDPresentation::ValidateGraph(Shared)) return false;
	const TCHAR* HUDPaths[] = { TEXT("/Game/Combat/UI/BP_CombatHUD"), TEXT("/Game/Combat/Escape/BP_EscapeHUD") };
	const TCHAR* ModePaths[] = { TEXT("/Game/Combat/BP_CombatGameMode"), TEXT("/Game/Combat/Escape/BP_EscapeGameMode") };
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* HUD = Load<UBlueprint>(HUDPaths[Index]); auto* Mode = Load<UBlueprint>(ModePaths[Index]);
		if (!HUD || !Mode || !HUD->GeneratedClass || !Mode->GeneratedClass)
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID index=%d reason=MissingActualHUDOrGameModeClass"), Index);
			return false;
		}
		// Actual HUDs keep their existing BeginPlay/Widget configuration and inherit one DrawHUD implementation.
		TArray<UEdGraph*> Graphs; HUD->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == TEXT("ReceiveDrawHUD"))
			{
				UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=ChildOverridesDrawHUD"), *HUD->GetPathName());
				return false;
			}
			if (const auto* Call = Cast<UK2Node_CallFunction>(Node); Call && Call->FunctionReference.GetMemberName().ToString().StartsWith(TEXT("Draw")))
			{
				UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=ChildContainsDrawCall"), *HUD->GetPathName());
				return false;
			}
		}
		if (!bValidateOnly)
		{
			if (HUD->ParentClass != AmultiplayerCombatHUD::StaticClass() && HUD->ParentClass != Shared->GeneratedClass) return false;
			HUD->Modify();
			// Official reparent validates inherited SCS roots before refreshing/compiling and preserves real child components.
			if (HUD->ParentClass != Shared->GeneratedClass) UBlueprintEditorLibrary::ReparentBlueprint(HUD, Shared->GeneratedClass);
			else if (!Compile(HUD)) return false;
			if (HUD->ParentClass != Shared->GeneratedClass || !HUD->GeneratedClass || HUD->Status == BS_Error) return false;
			auto* Defaults = Cast<AmultiplayerCombatHUD>(HUD->GeneratedClass->GetDefaultObject());
			const auto* SharedDefaults = Cast<AmultiplayerCombatHUD>(Shared->GeneratedClass->GetDefaultObject()); if (!Defaults || !SharedDefaults) return false;
			Defaults->Modify(); Defaults->Presentation = SharedDefaults->Presentation; if (!Save(HUD)) return false;
		}
		const auto* Defaults = Cast<AmultiplayerCombatHUD>(HUD->GeneratedClass->GetDefaultObject());
		if (HUD->ParentClass != Shared->GeneratedClass || !HUD->GeneratedClass->IsChildOf(Shared->GeneratedClass)
			|| !Defaults || !Defaults->Presentation.Font || Defaults->Presentation.RingSegments < 3 || Defaults->Presentation.SkillNames.Num() != 5
			|| Defaults->Presentation.SlotKeys.Num() != 5 || Defaults->Presentation.Idle.IsEmpty() || Defaults->Presentation.Resolved.IsEmpty())
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=ParentOrAuthoredPresentationDefaults"), *HUD->GetPathName());
			return false;
		}
		for (TFieldIterator<FTextProperty> Text(FmultiplayerCombatHUDPresentation::StaticStruct()); Text; ++Text)
			if (Text->GetPropertyValue_InContainer(&Defaults->Presentation).IsEmpty())
			{
				UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=EmptyAuthoredText field=%s"), *HUD->GetPathName(), *Text->GetName());
				return false;
			}
		const UFunction* DrawFunction = HUD->GeneratedClass->FindFunctionByName(TEXT("ReceiveDrawHUD"));
		if (!DrawFunction || DrawFunction->GetOuterUClass() != Shared->GeneratedClass)
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=WrongCompiledDrawFunctionOwner owner=%s"), *HUD->GetPathName(), DrawFunction ? *GetPathNameSafe(DrawFunction->GetOuterUClass()) : TEXT("None"));
			return false;
		}
		const FObjectPropertyBase* Field = FindFProperty<FObjectPropertyBase>(Mode->GeneratedClass, TEXT("HUDClass")); if (!Field) return false;
		if (!bValidateOnly && Field->GetObjectPropertyValue_InContainer(Mode->GeneratedClass->GetDefaultObject()) != HUD->GeneratedClass)
		{
			Mode->Modify(); Mode->GeneratedClass->GetDefaultObject()->Modify();
			if (!SetAsset(Mode->GeneratedClass->GetDefaultObject(), TEXT("HUDClass"), HUD->GeneratedClass) || !Compile(Mode) || !Save(Mode)) return false;
		}
		if (Field->GetObjectPropertyValue_InContainer(Mode->GeneratedClass->GetDefaultObject()) != HUD->GeneratedClass)
		{
			UE_LOG(LogCombatAssetBuild, Error, TEXT("COMBAT_HUD_INVALID asset=%s reason=GameModeHUDClassMismatch"), *Mode->GetPathName());
			return false;
		}
		UE_LOG(LogCombatAssetBuild, Display, TEXT("COMBAT_HUD_PRESENTATION mode=%s gameMode=%s actualHUDClass=%s sharedDrawGraph=%s execChain=verified dataEdges=verified nativeCanvasDraw=0 visualPreview=NOT_RUN"),
			bValidateOnly ? TEXT("Validate") : TEXT("Update"), *Mode->GetPathName(), *HUD->GeneratedClass->GetPathName(), *Shared->GetPathName());
	}
	return UpdateHUDWidgetText(bValidateOnly);
}
