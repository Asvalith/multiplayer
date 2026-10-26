// Editor implementation included once inside CoopResourceAssets by CoopResourcePresentationBuild.inl.
// Only explicit migration may save the two current combat maps; validation compares them without saving.
#if !WITH_EDITOR
#error CoopResourceMapMigration.inl requires the editor-only resource migration implementation.
#endif

FString Role(const AActor* Actor)
{
    if (Actor->IsA<AmultiplayerCoopGate>()) return TEXT("Gate");
    if (Actor->IsA<AmultiplayerPressurePlate>()) return TEXT("Plate");
    if (Actor->IsA<AmultiplayerMovingPlatform>()) return TEXT("Platform");
    if (Actor->IsA<AmultiplayerCoopKey>()) return TEXT("Key");
    if (Actor->IsA<AmultiplayerKeySocket>()) return TEXT("Socket");
    if (Actor->IsA<AmultiplayerCombatEncounter>()) return TEXT("Encounter");
    if (Actor->IsA<AmultiplayerCombatSupply>()) return TEXT("Supply");
    if (Actor->IsA<AmultiplayerCombatProjectile>()) return TEXT("Projectile");
    return FString();
}

FString PropertyValue(const FProperty* Property, const void* Value)
{
    if (const auto* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
        if (const auto* Actor = Cast<AActor>(Object)) return TEXT("ActorLabel:") + Actor->GetActorLabel();
        return GetPathNameSafe(Object);
    }
    if (const auto* Array = CastField<FArrayProperty>(Property))
    {
        FString Result; FScriptArrayHelper Values(Array, Value);
        for (int32 Index = 0; Index < Values.Num(); ++Index) Result += PropertyValue(Array->Inner, Values.GetRawPtr(Index)) + TEXT(";");
        return Result;
    }
    FString Result; Property->ExportTextItem_Direct(Result, Value, nullptr, nullptr, PPF_None); return Result;
}

TMap<FName, FString> GameplaySettings(const AActor* Actor)
{
    TMap<FName, FString> Result;
    for (const TCHAR* Name : {TEXT("DestinationSocket"), TEXT("ActivationPlate"), TEXT("RequiredPlates"), TEXT("ActivationSource"),
        TEXT("RequiredActivePlateCount"), TEXT("RequiredPlayers"), TEXT("bLatchOnceActivated"), TEXT("bStayOpenOnceActivated"),
        TEXT("bRequireObjectiveComplete"), TEXT("EnemyClass"), TEXT("WaveSizes"), TEXT("WaveInterval"), TEXT("MaxLivingEnemies"),
        TEXT("StartPlayers"), TEXT("HealthAmount"), TEXT("EnergyAmount")})
        if (const FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), Name))
            Result.Add(Name, PropertyValue(Property, Property->ContainerPtrToValuePtr<void>(Actor)));
    return Result;
}

bool MatchesCapturedValue(const FProperty* Property, const void* Value, const TSharedPtr<FJsonValue>& Expected)
{
    if (const auto* Object = CastField<FObjectPropertyBase>(Property))
    {
        UObject* Actual = Object->GetObjectPropertyValue(Value);
        if (Expected->Type == EJson::Null) return Actual == nullptr;
        const auto Description = Expected->AsObject(); if (!Description) return false;
        FString Label, Asset;
        if (Description->TryGetStringField(TEXT("actor_label"), Label))
        {
            const auto* Actor = Cast<AActor>(Actual); return Actor && Actor->GetActorLabel() == Label;
        }
        return Description->TryGetStringField(TEXT("asset"), Asset) && Actual && Actual->GetPathName() == Asset;
    }
    if (const auto* Array = CastField<FArrayProperty>(Property))
    {
        if (Expected->Type != EJson::Array) return false;
        const auto& Items = Expected->AsArray(); FScriptArrayHelper Actual(Array, Value);
        if (Items.Num() != Actual.Num()) return false;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
            if (!MatchesCapturedValue(Array->Inner, Actual.GetRawPtr(Index), Items[Index])) return false;
        return true;
    }
    if (const auto* Bool = CastField<FBoolProperty>(Property)) return Expected->Type == EJson::Boolean && Bool->GetPropertyValue(Value) == Expected->AsBool();
    if (const auto* Number = CastField<FNumericProperty>(Property))
    {
        if (Expected->Type != EJson::Number) return true; // Reflected enum labels retain their separate before/after invariant.
        const double Actual = Number->IsFloatingPoint() ? Number->GetFloatingPointPropertyValue(Value) : double(Number->GetSignedIntPropertyValue(Value));
        return FMath::IsNearlyEqual(Actual, Expected->AsNumber(), .0001);
    }
    return true; // Appearance vectors are compared in RestoreAppearance; all remaining rule exports are compared during conversion.
}

bool MatchesCapturedRules(AActor* Actor, const TSharedPtr<FJsonObject>& Record)
{
    const auto Properties = Record->GetObjectField(TEXT("properties"));
    for (const auto& Pair : Properties->Values)
    {
        const FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), *Pair.Key);
        if (!Property || !MatchesCapturedValue(Property, Property->ContainerPtrToValuePtr<void>(Actor), Pair.Value))
            return Fail(Actor->GetPathName() + TEXT(" captured reference/rule differs: ") + Pair.Key);
    }
    return true;
}

