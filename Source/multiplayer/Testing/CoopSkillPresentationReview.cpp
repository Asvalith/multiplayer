// 显式开发模式下录制真实技能输入、双方表现和截图落盘结果。
// 执行证据与视觉通过分开记录，工具不会把组件状态或截图成功当成画面验收。
#include "Testing/CoopSkillPresentationReview.h"

#if !UE_BUILD_SHIPPING
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

namespace
{
	// 一条时间轴动作既可注入按键，也可只检查预览或角色朝向。
	struct FReviewAction
	{
		double At = 0;
		FString Name;
		FKey Key;
		EInputEvent Event = IE_Pressed;
		int32 CheckSkill = -1;
		bool bRotate = false;
		bool bCheckBody = false;
	};

	// 单进程录制会话持有全部订阅和按键，结束时统一释放，避免影响手动操作。
	struct FSkillPresentationReview
	{
		FString Role, Directory, Failure;
		FTSTicker::FDelegateHandle TickHandle;
		FDelegateHandle ScreenshotHandle;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<AmultiplayerCombatCharacter> Player, Other;
		TArray<FReviewAction> Steps;
		TSet<FKey> HeldKeys;
		TArray<TSharedPtr<FJsonValue>> Actions, Observations, Frames, Unsupported, Errors;
		TSharedPtr<FJsonObject> PendingFrame;
		double Started = FPlatformTime::Seconds(), ReadySince = -1, RecordingStarted = -1;
		double LastCapture = -1, LastManifest = -1, Duration = 0;
		int32 NextAction = 0, FrameNumber = 0;
		FRotator GuardBody = FRotator::ZeroRotator;
		FVector ObserverPrepLocation = FVector::ZeroVector;
		double ObserverPrepStarted = -1;
		bool bObserverPrepReleased = false;
		bool bObserverPrepared = false;
		bool bDone = false;

		// 连接准备不计入技能时间轴，正式录制开始前统一返回零。
		double Elapsed() const { return RecordingStarted < 0 ? 0 : FPlatformTime::Seconds() - RecordingStarted; }

