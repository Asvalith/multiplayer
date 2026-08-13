# GAS M6 Intent verification: 20260813_155730

- Result: **PASS**
- Checks: 52 total, 52 passed, 0 failed
- Network: Listen Server + Client, both directions `PktLag=150`, `PktLoss=0`; approximate RTT 300ms
- Client results: `1:Accepted, 1:Duplicate, 2:InvalidOrigin, 3:InvalidDirection, 4:InvalidTime, 5:InvalidTime, 6:Miss, 7:Accepted`
- Host commits: Shot 1 and Shot 7, each exactly once, both to the target dummy
- Host rejections: Shot 1 Duplicate; Shot 2 InvalidOrigin; Shot 3 InvalidDirection; Shot 4/5 InvalidTime; Shot 6 Miss
- State checkpoints: rejected requests returned to Energy 90 / Cooldown 0; recovery ended at Energy 80 / Cooldown 1
- Damage evidence: exactly 2 `GAS_DAMAGE_EXEC` and exactly 2 dummy `GAS_DAMAGE_CONTEXT`

## Evidence limitation

`GAS_DAMAGE_EXEC` and `GAS_DAMAGE_CONTEXT` do not carry ShotId. Attribution to
Shot 1 and Shot 7 is verified by exact total counts and by requiring exactly one
`AuthorityTrace -> GAS_DAMAGE_EXEC -> GAS_DAMAGE_CONTEXT -> Committed` sequence
inside each accepted transaction interval.

The full 52-check table is generated locally by:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\VerifyGASM6IntentLogs.ps1 -RunId 20260813_155730
```
