// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AIController.h"
#include "multiplayerGuardianAIController.generated.h"

class AmultiplayerGuardianCharacter;

/**
 * Small server-only timer-driven controller for the portfolio Guardian.
 * It intentionally avoids per-frame target scans and remains replaceable by a
 * Blueprint/BehaviorTree controller once final assets are wired.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerGuardianAIController : public AAIController
{
	GENERATED_BODY()

public:
	AmultiplayerGuardianAIController();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void StartDecisionLoop();
	void StopDecisionLoop();
	void EvaluateGuardian();
	AActor* FindBestTarget(AmultiplayerGuardianCharacter& Guardian) const;
	AActor* FindBestCandidateFrom(
		AmultiplayerGuardianCharacter& Guardian,
		const TArray<TObjectPtr<AActor>>& Candidates,
		float MaxDistance) const;

	TWeakObjectPtr<AmultiplayerGuardianCharacter> GuardianCharacter;
	FTimerHandle DecisionTimer;
};
