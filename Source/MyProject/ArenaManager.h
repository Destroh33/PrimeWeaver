#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaManager.generated.h"

static constexpr int32 ArenaRoundCount = 10;

// Per-round configuration. Add exactly 10 of these to the RoundConfigs array on the ArenaManager actor.
USTRUCT(BlueprintType)
struct FArenaRoundConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Enemies")
	int32 BearCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Enemies")
	int32 MothCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Enemies")
	int32 SpidaCount = 2;

	// Multiplier applied to each enemy's default maxHealth for this round
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Scaling", meta=(ClampMin=0.1f))
	float HealthMultiplier = 1.0f;

	// Multiplier applied to each enemy's default DamageDealt for this round
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Scaling", meta=(ClampMin=0.1f))
	float DamageMultiplier = 1.0f;

	// Total time window over which all enemies for this round are staggered in
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spawning", meta=(ClampMin=0.5f, Units="s"))
	float SpawnStaggerDuration = 8.0f;
};

UCLASS()
class MYPROJECT_API AArenaManager : public AActor
{
	GENERATED_BODY()

public:
	AArenaManager();

protected:
	virtual void BeginPlay() override;

	// -----------------------------------------------------------------------
	// Configuration — set these in the editor on the placed ArenaManager actor
	// -----------------------------------------------------------------------

	// Must have exactly 10 entries (ArenaRoundCount). Pre-populated with defaults.
	UPROPERTY(EditAnywhere, Category="Arena|Rounds")
	TArray<FArenaRoundConfig> RoundConfigs;

	UPROPERTY(EditAnywhere, Category="Arena|Enemy Classes")
	TSubclassOf<AActor> BearClass;

	UPROPERTY(EditAnywhere, Category="Arena|Enemy Classes")
	TSubclassOf<AActor> MothClass;

	UPROPERTY(EditAnywhere, Category="Arena|Enemy Classes")
	TSubclassOf<AActor> SpidaClass;

	// Drag spawn-point actors from the level into these arrays — one per enemy type.
	// A random point is chosen for each individual enemy spawned.
	UPROPERTY(EditAnywhere, Category="Arena|Spawn Points")
	TArray<AActor*> BearSpawnPoints;

	UPROPERTY(EditAnywhere, Category="Arena|Spawn Points")
	TArray<AActor*> MothSpawnPoints;

	UPROPERTY(EditAnywhere, Category="Arena|Spawn Points")
	TArray<AActor*> SpidaSpawnPoints;

	// Duration of the pre-round phase before each round (seconds)
	UPROPERTY(EditAnywhere, Category="Arena|Timing", meta=(ClampMin=1.0f, Units="s"))
	float PreRoundDuration = 15.0f;

	// -----------------------------------------------------------------------
	// State — read these from Blueprint for HUD / game logic
	// -----------------------------------------------------------------------

	// 1-indexed current round number (0 = not started)
	UPROPERTY(BlueprintReadOnly, Category="Arena|State")
	int32 CurrentRound = 0;

	UPROPERTY(BlueprintReadOnly, Category="Arena|State")
	int32 EnemiesKilled = 0;

	UPROPERTY(BlueprintReadOnly, Category="Arena|State")
	int32 TotalEnemies = 0;

	// Counts down from PreRoundDuration to 0 during the pre-round phase
	UPROPERTY(BlueprintReadOnly, Category="Arena|State")
	float PreRoundTimeRemaining = 0.0f;

	// -----------------------------------------------------------------------
	// Blueprint hooks — override these in a child Blueprint to add behaviour
	// -----------------------------------------------------------------------

	// Called at the start of the pre-round window. Spawn chests, set-dressing, etc. here.
	UFUNCTION(BlueprintImplementableEvent, Category="Arena")
	void OnPreRoundBegin(int32 Round);

	// Called when the pre-round window ends and enemies start spawning
	UFUNCTION(BlueprintImplementableEvent, Category="Arena")
	void OnRoundBegin(int32 Round);

	// Called once per spawned enemy immediately after it enters the world.
	// The C++ code automatically binds onDeath → NotifyEnemyDied via reflection,
	// but you can use this hook for any additional per-enemy setup in Blueprint.
	UFUNCTION(BlueprintImplementableEvent, Category="Arena")
	void OnEnemySpawned(AActor* Enemy, int32 Round);

	// Called when all enemies in a round are dead
	UFUNCTION(BlueprintImplementableEvent, Category="Arena")
	void OnRoundEnd(int32 Round);

	// Called after round 10 ends — wire your win-state logic here
	UFUNCTION(BlueprintImplementableEvent, Category="Arena")
	void OnArenaComplete();

public:
	// -----------------------------------------------------------------------
	// Public API
	// -----------------------------------------------------------------------

	// Call this (e.g. from a trigger or level Blueprint) to begin the arena
	UFUNCTION(BlueprintCallable, Category="Arena")
	void StartArena();

	// Called automatically via onDeath binding. Expose as BlueprintCallable as a
	// manual fallback in case the reflection-based binding fails on a given enemy.
	UFUNCTION(BlueprintCallable, Category="Arena")
	void NotifyEnemyDied();

private:
	struct FEnemyBaseStats
	{
		float BaseDamage        = 0.f;
		float BaseMaxHealth     = 0.f;
		float CapsuleHalfHeight = 0.f;
	};

	FEnemyBaseStats BearBase;
	FEnemyBaseStats MothBase;
	FEnemyBaseStats SpidaBase;

	// Spawn queue for the current round: (class, world transform)
	TArray<TPair<TSubclassOf<AActor>, FTransform>> PendingSpawns;
	int32 SpawnIndex = 0;

	FTimerHandle PreRoundCountdownHandle;
	FTimerHandle RoundStartHandle;
	FTimerHandle SpawnHandle;

	void CacheBaseStats();
	void BeginPreRound();
	void TickPreRoundCountdown();
	void BeginRound();
	void SpawnNextEnemy();
	void CheckRoundEnd();

	void ApplyStatsToEnemy(AActor* Enemy, const FArenaRoundConfig& Config, const FEnemyBaseStats& Base) const;
	void BindToEnemyDeath(AActor* Enemy);

	// Receives the onDeath dispatcher from BPC_dmgSys via reflection binding
	UFUNCTION()
	void HandleEnemyDied();

	static float      GetFloatProp(UObject* Obj, FName Name, float Default = 0.f);
	static void       SetFloatProp(UObject* Obj, FName Name, float Value);
	static UActorComponent* FindDmgSys(AActor* Actor);
	static AActor*    PickRandomPoint(const TArray<AActor*>& Points);
};
