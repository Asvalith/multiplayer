#include "Testing/CoopEscapeBuildCommandlet.h"
#if WITH_EDITOR
#include "Combat/multiplayerCombatEncounter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ActorFactories/ActorFactory.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CapsuleComponent.h"
#include "Components/PanelWidget.h"
#include "Mechanisms/multiplayerCoopGate.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerWinArea.h"
#include "Builders/CubeBuilder.h"
#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/BrushComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "StaticMeshCompiler.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogCoopEscapeBuild, Log, All);
UCoopEscapeBuildCommandlet::UCoopEscapeBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

#if WITH_EDITOR
namespace CoopEscapeAssets
{
	const FString Folder = TEXT("/Game/Combat/Escape");
	// 构建器只保存新关卡专属的几个副本；不写第三方原素材和旧 UI。
	bool Save(UObject* Object)
	{
		if (!Object || !Object->GetPackage()->GetName().StartsWith(Folder)) return false;
		Object->MarkPackageDirty();
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Object->GetPackage(), Object,
			*FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}
	template<class T> T* Copy(const TCHAR* Source, const TCHAR* Name)
	{
		const FString Package = Folder / Name;
		if (T* Existing = LoadObject<T>(nullptr, *(Package + TEXT(".") + Name))) return Existing;
		auto* Original = LoadObject<T>(nullptr, Source);
		if (!Original) return nullptr;
		return Cast<T>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().DuplicateAsset(Name, Folder, Original));
	}
	void ObjectProperty(UObject* Object, const TCHAR* Name, UObject* Value)
	{
		auto* Property = FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name); check(Property);
		Property->SetObjectPropertyValue_InContainer(Object, Value);
	}
	void BoolProperty(UObject* Object, const TCHAR* Name, bool Value)
	{
		auto* Property = FindFProperty<FBoolProperty>(Object->GetClass(), Name); check(Property);
		Property->SetPropertyValue_InContainer(Object, Value);
	}
	AActor* Spawn(UWorld* World, UClass* Class, const TCHAR* Name, FVector Location, float Yaw = 0.f)
	{
		FActorSpawnParameters Params; Params.Name = Name;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Actor = World->SpawnActor<AActor>(Class, Location, FRotator(0,Yaw,0), Params);
		if (Actor) Actor->SetActorLabel(Name);
		return Actor;
	}
	ATextRenderActor* Sign(UWorld* World, const TCHAR* Name, FVector Location, const FString& Text, FColor Color = FColor::White, float Size = 30.f, UMaterialInterface* TextMaterial = nullptr)
	{
		auto* Actor = CastChecked<ATextRenderActor>(Spawn(World, ATextRenderActor::StaticClass(), Name, Location, 90.f));
		auto* Component = Actor->GetTextRender();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetText(FText::FromString(Text));
		Component->SetTextRenderColor(Color);
		if (TextMaterial) Component->SetTextMaterial(TextMaterial);
		Component->SetWorldSize(Size);
		Component->SetHorizontalAlignment(EHTA_Center);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return Actor;
	}
	void Wall(UWorld* World, const TCHAR* Name, FVector Center, FVector Size, UMaterialInterface* Material)
	{
		auto* Actor = CastChecked<AStaticMeshActor>(Spawn(World, AStaticMeshActor::StaticClass(), Name, Center));
		auto* Mesh = Actor->GetStaticMeshComponent();
		Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Mesh->SetWorldScale3D(Size / 100.f);
		Mesh->SetCollisionProfileName(TEXT("BlockAll"));
		if (Material) Mesh->SetMaterial(0, Material);
	}
	// 简单的项目自有补给外观：绿色盒体和白色十字，不修改素材包或使用默认棋盘材质。
	UMaterial* SupplyMaterial(const TCHAR* Name, FLinearColor Color)
	{
		const FString Package = Folder / Name;
		if (auto* Existing = LoadObject<UMaterial>(nullptr, *(Package + TEXT(".") + Name))) return Existing;
		auto* Material = NewObject<UMaterial>(CreatePackage(*Package), Name, RF_Public | RF_Standalone);
		auto* Tint = NewObject<UMaterialExpressionConstant3Vector>(Material);
		Tint->Constant = Color;
		Material->GetExpressionCollection().AddExpression(Tint);
		Material->GetEditorOnlyData()->BaseColor.Expression = Tint;
		auto* Roughness = NewObject<UMaterialExpressionConstant>(Material);
		Roughness->R = .8f;
		Material->GetExpressionCollection().AddExpression(Roughness);
		Material->GetEditorOnlyData()->Roughness.Expression = Roughness;
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		return Save(Material) ? Material : nullptr;
	}
	void SupplyCross(AActor* Supply, UMaterialInterface* Material)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			auto* Mesh = NewObject<UStaticMeshComponent>(Supply, Index ? TEXT("SupplyCrossVertical") : TEXT("SupplyCrossHorizontal"));
			Supply->AddInstanceComponent(Mesh);
			Mesh->SetupAttachment(Supply->GetRootComponent());
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
			Mesh->SetMaterial(0, Material);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetCastShadow(false);
			Mesh->SetRelativeLocation(FVector(0, 18.f, 0));
			Mesh->SetRelativeScale3D(Index ? FVector(.06f,.01f,.27f) : FVector(.28f,.01f,.06f));
			Mesh->RegisterComponent();
		}
	}
	FVector Ground(UWorld* World, FVector Point, float Offset)
	{
		FHitResult Hit;
		// 该入口有高处屋顶；从游玩层上方取地面，不能把地面触发器吸到一千厘米高的屋顶。
		const bool bHit = World->LineTraceSingleByChannel(Hit,FVector(Point.X,Point.Y,800),FVector(Point.X,Point.Y,-100),ECC_WorldStatic);
		checkf(bHit,TEXT("Escape placement needs a real collision surface at %s"),*Point.ToCompactString());
		Point.Z=Hit.ImpactPoint.Z+Offset;
		UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_PLACEMENT position=%s floor=%.1f actor=%s"),*Point.ToCompactString(),Hit.ImpactPoint.Z,*GetNameSafe(Hit.GetActor()));
		return Point;
	}
}
#endif