TMap<FName, FString> CollisionSettings(AActor* Actor)
{
    TMap<FName, FString> Result; TArray<UPrimitiveComponent*> Components; Actor->GetComponents(Components);
    for (const auto* Component : Components)
    {
        FString Value = FString::Printf(TEXT("Enabled=%d;Object=%d;Overlap=%d;Mobility=%d;"), int32(Component->GetCollisionEnabled()),
            int32(Component->GetCollisionObjectType()), Component->GetGenerateOverlapEvents(), int32(Component->Mobility));
        for (int32 Channel = 0; Channel < ECollisionChannel::ECC_MAX; ++Channel)
            Value += FString::FromInt(int32(Component->GetCollisionResponseToChannel(ECollisionChannel(Channel)))) + TEXT(",");
        if (const auto* Sphere = Cast<USphereComponent>(Component)) Value += FString::Printf(TEXT("Sphere=%.9g"), Sphere->GetUnscaledSphereRadius());
        if (const auto* Box = Cast<UBoxComponent>(Component)) Value += TEXT("Box=") + Box->GetUnscaledBoxExtent().ToString();
        Result.Add(Component->GetFName(), Value);
    }
    return Result;
}

// UEditorActorSubsystem owns reference replacement and actor destruction. There is no runtime actor swap.
AActor* ConvertNative(AActor* Source, UClass* Target)
{
    const FString Label = Source->GetActorLabel(); UWorld* World = Source->GetWorld();
    TSet<FString> Components; TArray<UActorComponent*> Owned; Source->GetComponents(Owned);
    for (const auto* Component : Owned) Components.Add(Component->GetName());
    UEditorActorSubsystem::DoConvertActors({Source}, Target, Components, false, FString());
    AActor* Result = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
        if (It->GetClass() == Target && It->GetActorLabel() == Label)
        {
            if (Result) return nullptr;
            Result = *It;
        }
    return Result;
}

bool MigrateResourceMaps(const TSharedPtr<FJsonObject>& Maps, const TMap<FString, UClass*>& Classes, bool bReadOnly, const FString& Backup)
{
    const TCHAR* Packages[] = {TEXT("/Game/Combat/Maps/CombatTest"), TEXT("/Game/Combat/Maps/CoopEscape")};
    for (const TCHAR* Package : Packages)
    {
        if (!bReadOnly && !BackupPackage(Package, true, Backup)) return false;
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Package); if (!World) return Fail(Package);
        const auto& Records = Maps->GetArrayField(Package);
        TMap<FString, AActor*> Actors;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (Role(*It).IsEmpty()) continue;
            const FString Label = It->GetActorLabel();
            if (Actors.Contains(Label)) return Fail(TEXT("Ambiguous resource actor label: ") + Label);
            Actors.Add(Label, *It);
        }
        if (Actors.Num() != Records.Num()) return Fail(FString(Package) + TEXT(" resource population differs from old-build snapshot"));
        // Restore fields that previously equalled a native parent default before establishing the invariants.
        for (const auto& Item : Records)
        {
            const auto Record = Item->AsObject(); if (!Record) return false;
            AActor** Actor = Actors.Find(Record->GetStringField(TEXT("label")));
            if (!Actor || !RestoreAppearance(*Actor, Record, bReadOnly)) return false;
        }
        TMap<FString, TMap<FName, FString>> Rules, Collisions;
        TMap<FString, FTransform> Transforms;
        for (const auto& Pair : Actors)
        {
            Rules.Add(Pair.Key, GameplaySettings(Pair.Value)); Collisions.Add(Pair.Key, CollisionSettings(Pair.Value));
            Transforms.Add(Pair.Key, Pair.Value->GetActorTransform());
        }
        int32 Converted = 0;
        for (const auto& Item : Records)
        {
            const auto Record = Item->AsObject(); if (!Record) return false;
            const FString Label = Record->GetStringField(TEXT("label")), ExpectedRole = Record->GetStringField(TEXT("role"));
            AActor** Found = Actors.Find(Label); if (!Found || Role(*Found) != ExpectedRole) return Fail(Label + TEXT(" resource owner mismatch"));
            AActor* Actor = *Found;
            UClass* const* Target = Classes.Find(ExpectedRole); if (!Target) return false;
            if (Actor->GetClass()->HasAnyClassFlags(CLASS_Native))
            {
                if (bReadOnly) return Fail(Label + TEXT(" still uses a native prototype instead of an authored archetype"));
                Actor = ConvertNative(Actor, *Target); if (!Actor) return Fail(Label + TEXT(" editor conversion failed; map not saved"));
                Actors[Label] = Actor; ++Converted;
            }
            if (!Actor->IsA(*Target) || !RestoreAppearance(Actor, Record, bReadOnly) || !MatchesCapturedRules(Actor, Record)) return false;
            if (!Actor->GetActorTransform().Equals(Transforms[Label], .001)) return Fail(Label + TEXT(" world transform changed"));
        }
        // Check after every replacement so references to another converted actor are compared by stable label.
        for (const auto& Pair : Actors)
        {
            if (GameplaySettings(Pair.Value).OrderIndependentCompareEqual(Rules[Pair.Key]) == false)
                return Fail(Pair.Key + TEXT(" gameplay settings/references changed; map not saved"));
            const auto Actual = CollisionSettings(Pair.Value);
            for (const auto& Collision : Collisions[Pair.Key])
            {
                const FString* Value = Actual.Find(Collision.Key);
                if (!Value || *Value != Collision.Value) return Fail(Pair.Key + TEXT(" collision policy changed; map not saved"));
            }
        }
        if (!bReadOnly && !FEditorFileUtils::SaveMap(World, FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()))) return false;
        UE_LOG(LogTemp, Display, TEXT("RESOURCE_MAP PASS map=%s actors=%d converted=%d readonly=%d rules=unchanged collision=unchanged"), Package, Actors.Num(), Converted, bReadOnly);
    }
    return true;
}
