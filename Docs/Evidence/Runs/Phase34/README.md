# Phase 3-4 network regression evidence

This archive keeps the small verifier outputs for the Stage 3-4 regression run. The original `Host.log`, `Client.log`, and `RunInfo.txt` files remain under `Saved/GASBaseline/<RunId>`; they are intentionally not duplicated here.

## Run matrix

| Run | Stage | Lag per direction | Approx. RTT | Loss per direction | Result | Evidence |
|---|---|---:|---:|---:|---|---|
| `20260815_002532` | M5 | 0 ms | 0 ms | 0% | Inventory generated; not a strict pass gate | [Markdown](20260815_002532/M5Summary.md), [JSON](20260815_002532/M5Summary.json) |
| `20260815_002809` | M6 | 0 ms | 0 ms | 0% | PASS, 95/95 strict checks | [Markdown](20260815_002809/M6Summary.md), [JSON](20260815_002809/M6Summary.json) |
| `20260815_002959` | M6Intent | 0 ms | 0 ms | 0% | PASS, 52/52 strict checks | [Markdown](20260815_002959/M6IntentSummary.md), [JSON](20260815_002959/M6IntentSummary.json) |
| `20260815_003155` | M6Intent | 150 ms | 300 ms | 0% | PASS, 52/52 strict checks | [Markdown](20260815_003155/M6IntentSummary.md), [JSON](20260815_003155/M6IntentSummary.json) |
| `20260815_004559` | M6Intent | 0 ms | 0 ms | 0% | **Final binary regression: PASS, 52/52 strict checks** | [Markdown](20260815_004559/M6IntentSummary.md), [JSON](20260815_004559/M6IntentSummary.json) |

All five runs used `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`, two game processes, automatic input sequences, and `Headless=True`. Both outgoing directions receive the listed lag, so the 150 ms/150 ms run is approximately 300 ms RTT.

`20260815_004559` is the final exact-binary regression executed after the AttributeSet finite-value/clamp guard and restart authorization gate were added. Its 52/52 result proves that the final binary preserved the M6Intent network contract; it does not by itself replace the numeric automation cases or an attended restart/UMG test.

## Source-file identity

The following SHA-256 values were calculated from the original local evidence files. They allow an archived summary to be matched to the exact raw files without committing the large logs.

| Run | File | SHA-256 |
|---|---|---|
| `20260815_002532` | `RunInfo.txt` | `CFB9A3FADD89153EE578811FDBF8348C0228B5D167577A2582A85BB66FDA233B` |
| `20260815_002532` | `Host.log` | `E47F2E08C2B612F6CF1AF4474A0BE016FB18AD7A53278452C76ADCF9C1268386` |
| `20260815_002532` | `Client.log` | `E386701BFD7132F98CE344FF06A4E421444EB883C5492E2D3DE48C5F94799E20` |
| `20260815_002809` | `RunInfo.txt` | `95008D4C991E552C1619AB8777FAB03E79BF1F2CD8D158E65C265BA3ECD4DB22` |
| `20260815_002809` | `Host.log` | `3CEABB0CC8AD7C4B3AF92E10DF040C1047656DD5B64305CC2F5A3B88D0625873` |
| `20260815_002809` | `Client.log` | `0155969CD08244581D85479B723C31499095B04EE2C019A440E161981C8D6C51` |
| `20260815_002959` | `RunInfo.txt` | `7739CCB9F837050B978971F958494F8E8760CB7B168EE568AC1EC15CD5D4732E` |
| `20260815_002959` | `Host.log` | `B17A4EB5366D2E2CB9F5B8DB02C97C03739A8BD38D7E199E2124CF4BE0BFB44F` |
| `20260815_002959` | `Client.log` | `EAE1DA45F5AA735959B85324C6C876F1A718917F1F4FE11482255C135483B41B` |
| `20260815_003155` | `RunInfo.txt` | `48D171A55FCF7968ABAD5CC2DD0B65D464472D8A6CC180307914BFC586E6DA48` |
| `20260815_003155` | `Host.log` | `7D346232CD1FA918B6E1CB9A124B082082847153F153C57E9BF813D281D9B61E` |
| `20260815_003155` | `Client.log` | `46449D15F70292D18F5DF725EFE8C809C528B2AE30A34BD90B60FFFD03FA7FF9` |
| `20260815_004559` | `RunInfo.txt` | `FF7D75846E30BFEB5960FEBAB2043022AEE8F967A127BF73FD07DF109A218A96` |
| `20260815_004559` | `Host.log` | `6AEAAB945244E3050CDFE02E6D8CFF1351EA907D0AE0E061CB69EA373172ED19` |
| `20260815_004559` | `Client.log` | `5518D1D4819A0935FD3C93D25E388F7E34E9BA19DFF1117500E637530D4A33C3` |

## What the tools prove

- `Scripts/VerifyGASM5Logs.ps1` produces an offline event inventory for an observed run. It is useful for counts and timing inspection, but it is not a strict acceptance gate.
- `Scripts/VerifyGASM6Logs.ps1` is a strict log gate for the prediction rejection, rollback cleanup, accepted recovery, state snapshots, event order, and GameplayCue reconciliation path.
- `Scripts/VerifyGASM6IntentLogs.ps1` is a strict log gate for server-side TargetData intent validation, duplicate rejection, accepted commits, unchanged state after rejection, and ordered recovery. The same 52 checks passed for both 0 ms runs and the approximately 300 ms RTT run.

## Evidence limits

- `Headless=True` validates code and log behavior, not UMG layout, mouse focus, animation, Niagara, audio, or two-window visual quality. Those require a separate attended run.
- These five runs contain no packet-loss case; every peer used `PktLoss=0`.
- The archived M6Intent summary records its known attribution limitation: damage/context lines do not contain `ShotId`, so attribution relies on count and event ordering.
- `RunInfo.txt` did not capture a source commit or executable hash. The hashes above prove raw-file identity, not complete binary-to-source provenance.
