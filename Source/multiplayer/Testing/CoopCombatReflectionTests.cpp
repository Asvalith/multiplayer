// Copyright Epic Games, Inc. All Rights Reserved.

// 回归弹反窗口与方向、攻击归属转移、有限反射次数和复制弹道重建。
// 服务器命中使用真实扫掠；模拟代理入口只验快照契约，不冒充专服网络传输。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "UObject/UnrealType.h"

namespace
{
	// 本组运行真实服务器扫掠和 GE；模拟代理只验证复制快照的重建入口，不冒充 DS 网络实测。
	struct FCombatReflectionWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		// 建立支持真实碰撞扫掠的独立世界，关闭无关 AI 和导航。
		FCombatReflectionWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL()); World->SetBegunPlay(true);
		}
		// 对称结束角色、效果和弹体，并恢复测试前全局帧号。
		~FCombatReflectionWorld()
		{
			World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		// 推进一个真实世界帧，供弹体扫掠及持续效果计时使用。
		void Tick(float Seconds) { ++GFrameCounter; World->Tick(LEVELTICK_All, Seconds); }
		// 小步推进，防止一帧跨过短弹反窗口或多段弹道而掩盖问题。
		void Advance(float Seconds)
		{
			for (float Time = 0; Time < Seconds;)
			{
				const float Step = FMath::Min(.01f, Seconds - Time); Tick(Step); Time += Step;
			}
		}
		// 有界等待真实碰撞／反射结果，不手动调用弹反结算函数。
		bool WaitFor(TFunctionRef<bool()> Condition, float Timeout = 1.f)
		{
			for (float Time = 0; Time < Timeout && !Condition(); Time += .01f) Tick(.01f);
			return Condition();
		}
		// 创建指定阵营及身体朝向的真实角色，禁用自主移动并可注入独立规则。
		AmultiplayerCombatCharacter* Character(bool Enemy, const FVector& Location, float Yaw = 0,
			UmultiplayerCombatConfig* Config = nullptr)
		{
			const FTransform Transform(FRotator(0, Yaw, 0), Location);
			auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(Enemy ? AmultiplayerCombatEnemy::StaticClass()
				: AmultiplayerCombatCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
			if (Config)
				FindFProperty<FObjectPropertyBase>(Pawn->GetClass(), TEXT("CombatConfig"))->SetObjectPropertyValue_InContainer(Pawn, Config);
			Pawn->FinishSpawning(Transform); Pawn->GetCharacterMovement()->DisableMovement();
			return Pawn;
		}
		// 初始化正式投射物并交给真实运动组件，归属与方向变化由生产逻辑处理。
		AmultiplayerCombatProjectile* Fire(AmultiplayerCombatCharacter* Source, float Speed = 1000.f)
		{
			const FTransform Transform(FRotator::ZeroRotator, Source->GetActorLocation() + FVector(75, 0, 0));
			auto* Projectile = World->SpawnActorDeferred<AmultiplayerCombatProjectile>(AmultiplayerCombatProjectile::StaticClass(),
				Transform, Source, Source, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Projectile->Initialize(Source, 25.f, Speed); Projectile->FinishSpawning(Transform); return Projectile;
		}
		// 通过正式限时 GE 打开弹反窗口，不用手写状态布尔值替代标签生命周期。
		bool Parry(AmultiplayerCombatCharacter* Character, float Duration = .3f)
		{
			auto* ASC = Character->GetAbilitySystemComponent();
			auto Spec = ASC->MakeOutgoingSpec(UmultiplayerParryEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
			if (!Spec.IsValid()) return false;
			Spec.Data->SetDuration(Duration, true);
			return ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get()).IsValid();
		}
		// 创建真实静态阻挡，检查反射后弹体仍遵守场景碰撞和消耗规则。
		void Wall(const FVector& Location)
		{
			auto* Actor = World->SpawnActor<AActor>(); auto* Box = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Box); Box->SetBoxExtent(FVector(10, 200, 200));
			Box->SetCollisionObjectType(ECC_WorldStatic); Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Block); Box->RegisterComponent(); Actor->SetActorLocation(Location);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatReflectionWindowTest, "Multiplayer.Combat.Reflection.WindowAndFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 正面短窗口接受，背面、锥外、到期与未接纳均仍按普通敌方命中，不把整个防御持续时间当弹反。
bool FCombatReflectionWindowTest::RunTest(const FString&)
{
	for (int32 Case = 0; Case < 7; ++Case)
	{
		FCombatReflectionWorld Fixture;
		auto* Source = Fixture.Character(false, FVector(0, 0, 100));
		auto* Defender = Fixture.Character(Case != 6, FVector(300, 0, 100), Case == 1 ? 0 : Case == 2 ? 105 : Case == 5 ? 135 : 180);
		if (!TestTrue(TEXT("Real timed parry GE applies"), Fixture.Parry(Defender))) return false;
		if (Case == 3) Fixture.Advance(.4f);
		if (Case == 4) Defender->SetCombatSessionReady(false);
		TWeakObjectPtr<AmultiplayerCombatProjectile> Projectile = Fixture.Fire(Source);
		Fixture.WaitFor([&] { return !Projectile.IsValid() || Projectile->GetLaunchState().Revision > 1; });
		if (Case == 0 || Case == 5)
		{
			TestTrue(TEXT("Real swept frontal contact reflects during the short window"), Projectile.IsValid() && Projectile->GetLaunchState().Revision == 2);
			TestEqual(TEXT("Successful parry does not damage its defender"), Defender->GetHealth(), 100.f);
		}
		else if (Case == 4 || Case == 6)
		{
			TestTrue(TEXT("Unadmitted or friendly character cannot intercept or reflect the projectile"),
				Projectile.IsValid() && Projectile->GetLaunchState().Revision == 1);
			TestEqual(TEXT("Unadmitted or friendly character retains its damage protection"), Defender->GetHealth(), 100.f);
		}
		else
		{
			TestFalse(TEXT("Rear, outside cone and expired contacts cannot reflect"), Projectile.IsValid());
			TestEqual(TEXT("Rejected direction/window deals normal damage"), Defender->GetHealth(), 75.f);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatReflectionOwnershipTest, "Multiplayer.Combat.Reflection.OwnershipAndOriginalSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实扫掠后，防御者变为伤害来源，旧来源解除移动忽略并真正承受原伤害，而非只检查朝向变量。
bool FCombatReflectionOwnershipTest::RunTest(const FString&)
{
	FCombatReflectionWorld Fixture;
	auto* Source = Fixture.Character(false, FVector(0, 0, 100));
	auto* Defender = Fixture.Character(true, FVector(300, 0, 100), 180);
	Fixture.Parry(Defender);
	TWeakObjectPtr<AmultiplayerCombatProjectile> Projectile = Fixture.Fire(Source);
	const double Expiry = Projectile->GetLaunchState().ServerExpiryTime;
	if (!TestTrue(TEXT("Swept contact produces a reflected segment"), Fixture.WaitFor([&]
		{ return Projectile.IsValid() && Projectile->GetLaunchState().Revision == 2; }))) return false;
	TestEqual(TEXT("Reflection transfers actor owner"), Projectile->GetOwner(), static_cast<AActor*>(Defender));
	TestEqual(TEXT("Reflection transfers instigator"), Projectile->GetInstigator(), static_cast<APawn*>(Defender));
	TestTrue(TEXT("Reflected direction heads toward the original source"), FVector(Projectile->GetLaunchState().Velocity).X < -999.f);
	TestEqual(TEXT("Reflection does not extend total authoritative lifetime"), Projectile->GetLaunchState().ServerExpiryTime, Expiry);
	auto* Sphere = Projectile->FindComponentByClass<USphereComponent>();
	TestTrue(TEXT("New source is ignored by swept movement"), Sphere->GetMoveIgnoreActors().Contains(Defender));
	TestFalse(TEXT("Old source is no longer ignored"), Sphere->GetMoveIgnoreActors().Contains(Source));
	FHitResult SameContact(Defender, Cast<UPrimitiveComponent>(Defender->GetRootComponent()), Projectile->GetActorLocation(), FVector(-1, 0, 0));
	TestFalse(TEXT("Same collision cannot reflect twice from its new source"), Projectile->TryReflectFrom(Defender, SameContact));
	TestEqual(TEXT("Rejected duplicate does not increment trajectory revision"), Projectile->GetLaunchState().Revision, uint16(2));
	Fixture.Advance(.5f);
	TestFalse(TEXT("The reflected hit consumes the projectile"), Projectile.IsValid());
	TestEqual(TEXT("Original source receives exactly the retained 25 damage"), Source->GetHealth(), 75.f);
	TestEqual(TEXT("Parrying defender remains undamaged"), Defender->GetHealth(), 100.f);
	Fixture.Advance(.3f);
	TestEqual(TEXT("Later callbacks cannot settle the same reflected hit again"), Source->GetHealth(), 75.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatReflectionLimitTest, "Multiplayer.Combat.Reflection.BoundedExchange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 两个真实敌对角色来回弹反；次数来自初次发射配置，不随新 Owner 的不同配置改变。
bool FCombatReflectionLimitTest::RunTest(const FString&)
{
	FCombatReflectionWorld Fixture;
	auto* SourceConfig = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), GetTransientPackage());
	SourceConfig->MaxProjectileReflections = 2;
	auto* DefenderConfig = DuplicateObject<UmultiplayerCombatConfig>(SourceConfig, GetTransientPackage());
	DefenderConfig->MaxProjectileReflections = 0;
	auto* Source = Fixture.Character(false, FVector(0, 0, 100), 0, SourceConfig);
	auto* Defender = Fixture.Character(true, FVector(300, 0, 100), 180, DefenderConfig);
	Fixture.Parry(Source, 2.f); Fixture.Parry(Defender, 2.f);
	TWeakObjectPtr<AmultiplayerCombatProjectile> Projectile = Fixture.Fire(Source);
	SourceConfig->MaxProjectileReflections = 0;
	if (!TestTrue(TEXT("Two reflections use the original snapshotted limit"), Fixture.WaitFor([&]
		{ return Projectile.IsValid() && Projectile->GetLaunchState().Revision == 3; }))) return false;
	TestEqual(TEXT("Second parry transfers ownership back"), Projectile->GetOwner(), static_cast<AActor*>(Source));
	Fixture.Advance(.5f);
	TestFalse(TEXT("Third contact consumes the bounded projectile rather than bouncing forever"), Projectile.IsValid());
	TestEqual(TEXT("Only the limit-exhausted target receives one damage settlement"), Defender->GetHealth(), 75.f);
	TestEqual(TEXT("The second parrying character remains undamaged"), Source->GetHealth(), 100.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatReflectionWallTest, "Multiplayer.Combat.Reflection.SweepWallAndSourceLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 一次长 Sweep 可同时缓存角色重叠与其后墙面命中；旧墙回调不能销毁已弹反的新轨迹。
bool FCombatReflectionWallTest::RunTest(const FString&)
{
	for (bool DestroyOldSource : { false, true })
	{
		FCombatReflectionWorld Fixture;
		auto* Source = Fixture.Character(false, FVector(0, 0, 100));
		auto* Defender = Fixture.Character(true, FVector(300, 0, 100), 180);
		Fixture.Parry(Defender, 2.f); Fixture.Wall(FVector(380, 0, 100));
		TWeakObjectPtr<AmultiplayerCombatProjectile> Projectile = Fixture.Fire(Source, 3000.f);
		Fixture.Tick(.15f);
		if (!TestTrue(TEXT("Parry survives the same old sweep's later wall notification"),
			Projectile.IsValid() && Projectile->GetLaunchState().Revision == 2)) return false;
		if (DestroyOldSource)
		{
			Source->Destroy();
			auto* OtherPlayer = Fixture.Character(false, FVector(-200, 0, 100));
			Fixture.Advance(.3f);
			TestFalse(TEXT("Reflected projectile resolves using the new living source"), Projectile.IsValid());
			TestEqual(TEXT("Destroying the old source does not erase the defender's reflected attack"), OtherPlayer->GetHealth(), 75.f);
		}
		else
		{
			Source->SetActorLocation(FVector(0, 1000, 100)); Fixture.Wall(FVector(-100, 0, 100));
			Fixture.Advance(.3f);
			TestFalse(TEXT("A new wall on the reflected path still destroys the projectile"), Projectile.IsValid());
			TestEqual(TEXT("Reflected wall collision cannot damage an unrelated player"), Source->GetHealth(), 100.f);
		}
		TestEqual(TEXT("The original parry contact never dealt damage"), Defender->GetHealth(), 100.f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatReflectionTrajectoryTest, "Multiplayer.Combat.Reflection.RevisionAndLateTrajectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 驱动实际 RepNotify/服务器时钟：已有代理收到新段落，晚相关代理只收最新段落，均不重放碰撞或重置寿命。
bool FCombatReflectionTrajectoryTest::RunTest(const FString&)
{
	FCombatReflectionWorld Fixture;
	auto* Clock = Fixture.World->SpawnActor<AGameStateBase>(); Fixture.World->SetGameState(Clock);
	auto* Source = Fixture.Character(false, FVector(0, 0, 100));
	auto* Defender = Fixture.Character(true, FVector(300, 0, 100), 180); Fixture.Parry(Defender);
	TWeakObjectPtr<AmultiplayerCombatProjectile> Server = Fixture.Fire(Source);
	const auto Initial = Server->GetLaunchState();
	if (!Fixture.WaitFor([&] { return Server.IsValid() && Server->GetLaunchState().Revision == 2; }))
		return TestTrue(TEXT("Authoritative sweep reflected before client rebuild"), false);
	const auto Reflected = Server->GetLaunchState();
	TestTrue(TEXT("Reflection advances the shared trajectory revision"), Reflected.Revision > Initial.Revision);
	TestFalse(TEXT("Dynamic trajectory snapshots do not restore per-frame RepMovement"), Server->IsReplicatingMovement());
	FStructProperty* Property = FindFProperty<FStructProperty>(Server->GetClass(), TEXT("LaunchState"));
	if (!TestNotNull(TEXT("Launch snapshot is a reflected replicated property"), Property)) return false;
	const auto Client = [&]()
	{
		const FTransform Transform(FVector(1000, 1000, 100));
		auto* Result = Fixture.World->SpawnActorDeferred<AmultiplayerCombatProjectile>(AmultiplayerCombatProjectile::StaticClass(),
			Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Result->SetRole(ROLE_SimulatedProxy); Result->FinishSpawning(Transform); return Result;
	};
	const auto Receive = [Property](AmultiplayerCombatProjectile* Client, const FMultiplayerCombatProjectileLaunchState& State)
	{
		*Property->ContainerPtrToValuePtr<FMultiplayerCombatProjectileLaunchState>(Client) = State;
		Client->ProcessEvent(Client->FindFunctionChecked(TEXT("OnRep_LaunchState")), nullptr);
	};
	auto* Existing = Client(); Receive(Existing, Initial); Receive(Existing, Reflected);
	auto* Late = Client(); Receive(Late, Reflected);
	Fixture.Advance(.1f);
	const auto Expected = [&]()
	{
		const double Age = FMath::Clamp(Clock->GetServerWorldTimeSeconds() - Reflected.ServerLaunchTime, 0.0,
			Reflected.ServerExpiryTime - Reflected.ServerLaunchTime);
		return FVector(Reflected.Origin) + FVector(Reflected.Velocity) * Age;
	};
	TestTrue(TEXT("Existing simulated proxy replaces the old path with the reflected snapshot"), Existing->GetActorLocation().Equals(Expected(), .1f));
	TestTrue(TEXT("Late simulated proxy reconstructs directly from the latest reflected segment"), Late->GetActorLocation().Equals(Expected(), .1f));
	TestTrue(TEXT("Both clients face the new segment direction"), Existing->GetActorForwardVector().X < -.99f && Late->GetActorForwardVector().X < -.99f);
	TestFalse(TEXT("Client cannot invoke the authority reflection entry"), Late->TryReflectFrom(Defender, FHitResult()));
	TestFalse(TEXT("Client projectile does not simulate a second trajectory"), Late->FindComponentByClass<UProjectileMovementComponent>()->bSimulationEnabled);
	Fixture.Advance(3.f);
	TestFalse(TEXT("Late trajectory hides at original expiry, not three seconds after reflection"), Late->FindComponentByClass<UStaticMeshComponent>()->IsVisible());
	TestTrue(TEXT("Expired client trajectory stays clamped to the absolute deadline"), Late->GetActorLocation().Equals(Expected(), .1f));
	return !HasAnyErrors();
}

#endif
