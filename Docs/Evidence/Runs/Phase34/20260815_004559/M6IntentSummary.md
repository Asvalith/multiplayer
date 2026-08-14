# GAS M6 Intent verification: 20260815_004559

- Result: **PASS**
- Checks: 52 total, 52 passed, 0 failed
- Client results: 1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted
- Host commits: 1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0
- Host rejections: 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss
- Host dummy damage contexts: 2

## Evidence limitation

- GAS_DAMAGE_EXEC and GAS_DAMAGE_CONTEXT do not carry ShotId; attribution to Shot1 and Shot7 is verified by exact total counts and AuthorityTrace -> GAS_DAMAGE_EXEC -> GAS_DAMAGE_CONTEXT -> Committed ordering for each accepted transaction.

## Checks

| Check | Result | Details |
|---|---|---|
| Evidence file RunInfo.txt exists | PASS | E:\ueprojrct\multiplayer\Saved\GASBaseline\20260815_004559\RunInfo.txt |
| Evidence file Host.log exists | PASS | E:\ueprojrct\multiplayer\Saved\GASBaseline\20260815_004559\Host.log |
| Evidence file Client.log exists | PASS | E:\ueprojrct\multiplayer\Saved\GASBaseline\20260815_004559\Client.log |
| RunInfo Stage is M6Intent | PASS | Stage=M6Intent |
| RunInfo client auto sequence is enabled | PASS | ClientAutoSequence=True |
| RunInfo host reached ready state | PASS | HostReady=true |
| RunInfo client joined host | PASS | ClientJoined=true |
| Host has no fatal, critical, ensure, or assertion | PASS | Matches=0 |
| Client has no fatal, critical, ensure, or assertion | PASS | Matches=0 |
| M6Intent sequence completed with one client Pass | PASS | ClientPass=1 HostPass=0 |
| M6Intent sequence emitted no Fail | PASS | Fail=0 |
| Client emitted exactly eight intent results | PASS | Expected=8 Actual=8 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot1 Accepted exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot1 Duplicate exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot2 InvalidOrigin exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot3 InvalidDirection exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot4 InvalidTime exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot5 InvalidTime exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot6 Miss exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client result Shot7 Accepted exactly once | PASS | Expected=1 Actual=1 Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Client intent results are in the expected order | PASS | Events=1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted |
| Host emitted exactly two commits | PASS | Expected=2 Actual=2 Events=1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0 |
| Host committed Shot1 to dummy exactly once | PASS | ShotMatches=1 DummyMatches=1 Events=1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0 |
| Host committed Shot7 to dummy exactly once | PASS | ShotMatches=1 DummyMatches=1 Events=1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0 |
| Host committed no shot except Shot1 and Shot7 | PASS | Unexpected=0 Events=1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0 |
| Host commits are ordered Shot1 then Shot7 | PASS | Events=1:multiplayerGASTargetDummy_0, 7:multiplayerGASTargetDummy_0 |
| Host emitted exactly six semantic rejections | PASS | Expected=6 Actual=6 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot1 Duplicate exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot2 InvalidOrigin exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot3 InvalidDirection exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot4 InvalidTime exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot5 InvalidTime exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host rejection Shot6 Miss exactly once | PASS | Expected=1 Actual=1 Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host semantic rejections are in the expected order | PASS | Events=1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss |
| Host decision ability Spec is present and stable | PASS | Decisions=8 Specs=4 |
| Client emitted exactly eight state checkpoints | PASS | Expected=8 Actual=8 |
| Checkpoint ValidAccepted has exact result and state | PASS | Expected=Shot1/Accepted/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint DuplicateRejected has exact result and state | PASS | Expected=Shot1/Duplicate/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint OriginRejected has exact result and state | PASS | Expected=Shot2/InvalidOrigin/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint DirectionRejected has exact result and state | PASS | Expected=Shot3/InvalidDirection/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint StaleTimeRejected has exact result and state | PASS | Expected=Shot4/InvalidTime/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint FutureTimeRejected has exact result and state | PASS | Expected=Shot5/InvalidTime/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint MissRejected has exact result and state | PASS | Expected=Shot6/Miss/Energy90.0/Cooldown0 ActualMatches=1 |
| Checkpoint RecoveryAccepted has exact result and state | PASS | Expected=Shot7/Accepted/Energy80.0/Cooldown1 ActualMatches=1 |
| Client checkpoints are in the expected order | PASS | Events=ValidAccepted:Shot1:Accepted:E90.0:CD0, DuplicateRejected:Shot1:Duplicate:E90.0:CD0, OriginRejected:Shot2:InvalidOrigin:E90.0:CD0, DirectionRejected:Shot3:InvalidDirection:E90.0:CD0, StaleTimeRejected:Shot4:InvalidTime:E90.0:CD0, FutureTimeRejected:Shot5:InvalidTime:E90.0:CD0, MissRejected:Shot6:Miss:E90.0:CD0, RecoveryAccepted:Shot7:Accepted:E80.0:CD1 |
| Each client result precedes its state checkpoint | PASS | Pairs=8 |
| Host traced exactly the two accepted shots | PASS | TraceCount=2 Shots=1,7 |
| Host executed exactly two damage contexts and both target dummy | PASS | AllDamageContexts=2 DummyDamageContexts=2 |
| Host executed exactly two damage calculations | PASS | GAS_DAMAGE_EXEC=2 |
| Both dummy damage executions have positive damage | PASS | Damage=25.0,27.5 |
| Accepted Shot1 executed damage on dummy exactly once | PASS | Trace=1 Commit=1 DamageExecBetween=1 DummyDamageContextBetween=1 TargetAndHealthConsistent=True |
| Accepted Shot7 executed damage on dummy exactly once | PASS | Trace=1 Commit=1 DamageExecBetween=1 DummyDamageContextBetween=1 TargetAndHealthConsistent=True |