int32 UCoopEscapeBuildCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	const FString Source = TEXT("/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo");
	const FString Target = TEXT("/Game/Combat/Maps/CoopEscape");
	const bool bAudit = FParse::Param(*Params, TEXT("Audit"));
	const bool bAuditSaved = FParse::Param(*Params, TEXT("AuditSaved"));
	if (!bAudit && !bAuditSaved && !FParse::Param(*Params, TEXT("Create"))) return 1;
	if (!FEditorFileUtils::LoadMap(FPackageName::LongPackageNameToFilename(bAuditSaved ? Target : Source, FPackageName::GetMapPackageExtension()), false, false)) return 1;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return 1;
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	World->UpdateWorldComponents(true, false);
	if (bAuditSaved)
	{
		auto* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant_C"));
		if (!PlayerClass) return 1;
		const auto* Capsule = PlayerClass->GetDefaultObject<AmultiplayerCombatCharacter>()->GetCapsuleComponent();
		const auto* Movement = PlayerClass->GetDefaultObject<AmultiplayerCombatCharacter>()->GetCharacterMovement();
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			auto* Mesh = It->GetStaticMeshComponent();
			if (!Mesh->IsPhysicsStateCreated()) Mesh->RecreatePhysicsState();
			if (It->GetName()==TEXT("SM_wood_box_563"))
				UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_BOX_NAV actor=%s canAffect=%d relevant=%d physics=%d bounds=%s"),
					*It->GetName(),Mesh->CanEverAffectNavigation(),Mesh->IsNavigationRelevant(),Mesh->IsPhysicsStateCreated(),*Mesh->Bounds.GetBox().ToString());
		}
		if (const auto* Navigation = UNavigationSystemV1::GetCurrent(World))
			if (const auto* Data = Navigation->GetDefaultNavDataInstance())
				UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_NAV_CONFIG radius=%.1f height=%.1f step=%.1f"),
					Data->GetConfig().AgentRadius,Data->GetConfig().AgentHeight,Data->GetConfig().AgentStepHeight);
		UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_MOVEMENT_CDO walkableAngle=%.2f walkableZ=%.3f maxStep=%.1f maxSpeed=%.1f"),
			Movement->GetWalkableFloorAngle(),Movement->GetWalkableFloorZ(),Movement->MaxStepHeight,Movement->MaxWalkSpeed);
		UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_MOVEMENT_CONSTRAINT enabled=%d normal=%s rvo=%d"),Movement->bConstrainToPlane,
			*Movement->GetPlaneConstraintNormal().ToCompactString(),Movement->bUseRVOAvoidance);
		const FVector Start(-1349.98,-3008.18,197.63);
		for (FVector End : {FVector(-1350,-3070,197.63), FVector(-1350,-3300,197.63), FVector(-1450,-3100,197.63), FVector(-1250,-3100,197.63)})
		{
			FHitResult Hit;
			const bool bHit = World->SweepSingleByProfile(Hit,Start,End,FQuat::Identity,Capsule->GetCollisionProfileName(),
				FCollisionShape::MakeCapsule(Capsule->GetUnscaledCapsuleRadius(),Capsule->GetUnscaledCapsuleHalfHeight()));
			UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_CAPSULE end=%s radius=%.1f halfHeight=%.1f hit=%d actor=%s component=%s location=%s normal=%s penetration=%d depth=%.1f"),
				*End.ToCompactString(),Capsule->GetUnscaledCapsuleRadius(),Capsule->GetUnscaledCapsuleHalfHeight(),bHit,
				*GetNameSafe(Hit.GetActor()),*GetNameSafe(Hit.GetComponent()),*Hit.Location.ToCompactString(),*Hit.ImpactNormal.ToCompactString(),Hit.bStartPenetrating,Hit.PenetrationDepth);
			if (Hit.GetComponent())
			{
				const auto& Slope = Hit.GetComponent()->GetWalkableSlopeOverride();
				UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_COLLISION_WALKABLE result=%d componentSlopeBehavior=%d componentSlopeAngle=%.1f"),
					Movement->IsWalkable(Hit),int32(Slope.GetWalkableSlopeBehavior()),Slope.GetWalkableSlopeAngle());
			}
			// 首次横扫会先碰到正常坡面；仅在只读诊断中排除它，继续查前方是否还有被首个 Hit 遮住的阻挡。
			if (Hit.GetActor() && Hit.GetActor()->GetName().StartsWith(TEXT("Landscape")))
			{
				FCollisionQueryParams Query;
				Query.AddIgnoredActor(Hit.GetActor());
				FHitResult Obstacle;
				const bool bObstacle = World->SweepSingleByProfile(Obstacle,Start,End,FQuat::Identity,Capsule->GetCollisionProfileName(),
					FCollisionShape::MakeCapsule(Capsule->GetUnscaledCapsuleRadius(),Capsule->GetUnscaledCapsuleHalfHeight()),Query);
				UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_OBSTACLE end=%s hit=%d actor=%s component=%s location=%s normal=%s penetration=%d depth=%.1f"),
					*End.ToCompactString(),bObstacle,*GetNameSafe(Obstacle.GetActor()),*GetNameSafe(Obstacle.GetComponent()),
					*Obstacle.Location.ToCompactString(),*Obstacle.ImpactNormal.ToCompactString(),Obstacle.bStartPenetrating,Obstacle.PenetrationDepth);
			}
		}
		for (FVector Point : {FVector(-1350,-2900,0),FVector(-1350,-3008,0),FVector(-1350,-3100,0),FVector(-1350,-3300,0),FVector(-2350,-3500,0),FVector(-1850,-3500,0),FVector(-2100,-4170,0)})
		{
			FHitResult Hit;
			const bool bHit = World->LineTraceSingleByChannel(Hit,Point+FVector(0,0,800),Point-FVector(0,0,100),ECC_WorldStatic);
			UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_SAVED_GROUND point=%s hit=%d z=%.1f actor=%s component=%s"),*Point.ToCompactString(),bHit,Hit.ImpactPoint.Z,*GetNameSafe(Hit.GetActor()),*GetNameSafe(Hit.GetComponent()));
		}
		return 0;
	}
	// 先输出真实碰撞地形高度，构建位置以地面而非视图里的装饰顶面为准。
	if (bAudit)
	{
		for (float Y : {-2200.f, -2600.f, -3000.f, -3400.f, -3800.f, -4200.f})
			for (float X : {-3000.f, -2600.f, -2200.f, -1800.f, -1400.f, -1000.f})
			{
				FHitResult Hit;
				const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(X,Y,700), FVector(X,Y,-300), ECC_WorldStatic);
				UE_LOG(LogCoopEscapeBuild, Display, TEXT("ESCAPE_GROUND x=%.0f y=%.0f hit=%d z=%.1f actor=%s"), X,Y,bHit,Hit.ImpactPoint.Z,*GetNameSafe(Hit.GetActor()));
			}
		return 0;
	}
	// Save-as creates a separate package before any edits. Existing original level remains untouched on disk.
	if (!FPackageName::DoesPackageExist(Target)
		&& !FEditorFileUtils::SaveMap(World, FPackageName::LongPackageNameToFilename(Target, FPackageName::GetMapPackageExtension()))) return 1;
	// SaveMap 另存时复制 World，不会把传入指针改成副本；明确重新加载副本，后续只编辑该包。
	if (!FEditorFileUtils::LoadMap(FPackageName::LongPackageNameToFilename(Target,FPackageName::GetMapPackageExtension()),false,false)) return 1;
	World=GEditor->GetEditorWorldContext().World();
	if (!World || World->GetPackage()->GetName()!=Target) return 1;
	using namespace CoopEscapeAssets;
	// 固定路牌只从正面显示；避免背面的镜像文字覆盖人物与 HUD。
	auto* WorldText=Copy<UMaterial>(TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque.DefaultTextMaterialOpaque"),TEXT("M_EscapeWorldText"));
	if (!WorldText) return 1;
	if (WorldText->TwoSided) { WorldText->TwoSided=false; WorldText->PostEditChange(); }
	if (!Save(WorldText)) return 1;
	// 专属副本启用已经过边界回归的护盾/持续伤害/禁锢；旧测试配置不变，三端加载同一份资产。
	auto* Config = Copy<UmultiplayerCombatConfig>(TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig"), TEXT("DA_EscapeCombat"));
	// AttributeSet 是角色默认子对象，完整复制旧 BP 的重实例会丢字段引用；关卡变体只继承现有表现蓝图。
	auto* Player = LoadObject<UBlueprint>(nullptr,TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant"));
	if (!Player)
	{
		auto* Parent = LoadClass<AmultiplayerCombatCharacter>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
		if (!Parent) return 1;
		Player=FKismetEditorUtilities::CreateBlueprint(Parent,CreatePackage(TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant")),
			TEXT("BP_EscapePlayerVariant"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass(),TEXT("CoopEscapeBuild"));
		FAssetRegistryModule::AssetCreated(Player);
	}
	auto* Mode = Copy<UBlueprint>(TEXT("/Game/Combat/BP_CombatGameMode.BP_CombatGameMode"), TEXT("BP_EscapeGameMode"));
	// 旧 HUD 为纯蓝图 AHUD，CreateWidget 的 Class 固定在图中；新副本用现成原生生命周期壳，控件布局仍完整复制。
	auto* HUD = LoadObject<UBlueprint>(nullptr,TEXT("/Game/Combat/Escape/BP_EscapeHUD.BP_EscapeHUD"));
	if (!HUD)
	{
		HUD = FKismetEditorUtilities::CreateBlueprint(AmultiplayerCombatHUD::StaticClass(),CreatePackage(TEXT("/Game/Combat/Escape/BP_EscapeHUD")),
			TEXT("BP_EscapeHUD"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass(),TEXT("CoopEscapeBuild"));
		FAssetRegistryModule::AssetCreated(HUD);
	}
	auto* Widget = Copy<UWidgetBlueprint>(TEXT("/Game/Combat/UI/WBP_CombatHUD.WBP_CombatHUD"), TEXT("WBP_EscapeHUD"));
	auto* Visuals = Copy<UWidgetBlueprint>(TEXT("/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals.WBP_ARPGCombatVisuals"),TEXT("WBP_EscapeCombatVisuals"));
	if (!Config || !Player || !Mode || !HUD || !Widget || !Widget->WidgetTree || !Visuals || !Visuals->WidgetTree) return 1;
	Config->DefenseShieldAmount = 35.f; Config->BurningDamagePerTick = 3.f; Config->AreaRootDuration = 2.f;
	if (!Save(Config)) return 1;
	// 仅在新 HUD 副本折叠旧开发诊断框，保留原引用与下方全部技能/生命/能量界面。
	if (auto* Panel = Widget->WidgetTree->FindWidget(TEXT("CombatPanel"))) Panel->SetVisibility(ESlateVisibility::Collapsed);
	if (auto* Status = Cast<UTextBlock>(Visuals->WidgetTree->FindWidget(TEXT("StateText"))))
	{
		Status->SetAutoWrapText(true);
		if (auto* Slot = Cast<UCanvasPanelSlot>(Status->Slot))
		{
			Slot->SetAnchors(FAnchors(.5f,0.f)); Slot->SetAlignment(FVector2D(.5f,0.f));
			Slot->SetPosition(FVector2D(0,24)); Slot->SetSize(FVector2D(1050,120));
		}
	}
	for (int32 Index=0;Index<4;++Index)
	{
		for (bool bKey : {false,true})
			{
				const FString Name=FString(bKey ? TEXT("SkillKey") : TEXT("SkillLabel"))+FString::FromInt(Index);
				if (auto* Text=Cast<UTextBlock>(Visuals->WidgetTree->FindWidget(*Name)))
				{
					auto Font=Text->GetFont(); Font.Size=bKey ? 16 : 18; Text->SetFont(Font);
				}
			}
	}
	FKismetEditorUtilities::CompileBlueprint(Visuals);
	if (!Save(Visuals)) return 1;
	if (auto* Old = Widget->WidgetTree->FindWidget(TEXT("ARPGCombatVisuals")); Old && Old->GetClass()!=Visuals->GeneratedClass)
	{
		auto* Parent = Old->GetParent();
		if (!Parent) return 1;
		Old->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
		auto* Replacement = Widget->WidgetTree->ConstructWidget<UUserWidget>(Visuals->GeneratedClass.Get(),TEXT("ARPGCombatVisuals"));
		Replacement->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (!Parent->ReplaceChild(Old,Replacement)) return 1;
		Old->Slot=nullptr;
	}
	FKismetEditorUtilities::CompileBlueprint(Widget);
	if (!Save(Widget)) return 1;
	ObjectProperty(HUD->GeneratedClass->GetDefaultObject(), TEXT("CombatWidgetClass"), Widget->GeneratedClass.Get());
	FKismetEditorUtilities::CompileBlueprint(HUD);
	if (!Save(HUD)) return 1;
	ObjectProperty(Player->GeneratedClass->GetDefaultObject(), TEXT("CombatConfig"), Config);
	FKismetEditorUtilities::CompileBlueprint(Player);
	const auto* Attributes = FindFProperty<FObjectPropertyBase>(Player->GeneratedClass,TEXT("CombatAttributes"));
	auto* PlayerDefaults = Player->GeneratedClass->GetDefaultObject();
	if (!Attributes || !Attributes->GetObjectPropertyValue_InContainer(PlayerDefaults)
		|| Attributes->GetObjectPropertyValue_InContainer(PlayerDefaults)!=PlayerDefaults->GetDefaultSubobjectByName(TEXT("CombatAttributes")))
	{
		UE_LOG(LogCoopEscapeBuild,Error,TEXT("Escape player must retain its own native AttributeSet reference after compile")); return 1;
	}
	if (!Save(Player)) return 1;
	ObjectProperty(Mode->GeneratedClass->GetDefaultObject(), TEXT("DefaultPawnClass"), Player->GeneratedClass.Get());
	ObjectProperty(Mode->GeneratedClass->GetDefaultObject(), TEXT("HUDClass"), HUD->GeneratedClass.Get());
	FKismetEditorUtilities::CompileBlueprint(Mode);
	if (!Save(Mode)) return 1;
	World->GetWorldSettings()->DefaultGameMode = Mode->GeneratedClass;
	// 原机关原位留在原地图；新副本将交互集中到出生庭院，避免第一次游玩先寻找数层高的测试钥匙。
	TArray<AActor*> Remove;
	for (TActorIterator<AActor> It(World); It; ++It)
		if (It->IsA<AmultiplayerCoopKey>() || It->IsA<AmultiplayerKeySocket>() || It->IsA<AmultiplayerPressurePlate>()
			|| It->IsA<AmultiplayerCoopGate>() || It->IsA<AmultiplayerMovingPlatform>() || It->IsA<AmultiplayerWinArea>()
			|| It->IsA<APlayerStart>() || It->IsA<AmultiplayerEnemySpawner>() || It->IsA<ANavMeshBoundsVolume>()
			|| It->GetName().StartsWith(TEXT("Escape_"))) Remove.Add(*It);
	for (auto* Actor : Remove)
	{
		World->EditorDestroyActor(Actor,true);
		// 重复显式构建时释放旧实例名，避免新对象自动带后缀而失去关卡的稳定标识。
		Actor->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
	}
	Spawn(World, APlayerStart::StaticClass(), TEXT("Escape_PlayerStart_1"), FVector(-2250,-2500,210), -90);
	Spawn(World, APlayerStart::StaticClass(), TEXT("Escape_PlayerStart_2"), FVector(-1950,-2500,210), -90);
	auto* KeyClass = LoadClass<AmultiplayerCoopKey>(nullptr, TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpkey.bpkey_C"));
	auto* SocketClass = LoadClass<AmultiplayerKeySocket>(nullptr, TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/bpkeysocket.bpkeysocket_C"));
	auto* PlateClass = LoadClass<AmultiplayerPressurePlate>(nullptr, TEXT("/Game/ThirdPerson/Blueprints/gameplayelements/pressplate.pressplate_C"));
	if (!KeyClass || !SocketClass || !PlateClass) return 1;
	FVector Keys[] = {{-3000,-2800,180}, {-1200,-2800,180}, {-2750,-3100,200}, {-1350,-3300,200}};
	for (FVector& Point : Keys) Point=Ground(World,Point,80.f);
	for (int32 Index=0; Index<4; ++Index)
	{
		auto* Socket = Spawn(World, SocketClass, *FString::Printf(TEXT("Escape_Socket_%d"),Index+1), Ground(World,FVector(-2400+Index*200,-3660,0),20.f));
		auto* Key = Spawn(World, KeyClass, *FString::Printf(TEXT("Escape_Key_%d"),Index+1), Keys[Index]);
		ObjectProperty(Key, TEXT("DestinationSocket"), Socket);
		auto* Label = Sign(World, *FString::Printf(TEXT("Escape_KeySign_%d"), Index+1), Keys[Index]+FVector(0,0,160), FString::Printf(TEXT("KEY %d"),Index+1), FColor(255,215,80), 26,WorldText);
		// 标识随钥匙安装到终点，不在原拾取点留下仍可领取的误导提示。
		Label->AttachToActor(Key,FAttachmentTransformRules::KeepWorldTransform);
	}
	auto* Plate1 = Spawn(World, PlateClass, TEXT("Escape_Plate_1"), Ground(World,FVector(-2350,-3500,0),15.f));
	auto* Plate2 = Spawn(World, PlateClass, TEXT("Escape_Plate_2"), Ground(World,FVector(-1850,-3500,0),15.f));
	auto* Gate = Spawn(World, AmultiplayerCoopGate::StaticClass(), TEXT("Escape_Gate"), Ground(World,FVector(-2100,-3870,0),0.f));
	const FArrayProperty* Plates = FindFProperty<FArrayProperty>(Gate->GetClass(), TEXT("RequiredPlates")); check(Plates);
	FScriptArrayHelper PlateValues(Plates, Plates->ContainerPtrToValuePtr<void>(Gate)); PlateValues.Resize(2);
	CastFieldChecked<FObjectPropertyBase>(Plates->Inner)->SetObjectPropertyValue(PlateValues.GetRawPtr(0),Plate1);
	CastFieldChecked<FObjectPropertyBase>(Plates->Inner)->SetObjectPropertyValue(PlateValues.GetRawPtr(1),Plate2);
	FindFProperty<FIntProperty>(Gate->GetClass(), TEXT("RequiredActivePlateCount"))->SetPropertyValue_InContainer(Gate,2);
	BoolProperty(Gate,TEXT("bRequireObjectiveComplete"),true); BoolProperty(Gate,TEXT("bStayOpenOnceActivated"),true);
	auto* Door = CastChecked<UStaticMeshComponent>(Gate->GetDefaultSubobjectByName(TEXT("DoorMesh")));
	Door->SetRelativeLocation(FVector(0,0,200)); Door->SetRelativeScale3D(FVector(7,.3f,4)); Door->SetCanEverAffectNavigation(false);
	CastChecked<UArrowComponent>(Gate->GetDefaultSubobjectByName(TEXT("ClosedPoint")))->SetRelativeLocation(FVector(0,0,200));
	CastChecked<UArrowComponent>(Gate->GetDefaultSubobjectByName(TEXT("OpenPoint")))->SetRelativeLocation(FVector(0,0,650));
	auto* Stone = LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Stylized_Egypt/Meshes/building/SM_wall_01.SM_wall_01"));
	auto* StoneMaterial = Stone ? Stone->GetMaterial(0) : nullptr;
	if (StoneMaterial) Door->SetMaterial(0,StoneMaterial);
	Wall(World,TEXT("Escape_ExitLeft"),FVector(-2530,-4070,550),FVector(100,500,500),StoneMaterial);
	Wall(World,TEXT("Escape_ExitRight"),FVector(-1670,-4070,550),FVector(100,500,500),StoneMaterial);
	Wall(World,TEXT("Escape_ExitBack"),FVector(-2100,-4340,550),FVector(960,80,500),StoneMaterial);
	auto* Exit = Spawn(World,AmultiplayerWinArea::StaticClass(),TEXT("Escape_Exit"),Ground(World,FVector(-2100,-4170,0),100.f));
	Exit->FindComponentByClass<UBoxComponent>()->SetBoxExtent(FVector(310,145,180));
	Sign(World,TEXT("Escape_GateSign"),FVector(-2100,-3770,740),TEXT("4 KEYS  >  TWO PLATES  >  EXIT"),FColor(130,255,150),29,WorldText);
	Sign(World,TEXT("Escape_StartSign"),FVector(-2100,-2760,400),TEXT("SURVIVE TOGETHER\nCOLLECT FOUR KEYS AND ESCAPE"),FColor(255,235,170),27,WorldText);
	Sign(World,TEXT("Escape_ExitSign"),FVector(-2100,-4250,500),TEXT("EXIT\nBOTH PLAYERS"),FColor(130,255,150),32,WorldText);
	auto* SupplyGreen = SupplyMaterial(TEXT("M_EscapeSupplyGreen"), FLinearColor(.025f,.28f,.065f));
	auto* SupplyWhite = SupplyMaterial(TEXT("M_EscapeSupplyWhite"), FLinearColor(.9f,.95f,.9f));
	if (!SupplyGreen || !SupplyWhite) return 1;
	for (int32 Index=0; Index<3; ++Index)
	{
		const FVector Point=Ground(World,FVector(-2900+Index*800,Index==2 ? -2850 : -3100,0),20.f);
		auto* Supply = Spawn(World,AmultiplayerCombatSupply::StaticClass(),*FString::Printf(TEXT("Escape_Supply_%d"),Index+1),Point);
		auto* Visual = CastChecked<UStaticMeshComponent>(Supply->GetDefaultSubobjectByName(TEXT("SupplyVisual")));
		Visual->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
		Visual->SetRelativeScale3D(FVector(.5f,.35f,.35f));
		Visual->SetMaterial(0,SupplyGreen);
		SupplyCross(Supply,SupplyWhite);
		if (auto* Label=Supply->FindComponentByClass<UTextRenderComponent>()) Label->SetTextMaterial(WorldText);
	}
	// 移动平台保留原生 Occupancy/Transporter：作为侧面高点，不让基础撤离必须掌握跳台时序。
	auto* Platform = Spawn(World,AmultiplayerMovingPlatform::StaticClass(),TEXT("Escape_Platform"),FVector(-1000,-3000,130));
	CastChecked<UArrowComponent>(Platform->GetDefaultSubobjectByName(TEXT("TargetPoint")))->SetRelativeLocation(FVector(0,0,350));
	Spawn(World,AmultiplayerCombatEncounter::StaticClass(),TEXT("Escape_Encounter"),FVector(-2100,-2900,200));
	// 保存实际 Recast 数据，并验证出生区到每把钥匙、压力板均有可行走路径。
	auto* Volume = CastChecked<ANavMeshBoundsVolume>(Spawn(World,ANavMeshBoundsVolume::StaticClass(),TEXT("Escape_Navigation"),FVector(-2100,-3300,800)));
	auto* Builder = NewObject<UCubeBuilder>(); Builder->X=5000; Builder->Y=5000; Builder->Z=2200;
	UActorFactory::CreateBrushForVolumeActor(Volume,Builder);
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	World->UpdateWorldComponents(true,false);
	FNavigationSystem::AddNavigationSystemToWorld(*World,FNavigationSystemRunMode::EditorMode);
	auto* Navigation = UNavigationSystemV1::GetCurrent(World);
	if (!Navigation) return 1;
	int32 BlockingDecor = 0;
	// 只在新关卡导出真实阻挡玩家的静态装饰；门和平台保留各自有意设置的动态导航策略。
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		auto* Mesh = It->GetStaticMeshComponent();
		if (Mesh->Mobility != EComponentMobility::Static || Mesh->GetCollisionEnabled()==ECollisionEnabled::NoCollision
			|| Mesh->GetCollisionResponseToChannel(ECC_Pawn)!=ECR_Block) continue;
		Mesh->RecreatePhysicsState();
		Mesh->SetCanEverAffectNavigation(true);
		FNavigationSystem::UpdateComponentData(*Mesh);
		++BlockingDecor;
	}
	Navigation->OnNavigationBoundsUpdated(Volume);
	Navigation->Tick(0.f);
	Navigation->Build();
	UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_NAV_READY blockingDecor=%d staticMeshCompilationComplete=1"),BlockingDecor);
	int32 Paths=0;
	for (FVector Point : Keys)
	{
		FNavLocation Projected;
		const bool bProjected = Navigation->ProjectPointToNavigation(Point,Projected,FVector(150,150,350));
		auto* Path = bProjected ? UNavigationSystemV1::FindPathToLocationSynchronously(World,FVector(-2100,-2500,150),Projected.Location) : nullptr;
		const bool bValid = Path && Path->IsValid() && !Path->IsPartial(); Paths += bValid;
		UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_KEY_PATH destination=%s projected=%d valid=%d"),*Point.ToString(),bProjected,bValid);
	}
	FNavLocation FourthKey;
	if (Navigation->ProjectPointToNavigation(Keys[3],FourthKey,FVector(150,150,350)))
		if (auto* Path=UNavigationSystemV1::FindPathToLocationSynchronously(World,Keys[1],FourthKey.Location))
		{
			FString Points;
			for (const auto& Point : Path->PathPoints) Points+=TEXT(" ")+Point.ToCompactString();
			UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_KEY4_BOX_ROUTE points=%d partial=%d path=%s"),Path->PathPoints.Num(),Path->IsPartial(),*Points);
		}
	if (!FEditorFileUtils::SaveLevel(World->PersistentLevel)) return 1;
	UE_LOG(LogCoopEscapeBuild,Display,TEXT("ESCAPE_MAP_CREATED map=%s keys=4 plates=2 supplies=3 waves=3 paths=%d/4 originalMap=preserved originalHUD=preserved"),*Target,Paths);
	return Paths==4 ? 0 : 1;
#else
	return 1;
#endif
}