		// 汇总原始证据与执行错误；视觉结果固定待人工判断，不自动宣称画面通过。
		void Write(bool bComplete)
		{
			auto Report = MakeShared<FJsonObject>();
			Report->SetStringField(TEXT("role"), Role);
			Report->SetBoolField(TEXT("complete"), bComplete);
			int32 Written = 0;
			for (const auto& Frame : Frames) Written += Frame->AsObject()->GetBoolField(TEXT("written"));
			Report->SetNumberField(TEXT("frames_requested"), Frames.Num());
			Report->SetNumberField(TEXT("frames_written"), Written);
			Report->SetBoolField(TEXT("execution_passed"), bComplete && Failure.IsEmpty() && Errors.IsEmpty() && Written > 0 && Written == Frames.Num());
			Report->SetBoolField(TEXT("visual_passed"), false);
			Report->SetStringField(TEXT("visual_status"), TEXT("Raw viewport evidence only; human visual inspection remains required"));
			Report->SetStringField(TEXT("failure"), Failure);
			Report->SetNumberField(TEXT("recording_duration_seconds"), Elapsed());
			Report->SetNumberField(TEXT("planned_duration_seconds"), Duration);
			Report->SetArrayField(TEXT("actions"), Actions);
			Report->SetArrayField(TEXT("observations"), Observations);
			Report->SetArrayField(TEXT("frames"), Frames);
			Report->SetArrayField(TEXT("unsupported"), Unsupported);
			Report->SetArrayField(TEXT("execution_errors"), Errors);
			FString Json;
			FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
			FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Directory, TEXT("manifest.json")), FFileHelper::EEncodingOptions::ForceUTF8);
			if (bComplete) FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Directory, TEXT("complete.json")), FFileHelper::EEncodingOptions::ForceUTF8);
		}

		// 补发所有已注入按键的松开事件，结束或异常中止都不遗留持续输入。
		void ReleaseKeys()
		{
			if (auto* PC = Controller.Get())
				for (const auto& Key : HeldKeys) PC->InputKey(FInputKeyParams(Key, IE_Released, 0.));
			HeldKeys.Empty();
		}

		// 只完成一次：松键、确认最后截图、解绑回调，再输出最终清单。
		void Finish(const FString& Reason = FString())
		{
			if (bDone) return;
			Failure = Reason;
			ReleaseKeys();
			if (PendingFrame)
			{
				PendingFrame->SetBoolField(TEXT("written"), IFileManager::Get().FileSize(*PendingFrame->GetStringField(TEXT("path"))) > 0);
				PendingFrame.Reset();
			}
			FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
			bDone = true;
			Write(true);
			UE_LOG(LogTemp, Display, TEXT("COOP_SKILL_REVIEW_COMPLETE role=%s errors=%d frames=%d reason=%s visualPassed=NOT_EVALUATED"),
				*Role, Errors.Num(), Frames.Num(), *Reason);
		}

		// 主动停止保留中止原因，并移除会话持有的全局 Ticker 与截图委托。
		void Stop()
		{
			if (!bDone) Finish(TEXT("Review stopped before completion"));
			ReleaseKeys();
			FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotHandle);
			FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		}

		// 采集当前端真正可见的角色状态；观察端不假装可读取拥有者私有能量。
		void Snapshot(const FString& Name)
		{
			const auto* Pawn = Role == TEXT("Observer") ? Other.Get() : Player.Get();
			if (!Pawn) return;
			auto State = MakeShared<FJsonObject>();
			State->SetNumberField(TEXT("timeSeconds"), Elapsed());
			State->SetNumberField(TEXT("processElapsedSeconds"), FPlatformTime::Seconds() - Started);
			State->SetStringField(TEXT("name"), Name);
			State->SetStringField(TEXT("pawn"), Pawn->GetPathName());
			State->SetBoolField(TEXT("preview"), Pawn->IsTargetingSkill());
			State->SetNumberField(TEXT("previewSkill"), int32(Pawn->GetTargetingSkill()));
			State->SetBoolField(TEXT("casting"), Pawn->IsCasting());
			State->SetBoolField(TEXT("defending"), Pawn->IsDefending());
			State->SetNumberField(TEXT("health"), Pawn->GetHealth());
			State->SetBoolField(TEXT("ownerEnergyReadable"), Role == TEXT("Owner"));
			if (Role == TEXT("Owner")) State->SetNumberField(TEXT("energy"), Pawn->GetEnergy());
			State->SetStringField(TEXT("location"), Pawn->GetActorLocation().ToString());
			State->SetStringField(TEXT("bodyRotation"), Pawn->GetActorRotation().ToString());
			if (const auto* PC = Controller.Get())
			{
				State->SetStringField(TEXT("cameraControl"), PC->GetControlRotation().ToString());
				State->SetBoolField(TEXT("cursorVisible"), PC->bShowMouseCursor);
			}
			Observations.Add(MakeShared<FJsonValueObject>(State));
		}

		// 按实际技能时长编排普通释放、切换和取消，不缩短技能或绕过正式输入。
		void BuildSequence()
		{
			const auto* Pawn = Player.Get();
			const auto& Config = Pawn->GetCombatConfig();
			const double AttackWait = FMath::Max(.8, double(Config.Attack.Windup + Config.Attack.Recovery) + .3);
			const double AreaWait = Config.Area.Windup + Config.Area.Recovery + .6;
			const auto* Heal = Pawn->GetActionMontage(EMultiplayerCombatAnimation::Heal);
			const double HealWait = Heal ? Heal->GetPlayLength() + .3 : 1.5;
			double At = .8;
			// 记录未来的真实输入，不在编排阶段执行技能。
			auto Key = [&](double Time, const TCHAR* Name, const FKey& Input, EInputEvent Event)
			{
				FReviewAction Action; Action.At = Time; Action.Name = Name; Action.Key = Input; Action.Event = Event; Steps.Add(Action);
			};
			// 插入只读预览检查，验证实际按键是否选择到对应技能。
			auto Check = [&](double Time, const TCHAR* Name, EMultiplayerCombatSkill Skill)
			{
				FReviewAction Action; Action.At = Time; Action.Name = Name; Action.CheckSkill = int32(Skill); Steps.Add(Action);
			};
			// 统一安排按住预览与可选松开确认，让取消用例保留原按键状态。
			auto Preview = [&](const TCHAR* Name, const FKey& Input, EMultiplayerCombatSkill Skill, bool bRelease = true)
			{
				Key(At, Name, Input, IE_Pressed); Check(At + .3, Name, Skill);
				if (bRelease) Key(At + .8, Name, Input, IE_Released);
				At += .8;
			};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				Key(At, TEXT("RepeatedAttack"), EKeys::LeftMouseButton, IE_Pressed);
				Key(At + .1, TEXT("RepeatedAttack"), EKeys::LeftMouseButton, IE_Released); At += AttackWait;
			}
			Preview(TEXT("DefensePreviewConfirm"), EKeys::Q, EMultiplayerCombatSkill::Defend); At += Config.Defend.Duration + .4;
			Preview(TEXT("HealPreviewConfirm"), EKeys::E, EMultiplayerCombatSkill::Heal); At += HealWait;
			Preview(TEXT("AreaPreviewReleaseConfirm"), EKeys::R, EMultiplayerCombatSkill::Area); At += AreaWait;
			Preview(TEXT("AreaPreviewCancel"), EKeys::R, EMultiplayerCombatSkill::Area, false);
			Key(At, TEXT("CancelAreaPreview"), EKeys::RightMouseButton, IE_Pressed);
			Key(At + .1, TEXT("CancelAreaPreview"), EKeys::RightMouseButton, IE_Released);
			Key(At + .2, TEXT("ReleaseCancelledArea"), EKeys::R, IE_Released); At += .7;
			Preview(TEXT("AreaWindupCancel"), EKeys::R, EMultiplayerCombatSkill::Area);
			At += FMath::Clamp(double(Config.Area.Windup) * .35, .04, .25);
			Key(At, TEXT("CancelAreaWindup"), EKeys::RightMouseButton, IE_Pressed);
			Key(At + .1, TEXT("CancelAreaWindup"), EKeys::RightMouseButton, IE_Released); At += .6;
			Key(At, TEXT("QuickSwitchQ"), EKeys::Q, IE_Pressed);
			Key(At + .2, TEXT("QuickSwitchE"), EKeys::E, IE_Pressed);
			Key(At + .3, TEXT("ReleaseOldQPreview"), EKeys::Q, IE_Released);
			Check(At + .5, TEXT("QuickSwitchKeepsEPreview"), EMultiplayerCombatSkill::Heal);
			Key(At + .9, TEXT("ConfirmSwitchedHeal"), EKeys::E, IE_Released); At += .9 + HealWait;
			Preview(TEXT("DashPreviewConfirm"), EKeys::LeftShift, EMultiplayerCombatSkill::Dash); At += Config.Dash.Duration + .8;
			Preview(TEXT("AreaLMBConfirm"), EKeys::R, EMultiplayerCombatSkill::Area, false);
			Key(At, TEXT("AreaLMBConfirm"), EKeys::LeftMouseButton, IE_Pressed);
			Key(At + .1, TEXT("AreaLMBConfirm"), EKeys::LeftMouseButton, IE_Released);
			Key(At + .2, TEXT("ReleaseAlreadyConfirmedArea"), EKeys::R, IE_Released); At += AreaWait;
			Preview(TEXT("FinalGuard"), EKeys::Q, EMultiplayerCombatSkill::Defend); At += .6;
			FReviewAction Rotate; Rotate.At = At; Rotate.Name = TEXT("GuardCameraYaw90"); Rotate.bRotate = true; Steps.Add(Rotate);
			FReviewAction Body; Body.At = At + .5; Body.Name = TEXT("GuardBodyDoesNotFollowCamera"); Body.bCheckBody = true; Steps.Add(Body);
			At += FMath::Max(1.0, double(Config.Defend.Duration));
			Key(At, TEXT("FinalForwardAttack"), EKeys::LeftMouseButton, IE_Pressed);
			Key(At + .1, TEXT("FinalForwardAttack"), EKeys::LeftMouseButton, IE_Released);
			Duration = FMath::Max(40., At + AttackWait + 1);
			Steps.Sort([](const auto& A, const auto& B) { return A.At < B.At; });
		}

		// 等待双客户端角色、HUD 和所需特效就绪；场景含 AI 时明确拒绝，不删除世界内容。
		bool FindPlayers()
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (!World || World->GetNetMode() != NM_Client || !World->HasBegunPlay() || !World->GetGameInstance()) continue;
				auto* Local = World->GetGameInstance()->GetFirstGamePlayer();
				auto* PC = Local ? Local->GetPlayerController(World) : nullptr;
				auto* Pawn = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
				if (!Pawn || !Pawn->IsCombatReady() || !Pawn->GetPlayerState() || !PC->GetHUD()) continue;
				AmultiplayerCombatCharacter* Remote = nullptr;
				for (TActorIterator<AmultiplayerCombatCharacter> It(World); It; ++It)
					if (*It != Pawn && It->GetPlayerState() && It->IsCombatReady() && !It->IsHostileTo(Pawn)) { Remote = *It; break; }
				if (!Remote) continue;
				Controller = PC; Player = Pawn; Other = Remote;
				for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
					if (!It->IsActorBeingDestroyed()) { Finish(TEXT("Review requires the existing no-AI SkillDemo; no actors were removed")); return false; }
				const auto& Config = Pawn->GetCombatConfig();
				for (const auto* Feedback : {&Config.AttackFeedback, &Config.DefenseFeedback, &Config.HealFeedback, &Config.AreaFeedback})
					if (!Feedback->CascadeEffect.IsNull() ? !Feedback->CascadeEffect.Get() : !Feedback->Effect.IsNull() && !Feedback->Effect.Get()) return false;
				if (!Config.ProjectileCascadeEffect.IsNull() ? !Config.ProjectileCascadeEffect.Get() : !Config.ProjectileEffect.IsNull() && !Config.ProjectileEffect.Get()) return false;
				return true;
			}
			return false;
		}

		// 观察端只调整自己的镜头观察远端角色，不改远端位置或施法方向。
		void ObserveOther()
		{
			if (Role != TEXT("Observer") || !Controller.IsValid() || !Other.IsValid()) return;
			const float Yaw = (Other->GetActorLocation() - Player->GetActorLocation()).Rotation().Yaw + 25.f;
			Controller->SetControlRotation(FRotator(-12.f, Yaw, 0));
		}

		// 用真实 S+D 输入拉开观察距离，位移不足则报告不可覆盖，不瞬移伪造机位。
		bool PrepareObserver(double Now)
		{
			if (Role != TEXT("Observer") || bObserverPrepared) return true;
			auto* PC = Controller.Get();
			PC->SetControlRotation(FRotator(-12.f, 0, 0));
			// 准备阶段同样记录实际输入及接纳结果，并纳入结束松键清理。
			const auto MovementInput = [&](const FKey& Key, EInputEvent Event)
			{
				const bool bAccepted = PC->InputKey(FInputKeyParams(Key, Event, Event == IE_Pressed ? 1. : 0.));
				if (Event == IE_Pressed) HeldKeys.Add(Key); else HeldKeys.Remove(Key);
				auto Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("timeSeconds"), 0);
				Entry->SetNumberField(TEXT("processElapsedSeconds"), Now - Started);
				Entry->SetStringField(TEXT("name"), TEXT("ObserverPreparationRealMovement"));
				Entry->SetStringField(TEXT("key"), Key.ToString());
				Entry->SetNumberField(TEXT("event"), int32(Event));
				Entry->SetBoolField(TEXT("inputAccepted"), bAccepted);
				Actions.Add(MakeShared<FJsonValueObject>(Entry));
			};
			if (ObserverPrepStarted < 0)
			{
				ObserverPrepStarted = Now;
				ObserverPrepLocation = Player->GetActorLocation();
				MovementInput(EKeys::S, IE_Pressed); MovementInput(EKeys::D, IE_Pressed);
				return false;
			}
			if (!bObserverPrepReleased && Now - ObserverPrepStarted >= .9)
			{
				MovementInput(EKeys::S, IE_Released); MovementInput(EKeys::D, IE_Released);
				bObserverPrepReleased = true;
			}
			if (Now - ObserverPrepStarted < 1.4) return false;
			const float Displacement = FVector::Dist2D(ObserverPrepLocation, Player->GetActorLocation());
			const float Separation = FVector::Dist2D(Player->GetActorLocation(), Other->GetActorLocation());
			auto State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("name"), TEXT("ObserverPreparationMeasured"));
			State->SetNumberField(TEXT("timeSeconds"), 0);
			State->SetNumberField(TEXT("processElapsedSeconds"), Now - Started);
			State->SetStringField(TEXT("startLocation"), ObserverPrepLocation.ToString());
			State->SetStringField(TEXT("actualLocation"), Player->GetActorLocation().ToString());
			State->SetNumberField(TEXT("actualDisplacementCm"), Displacement);
			State->SetNumberField(TEXT("otherPlayerDistanceCm"), Separation);
			State->SetNumberField(TEXT("cameraPitch"), -12);
			State->SetNumberField(TEXT("cameraYawOffset"), 25);
			Observations.Add(MakeShared<FJsonValueObject>(State));
			if (Displacement <= 150.f || Separation <= 150.f)
			{
				Unsupported.Add(MakeShared<FJsonValueString>(TEXT("Observer could not separate through real S+D movement; no teleport or camera asset override was used")));
				Finish(TEXT("Observer preparation was blocked or remained too close to the other player"));
				return false;
			}
			bObserverPrepared = true;
			ObserveOther();
			return true;
		}

		// 拥有者执行时间轴动作，观察端仅记证据；朝向检查不改变角色权威状态。
		void Run(const FReviewAction& Action)
		{
			APlayerController* PC = Controller.Get();
			auto* Pawn = Player.Get();
			auto Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("timeSeconds"), Elapsed());
			Entry->SetNumberField(TEXT("scheduledTimeSeconds"), Action.At);
			Entry->SetStringField(TEXT("name"), Action.Name);
			Entry->SetStringField(TEXT("key"), Action.Key.ToString());
			Entry->SetNumberField(TEXT("event"), int32(Action.Event));
			if (Role == TEXT("Owner"))
			{
				if (Action.bRotate)
				{
					if (!Pawn->IsDefending()) Errors.Add(MakeShared<FJsonValueString>(TEXT("Final Q was not active before camera-only rotation")));
					GuardBody = Pawn->GetActorRotation();
					FRotator Camera = PC->GetControlRotation(); Camera.Yaw += 90;
					PC->SetControlRotation(Camera);
				}
				else if (Action.bCheckBody)
				{
					const float Delta = FMath::Abs(FMath::FindDeltaAngleDegrees(GuardBody.Yaw, Pawn->GetActorRotation().Yaw));
					Entry->SetNumberField(TEXT("bodyYawDelta"), Delta);
					if (Delta > 2.f) Errors.Add(MakeShared<FJsonValueString>(TEXT("Camera-only guard rotation changed body yaw")));
				}
				else if (Action.CheckSkill >= 0)
				{
					const bool bPreview = Pawn->IsTargetingSkill() && int32(Pawn->GetTargetingSkill()) == Action.CheckSkill;
					Entry->SetBoolField(TEXT("previewMatched"), bPreview);
					if (!bPreview) Errors.Add(MakeShared<FJsonValueString>(Action.Name + TEXT(": actual input did not select the requested preview")));
				}
				else
				{
					if (Action.Name == TEXT("CancelAreaWindup") && Action.Event == IE_Pressed)
					{
						Entry->SetBoolField(TEXT("castingBeforeInput"), Pawn->IsCasting());
						if (!Pawn->IsCasting()) Errors.Add(MakeShared<FJsonValueString>(TEXT("R did not enter its natural windup before production RMB cancellation")));
					}
					Entry->SetBoolField(TEXT("inputAccepted"), PC->InputKey(FInputKeyParams(Action.Key, Action.Event, Action.Event == IE_Pressed ? 1. : 0.)));
					if (Action.Event == IE_Pressed) HeldKeys.Add(Action.Key); else HeldKeys.Remove(Action.Key);
				}
			}
			else Entry->SetBoolField(TEXT("observerInputInjected"), false);
			Actions.Add(MakeShared<FJsonValueObject>(Entry));
			Snapshot(Action.Name);
		}

		// 截图回调按实际文件确认落盘，失败计入执行错误而非忽略。
		void ScreenshotWritten()
		{
			if (!PendingFrame) return;
			const FString Path = PendingFrame->GetStringField(TEXT("path"));
			const bool bWritten = IFileManager::Get().FileSize(*Path) > 0;
			PendingFrame->SetBoolField(TEXT("written"), bWritten);
			PendingFrame->SetStringField(TEXT("writeWallUtc"), FDateTime::UtcNow().ToIso8601());
			PendingFrame->SetNumberField(TEXT("writeTimeSeconds"), Elapsed());
			if (!bWritten) Errors.Add(MakeShared<FJsonValueString>(TEXT("Requested viewport PNG was not written: ") + Path));
			PendingFrame.Reset();
		}

		// 等待合法场景后推进真实输入和限频截图，连接丢失或超时立即统一结束。
		bool Tick(float)
		{
			if (bDone) return false;
			const double Now = FPlatformTime::Seconds();
			if (Now - Started > 120) { Finish(TEXT("120-second connection/recording timeout")); return false; }
			if (RecordingStarted >= 0 && Elapsed() >= 50) { Finish(TEXT("50-second recording safety bound reached")); return false; }
			if (!FindPlayers())
			{
				ReadySince = -1;
				if (RecordingStarted >= 0 && !bDone) Finish(TEXT("A required player or presentation asset disappeared during recording"));
				return !bDone;
			}
			if (ReadySince < 0) ReadySince = Now;
			if (!PrepareObserver(Now)) return !bDone;
			ObserveOther();
			if (RecordingStarted < 0)
			{
				if (Now - ReadySince < 5) return true;
				if (!GEngine->GameViewport || !GEngine->GameViewport->Viewport) return true;
				const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
				if (Size.X != 960 || Size.Y != 540) { Finish(TEXT("Review requires the runner's original 960x540 viewport")); return false; }
				if (!UAbilitySystemGlobals::Get().ShouldIgnoreCosts() || !UAbilitySystemGlobals::Get().ShouldIgnoreCooldowns())
				{ Finish(TEXT("SkillDemo requires existing unlimited cost/cooldown CVars; review did not change them")); return false; }
				BuildSequence();
				if (Duration > 49) { Unsupported.Add(MakeShared<FJsonValueString>(TEXT("Current natural skill durations exceed the bounded 49-second sequence"))); Finish(TEXT("Unsupported skill timing; no gameplay state changed")); return false; }
				RecordingStarted = Now;
				if (Role == TEXT("Owner")) Controller->SetControlRotation(FRotator(-15, Player->GetActorRotation().Yaw, 0));
				Snapshot(TEXT("RecordingStarted"));
			}
			const double Time = Elapsed();
			while (NextAction < Steps.Num() && Time >= Steps[NextAction].At) Run(Steps[NextAction++]);
			if (Time >= Duration)
			{
				if (!PendingFrame || Time >= Duration + .5) { Snapshot(TEXT("RecordingComplete")); Finish(); return false; }
			}
			if (Time < Duration && Time - LastCapture >= .1 && !PendingFrame && !FScreenshotRequest::IsScreenshotRequested())
			{
				LastCapture = Time;
				const FString Path = FPaths::Combine(Directory, TEXT("Frames"), FString::Printf(TEXT("frame_%06d.png"), ++FrameNumber));
				PendingFrame = MakeShared<FJsonObject>();
				PendingFrame->SetStringField(TEXT("path"), Path);
				PendingFrame->SetNumberField(TEXT("timeSeconds"), Time);
				PendingFrame->SetStringField(TEXT("requestWallUtc"), FDateTime::UtcNow().ToIso8601());
				PendingFrame->SetNumberField(TEXT("requestFrame"), double(GFrameCounter));
				PendingFrame->SetBoolField(TEXT("written"), false);
				Frames.Add(MakeShared<FJsonValueObject>(PendingFrame));
				FScreenshotRequest::RequestScreenshot(Path, true, false);
			}
			if (Time - LastManifest > 1) { LastManifest = Time; Write(false); }
			return true;
		}

		// 只接受显式渲染演示和绝对输出目录，通过后才注册截图与 Ticker。
		void Start()
		{
			FParse::Value(FCommandLine::Get(), TEXT("CoopSkillReviewRole="), Role);
			FString Output; FParse::Value(FCommandLine::Get(), TEXT("CoopSkillReviewOutput="), Output);
			Directory = FPaths::Combine(Output, Role);
			if (Output.IsEmpty() || FPaths::IsRelative(Output) || (Role != TEXT("Owner") && Role != TEXT("Observer")))
			{
				UE_LOG(LogTemp, Error, TEXT("COOP_SKILL_REVIEW_UNSUPPORTED requires absolute output and Owner|Observer role")); bDone = true; return;
			}
			IFileManager::Get().MakeDirectory(*FPaths::Combine(Directory, TEXT("Frames")), true);
			if (!FParse::Param(FCommandLine::Get(), TEXT("CoopSkillDemo")) || FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
			{
				Unsupported.Add(MakeShared<FJsonValueString>(TEXT("Only the rendered no-AI unlimited SkillDemo is supported")));
				Finish(TEXT("Not a rendered CoopSkillDemo")); return;
			}
			ScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddRaw(this, &FSkillPresentationReview::ScreenshotWritten);
			TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSkillPresentationReview::Tick), .01f);
		}
	};

	TUniquePtr<FSkillPresentationReview> Review;
}
#endif

// 仅显式非 Shipping 开关创建唯一会话，不自动启用无限资源或创建敌人。
void StartCoopSkillPresentationReview()
{
#if !UE_BUILD_SHIPPING
	if (!Review && FParse::Param(FCommandLine::Get(), TEXT("CoopSkillReview")))
	{
		Review = MakeUnique<FSkillPresentationReview>();
		Review->Start();
	}
#endif
}

// 模块关闭时先清理外部回调，再释放会话对象，Shipping 下为空操作。
void StopCoopSkillPresentationReview()
{
#if !UE_BUILD_SHIPPING
	if (Review) { Review->Stop(); Review.Reset(); }
#endif
}
