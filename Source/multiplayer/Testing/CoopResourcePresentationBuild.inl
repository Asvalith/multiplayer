// Editor implementation included once at global scope inside WITH_EDITOR by CombatAssetBuildCommandlet.cpp.
// Explicit migration writes authored resource packages; native rules stay in their existing components.
#if !WITH_EDITOR
#error CoopResourcePresentationBuild.inl requires the editor-only commandlet implementation.
#endif

#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEncounter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Mechanisms/multiplayerCoopGate.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/SphereComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "FileHelpers.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace CoopResourceAssets
{
TArray<TStrongObjectPtr<UObject>> Inputs;
template<class T> T* Hold(T* Object) { if (Object) Inputs.Emplace(Object); return Object; }
FString ObjectPath(const FString& Package) { return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package); }
constexpr const TCHAR* GatePath = TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpcoopgate");
constexpr const TCHAR* PlatePath = TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/pressplate");
constexpr const TCHAR* PlatformPath = TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpmovingplatform");
constexpr const TCHAR* KeyPath = TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpkey");
constexpr const TCHAR* SocketPath = TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpkeysocket");
constexpr const TCHAR* SupplyPath = TEXT("/Game/Combat/Presentation/BP_CombatSupply");
constexpr const TCHAR* EncounterPath = TEXT("/Game/Combat/Presentation/BP_CombatEncounter");
constexpr const TCHAR* ProjectilePath = TEXT("/Game/Combat/Presentation/BP_CombatProjectile");

bool Fail(const FString& Message)
{
    UE_LOG(LogTemp, Error, TEXT("RESOURCE_PRESENTATION_INVALID %s"), *Message); return false;
}

bool Compile(UBlueprint* Blueprint)
{
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &Results);
    return Results.NumErrors == 0 && Blueprint->Status != BS_Error;
}

bool SaveAsset(UObject* Asset)
{
    const FString Package = Asset->GetOutermost()->GetName();
    const bool bAllowed = Package == GatePath || Package == PlatePath || Package == PlatformPath || Package == KeyPath
        || Package == SocketPath || Package == SupplyPath || Package == EncounterPath || Package == ProjectilePath
        || Package == TEXT("/Game/Combat/DA_CombatConfig") || Package == TEXT("/Game/Combat/DA_EnemyCombatConfig")
        || Package == TEXT("/Game/Combat/Escape/DA_EscapeCombat");
    if (!bAllowed) return Fail(TEXT("Save outside resource whitelist: ") + Package);
    Asset->MarkPackageDirty(); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Asset->GetOutermost(), Asset,
        *FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()), Args);
}

