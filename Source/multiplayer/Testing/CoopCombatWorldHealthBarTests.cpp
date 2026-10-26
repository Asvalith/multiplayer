// Copyright Epic Games, Inc. All Rights Reserved.

// 敌人不再挂接头顶血条；原生、保存蓝图与专服仍走自己的 GAS 伤害和死亡链路。
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Components/WidgetComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	struct FEnemyWithoutUIWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;

		// 两种真实端类型都生成同一生产敌人，AI 关闭以免自主攻击污染伤害断言。
		explicit FEnemyWithoutUIWorld(ENetMode Mode)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(Mode);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			if (Mode != NM_Standalone) World->WorldType = EWorldType::PIE;
		}

		~FEnemyWithoutUIWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		AmultiplayerCombatEnemy* Spawn(UClass* Class)
		{
			const FTransform Transform(FRotator::ZeroRotator, FVector(500.f, 0.f, 150.f));
			auto* Enemy = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(Class, Transform, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Enemy) return nullptr;
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(Transform);
			Enemy->GetCharacterMovement()->DisableMovement();
			return Enemy;
		}

		void Advance()
		{
			++GFrameCounter;
			World->Tick(LEVELTICK_All, .04f);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEnemyWithoutHealthUITest, "Multiplayer.Combat.EnemyWithoutHealthUI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 检查实际组件列表和权威伤害，不能只隐藏 Widget 后仍保留订阅或每视角 Tick。
bool FCoopEnemyWithoutHealthUITest::RunTest(const FString&)
{
	UClass* Blueprint = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	if (!TestNotNull(TEXT("Real saved minion BP loads"), Blueprint)) return false;
	for (ENetMode Mode : { NM_Standalone, NM_DedicatedServer })
	{
		FEnemyWithoutUIWorld Fixture(Mode);
		if (!TestTrue(TEXT("Fixture uses the requested actual NetMode"), Fixture.World->GetNetMode() == Mode)) return false;
		auto* Source = Fixture.World->SpawnActor<AmultiplayerCombatCharacter>();
		if (!TestNotNull(TEXT("Authoritative damage source exists"), Source)) return false;
		for (UClass* Class : { AmultiplayerCombatEnemy::StaticClass(), Blueprint })
		{
			auto* Enemy = Fixture.Spawn(Class);
			if (!TestNotNull(TEXT("Native and saved BP enemies spawn"), Enemy)) return false;
			TestNull(TEXT("Enemy creates no health display or other WidgetComponent"), Enemy->FindComponentByClass<UWidgetComponent>());
			const float Health = Enemy->GetHealth();
			TestTrue(TEXT("Removing UI does not reject real server damage"), Source->ApplyCombatDamage(Enemy, 25.f));
			TestTrue(TEXT("Real GAS damage still decreases health"), Enemy->GetHealth() < Health);
			Fixture.Advance();
			TestNull(TEXT("A later frame does not recreate an enemy widget"), Enemy->FindComponentByClass<UWidgetComponent>());
			TestTrue(TEXT("Enemy still accepts lethal server damage"), Source->ApplyCombatDamage(Enemy, 10000.f));
			TestFalse(TEXT("Lethal GAS damage still commits death"), Enemy->IsAlive());
			Fixture.Advance();
			TestNull(TEXT("Death leaves no enemy UI component"), Enemy->FindComponentByClass<UWidgetComponent>());
		}
	}
	return !HasAnyErrors();
}

#endif
