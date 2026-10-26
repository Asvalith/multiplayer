#include "Testing/CoopNetTestDriver.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StreamableManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Animation/AnimMontage.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

// 共用技能视觉夹具与截图支持；只核对正式资产、状态与保存画面，不代替完整地图通关。

// 等本轮唯一 token 目录内实际 PNG 落盘才确认截图；无渲染时明确记录缺少视觉证据。
bool UCoopNetTestDriver::CaptureEscapeView(const FString& Name)
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(),TEXT("NullRHI")) || !FApp::CanEverRender())
	{
		if (!SentReceipts.Contains(TEXT("EscapeNonVisual")))
		{
			SentReceipts.Add(TEXT("EscapeNonVisual"));
			Emit(TEXT("NON_VISUAL"),TEXT("No rendered screenshot evidence; all physical gameplay and replicated-state assertions remain required"));
		}
		return true;
	}
	const FString SavedMarker = TEXT("ScreenshotSaved:") + Name;
	if (SentReceipts.Contains(SavedMarker)) return true;
	const double Started = EscapeCaptureWaitStarted.FindOrAdd(Name,FPlatformTime::Seconds());
	if (FPlatformTime::Seconds()-Started > 15.f)
	{
		Assert(TEXT("EscapeScreenshotSaved"),false,TEXT("Current-token screenshot did not produce a nonempty PNG within 15 seconds: ")+Name);
		return false;
	}
	const FString Marker = TEXT("Screenshot:") + Name;
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("CoopEscapeCaptureDir="), Directory) || Directory.IsEmpty())
	{
		Assert(TEXT("EscapeScreenshotDirectory"),false,TEXT("Rendered escape acceptance requires a capture directory"));
		return false;
	}
	// 目录包含本轮唯一 token，重用输出目录时旧 PNG 不能替本轮阶段过关。
	Directory=FPaths::Combine(Directory,Token);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, Name + TEXT(".png"));
	if (SentReceipts.Contains(Marker))
	{
		if (IFileManager::Get().FileSize(*Path) <= 0) return false;
		SentReceipts.Add(SavedMarker);
		Emit(TEXT("SCREENSHOT_SAVED"),Path);
		return true;
	}
	if (!GEngine || !GEngine->GameViewport || FScreenshotRequest::IsScreenshotRequested()) return false;
	FScreenshotRequest::RequestScreenshot(Path, true, false);
	SentReceipts.Add(Marker);
	Emit(TEXT("SCREENSHOT_REQUESTED"), Path);
	return false;
#else
	return false;
#endif
}

// 只校验当前保存的正式地图/角色/HUD/配置与成本开关，不用替代资产制造视觉通过。
bool UCoopNetTestDriver::PrepareVisualFixture(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	const bool bEscapeMap=World->GetMapName().Contains(TEXT("CoopEscape"));
	const TCHAR* PlayerPath=bEscapeMap ? TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant_C")
		: TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C");
	const TCHAR* HUDPath=bEscapeMap ? TEXT("/Game/Combat/Escape/BP_EscapeHUD.BP_EscapeHUD_C")
		: TEXT("/Game/Combat/UI/BP_CombatHUD.BP_CombatHUD_C");
	const TCHAR* ModePath=bEscapeMap ? TEXT("/Game/Combat/Escape/BP_EscapeGameMode.BP_EscapeGameMode_C")
		: TEXT("/Game/Combat/BP_CombatGameMode.BP_CombatGameMode_C");
	const TCHAR* ConfigPath=bEscapeMap ? TEXT("/Game/Combat/Escape/DA_EscapeCombat.DA_EscapeCombat")
		: TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig");
	UClass* PlayerClass=LoadClass<AmultiplayerCombatCharacter>(nullptr,PlayerPath);
	UClass* HUDClass=LoadClass<AmultiplayerCombatHUD>(nullptr,HUDPath);
	UClass* EnemyClass=CombatFixtureEnemyClass();
	auto* FixtureMode=World->GetAuthGameMode();
	const auto* Player=PlayerClass ? PlayerClass->GetDefaultObject<AmultiplayerCombatCharacter>() : nullptr;
	const auto* Rules=Player ? Player->GetCombatConfigAsset() : nullptr;
	const auto* DeathField=Player ? FindFProperty<FObjectPropertyBase>(Player->GetClass(),TEXT("DeathMontage")) : nullptr;
	const auto* Death=DeathField ? Cast<UAnimMontage>(DeathField->GetObjectPropertyValue_InContainer(Player)) : nullptr;
	const bool bReady=FixtureMode && Player && HUDClass && EnemyClass && Rules && Player->GetMesh()->GetAnimClass()
		&& FixtureMode->GetClass()->GetPathName()==ModePath && Rules->GetPathName()==ConfigPath
		&& FixtureMode->DefaultPawnClass.Get()==PlayerClass && FixtureMode->HUDClass.Get()==HUDClass
		&& !FParse::Param(FCommandLine::Get(),TEXT("CoopSkillDemo"))
		&& !UAbilitySystemGlobals::Get().ShouldIgnoreCosts() && !UAbilitySystemGlobals::Get().ShouldIgnoreCooldowns()
		&& Player->GetActionMontage(EMultiplayerCombatAnimation::Attack)
		&& Player->GetActionMontage(EMultiplayerCombatAnimation::Area) && Death && !Death->bEnableAutoBlendOut
		&& Rules->DefenseFeedback.Effect.IsNull() && Rules->DefenseFeedback.CascadeEffect.IsNull()
		&& Rules->HealFeedback.CascadeEffect.IsNull() && Rules->HealFeedback.Effect.ToSoftObjectPath().ToString()
			==TEXT("/Game/Combat/Presentation/Effects/NS_CombatDefenseShield.NS_CombatDefenseShield") && !Rules->AreaMarkerMaterial.IsNull()
		&& Rules->Attack.Amount==25.f && Rules->Heal.Amount==30.f
		&& (bEscapeMap || (Rules->DefenseShieldAmount==0.f && Rules->BurningDamagePerTick==0.f && Rules->AreaRootDuration==0.f));
	Assert(TEXT("ServerVisualFixture"),bReady,TEXT("Controlled combat fixture requires the saved map's formal GameMode/player/config/HUD before login, real costs/cooldowns, no CoopSkillDemo, Heal feedback and particle-free defense; CombatTest retains its legacy numeric rules. This is not original-map mechanism acceptance"));
	if (!bReady) return false;
	// 正式界面的接线也是验收对象，不能换成另一套 HUD 后再将夹具通过当成正式画面通过。
	return true;
#else
	return false;
#endif
}