bool BackupPackage(const FString& Package, bool bMap, const FString& Backup)
{
    const FString Source = FPackageName::LongPackageNameToFilename(Package, bMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
    if (!IFileManager::Get().FileExists(*Source)) return true; // A deliberately new Blueprint has no old package.
    const FString Destination = Backup / Package.RightChop(6) + (bMap ? TEXT(".umap") : TEXT(".uasset"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
    return IFileManager::Get().Copy(*Destination, *Source, false) == COPY_OK;
}

UBlueprint* Blueprint(const FString& Path, UClass* Parent, bool bReadOnly, AActor* Source = nullptr)
{
    UBlueprint* Result = Hold(LoadObject<UBlueprint>(nullptr, *ObjectPath(Path)));
    if (!Result && !bReadOnly)
    {
        if (Source)
        {
            FKismetEditorUtilities::FCreateBlueprintFromActorParams Params;
            Params.bReplaceActor = false; Params.bKeepMobility = true; Params.bOpenBlueprint = false;
            Result = Hold(FKismetEditorUtilities::CreateBlueprintFromActor(Path, Source, Params));
        }
        else
        {
            Result = Hold(FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("ResourcePresentation")));
            if (Result) FAssetRegistryModule::AssetCreated(Result);
        }
    }
    if (!Result || Result->ParentClass != Parent || !Result->GeneratedClass) return nullptr;
    return Result;
}

FVector Vector(const TArray<TSharedPtr<FJsonValue>>& Values)
{
    return Values.Num() == 3 ? FVector(Values[0]->AsNumber(), Values[1]->AsNumber(), Values[2]->AsNumber()) : FVector::ZeroVector;
}
FRotator Rotator(const TArray<TSharedPtr<FJsonValue>>& Values)
{
    return Values.Num() == 3 ? FRotator(Values[0]->AsNumber(), Values[1]->AsNumber(), Values[2]->AsNumber()) : FRotator::ZeroRotator;
}

// Apply only the recorded authoring fields. Collision, movement, references and gameplay rules are untouched.
bool RestoreAppearance(AActor* Actor, const TSharedPtr<FJsonObject>& Record, bool bReadOnly)
{
    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    if (!Record.IsValid() || !Record->TryGetArrayField(TEXT("components"), Components)) return false;
    for (const auto& Item : *Components)
    {
        const auto Data = Item->AsObject(); if (!Data) return false;
        const FName Name(*Data->GetStringField(TEXT("name")));
        USceneComponent* Component = Cast<USceneComponent>(Actor->GetDefaultSubobjectByName(Name));
        if (!Component)
        {
            TArray<USceneComponent*> Owned; Actor->GetComponents(Owned);
            for (auto* Candidate : Owned) if (Candidate->GetFName() == Name) { Component = Candidate; break; }
        }
        if (!Component)
            if (auto* OwnerBlueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy); OwnerBlueprint && OwnerBlueprint->SimpleConstructionScript)
                for (auto* Node : OwnerBlueprint->SimpleConstructionScript->GetAllNodes())
                    if (Node->GetVariableName() == Name) { Component = Cast<USceneComponent>(Node->ComponentTemplate); break; }
        if (!Component) return Fail(Actor->GetPathName() + TEXT(" missing authored component ") + Name.ToString());
        const FVector Location = Vector(Data->GetArrayField(TEXT("location")));
        const FRotator Rotation = Rotator(Data->GetArrayField(TEXT("rotation")));
        const FVector Scale = Vector(Data->GetArrayField(TEXT("scale")));
        if (!bReadOnly)
        {
            Component->Modify(); Component->SetRelativeLocationAndRotation(Location, Rotation); Component->SetRelativeScale3D(Scale);
        }
        if (!Component->GetRelativeLocation().Equals(Location, .001) || !Component->GetRelativeRotation().Equals(Rotation, .001)
            || !Component->GetRelativeScale3D().Equals(Scale, .001)) return Fail(Component->GetPathName() + TEXT(" authored transform mismatch"));
        FString MeshPath;
        if (Data->TryGetStringField(TEXT("mesh"), MeshPath))
        {
            auto* Mesh = Cast<UStaticMeshComponent>(Component); if (!Mesh) return false;
            if (!bReadOnly) Mesh->SetStaticMesh(MeshPath.IsEmpty() ? nullptr : Hold(LoadObject<UStaticMesh>(nullptr, *MeshPath)));
            if (GetPathNameSafe(Mesh->GetStaticMesh()) != (MeshPath.IsEmpty() ? TEXT("None") : MeshPath)) return Fail(Component->GetPathName() + TEXT(" authored mesh mismatch"));
        }
        FString Text;
        if (Data->TryGetStringField(TEXT("text"), Text))
        {
            auto* Label = Cast<UTextRenderComponent>(Component); if (!Label) return false;
            const auto& Color = Data->GetArrayField(TEXT("color")); if (Color.Num() != 4) return false;
            if (!bReadOnly)
            {
                Label->SetText(FText::FromString(Text)); Label->SetWorldSize(float(Data->GetNumberField(TEXT("world_size"))));
                Label->SetTextRenderColor(FColor(uint8(Color[0]->AsNumber()), uint8(Color[1]->AsNumber()), uint8(Color[2]->AsNumber()), uint8(Color[3]->AsNumber())));
                Label->SetHiddenInGame(Data->GetBoolField(TEXT("hidden")));
            }
            if (Label->Text.ToString() != Text || !FMath::IsNearlyEqual(Label->WorldSize, float(Data->GetNumberField(TEXT("world_size"))))
                || Label->bHiddenInGame != Data->GetBoolField(TEXT("hidden"))) return Fail(Component->GetPathName() + TEXT(" label defaults mismatch"));
        }
    }
    const auto Properties = Record->GetObjectField(TEXT("properties"));
    if (Actor->IsA<AmultiplayerCoopKey>())
    {
        const FFloatProperty* Speed = FindFProperty<FFloatProperty>(Actor->GetClass(), TEXT("RotationSpeedDegrees"));
        const FStructProperty* Axis = FindFProperty<FStructProperty>(Actor->GetClass(), TEXT("RotationAxis"));
        if (!Speed || !Axis) return false;
        const float ExpectedSpeed = float(Properties->GetNumberField(TEXT("RotationSpeedDegrees")));
        const FVector ExpectedAxis = Vector(Properties->GetArrayField(TEXT("RotationAxis")));
        if (!bReadOnly) { Speed->SetPropertyValue_InContainer(Actor, ExpectedSpeed); *Axis->ContainerPtrToValuePtr<FVector>(Actor) = ExpectedAxis; }
        if (!FMath::IsNearlyEqual(Speed->GetPropertyValue_InContainer(Actor), ExpectedSpeed)
            || !Axis->ContainerPtrToValuePtr<FVector>(Actor)->Equals(ExpectedAxis)) return false;
    }
    if (Actor->IsA<AmultiplayerCombatEncounter>())
    {
        const auto Description = Properties->GetObjectField(TEXT("EnemyClass"));
        UClass* Enemy = Hold(LoadObject<UClass>(nullptr, *Description->GetStringField(TEXT("asset"))));
        const FClassProperty* Property = FindFProperty<FClassProperty>(Actor->GetClass(), TEXT("EnemyClass"));
        if (!Property || !Enemy) return false;
        if (!bReadOnly) Property->SetObjectPropertyValue_InContainer(Actor, Enemy);
        if (Property->GetObjectPropertyValue_InContainer(Actor) != Enemy) return Fail(Actor->GetPathName() + TEXT(" encounter class differs from snapshot"));
    }
    return true;
}

TSharedPtr<FJsonObject> SupplyDefaults(const TSharedPtr<FJsonObject>& Snapshot)
{
    const auto& Records = Snapshot->GetObjectField(TEXT("maps"))->GetArrayField(TEXT("/Game/Combat/Maps/CoopEscape"));
    for (const auto& Item : Records)
    {
        const auto Record = Item->AsObject(); if (!Record || Record->GetStringField(TEXT("role")) != TEXT("Supply")) continue;
        FString Json; FJsonSerializer::Serialize(Record.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
        TSharedPtr<FJsonObject> Result; if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result)) return nullptr;
        // Actor root translation belongs to the map, never to the reusable supply archetype.
        for (const auto& Component : Result->GetArrayField(TEXT("components")))
            if (Component->AsObject()->GetStringField(TEXT("name")) == TEXT("SupplyTrigger"))
            {
                const auto Root = Component->AsObject();
                Root->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)});
                Root->SetArrayField(TEXT("rotation"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)});
                Root->SetArrayField(TEXT("scale"), {MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(1)});
            }
        return Result;
    }
    return nullptr;
}

