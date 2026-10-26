// Editor implementation included once inside CoopResourceAssets by CoopResourcePresentationBuild.inl.
// Authors and validates the key Blueprint graph; the saved graph performs rotation during play.
#if !WITH_EDITOR
#error CoopResourceKeyGraph.inl requires the editor-only resource migration implementation.
#endif

UEdGraphPin* KeyPin(UEdGraphNode* Node, const TCHAR* Name, EEdGraphPinDirection Direction)
{
    if (!Node) return nullptr;
    for (auto* Pin : Node->Pins) if (Pin && Pin->Direction == Direction && Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")) == Name) return Pin;
    return nullptr;
}
bool KeyLinked(const UEdGraphPin* Input, const UEdGraphNode* Expected, const TCHAR* Output, int32 Depth = 3)
{
    if (!Input || Input->LinkedTo.Num() != 1) return false;
    const auto* Source = Input->LinkedTo[0];
    if (Source == KeyPin(const_cast<UEdGraphNode*>(Expected), Output, EGPD_Output)) return true;
    const auto* Conversion = Cast<UK2Node_CallFunction>(Source->GetOwningNode());
    if (!Depth || !Conversion || (Conversion->FunctionReference.GetMemberName() != TEXT("Conv_FloatToDouble")
        && Conversion->FunctionReference.GetMemberName() != TEXT("Conv_DoubleToFloat"))) return false;
    const UEdGraphPin* Value = KeyPin(const_cast<UK2Node_CallFunction*>(Conversion), TEXT("InFloat"), EGPD_Input);
    if (!Value) Value = KeyPin(const_cast<UK2Node_CallFunction*>(Conversion), TEXT("InDouble"), EGPD_Input);
    return KeyLinked(Value, Expected, Output, Depth - 1);
}

bool RemoveOldKeyTick(UBlueprint* Blueprint)
{
    TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        TSet<UEdGraphNode*> Remove;
        for (UEdGraphNode* Node : Graph->Nodes)
            if (const auto* Event = Cast<UK2Node_Event>(Node); Event && (Event->EventReference.GetMemberName() == TEXT("ReceiveTick")
                || Event->EventReference.GetMemberName() == TEXT("ApplyInstalledPresentation"))) Remove.Add(Node);
        if (Remove.IsEmpty()) continue;
        TArray<UEdGraphNode*> Pending = Remove.Array();
        for (int32 Index = 0; Index < Pending.Num(); ++Index)
            for (auto* Pin : Pending[Index]->Pins) if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                for (auto* Link : Pin->LinkedTo) if (Link && !Remove.Contains(Link->GetOwningNode()))
                {
                    auto* Node = Link->GetOwningNode(); const auto* Call = Cast<UK2Node_CallFunction>(Node);
                    if (!Call || (Call->FunctionReference.GetMemberName() != TEXT("K2_AddLocalRotation")
                        && Call->FunctionReference.GetMemberName() != TEXT("K2_AddActorLocalRotation")
                        && Call->FunctionReference.GetMemberName() != TEXT("SetActorTickEnabled")))
                        return Fail(Blueprint->GetPathName() + TEXT(" old Tick contains non-presentation work; no guess/no save"));
                    Remove.Add(Node); Pending.Add(Node);
                }
        // Delete pure inputs only when every consumer is in the retired branch.
        bool bAdded;
        do
        {
            bAdded = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                auto* K2 = Cast<UK2Node>(Node); if (!K2 || !K2->IsNodePure() || Remove.Contains(Node)) continue;
                bool bHasConsumer = false, bOnlyRetired = true;
                for (auto* Pin : Node->Pins) if (Pin && Pin->Direction == EGPD_Output)
                    for (auto* Link : Pin->LinkedTo) { bHasConsumer = true; bOnlyRetired &= Remove.Contains(Link->GetOwningNode()); }
                if (bHasConsumer && bOnlyRetired) { Remove.Add(Node); bAdded = true; }
            }
        } while (bAdded);
        for (auto* Node : Remove) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
    }
    return true;
}

