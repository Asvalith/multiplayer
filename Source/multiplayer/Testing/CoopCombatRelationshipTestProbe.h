// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CoopCombatRelationshipTestProbe.generated.h"

// 动态事件需要真实反射接收器；只记录测试通知，不参与技能资格或业务结算。
UCLASS(Transient, NotBlueprintable)
class UCoopCombatRelationshipTestProbe final : public UObject
{
	GENERATED_BODY()
public:
	TFunction<void()> StateCallback;
	TArray<FText> Messages;

	UFUNCTION()
	void HandleState()
	{
		const TFunction<void()> Callback = StateCallback;
		if (Callback) Callback();
	}

	UFUNCTION()
	void HandleMessage(FText Message)
	{
		Messages.Add(Message);
	}
};