bool ConfigureFeedback(UmultiplayerCombatConfig& Config, UClass* ProjectileClass, bool bReadOnly)
{
    if (!bReadOnly)
    {
        Config.Modify(); Config.ProjectileClass = ProjectileClass;
        Config.ProjectileEffectTransform = FTransform::Identity; Config.ProjectileDurationParameter = TEXT("User.Duration");
        Config.AreaMarkerDepth = 32.f;
        Config.AttackFeedback.Anchor = EMultiplayerFeedbackAnchor::Muzzle;
        Config.ImpactFeedback.Anchor = EMultiplayerFeedbackAnchor::CueLocation;
        Config.AreaFeedback.Anchor = EMultiplayerFeedbackAnchor::CueLocation; Config.AreaFeedback.Duration = .6f;
        Config.AreaFeedback.AreaScaleParameter = TEXT("User.Scale_All");
        Config.HealFeedback.Anchor = EMultiplayerFeedbackAnchor::Feet; Config.HealFeedback.bAttachToTarget = true; Config.HealFeedback.Duration = .8f;
        Config.HealFeedback.SuccessfulCasterMessage = NSLOCTEXT("CombatFeedback", "HealedPartner", "治疗成功：已为标记队友恢复生命");
        Config.DefenseFeedback.Anchor = EMultiplayerFeedbackAnchor::Feet; Config.DefenseFeedback.bAttachToTarget = true;
        Config.StunFeedback.bAttachToTarget = true;
    }
    if (Config.ProjectileClass != ProjectileClass || !Config.ProjectileEffectTransform.Equals(FTransform::Identity)
        || Config.ProjectileDurationParameter != TEXT("User.Duration") || Config.AreaMarkerDepth != 32.f
        || Config.AttackFeedback.Anchor != EMultiplayerFeedbackAnchor::Muzzle
        || Config.ImpactFeedback.Anchor != EMultiplayerFeedbackAnchor::CueLocation
        || Config.AreaFeedback.Anchor != EMultiplayerFeedbackAnchor::CueLocation || Config.AreaFeedback.AreaScaleParameter != TEXT("User.Scale_All")
        || !FMath::IsNearlyEqual(Config.AreaFeedback.Duration, .6f) || !FMath::IsNearlyEqual(Config.HealFeedback.Duration, .8f)
        || Config.HealFeedback.Anchor != EMultiplayerFeedbackAnchor::Feet || !Config.HealFeedback.bAttachToTarget
        || Config.HealFeedback.SuccessfulCasterMessage.IsEmpty()
        || Config.DefenseFeedback.Anchor != EMultiplayerFeedbackAnchor::Feet || !Config.DefenseFeedback.bAttachToTarget
        || !Config.StunFeedback.bAttachToTarget) return Fail(Config.GetPathName() + TEXT(" feedback wiring mismatch"));
    for (const auto* Feedback : {&Config.AttackFeedback, &Config.ImpactFeedback, &Config.AreaFeedback, &Config.HealFeedback,
        &Config.DefenseFeedback, &Config.CastingFeedback, &Config.StunFeedback, &Config.FailureFeedback})
        if (Feedback->Duration <= 0 || Feedback->LoopDuration <= 0 || Feedback->NiagaraCleanupDelay <= 0 || Feedback->CascadeCleanupDelay <= 0
            || Feedback->Scale.ContainsNaN() || Feedback->Scale.GetAbsMin() <= 0) return false;
    return true;
}

