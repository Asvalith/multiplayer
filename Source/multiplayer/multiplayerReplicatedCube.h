// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerReplicatedCube.generated.h"

class UStaticMeshComponent;

UCLASS()
class MULTIPLAYER_API AmultiplayerReplicatedCube : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerReplicatedCube();

private:
	UPROPERTY(VisibleAnywhere, Category = "Mesh")
	TObjectPtr<UStaticMeshComponent> MeshComponent;
};