bool GenerateKeyGraph(UBlueprint* Blueprint)
{
    if (!RemoveOldKeyTick(Blueprint)) return false;
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : Blueprint->UbergraphPages) if (Candidate->GetFName() == TEXT("KeyPresentation")) Graph = Candidate;
    if (!Graph)
    {
        Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("KeyPresentation"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
    }
    TArray<UEdGraphNode*> Old; for (UEdGraphNode* Node : Graph->Nodes) Old.Add(Node);
    for (auto* Node : Old) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
    const auto* Schema = GetDefault<UEdGraphSchema_K2>(); bool bOK = true;
    const auto Link = [&](UEdGraphPin* A, UEdGraphPin* B) { bOK &= A && B && Schema->TryCreateConnection(A, B); };
    const auto Add = [&](UEdGraphNode* Node, const TCHAR* Name, int32 X, int32 Y)
    {
        Node->Rename(Name, Graph, REN_DontCreateRedirectors); Node->CreateNewGuid(); Node->NodePosX = X; Node->NodePosY = Y; Graph->AddNode(Node, false, false);
    };
    const auto Event = [&](const TCHAR* Name, const TCHAR* Member, UClass* Owner, int32 Y)
    {
        auto* Node = NewObject<UK2Node_Event>(Graph); Add(Node, Name, 0, Y);
        Node->EventReference.SetExternalMember(Member, Owner); Node->bOverrideFunction = true; Node->PostPlacedNewNode(); Node->AllocateDefaultPins(); return Node;
    };
    const auto Get = [&](const TCHAR* Name, const TCHAR* Member, int32 X, int32 Y)
    {
        auto* Node = NewObject<UK2Node_VariableGet>(Graph); Add(Node, Name, X, Y);
        const FProperty* Property = FindFProperty<FProperty>(AmultiplayerCoopKey::StaticClass(), Member);
        if (!Property) { bOK = false; return Node; }
        Node->SetFromProperty(Property, true, AmultiplayerCoopKey::StaticClass()); Node->PostPlacedNewNode(); Node->AllocateDefaultPins(); return Node;
    };
    const auto Call = [&](const TCHAR* Name, const TCHAR* Member, UClass* Owner, int32 X, int32 Y)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Graph); Add(Node, Name, X, Y);
        UFunction* Function = Owner->FindFunctionByName(Member); if (!Function) { bOK = false; return Node; }
        Node->SetFromFunction(Function); Node->PostPlacedNewNode(); Node->AllocateDefaultPins(); return Node;
    };
    auto* Tick = Event(TEXT("KeyVisual_Tick"), TEXT("ReceiveTick"), AActor::StaticClass(), 0);
    auto* Apply = Event(TEXT("KeyVisual_Installed"), TEXT("ApplyInstalledPresentation"), AmultiplayerCoopKey::StaticClass(), 700);
    auto* Mesh = Get(TEXT("KeyVisual_Mesh"), TEXT("KeyMesh"), 600, 300);
    auto* Speed = Get(TEXT("KeyVisual_Speed"), TEXT("RotationSpeedDegrees"), 0, 300);
    auto* Axis = Get(TEXT("KeyVisual_Axis"), TEXT("RotationAxis"), 0, 450);
    auto* Multiply = Call(TEXT("KeyVisual_Angle"), TEXT("Multiply_DoubleDouble"), UKismetMathLibrary::StaticClass(), 250, 220);
    auto* Rotation = Call(TEXT("KeyVisual_AxisRotation"), TEXT("RotatorFromAxisAndAngle"), UKismetMathLibrary::StaticClass(), 500, 170);
    auto* AddRotation = Call(TEXT("KeyVisual_RotateMesh"), TEXT("K2_AddLocalRotation"), USceneComponent::StaticClass(), 900, 0);
    auto* Free = Call(TEXT("KeyVisual_Free"), TEXT("Not_PreBool"), UKismetMathLibrary::StaticClass(), 220, 700);
    auto* Positive = Call(TEXT("KeyVisual_PositiveSpeed"), TEXT("Greater_DoubleDouble"), UKismetMathLibrary::StaticClass(), 220, 850);
    auto* Zero = Call(TEXT("KeyVisual_ZeroAxis"), TEXT("Vector_IsNearlyZero"), UKismetMathLibrary::StaticClass(), 220, 1000);
    auto* HasAxis = Call(TEXT("KeyVisual_HasAxis"), TEXT("Not_PreBool"), UKismetMathLibrary::StaticClass(), 450, 1000);
    auto* Enabled = Call(TEXT("KeyVisual_CanRotate"), TEXT("BooleanAND"), UKismetMathLibrary::StaticClass(), 650, 850);
    auto* Active = Call(TEXT("KeyVisual_FreeAndRotating"), TEXT("BooleanAND"), UKismetMathLibrary::StaticClass(), 850, 700);
    auto* SetTick = Call(TEXT("KeyVisual_SetTick"), TEXT("SetActorTickEnabled"), AActor::StaticClass(), 1100, 700);
    const auto In = [](UEdGraphNode* N, const TCHAR* P) { return KeyPin(N, P, EGPD_Input); };
    const auto Out = [](UEdGraphNode* N, const TCHAR* P) { return KeyPin(N, P, EGPD_Output); };
    Link(Out(Tick,TEXT("then")),In(AddRotation,TEXT("execute"))); Link(Out(Mesh,TEXT("KeyMesh")),In(AddRotation,TEXT("self")));
    Link(Out(Tick,TEXT("DeltaSeconds")),In(Multiply,TEXT("A"))); Link(Out(Speed,TEXT("RotationSpeedDegrees")),In(Multiply,TEXT("B")));
    Link(Out(Multiply,TEXT("ReturnValue")),In(Rotation,TEXT("Angle"))); Link(Out(Axis,TEXT("RotationAxis")),In(Rotation,TEXT("Axis")));
    Link(Out(Rotation,TEXT("ReturnValue")),In(AddRotation,TEXT("DeltaRotation")));
    Link(Out(Apply,TEXT("then")),In(SetTick,TEXT("execute"))); Link(Out(Apply,TEXT("bIsInstalled")),In(Free,TEXT("A")));
    Link(Out(Speed,TEXT("RotationSpeedDegrees")),In(Positive,TEXT("A")));
    Link(Out(Axis,TEXT("RotationAxis")),In(Zero,TEXT("A"))); Link(Out(Zero,TEXT("ReturnValue")),In(HasAxis,TEXT("A")));
    Link(Out(Positive,TEXT("ReturnValue")),In(Enabled,TEXT("A"))); Link(Out(HasAxis,TEXT("ReturnValue")),In(Enabled,TEXT("B")));
    Link(Out(Free,TEXT("ReturnValue")),In(Active,TEXT("A"))); Link(Out(Enabled,TEXT("ReturnValue")),In(Active,TEXT("B")));
    Link(Out(Active,TEXT("ReturnValue")),In(SetTick,TEXT("bEnabled")));
    return bOK || Fail(TEXT("Key presentation pin connection failed"));
}

