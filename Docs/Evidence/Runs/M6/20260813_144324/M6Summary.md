# GAS M6 verification: 20260813_144324

- Result: **PASS**
- Per-direction lag: 150 ms
- Approximate RTT: 300 ms
- Per-direction loss: 0%
- Pending Cue inventory: OnActive=2, WhileActive=2, Removed=1
- Immunity Cue inventory: OnActive=2, WhileActive=2, Removed=2

| Check | Result | Details |
|---|---|---|
| RunInfo Stage | PASS | Stage=M6 |
| RunInfo AutoSequence | PASS | ClientAutoSequence=True |
| Host reached listen state | PASS | HostReady=true |
| Client joined host | PASS | ClientJoined=true |
| Host lag injection was applied | PASS | PktLag=150 |
| Client lag injection was applied | PASS | PktLag=150 |
| Host loss injection was applied | PASS | PktLoss=0 |
| Client loss injection was applied | PASS | PktLoss=0 |
| Host has no fatal/assert/ensure | PASS | Fatal/Critical/Ensure scan |
| Client has no fatal/assert/ensure | PASS | Fatal/Critical/Ensure scan |
| Authority armed once | PASS | Expected=1 Actual=1 |
| Authority arm result succeeded once | PASS | Expected=1 Actual=1 |
| Authority rejected once | PASS | Expected=1 Actual=1 |
| Authority accepted recovery once | PASS | Expected=1 Actual=1 |
| Client arm acknowledgement once | PASS | Expected=1 Actual=1 |
| Forced reject input once | PASS | Expected=1 Actual=1 |
| Recovery input once | PASS | Expected=1 Actual=1 |
| Client predicted exactly two activations | PASS | Expected=2 Actual=2 |
| Client rejection callback once | PASS | Expected=1 Actual=1 |
| Engine ClientActivateAbilityFailed once | PASS | Expected=1 Actual=1 |
| Rejected state snapshot once | PASS | Expected=1 Actual=1 |
| Accepted CatchUp once | PASS | Expected=1 Actual=1 |
| Accepted state snapshot once | PASS | Expected=1 Actual=1 |
| Rejected pending visual cleared once | PASS | Expected=1 Actual=1 |
| Accepted pending visual cleared once | PASS | Expected=1 Actual=1 |
| Post-reject checkpoint once | PASS | Expected=1 Actual=1 |
| Post-recovery checkpoint once | PASS | Expected=1 Actual=1 |
| Sequence pass once | PASS | Expected=1 Actual=1 |
| Sequence has no failure | PASS | Expected=0 Actual=0 |
| Rejected key numeric field matches serialized key | PASS | Key=1 |
| Forced rejection consumed its server-only tag | PASS | TagRemaining=0 |
| Rejected activation uses one prediction key | PASS | RejectedKey=1 |
| Rejected key did not become an accepted authority commit | PASS | Rejected=1 AuthorityAccepted=2 |
| Rejected key did not become normal client acceptance | PASS | Rejected=1 Accepted=2 |
| Recovery uses a new key | PASS | Rejected=1 Recovery=2 |
| Ability spec is stable across reject and recovery | PASS | Spec=6 |
| TrialId is stable across both processes | PASS | TrialId=6001 |
| PredictedReject PersistentGECount | PASS | PersistentGECount Expected=1 Actual=1 |
| PredictedReject CooldownTagCount | PASS | CooldownTagCount Expected=1 Actual=1 |
| PredictedReject EnergyCurrent | PASS | EnergyCurrent Expected=70.0 Actual=70.0 |
| PredictedReject CooldownGECount | PASS | CooldownGECount Expected=1 Actual=1 |
| PredictedReject PendingGECount | PASS | PendingGECount Expected=1 Actual=1 |
| PredictedReject PendingCueCount | PASS | PendingCueCount Expected=1 Actual=1 |
| PredictedReject EnergyBase | PASS | EnergyBase Expected=100.0 Actual=100.0 |
| PredictedReject ImmuneCount | PASS | ImmuneCount Expected=1 Actual=1 |
| PredictedReject PredictionKey | PASS | PredictionKey Expected=1 Actual=1 |
| PredictedReject Spec | PASS | Spec Expected=6 Actual=6 |
| PredictedReject CostGECount | PASS | CostGECount Expected=1 Actual=1 |
| PredictedReject TrialId | PASS | TrialId Expected=6001 Actual=6001 |
| PredictedRecovery PersistentGECount | PASS | PersistentGECount Expected=1 Actual=1 |
| PredictedRecovery CooldownTagCount | PASS | CooldownTagCount Expected=1 Actual=1 |
| PredictedRecovery EnergyCurrent | PASS | EnergyCurrent Expected=70.0 Actual=70.0 |
| PredictedRecovery CooldownGECount | PASS | CooldownGECount Expected=1 Actual=1 |
| PredictedRecovery PendingGECount | PASS | PendingGECount Expected=1 Actual=1 |
| PredictedRecovery PendingCueCount | PASS | PendingCueCount Expected=1 Actual=1 |
| PredictedRecovery EnergyBase | PASS | EnergyBase Expected=100.0 Actual=100.0 |
| PredictedRecovery ImmuneCount | PASS | ImmuneCount Expected=1 Actual=1 |
| PredictedRecovery PredictionKey | PASS | PredictionKey Expected=2 Actual=2 |
| PredictedRecovery Spec | PASS | Spec Expected=6 Actual=6 |
| PredictedRecovery CostGECount | PASS | CostGECount Expected=1 Actual=1 |
| PredictedRecovery TrialId | PASS | TrialId Expected=6001 Actual=6001 |
| Post-reject CatchUp is classified as bookkeeping only | PASS | Count=1 RejectedKey=1 |
| PostReject RejectedKey | PASS | RejectedKey Expected=1 Actual=1 |
| PostReject PendingVisual | PASS | PendingVisual Expected=0 Actual=0 |
| PostReject EnergyCurrent | PASS | EnergyCurrent Expected=100.0 Actual=100.0 |
| PostReject PendingCue | PASS | PendingCue Expected=0 Actual=0 |
| PostReject PendingGECount | PASS | PendingGECount Expected=0 Actual=0 |
| PostReject CooldownGECount | PASS | CooldownGECount Expected=0 Actual=0 |
| PostReject PersistentGECount | PASS | PersistentGECount Expected=0 Actual=0 |
| PostReject CaughtUpKey | PASS | CaughtUpKey Expected=0 Actual=0 |
| PostReject EnergyBase | PASS | EnergyBase Expected=100.0 Actual=100.0 |
| PostReject ImmuneCount | PASS | ImmuneCount Expected=0 Actual=0 |
| PostReject CostGECount | PASS | CostGECount Expected=0 Actual=0 |
| PostReject TrialId | PASS | TrialId Expected=6001 Actual=6001 |
| PostReject ImmunityCooldown | PASS | ImmunityCooldown Expected=0 Actual=0 |
| PostRecovery RejectedKey | PASS | RejectedKey Expected=1 Actual=1 |
| PostRecovery PendingVisual | PASS | PendingVisual Expected=0 Actual=0 |
| PostRecovery EnergyCurrent | PASS | EnergyCurrent Expected=70.0 Actual=70.0 |
| PostRecovery PendingCue | PASS | PendingCue Expected=0 Actual=0 |
| PostRecovery PendingGECount | PASS | PendingGECount Expected=0 Actual=0 |
| PostRecovery CooldownGECount | PASS | CooldownGECount Expected=1 Actual=1 |
| PostRecovery PersistentGECount | PASS | PersistentGECount Expected=1 Actual=1 |
| PostRecovery CaughtUpKey | PASS | CaughtUpKey Expected=2 Actual=2 |
| PostRecovery EnergyBase | PASS | EnergyBase Expected=70.0 Actual=70.0 |
| PostRecovery ImmuneCount | PASS | ImmuneCount Expected=1 Actual=1 |
| PostRecovery CostGECount | PASS | CostGECount Expected=0 Actual=0 |
| PostRecovery TrialId | PASS | TrialId Expected=6001 Actual=6001 |
| PostRecovery ImmunityCooldown | PASS | ImmunityCooldown Expected=1 Actual=1 |
| Client event order is valid | PASS | 143908 < 144170 < 146239 < 148606 < 149592 < 149903 < 151968 < 154634 < 155246 < 155557 |
| Host event order is valid | PASS | 146196 < 146560 < 147829 |
| Pending Cue entered twice | PASS | OnActive=2 WhileActive=2 |
| Rejected Pending Cue emitted one Removed callback | PASS | Removed=1; accepted presentation is explicitly reconciled |
| Immunity Cue entered twice | PASS | OnActive=2 WhileActive=2 |
| Immunity Cue emitted reject cleanup and recovery expiry | PASS | Removed=2; first is reject cleanup, second is recovery natural expiry after checkpoint |
| Immunity Cue Removed callbacks are ordered by transaction | PASS | 148144 < Reject=148606; 155856 > RecoveryCheckpoint=155246 |
