param(
    [string]$ExePath = "C:\Users\youta\OneDrive\Desktop\2026\TR2\generated\outputs\Debug\GE3.exe",
    [string]$Scenario = "hybrid / fec g4",
    [string]$Seeds = "424242",
    [double]$DurationSec = 7.0,
    [double]$WarmupSec = 2.0,
    [int]$TimeoutSec = 90,
    [string]$OutputCsv = "logs\repair_budget_ab_latest.csv",
    [string]$AggregateCsv = "logs\repair_budget_ab_aggregate_latest.csv"
)

$ErrorActionPreference = "Stop"

$seedValues = $Seeds -split "," |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_.Length -gt 0 } |
    ForEach-Object { [int]$_ }

function Get-LatestNetworkCsv {
    Get-ChildItem -Path "logs" -Filter "network_*.csv" |
        Where-Object { $_.Name -notlike "network_summary_*" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-LatestLogCsv {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Filter
    )

    Get-ChildItem -Path "logs" -Filter $Filter |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-Percentile {
    param(
        [object[]]$Values,
        [double]$Percentile
    )

    $numbers =
        @($Values |
            Where-Object { $null -ne $_ } |
            ForEach-Object { [double]$_ } |
            Sort-Object)
    if ($numbers.Count -eq 0) {
        return 0
    }

    $index = [int][math]::Ceiling(
        ($Percentile / 100.0) * [double]$numbers.Count)
    $index = [math]::Max(0, [math]::Min($numbers.Count - 1, $index - 1))
    return $numbers[$index]
}

function Get-LateImpactSummary {
    param(
        [string]$OpportunityCsv,
        [string]$FrameRecoveryCsv,
        [int64]$RawLateCompleted
    )

    $fallback = {
        param([string]$Reason)
        [pscustomobject]@{
            LateCompletedTraceRows = $RawLateCompleted
            BenignCompletedEarly = 0
            DeadlineEstimateRisk = 0
            HarmfulActionableLate = $RawLateCompleted
            PolicyExcludedLate = 0
            UnknownLate = $RawLateCompleted
            AdjustedActionableLate = $RawLateCompleted
            DeadlineDeficitAvgMs = 0.0
            DeadlineDeficitP95Ms = 0.0
            TraceAdjustmentReason = $Reason
        }
    }

    if ([string]::IsNullOrWhiteSpace($OpportunityCsv) -or
        [string]::IsNullOrWhiteSpace($FrameRecoveryCsv) -or
        -not (Test-Path $OpportunityCsv) -or
        -not (Test-Path $FrameRecoveryCsv)) {
        return & $fallback "missing-trace"
    }

    $opportunities = Import-Csv $OpportunityCsv
    $opportunityByKey = @{}
    foreach ($row in $opportunities) {
        $key = "$($row.streamId)|$($row.frameId)|$($row.chunkIndex)"
        if (!$opportunityByKey.ContainsKey($key)) {
            $opportunityByKey[$key] =
                New-Object System.Collections.Generic.List[object]
        }
        $opportunityByKey[$key].Add($row)
    }

    $lateRows =
        @(Import-Csv $FrameRecoveryCsv |
            Where-Object { $_.eventName -eq "retransmit-late-after-completed" })

    $benign = 0L
    $deadlineRisk = 0L
    $harmful = 0L
    $policyExcluded = 0L
    $unknown = 0L
    $deadlineDeficits = @()

    foreach ($late in $lateRows) {
        $key = "$($late.streamId)|$($late.frameId)|$($late.eventPacketChunkIndex)"
        $match = $null
        if ($opportunityByKey.ContainsKey($key)) {
            $lateTime = [int64]$late.eventTimeUs
            $matches =
                $opportunityByKey[$key] |
                Where-Object { [int64]$_.eventTimeUs -le $lateTime } |
                Sort-Object {[int64]$_.eventTimeUs} -Descending
            if ($matches.Count -eq 0) {
                $matches =
                    $opportunityByKey[$key] |
                    Sort-Object {[int64]$_.eventTimeUs} -Descending
            }
            $match = $matches | Select-Object -First 1
        }

        if ($null -eq $match) {
            $unknown++
            $harmful++
            continue
        }

        $reason =
            if ($match.PSObject.Properties.Name -contains "guardExclusionReason") {
                $match.guardExclusionReason
            }
            else {
                "unknown"
            }

        $estimatedDeliveryUs =
            if ($match.PSObject.Properties.Name -contains "estimatedDeliveryUs") {
                [int64]$match.estimatedDeliveryUs
            }
            else {
                0L
            }
        $usableSlackUs =
            if ($match.PSObject.Properties.Name -contains "usableSlackUs") {
                [int64]$match.usableSlackUs
            }
            else {
                0L
            }
        $deficitUs = [math]::Max(0L, $estimatedDeliveryUs - $usableSlackUs)

        if ($reason -eq "keyframe-slack-too-tight") {
            $deadlineRisk++
            $deadlineDeficits += $deficitUs
        }
        elseif ($reason -eq "keyframe-slack-too-wide" -or
            $reason -eq "keyframe-upper-slack-small-frame") {
            $benign++
        }
        elseif ($reason -eq "keyframe" -or
            $reason -eq "fec-unlikely-maintain-repair" -or
            $reason -eq "profile-not-jitter-ack-loss") {
            $policyExcluded++
        }
        else {
            $harmful++
        }
    }

    $adjustedActionable = $deadlineRisk + $harmful
    [pscustomobject]@{
        LateCompletedTraceRows = $lateRows.Count
        BenignCompletedEarly = $benign
        DeadlineEstimateRisk = $deadlineRisk
        HarmfulActionableLate = $harmful
        PolicyExcludedLate = $policyExcluded
        UnknownLate = $unknown
        AdjustedActionableLate = $adjustedActionable
        DeadlineDeficitAvgMs = if ($deadlineDeficits.Count -gt 0) {
            [math]::Round(
                (($deadlineDeficits | Measure-Object -Average).Average / 1000.0),
                3)
        } else { 0.0 }
        DeadlineDeficitP95Ms = [math]::Round(
            (Get-Percentile $deadlineDeficits 95) / 1000.0,
            3)
        TraceAdjustmentReason = "trace-classified"
    }
}

function Run-Variant {
    param(
        [string]$Variant,
        [string]$Stateful,
        [int]$Seed
    )

    $env:RNVP_CODEC = "h264"
    $env:TR2_NETWORK_EXPERIMENT_AUTO = "1"
    $env:TR2_NETWORK_EXPERIMENT_SCENARIO = $Scenario
    $env:TR2_NETWORK_EXPERIMENT_DURATION_SEC = [string]$DurationSec
    $env:TR2_NETWORK_EXPERIMENT_WARMUP_SEC = [string]$WarmupSec
    $env:TR2_NETWORK_SIM_SEED = [string]$Seed
    $env:RNVP_REPAIR_BUDGET_STATEFUL = $Stateful
    $env:RNVP_REPAIR_RACE_OPPORTUNITY_TRACE = "1"

    $before = Get-LatestNetworkCsv
    $beforeOpportunity = Get-LatestLogCsv -Filter "repair_race_opportunity_trace_*.csv"
    $beforeFrameRecovery = Get-LatestLogCsv -Filter "frame_recovery_trace_*.csv"
    $process = Start-Process `
        -FilePath $ExePath `
        -WorkingDirectory (Get-Location).Path `
        -PassThru `
        -WindowStyle Hidden

    Wait-Process -Id $process.Id -Timeout $TimeoutSec -ErrorAction SilentlyContinue
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        throw "Variant '$Variant' timed out after $TimeoutSec seconds."
    }

    $after = Get-LatestNetworkCsv
    if ($null -eq $after -or ($null -ne $before -and $after.FullName -eq $before.FullName)) {
        throw "Variant '$Variant' did not produce a new network CSV."
    }
    $afterOpportunity = Get-LatestLogCsv -Filter "repair_race_opportunity_trace_*.csv"
    $afterFrameRecovery = Get-LatestLogCsv -Filter "frame_recovery_trace_*.csv"

    $opportunityCsv = ""
    if ($null -ne $afterOpportunity -and
        ($null -eq $beforeOpportunity -or
            $afterOpportunity.FullName -ne $beforeOpportunity.FullName)) {
        $opportunityCsv = $afterOpportunity.FullName
    }

    $frameRecoveryCsv = ""
    if ($null -ne $afterFrameRecovery -and
        ($null -eq $beforeFrameRecovery -or
            $afterFrameRecovery.FullName -ne $beforeFrameRecovery.FullName)) {
        $frameRecoveryCsv = $afterFrameRecovery.FullName
    }

    [pscustomobject]@{
        Variant = $Variant
        Seed = $Seed
        Csv = $after.FullName
        OpportunityCsv = $opportunityCsv
        FrameRecoveryCsv = $frameRecoveryCsv
        ExitCode = $process.ExitCode
    }
}

function Summarize-Csv {
    param(
        [string]$Path,
        [string]$Variant
    )

    Import-Csv $Path |
        Where-Object { $_.scenarioName -and $_.scenarioName -ne "Auto" } |
        Group-Object scenarioName |
        ForEach-Object {
            $rows = $_.Group
            $last = $rows[-1]
            $hasEvaluationOutputDrops =
                $last.PSObject.Properties.Name -contains "evaluationOutputQueueDroppedFrames"
            $hasStartupOutputDrops =
                $last.PSObject.Properties.Name -contains "startupOutputQueueDroppedFrames"
            $rawOutputDrops = [int64]$last.outputQueueDroppedFrames
            $evaluationOutputDrops =
                if ($hasEvaluationOutputDrops -and
                    $last.evaluationOutputQueueDroppedFrames -ne "") {
                    [int64]$last.evaluationOutputQueueDroppedFrames
                }
                else {
                    $rawOutputDrops
                }
            $startupOutputDrops =
                if ($hasStartupOutputDrops -and
                    $last.startupOutputQueueDroppedFrames -ne "") {
                    [int64]$last.startupOutputQueueDroppedFrames
                }
                else {
                    0
                }
            [pscustomobject]@{
                Variant = $Variant
                Seed = 0
                Scenario = $_.Name
                Samples = $rows.Count
                DisplayFpsAvg = [math]::Round((($rows | Measure-Object displayFps -Average).Average), 3)
                LatencyAvgMs = [math]::Round((($rows | Measure-Object averageLatencyMs -Average).Average), 3)
                JitterAvgMs = [math]::Round((($rows | Measure-Object currentJitterMs -Average).Average), 3)
                AckRetransChunks = [int64]$last.ackRetransmittedChunks
                RepairCanceledByCompleteAck = [int64]$last.repairCanceledByCompleteAckPackets
                RepairQueuedCanceled = [int64]$last.repairQueuedButCanceledPackets
                RepairSentAfterCompleteAck = [int64]$last.repairSentAfterCompleteAckPackets
                RepairSentAfterCompleteAckLarge = [int64]$last.repairSentAfterCompleteAckLargePackets
                RepairBudgetSuppressed = [int64]$last.repairBudgetSuppressedPackets
                RepairBudgetLargeSuppressed = [int64]$last.repairBudgetSuppressedLargePackets
                RepairRaceGuardSuppressed = [int64]$last.repairRaceGuardSuppressedPackets
                RepairRaceGuardLargeSuppressed = [int64]$last.repairRaceGuardSuppressedLargePackets
                FecLikelySuppressed = [int64]$last.repairSuppressedByFecLikelyPackets
                FecLikelyLargeSuppressed = [int64]$last.repairSuppressedByFecLikelyLargePackets
                FecLikelyExpired = [int64]$last.repairFecLikelySuppressedExpiredPackets
                RepairProfile = $last.repairBudgetProfile
                RepairSwitches = [int64]$last.repairBudgetProfileSwitches
                DuplicatePackets = [int64]$last.duplicatePackets
                LateCompletedRetrans = [int64]$last.duplicateLateAfterCompletedRetransmitPackets
                RetransLateCompleted = [int64]$last.retransmitLateAfterCompletedPackets
                RetransLateCompletedLarge = [int64]$last.retransmitLateAfterCompletedLargePackets
                RetransLateCompletedSentBeforeComplete = [int64]$last.retransmitLateAfterCompletedSentBeforeCompletePackets
                RetransLateCompletedSentAfterComplete = [int64]$last.retransmitLateAfterCompletedSentAfterCompletePackets
                RetransLateCompletedAvgSendToCompleteMs = [double]$last.retransmitLateAfterCompletedAvgSendToCompleteMs
                RetransLateCompletedAvgDelayMs = [double]$last.retransmitLateAfterCompletedAvgDelayMs
                RetransLateCompletedMaxDelayMs = [double]$last.retransmitLateAfterCompletedMaxDelayMs
                DeadlineDrops = [int64]$last.deadlineDroppedFrames
                OutputDrops = $evaluationOutputDrops
                OutputDropsRaw = $rawOutputDrops
                StartupOutputDrops = $startupOutputDrops
                Displayed = [int64]$last.displayedFrames
                FecRecovered = [int64]$last.fecRecoveredFrames
                PacingMaxMs = [math]::Round((($rows | Measure-Object pacingCurrentQueueDelayMs -Maximum).Maximum), 3)
            }
        }
}

New-Item -ItemType Directory -Force -Path "logs" | Out-Null

$allDiffs = foreach ($seed in $seedValues) {
    $off = Run-Variant -Variant "stateful_off" -Stateful "0" -Seed $seed
    $on = Run-Variant -Variant "stateful_on" -Stateful "1" -Seed $seed

    $offSummary = Summarize-Csv -Path $off.Csv -Variant $off.Variant
    $onSummary = Summarize-Csv -Path $on.Csv -Variant $on.Variant

    foreach ($onRow in $onSummary) {
        $offRow = $offSummary | Where-Object Scenario -eq $onRow.Scenario | Select-Object -First 1
        if ($null -eq $offRow) {
            continue
        }
        $offLateImpact = Get-LateImpactSummary `
            -OpportunityCsv $off.OpportunityCsv `
            -FrameRecoveryCsv $off.FrameRecoveryCsv `
            -RawLateCompleted $offRow.RetransLateCompleted
        $onLateImpact = Get-LateImpactSummary `
            -OpportunityCsv $on.OpportunityCsv `
            -FrameRecoveryCsv $on.FrameRecoveryCsv `
            -RawLateCompleted $onRow.RetransLateCompleted

        [pscustomobject]@{
            Seed = $seed
            OffCsv = $off.Csv
            OnCsv = $on.Csv
            OffOpportunityCsv = $off.OpportunityCsv
            OnOpportunityCsv = $on.OpportunityCsv
            OffFrameRecoveryCsv = $off.FrameRecoveryCsv
            OnFrameRecoveryCsv = $on.FrameRecoveryCsv
            Scenario = $onRow.Scenario
            SentOff = $offRow.AckRetransChunks
            SentOn = $onRow.AckRetransChunks
            SentDelta = $onRow.AckRetransChunks - $offRow.AckRetransChunks
            CompleteAckCanceledDelta = $onRow.RepairCanceledByCompleteAck - $offRow.RepairCanceledByCompleteAck
            QueuedCanceledDelta = $onRow.RepairQueuedCanceled - $offRow.RepairQueuedCanceled
            SentAfterCompleteAckDelta = $onRow.RepairSentAfterCompleteAck - $offRow.RepairSentAfterCompleteAck
            SentAfterCompleteAckLargeDelta = $onRow.RepairSentAfterCompleteAckLarge - $offRow.RepairSentAfterCompleteAckLarge
            BudgetSuppOn = $onRow.RepairBudgetSuppressed
            BudgetLargeSuppOn = $onRow.RepairBudgetLargeSuppressed
            RaceGuardSuppOn = $onRow.RepairRaceGuardSuppressed
            RaceGuardLargeSuppOn = $onRow.RepairRaceGuardLargeSuppressed
            FecLikelySuppDelta = $onRow.FecLikelySuppressed - $offRow.FecLikelySuppressed
            FecLikelyLargeSuppDelta = $onRow.FecLikelyLargeSuppressed - $offRow.FecLikelyLargeSuppressed
            FecLikelyExpiredDelta = $onRow.FecLikelyExpired - $offRow.FecLikelyExpired
            DupDelta = $onRow.DuplicatePackets - $offRow.DuplicatePackets
            LateRetransDelta = $onRow.LateCompletedRetrans - $offRow.LateCompletedRetrans
            RetransLateCompletedDelta = $onRow.RetransLateCompleted - $offRow.RetransLateCompleted
            RetransLateCompletedLargeDelta = $onRow.RetransLateCompletedLarge - $offRow.RetransLateCompletedLarge
            RetransLateCompletedSentBeforeCompleteDelta = $onRow.RetransLateCompletedSentBeforeComplete - $offRow.RetransLateCompletedSentBeforeComplete
            RetransLateCompletedSentAfterCompleteDelta = $onRow.RetransLateCompletedSentAfterComplete - $offRow.RetransLateCompletedSentAfterComplete
            RetransLateCompletedAvgSendToCompleteDelta = [math]::Round($onRow.RetransLateCompletedAvgSendToCompleteMs - $offRow.RetransLateCompletedAvgSendToCompleteMs, 3)
            RetransLateCompletedAvgDelayDelta = [math]::Round($onRow.RetransLateCompletedAvgDelayMs - $offRow.RetransLateCompletedAvgDelayMs, 3)
            RetransLateCompletedMaxDelayDelta = [math]::Round($onRow.RetransLateCompletedMaxDelayMs - $offRow.RetransLateCompletedMaxDelayMs, 3)
            AdjustedActionableLateOff = $offLateImpact.AdjustedActionableLate
            AdjustedActionableLateOn = $onLateImpact.AdjustedActionableLate
            AdjustedActionableLateDelta = $onLateImpact.AdjustedActionableLate - $offLateImpact.AdjustedActionableLate
            BenignCompletedEarlyLateOff = $offLateImpact.BenignCompletedEarly
            BenignCompletedEarlyLateOn = $onLateImpact.BenignCompletedEarly
            BenignCompletedEarlyLateDelta = $onLateImpact.BenignCompletedEarly - $offLateImpact.BenignCompletedEarly
            DeadlineEstimateRiskLateOff = $offLateImpact.DeadlineEstimateRisk
            DeadlineEstimateRiskLateOn = $onLateImpact.DeadlineEstimateRisk
            DeadlineEstimateRiskLateDelta = $onLateImpact.DeadlineEstimateRisk - $offLateImpact.DeadlineEstimateRisk
            HarmfulActionableLateOff = $offLateImpact.HarmfulActionableLate
            HarmfulActionableLateOn = $onLateImpact.HarmfulActionableLate
            HarmfulActionableLateDelta = $onLateImpact.HarmfulActionableLate - $offLateImpact.HarmfulActionableLate
            LateDeadlineDeficitAvgDelta = [math]::Round($onLateImpact.DeadlineDeficitAvgMs - $offLateImpact.DeadlineDeficitAvgMs, 3)
            LateDeadlineDeficitP95Delta = [math]::Round($onLateImpact.DeadlineDeficitP95Ms - $offLateImpact.DeadlineDeficitP95Ms, 3)
            OffLateTraceAdjustment = $offLateImpact.TraceAdjustmentReason
            OnLateTraceAdjustment = $onLateImpact.TraceAdjustmentReason
            DeadlineDropDelta = $onRow.DeadlineDrops - $offRow.DeadlineDrops
            OutputDropDelta = $onRow.OutputDrops - $offRow.OutputDrops
            DisplayedDelta = $onRow.Displayed - $offRow.Displayed
            FecRecoveredDelta = $onRow.FecRecovered - $offRow.FecRecovered
            FpsDelta = [math]::Round($onRow.DisplayFpsAvg - $offRow.DisplayFpsAvg, 3)
            LatencyDelta = [math]::Round($onRow.LatencyAvgMs - $offRow.LatencyAvgMs, 3)
            JitterDelta = [math]::Round($onRow.JitterAvgMs - $offRow.JitterAvgMs, 3)
            PacingMaxDelta = [math]::Round($onRow.PacingMaxMs - $offRow.PacingMaxMs, 3)
            OnProfile = $onRow.RepairProfile
            OnSwitches = $onRow.RepairSwitches
        }
    }
}

$aggregate = $allDiffs |
    Group-Object Scenario |
    ForEach-Object {
        $rows = $_.Group
        $displayedAvg = ($rows | Measure-Object DisplayedDelta -Average).Average
        $fpsAvg = ($rows | Measure-Object FpsDelta -Average).Average
        $deadlineAvg = ($rows | Measure-Object DeadlineDropDelta -Average).Average
        $outputAvg = ($rows | Measure-Object OutputDropDelta -Average).Average
        $dupAvg = ($rows | Measure-Object DupDelta -Average).Average
        $lateAvg = ($rows | Measure-Object LateRetransDelta -Average).Average
        $adjustedActionableLateAvg =
            ($rows | Measure-Object AdjustedActionableLateDelta -Average).Average
        $benignLateAvg =
            ($rows | Measure-Object BenignCompletedEarlyLateDelta -Average).Average

        $decision = "watch"
        if ($displayedAvg -lt 0 -or
            $fpsAvg -lt -1.0 -or
            $deadlineAvg -gt 0 -or
            $outputAvg -gt 0) {
            $decision = "reject-or-retune"
        }
        elseif ($adjustedActionableLateAvg -gt 5) {
            $decision = "watch-adjusted-late"
        }
        elseif ($lateAvg -gt 0 -and $benignLateAvg -ge $adjustedActionableLateAvg) {
            $decision = "accept-benign-late"
        }
        else {
            $decision = "accept"
        }

        [pscustomobject]@{
            Scenario = $_.Name
            Runs = $rows.Count
            SentDeltaAvg = [math]::Round((($rows | Measure-Object SentDelta -Average).Average), 3)
            CompleteAckCanceledDeltaAvg = [math]::Round((($rows | Measure-Object CompleteAckCanceledDelta -Average).Average), 3)
            QueuedCanceledDeltaAvg = [math]::Round((($rows | Measure-Object QueuedCanceledDelta -Average).Average), 3)
            SentAfterCompleteAckDeltaAvg = [math]::Round((($rows | Measure-Object SentAfterCompleteAckDelta -Average).Average), 3)
            SentAfterCompleteAckLargeDeltaAvg = [math]::Round((($rows | Measure-Object SentAfterCompleteAckLargeDelta -Average).Average), 3)
            BudgetSuppAvg = [math]::Round((($rows | Measure-Object BudgetSuppOn -Average).Average), 3)
            BudgetLargeSuppAvg = [math]::Round((($rows | Measure-Object BudgetLargeSuppOn -Average).Average), 3)
            RaceGuardSuppAvg = [math]::Round((($rows | Measure-Object RaceGuardSuppOn -Average).Average), 3)
            RaceGuardLargeSuppAvg = [math]::Round((($rows | Measure-Object RaceGuardLargeSuppOn -Average).Average), 3)
            FecLikelySuppDeltaAvg = [math]::Round((($rows | Measure-Object FecLikelySuppDelta -Average).Average), 3)
            FecLikelyLargeSuppDeltaAvg = [math]::Round((($rows | Measure-Object FecLikelyLargeSuppDelta -Average).Average), 3)
            FecLikelyExpiredDeltaAvg = [math]::Round((($rows | Measure-Object FecLikelyExpiredDelta -Average).Average), 3)
            DupDeltaAvg = [math]::Round($dupAvg, 3)
            LateRetransDeltaAvg = [math]::Round($lateAvg, 3)
            RetransLateCompletedDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedDelta -Average).Average), 3)
            RetransLateCompletedLargeDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedLargeDelta -Average).Average), 3)
            RetransLateCompletedSentBeforeCompleteDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedSentBeforeCompleteDelta -Average).Average), 3)
            RetransLateCompletedSentAfterCompleteDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedSentAfterCompleteDelta -Average).Average), 3)
            RetransLateCompletedAvgSendToCompleteDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedAvgSendToCompleteDelta -Average).Average), 3)
            RetransLateCompletedAvgDelayDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedAvgDelayDelta -Average).Average), 3)
            RetransLateCompletedMaxDelayDeltaAvg = [math]::Round((($rows | Measure-Object RetransLateCompletedMaxDelayDelta -Average).Average), 3)
            AdjustedActionableLateDeltaAvg = [math]::Round($adjustedActionableLateAvg, 3)
            BenignCompletedEarlyLateDeltaAvg = [math]::Round($benignLateAvg, 3)
            DeadlineEstimateRiskLateDeltaAvg = [math]::Round((($rows | Measure-Object DeadlineEstimateRiskLateDelta -Average).Average), 3)
            HarmfulActionableLateDeltaAvg = [math]::Round((($rows | Measure-Object HarmfulActionableLateDelta -Average).Average), 3)
            LateDeadlineDeficitAvgDeltaAvg = [math]::Round((($rows | Measure-Object LateDeadlineDeficitAvgDelta -Average).Average), 3)
            LateDeadlineDeficitP95DeltaAvg = [math]::Round((($rows | Measure-Object LateDeadlineDeficitP95Delta -Average).Average), 3)
            DeadlineDropDeltaAvg = [math]::Round($deadlineAvg, 3)
            OutputDropDeltaAvg = [math]::Round($outputAvg, 3)
            DisplayedDeltaAvg = [math]::Round($displayedAvg, 3)
            FecRecoveredDeltaAvg = [math]::Round((($rows | Measure-Object FecRecoveredDelta -Average).Average), 3)
            FpsDeltaAvg = [math]::Round($fpsAvg, 3)
            LatencyDeltaAvg = [math]::Round((($rows | Measure-Object LatencyDelta -Average).Average), 3)
            JitterDeltaAvg = [math]::Round((($rows | Measure-Object JitterDelta -Average).Average), 3)
            PacingMaxDeltaAvg = [math]::Round((($rows | Measure-Object PacingMaxDelta -Average).Average), 3)
            Profiles = (($rows | Select-Object -ExpandProperty OnProfile -Unique) -join "|")
            Decision = $decision
        }
    }

$allDiffs | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $OutputCsv
$aggregate | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $AggregateCsv
$aggregate | Format-Table -AutoSize

[pscustomobject]@{
    DiffCsv = (Resolve-Path $OutputCsv).Path
    AggregateCsv = (Resolve-Path $AggregateCsv).Path
}