bool ValidateKeyGraph(UBlueprint* Blueprint)
{
    UEdGraph* Graph = nullptr; int32 Ticks = 0, Installs = 0;
    TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
    for (auto* Candidate : Graphs) for (UEdGraphNode* Node : Candidate->Nodes)
        if (const auto* Event = Cast<UK2Node_Event>(Node))
        {
            if (Event->EventReference.GetMemberName() == TEXT("ReceiveTick")) { ++Ticks; Graph = Candidate; }
            if (Event->EventReference.GetMemberName() == TEXT("ApplyInstalledPresentation")) ++Installs;
        }
    if (Ticks != 1 || Installs != 1 || !Graph || Graph->GetFName() != TEXT("KeyPresentation")) return Fail(TEXT("Key presentation requires one Tick and installation event"));
    const auto Node = [&](const TCHAR* Name) { return FindObject<UEdGraphNode>(Graph, Name); };
    auto* Tick = Node(TEXT("KeyVisual_Tick")); auto* Apply = Node(TEXT("KeyVisual_Installed"));
    auto* Mesh = Node(TEXT("KeyVisual_Mesh")); auto* Speed = Node(TEXT("KeyVisual_Speed")); auto* Axis = Node(TEXT("KeyVisual_Axis"));
    auto* Multiply = Node(TEXT("KeyVisual_Angle")); auto* Rotation = Node(TEXT("KeyVisual_AxisRotation")); auto* AddRotation = Node(TEXT("KeyVisual_RotateMesh"));
    auto* Free = Node(TEXT("KeyVisual_Free")); auto* Positive = Node(TEXT("KeyVisual_PositiveSpeed")); auto* Zero = Node(TEXT("KeyVisual_ZeroAxis"));
    auto* HasAxis = Node(TEXT("KeyVisual_HasAxis")); auto* Enabled = Node(TEXT("KeyVisual_CanRotate")); auto* Active = Node(TEXT("KeyVisual_FreeAndRotating")); auto* SetTick = Node(TEXT("KeyVisual_SetTick"));
    for (auto Pair : {TPair<UEdGraphNode*, FName>(Mesh,TEXT("KeyMesh")), {Speed,TEXT("RotationSpeedDegrees")}, {Axis,TEXT("RotationAxis")}})
    {
        auto* Getter = Cast<UK2Node_VariableGet>(Pair.Key);
        if (!Getter || !Getter->VariableReference.IsSelfContext() || Getter->VariableReference.GetMemberName() != Pair.Value
            || Getter->GetPropertyForVariable() != FindFProperty<FProperty>(AmultiplayerCoopKey::StaticClass(), Pair.Value)) return Fail(TEXT("Key reads must resolve native config on self"));
    }
    const auto In = [](UEdGraphNode* N, const TCHAR* P) { return KeyPin(N, P, EGPD_Input); };
    const bool OK = KeyLinked(In(AddRotation,TEXT("execute")),Tick,TEXT("then")) && KeyLinked(In(AddRotation,TEXT("self")),Mesh,TEXT("KeyMesh"))
        && KeyLinked(In(Multiply,TEXT("A")),Tick,TEXT("DeltaSeconds")) && KeyLinked(In(Multiply,TEXT("B")),Speed,TEXT("RotationSpeedDegrees"))
        && KeyLinked(In(Rotation,TEXT("Angle")),Multiply,TEXT("ReturnValue")) && KeyLinked(In(Rotation,TEXT("Axis")),Axis,TEXT("RotationAxis"))
        && KeyLinked(In(AddRotation,TEXT("DeltaRotation")),Rotation,TEXT("ReturnValue")) && KeyLinked(In(SetTick,TEXT("execute")),Apply,TEXT("then"))
        && KeyLinked(In(Free,TEXT("A")),Apply,TEXT("bIsInstalled")) && KeyLinked(In(Positive,TEXT("A")),Speed,TEXT("RotationSpeedDegrees"))
        && KeyLinked(In(Zero,TEXT("A")),Axis,TEXT("RotationAxis")) && KeyLinked(In(HasAxis,TEXT("A")),Zero,TEXT("ReturnValue"))
        && KeyLinked(In(Enabled,TEXT("A")),Positive,TEXT("ReturnValue")) && KeyLinked(In(Enabled,TEXT("B")),HasAxis,TEXT("ReturnValue"))
        && KeyLinked(In(Active,TEXT("A")),Free,TEXT("ReturnValue")) && KeyLinked(In(Active,TEXT("B")),Enabled,TEXT("ReturnValue"))
        && KeyLinked(In(SetTick,TEXT("bEnabled")),Active,TEXT("ReturnValue"));
    if (!OK) return Fail(TEXT("Key presentation exec/data chain mismatch"));
    for (const TCHAR* Field : {TEXT("bSweep"), TEXT("bTeleport")})
    {
        const auto* Value = In(AddRotation, Field);
        if (!Value || !Value->LinkedTo.IsEmpty() || Value->DefaultValue.ToBool()) return Fail(TEXT("Key mesh presentation must not sweep/teleport gameplay"));
    }
    const auto* Threshold = In(Positive, TEXT("B"));
    const auto* Tolerance = In(Zero, TEXT("Tolerance"));
    const auto* TickTarget = In(SetTick, TEXT("self"));
    if (!Threshold || !Threshold->LinkedTo.IsEmpty() || !FMath::IsNearlyZero(FCString::Atod(*Threshold->DefaultValue))
        || !Tolerance || !Tolerance->LinkedTo.IsEmpty() || !FMath::IsNearlyEqual(FCString::Atod(*Tolerance->DefaultValue), .0001, .000001)
        || (TickTarget && (!TickTarget->LinkedTo.IsEmpty() || TickTarget->DefaultObject))) return Fail(TEXT("Key Tick policy defaults/owner mismatch"));
    for (const auto& Expected : {TPair<UEdGraphNode*,FName>(Multiply,TEXT("Multiply_DoubleDouble")), {Rotation,TEXT("RotatorFromAxisAndAngle")},
        {AddRotation,TEXT("K2_AddLocalRotation")}, {Free,TEXT("Not_PreBool")}, {Positive,TEXT("Greater_DoubleDouble")},
        {Zero,TEXT("Vector_IsNearlyZero")}, {HasAxis,TEXT("Not_PreBool")}, {Enabled,TEXT("BooleanAND")}, {Active,TEXT("BooleanAND")}, {SetTick,TEXT("SetActorTickEnabled")}})
    {
        const auto* Call = Cast<UK2Node_CallFunction>(Expected.Key);
        if (!Call || Call->FunctionReference.GetMemberName() != Expected.Value || !Call->GetTargetFunction()) return Fail(TEXT("Key function target mismatch"));
    }
    UE_LOG(LogTemp, Display, TEXT("RESOURCE_KEY_GRAPH PASS oneTick=1 oneInstallEvent=1 meshOnly=1 privateConfig=resolved nativeRotation=removed"));
    return true;
}