// Fresh configs created by the older authoring entry points must bind the same production Blueprint.
// Existing configs retain their authored policy; the explicit migration is the only bulk rewrite.
bool InitializeAuthoredFeedback(UmultiplayerCombatConfig& Config)
{
    UClass* Projectile = LoadClass<AmultiplayerCombatProjectile>(nullptr, *(ObjectPath(ProjectilePath) + TEXT("_C")));
    if (!Projectile || Projectile->ClassGeneratedBy == nullptr) return Fail(TEXT("Production projectile Blueprint is required before creating combat config"));
    return ConfigureFeedback(Config, Projectile, false);
}

// Graph authoring and map conversion are separate editor implementation files, not runtime layers.
#include "Testing/CoopResourceKeyGraph.inl"
#include "Testing/CoopResourceMapMigration.inl"

bool RunResourcePresentation(bool bReadOnly)
{
    Inputs.Reset(); ON_SCOPE_EXIT { Inputs.Reset(); };
    FString SnapshotPath;
    if (!FParse::Value(FCommandLine::Get(), TEXT("PresentationSnapshot="), SnapshotPath)) return Fail(TEXT("Provide old-build -PresentationSnapshot=<JSON>"));
    FString Json; TSharedPtr<FJsonObject> Snapshot;
    if (!FFileHelper::LoadFileToString(Json, *SnapshotPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Snapshot)
        || !Snapshot || Snapshot->GetIntegerField(TEXT("schema")) != 1) return Fail(TEXT("Invalid old-build snapshot"));
    const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/ResourcePresentation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const TCHAR* Paths[] = {GatePath, PlatePath, PlatformPath, KeyPath, SocketPath, SupplyPath, EncounterPath, ProjectilePath};
    UClass* Parents[] = {AmultiplayerCoopGate::StaticClass(), AmultiplayerPressurePlate::StaticClass(), AmultiplayerMovingPlatform::StaticClass(),
        AmultiplayerCoopKey::StaticClass(), AmultiplayerKeySocket::StaticClass(), AmultiplayerCombatSupply::StaticClass(),
        AmultiplayerCombatEncounter::StaticClass(), AmultiplayerCombatProjectile::StaticClass()};
    const TCHAR* Roles[] = {TEXT("Gate"), TEXT("Plate"), TEXT("Platform"), TEXT("Key"), TEXT("Socket"), TEXT("Supply"), TEXT("Encounter"), TEXT("Projectile")};
    TMap<FString, AActor*> OriginalSources;
    if (!bReadOnly)
    {
        // The active editor context already owns this world. Do not keep old worlds alive across the next LoadMap.
        UWorld* Original = UEditorLoadingAndSavingUtils::LoadMap(TEXT("/Game/Combat/Maps/CoopEscape")); if (!Original) return false;
        const auto& Records = Snapshot->GetObjectField(TEXT("maps"))->GetArrayField(TEXT("/Game/Combat/Maps/CoopEscape"));
        for (const auto& Item : Records)
        {
            const auto Record = Item->AsObject(); const FString Owner = Record->GetStringField(TEXT("role"));
            if ((Owner != TEXT("Supply") && Owner != TEXT("Encounter")) || OriginalSources.Contains(Owner)) continue;
            for (TActorIterator<AActor> It(Original); It; ++It)
                if (It->GetActorLabel() == Record->GetStringField(TEXT("label")))
                {
                    if (!RestoreAppearance(*It, Record, false)) return false;
                    OriginalSources.Add(Owner, *It); break;
                }
        }
        if (!OriginalSources.Contains(TEXT("Supply")) || !OriginalSources.Contains(TEXT("Encounter"))) return Fail(TEXT("Production supply/encounter source missing"));
    }
    TMap<FString, UClass*> Classes;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Paths); ++Index)
    {
        if (!bReadOnly && !BackupPackage(Paths[Index], false, Backup)) return false;
        UBlueprint* Asset = Blueprint(Paths[Index], Parents[Index], bReadOnly, OriginalSources.FindRef(Roles[Index])); if (!Asset) return Fail(Paths[Index]);
        auto* Defaults = Cast<AActor>(Asset->GeneratedClass->GetDefaultObject()); if (!Defaults) return false;
        if (!bReadOnly) { Asset->Modify(); Defaults->Modify(); }
        const auto Blueprints = Snapshot->GetObjectField(TEXT("blueprints"));
        const TSharedPtr<FJsonObject>* Authored = nullptr;
        TSharedPtr<FJsonObject> Record = Blueprints->TryGetObjectField(Paths[Index], Authored) ? *Authored
            : Snapshot->GetObjectField(TEXT("native_defaults"))->GetObjectField(Roles[Index]);
        if (Index == 5) Record = SupplyDefaults(Snapshot);
        if (!RestoreAppearance(Defaults, Record, bReadOnly)) return false;
        if (Index == 3 && !(bReadOnly ? ValidateKeyGraph(Asset) : GenerateKeyGraph(Asset))) return false;
        if (!bReadOnly && (!Compile(Asset) || !SaveAsset(Asset))) return false;
        if (Index == 3 && !ValidateKeyGraph(Asset)) return false;
        Classes.Add(Roles[Index], Asset->GeneratedClass);
        UE_LOG(LogTemp, Display, TEXT("RESOURCE_BLUEPRINT PASS asset=%s role=%s readonly=%d"), Paths[Index], Roles[Index], bReadOnly);
    }
    for (const TCHAR* ConfigPath : {TEXT("/Game/Combat/DA_CombatConfig"), TEXT("/Game/Combat/DA_EnemyCombatConfig"), TEXT("/Game/Combat/Escape/DA_EscapeCombat")})
    {
        auto* Config = Hold(LoadObject<UmultiplayerCombatConfig>(nullptr, *ObjectPath(ConfigPath)));
        if (!Config || (!bReadOnly && !BackupPackage(ConfigPath, false, Backup))
            || !ConfigureFeedback(*Config, Classes.FindChecked(TEXT("Projectile")), bReadOnly) || (!bReadOnly && !SaveAsset(Config))) return false;
    }
    if (!MigrateResourceMaps(Snapshot->GetObjectField(TEXT("maps")), Classes, bReadOnly, Backup)) return false;
    UE_LOG(LogTemp, Display, TEXT("RESOURCE_PRESENTATION PASS readonly=%d blueprints=8 configs=3 backup=%s"), bReadOnly, *Backup);
    return true;
}
}