// 视觉验收使用保存的敌人蓝图，纯数值夹具沿用原生类，避免将两种证据混称。
UClass* UCoopNetTestDriver::CombatFixtureEnemyClass() const
{
#if !UE_BUILD_SHIPPING
	if (bVisualAcceptance) return LoadClass<AmultiplayerCombatEnemy>(nullptr,TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
#endif
	return AmultiplayerCombatEnemy::StaticClass();
}

// 各客户端核对自己实际角色、HUD、动画/特效配置并等待资源编译，不能只查服务器 CDO。
bool UCoopNetTestDriver::ValidateVisualFixturePawn(AmultiplayerCombatCharacter* Character)
{
#if !UE_BUILD_SHIPPING
	if (!bVisualAcceptance) return true;
	const FString Receipt=(bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+TEXT("PresentationReady");
	if (SentReceipts.Contains(Receipt)) return true;
	const auto* Controller=Character ? Cast<APlayerController>(Character->GetController()) : nullptr;
	if (!Controller || !Controller->GetHUD()) return false;
	const bool bEscapeMap=Character->GetWorld()->GetMapName().Contains(TEXT("CoopEscape"));
	const TCHAR* PlayerPath=bEscapeMap ? TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant_C")
		: TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C");
	const TCHAR* HUDPath=bEscapeMap ? TEXT("/Game/Combat/Escape/BP_EscapeHUD.BP_EscapeHUD_C")
		: TEXT("/Game/Combat/UI/BP_CombatHUD.BP_CombatHUD_C");
	const TCHAR* ConfigPath=bEscapeMap ? TEXT("/Game/Combat/Escape/DA_EscapeCombat.DA_EscapeCombat")
		: TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig");
	const auto* Rules=Character->GetCombatConfigAsset();
	const bool bReady=Character->GetClass()->GetPathName()==PlayerPath
		&& Controller->GetHUD()->GetClass()->GetPathName()==HUDPath
		&& Controller->GetHUD()->IsA<AmultiplayerCombatHUD>()
		&& Character->GetMesh()->GetAnimInstance() && Rules && Rules->GetPathName()==ConfigPath
		&& !FParse::Param(FCommandLine::Get(),TEXT("CoopSkillDemo"))
		&& !UAbilitySystemGlobals::Get().ShouldIgnoreCosts() && !UAbilitySystemGlobals::Get().ShouldIgnoreCooldowns()
		&& Rules->DefenseFeedback.Effect.IsNull() && Rules->DefenseFeedback.CascadeEffect.IsNull()
		&& Rules->HealFeedback.CascadeEffect.IsNull() && Rules->HealFeedback.Effect.ToSoftObjectPath().ToString()
			==TEXT("/Game/Combat/Presentation/Effects/NS_CombatDefenseShield.NS_CombatDefenseShield")
		&& Character->GetActionMontage(EMultiplayerCombatAnimation::Attack) && Character->GetActionMontage(EMultiplayerCombatAnimation::Area);
	if (bReady && !bServer && Character->GetNetMode()!=NM_DedicatedServer
		&& FApp::CanEverRender() && !FParse::Param(FCommandLine::Get(),TEXT("NullRHI")))
	{
		const FString Preparing=Receipt+TEXT(":Preparing");
		if (!SentReceipts.Contains(Preparing))
		{
			SentReceipts.Add(Preparing);
			Emit(TEXT("VISUAL_PRESENTATION_PREPARING"),TEXT("Waiting for the actual Heal preload, Niagara VM/GPU compilation and submitted shader jobs; no Cue is played and this does not guarantee first-frame pixels or all PSOs"));
		}
		// 只在验收客户端沿已有 tick 等待角色的异步预载，不预播 Cue、不阻塞生产施法路径。
		if (!UAssetManager::GetStreamableManager().IsAsyncLoadComplete(Rules->HealFeedback.Effect.ToSoftObjectPath())) return false;
		auto* HealEffect=Rules->HealFeedback.Effect.Get();
		if (!HealEffect || (!Rules->HealFeedback.Sound.IsNull() && !Rules->HealFeedback.Sound.IsValid())) return false;
#if WITH_EDITORONLY_DATA
		const FString CompileRequested=Receipt+TEXT(":HealCompileRequested");
		if (!SentReceipts.Contains(CompileRequested))
		{
			SentReceipts.Add(CompileRequested);
			HealEffect->RequestCompile(false);
		}
		HealEffect->PollForCompilationComplete(false);
#endif
#if WITH_EDITOR
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->ProcessAsyncResults(true,false);
			if (GShaderCompilingManager->IsCompiling()) return false;
		}
#endif
#if WITH_EDITORONLY_DATA
		if (HealEffect->HasOutstandingCompilationRequests(true)) return false;
#endif
		if (!HealEffect->IsReadyToRun()) return false;
		if (!HealEffect->IsValid())
		{
			SendReceipt(Receipt,false,TEXT("Actual Heal Niagara compilation finished but the system is invalid"));
			return false;
		}
	}
	SendReceipt(Receipt,bReady,TEXT("Actual admitted owning pawn uses its saved map's formal player/config/HUD and live animation, real costs/cooldowns, Heal feedback and particle-free defense; no CoopSkillDemo or substituted HUD"));
	return bReady;
#else
	return false;
#endif
}

// 专项仍使用本地玩家原相机，只以正常控制朝向把观察对象放进画面；不替换 ViewTarget/FOV。
bool UCoopNetTestDriver::CaptureSkillView(const FString& Name, AActor* Subject, AActor* Other)
{
	if (!bVisualAcceptance) return true;
	// 原状态条件满足后才开始等待，保留首帧的真实数值证据；截图观察稳定动画姿态而非通知到达的第一帧。
	const double SettleSeconds=Name==TEXT("Downed") ? .8 : (Name==TEXT("RescueDone") ? .35 : 0.);
	if (SettleSeconds>0.)
	{
		const FString Key=TEXT("VisualSettle:")+Name;
		if (!EscapeCaptureWaitStarted.Contains(Key))
		{
			auto* Character=Cast<AmultiplayerCombatCharacter>(Subject);
			const bool bDowned=Name==TEXT("Downed");
			const bool bState=Character && Character->IsAlive() && (bDowned
				? Character->GetCoopComponent()->IsDowned() && !Character->IsCombatReady() && Character->GetHealth()==1.f
				: !Character->GetCoopComponent()->IsDowned() && Character->IsCombatReady()
					&& FMath::IsNearlyEqual(Character->GetHealth(),Character->GetMaxHealth()*.3f));
			SendReceipt((bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+Name+TEXT("StateBeforeSettle"),bState,
				TEXT("Original authoritative life/HP conditions observed before waiting solely for the presentation blend"));
			EscapeCaptureWaitStarted.Add(Key,FPlatformTime::Seconds());
		}
		if (FPlatformTime::Seconds()-EscapeCaptureWaitStarted[Key]<SettleSeconds) return false;
	}
	if (UWorld* World=FindWorld()) if (auto* PC=World->GetFirstPlayerController())
		if (auto* Local=PC->GetPawn(); Local && Subject && (Subject!=Local || Other))
		{
			const FVector Focus=Other ? (Subject->GetActorLocation()+Other->GetActorLocation())*.5f : Subject->GetActorLocation();
			if (FVector::DistSquared2D(Focus,Local->GetActorLocation())>1.f)
				PC->SetControlRotation((Focus-Local->GetActorLocation()).Rotation());
		}
	if (auto* World=FindWorld())
		if (!ValidateVisualFixturePawn(Cast<AmultiplayerCombatCharacter>(World->GetFirstPlayerController()->GetPawn()))) return false;
	if (!CaptureEscapeView(TEXT("FIXTURE-")+Mode+TEXT("-")+Name)) return false;
	SendReceipt((bPartner ? FString(TEXT("PartnerVisual")) : FString(TEXT("PrimaryVisual")))+Name,true,
		TEXT("FIXTURE: actual current-token PNG saved; underlying gameplay/state assertions remain independent"));
	return true;
}
