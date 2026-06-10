param(
    [string]$OpportunityCsv = "",
    [string]$FrameRecoveryCsv = "",
    [string]$DiffCsv = "",
    [string]$OutputCsv = "logs\repair_race_opportunity_analysis_latest.csv"
)

function Get-LatestFile {
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

if ([string]::IsNullOrWhiteSpace($OpportunityCsv)) {
    $latest = Get-LatestFile -Filter "repair_race_opportunity_trace_*.csv"
    if ($null -eq $latest) {
        throw "repair_race_opportunity_trace_*.csv was not found."
    }
    $OpportunityCsv = $latest.FullName
}

if ([string]::IsNullOrWhiteSpace($FrameRecoveryCsv)) {
    $latest = Get-LatestFile -Filter "frame_recovery_trace_*.csv"
    if ($null -eq $latest) {
        throw "frame_recovery_trace_*.csv was not found."
    }
    $FrameRecoveryCsv = $latest.FullName
}

$opportunities = Import-Csv $OpportunityCsv
$opportunityByKey = @{}
foreach ($row in $opportunities) {
    $key = "$($row.streamId)|$($row.frameId)|$($row.chunkIndex)"
    if (!$opportunityByKey.ContainsKey($key)) {
        $opportunityByKey[$key] = New-Object System.Collections.Generic.List[object]
    }
    $opportunityByKey[$key].Add($row)
}

$lateRows =
    Import-Csv $FrameRecoveryCsv |
    Where-Object { $_.eventName -eq "retransmit-late-after-completed" }

$classified = foreach ($late in $lateRows) {
    $key = "$($late.streamId)|$($late.frameId)|$($late.eventPacketChunkIndex)"
    $matches = @()
    if ($opportunityByKey.ContainsKey($key)) {
        $lateTime = [int64]$late.eventTimeUs
        $matches =
            $opportunityByKey[$key] |
            Where-Object { [int64]$_.eventTimeUs -le $lateTime } |
            Sort-Object {[int64]$_.eventTimeUs} -Descending
        if ($matches.Count -eq 0) {
            $matches = $opportunityByKey[$key] |
                Sort-Object {[int64]$_.eventTimeUs} -Descending
        }
    }

    $match = $matches | Select-Object -First 1
    $classification = "no-opportunity-log"
    $largeMissingFecClass = "not-large-5-13"
    $guardExclusionReason = "no-opportunity-log"
    $lateImpactClass = "unknown-late"
    $estimatedDeliveryUs = 0L
    $usableSlackUsForOutput = 0L
    $deliverySlackDeficitUs = 0L
    if ($null -ne $match) {
        $guardCandidate = [int]$match.guardCandidate -ne 0
        $guardApplied = [int]$match.guardApplied -ne 0
        $deliveryWindow = [int]$match.deliveryWindow -ne 0
        $slackWindow = [int]$match.slackWindow -ne 0
        $lowerSlackWindow =
            $match.PSObject.Properties.Name -contains "lowerSlackWindow" -and
            [int]$match.lowerSlackWindow -ne 0
        $upperSafeSlackWindow =
            $match.PSObject.Properties.Name -contains "upperSafeSlackWindow" -and
            [int]$match.upperSafeSlackWindow -ne 0
        $missingWindow = [int]$match.missingWindow -ne 0
        $keyFrame = [int]$match.keyFrame -ne 0
        $usableSlackUs = [int64]$match.usableSlackUs
        $estimatedDeliveryUs = [int64]$match.estimatedDeliveryUs
        $usableSlackUsForOutput = $usableSlackUs
        $deliverySlackDeficitUs =
            [math]::Max(0L, $estimatedDeliveryUs - $usableSlackUs)
        $tightSlackUs = if (
            $match.PSObject.Properties.Name -contains "tightSlackUs"
        ) { [int64]$match.tightSlackUs } else { 0 }
        $lowSlackUs = if (
            $match.PSObject.Properties.Name -contains "lowSlackUs"
        ) { [int64]$match.lowSlackUs } else { 0 }
        $fecClass =
            if ($match.PSObject.Properties.Name -contains "fecLikelyClass") {
                $match.fecLikelyClass
            }
            else {
                "fec-unknown"
            }
        $isLargeFiveToThirteen =
            [int]$late.largeFrame -ne 0 -and
            [int]$match.ackMissingChunks -ge 5 -and
            [int]$match.ackMissingChunks -le 13
        if ($isLargeFiveToThirteen) {
            $largeMissingFecClass = "large-5-13-$fecClass"
        }
        $guardExclusionReason =
            if ($match.PSObject.Properties.Name -contains "guardExclusionReason") {
                $match.guardExclusionReason
            }
            elseif ($guardCandidate) {
                "candidate"
            }
            elseif ($keyFrame) {
                "keyframe"
            }
            elseif (!$deliveryWindow) {
                "delivery-window"
            }
            elseif (!$slackWindow) {
                if ($tightSlackUs -gt 0 -and $usableSlackUs -le $tightSlackUs) {
                    "slack-too-tight"
                }
                elseif ($lowSlackUs -gt 0 -and $usableSlackUs -gt $lowSlackUs) {
                    "slack-too-wide-or-small-frame"
                }
                else {
                    "slack-window"
                }
            }
            elseif (!$missingWindow) {
                if (
                    $isLargeFiveToThirteen -and
                    $fecClass -eq "fec-unlikely-multimissing"
                ) {
                    "fec-unlikely-maintain-repair"
                }
                else {
                    "missing-window"
                }
            }
            else {
                "unknown"
            }

        if ($guardApplied) {
            $classification =
                if ($upperSafeSlackWindow) {
                    "guard-active-upper-safe-sent-late"
                }
                elseif ($lowerSlackWindow) {
                    "guard-active-lower-sent-late"
                }
                else {
                    "guard-active-sent-late"
                }
        }
        elseif ($guardCandidate) {
            $classification = "candidate-not-guarded-late"
        }
        elseif ($guardExclusionReason -eq "keyframe") {
            $classification = "guard-excluded-keyframe-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-delivery-window") {
            $classification = "guard-excluded-keyframe-delivery-window-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-slack-window") {
            $classification = "guard-excluded-keyframe-slack-window-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-slack-too-tight") {
            $classification = "guard-excluded-keyframe-slack-too-tight-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-slack-too-wide") {
            $classification = "guard-excluded-keyframe-slack-too-wide-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-upper-slack-small-frame") {
            $classification = "guard-excluded-keyframe-upper-slack-small-frame-late"
        }
        elseif ($guardExclusionReason -eq "keyframe-missing-window") {
            $classification = "guard-excluded-keyframe-missing-window-late"
        }
        elseif ($guardExclusionReason -eq "fec-unlikely-maintain-repair") {
            $classification = "guard-excluded-fec-unlikely-maintain-repair-late"
        }
        elseif ($guardExclusionReason -eq "profile-not-jitter-ack-loss") {
            $classification = "guard-excluded-profile-late"
        }
        elseif ($guardExclusionReason -eq "delivery-window") {
            $classification = "missed-delivery-window-late"
        }
        elseif (
            $guardExclusionReason -eq "slack-too-tight" -or
            $guardExclusionReason -eq "slack-too-wide" -or
            $guardExclusionReason -eq "upper-slack-small-frame" -or
            $guardExclusionReason -eq "slack-too-wide-or-small-frame" -or
            $guardExclusionReason -eq "slack-window"
        ) {
            $classification = "missed-slack-window-late"
        }
        elseif ($guardExclusionReason -eq "missing-window") {
            $classification = "missed-missing-window-late"
        }
        else {
            $classification = "guard-excluded-unknown-late"
        }

        if ($classification -eq "guard-excluded-keyframe-slack-too-tight-late") {
            $lateImpactClass = "deadline-estimate-risk"
        }
        elseif (
            $classification -eq "guard-excluded-keyframe-slack-too-wide-late" -or
            $classification -eq "guard-excluded-keyframe-upper-slack-small-frame-late"
        ) {
            $lateImpactClass = "benign-keyframe-completed-early"
        }
        elseif (
            $classification -eq "guard-active-upper-safe-sent-late" -or
            $classification -eq "guard-active-lower-sent-late" -or
            $classification -eq "guard-active-sent-late" -or
            $classification -eq "candidate-not-guarded-late" -or
            $classification -eq "missed-delivery-window-late" -or
            $classification -eq "missed-slack-window-late" -or
            $classification -eq "missed-missing-window-late" -or
            $classification -eq "guard-excluded-keyframe-missing-window-late"
        ) {
            $lateImpactClass = "harmful-or-actionable-late"
        }
        else {
            $lateImpactClass = "policy-excluded-late"
        }
    }

    [pscustomobject]@{
        Classification = $classification
        FrameId = [uint32]$late.frameId
        StreamId = [uint32]$late.streamId
        ChunkIndex = [uint32]$late.eventPacketChunkIndex
        LargeFrame = [int]$late.largeFrame
        LateEventTimeUs = [int64]$late.eventTimeUs
        PacketSendTimeUs = [int64]$late.sendTimeUs
        CompletedFirstReceiveTimeUs = [int64]$late.firstReceiveTimeUs
        OpportunityEventTimeUs = if ($null -ne $match) { [int64]$match.eventTimeUs } else { 0 }
        OpportunityEventName = if ($null -ne $match) { $match.eventName } else { "" }
        KeyFrame = if ($null -ne $match) { [int]$match.keyFrame } else { [int]$late.keyFrame }
        GuardCandidate = if ($null -ne $match) { [int]$match.guardCandidate } else { 0 }
        GuardApplied = if ($null -ne $match) { [int]$match.guardApplied } else { 0 }
        GuardExclusionReason = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "guardExclusionReason"
        ) { $match.guardExclusionReason } elseif ($null -ne $match) { $guardExclusionReason } else { "no-opportunity-log" }
        LateImpactClass = $lateImpactClass
        DeliveryWindow = if ($null -ne $match) { [int]$match.deliveryWindow } else { 0 }
        SlackWindow = if ($null -ne $match) { [int]$match.slackWindow } else { 0 }
        LowerSlackWindow = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "lowerSlackWindow"
        ) { [int]$match.lowerSlackWindow } else { 0 }
        UpperSafeSlackWindow = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "upperSafeSlackWindow"
        ) { [int]$match.upperSafeSlackWindow } else { 0 }
        MissingWindow = if ($null -ne $match) { [int]$match.missingWindow } else { 0 }
        FecEnabled = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecEnabled"
        ) { [int]$match.fecEnabled } else { 0 }
        FecGroupChunkCount = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecGroupChunkCount"
        ) { [uint32]$match.fecGroupChunkCount } else { 0 }
        FecMissingGroups = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecMissingGroups"
        ) { [uint32]$match.fecMissingGroups } else { 0 }
        FecSingletonMissingGroups = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecSingletonMissingGroups"
        ) { [uint32]$match.fecSingletonMissingGroups } else { 0 }
        FecMultiMissingGroups = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecMultiMissingGroups"
        ) { [uint32]$match.fecMultiMissingGroups } else { 0 }
        FecLikelyRecoverableChunks = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecLikelyRecoverableChunks"
        ) { [uint32]$match.fecLikelyRecoverableChunks } else { 0 }
        FecRepairNeededChunks = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecRepairNeededChunks"
        ) { [uint32]$match.fecRepairNeededChunks } else { 0 }
        FecLikelyClass = if (
            $null -ne $match -and
            $match.PSObject.Properties.Name -contains "fecLikelyClass"
        ) { $match.fecLikelyClass } else { "fec-unknown" }
        LargeMissingFecClass = $largeMissingFecClass
        EstimatedDeliveryUs = $estimatedDeliveryUs
        UsableSlackUs = $usableSlackUsForOutput
        DeliverySlackDeficitUs = $deliverySlackDeficitUs
        AckMissingChunks = if ($null -ne $match) { [uint32]$match.ackMissingChunks } else { 0 }
        AckRequestedChunks = if ($null -ne $match) { [uint32]$match.ackRequestedChunks } else { 0 }
        FinalCap = if ($null -ne $match) { [uint32]$match.finalCap } else { 0 }
        BudgetReason = if ($null -ne $match) { $match.budgetReason } else { "" }
    }
}

$outputDirectory = Split-Path -Parent $OutputCsv
if (![string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$resolvedOutputCsv = $OutputCsv
try {
    $classified |
        Export-Csv -NoTypeInformation -Encoding UTF8 -Force -Path $OutputCsv
    $resolvedOutputCsv = (Resolve-Path $OutputCsv).Path
}
catch {
    Write-Warning "Failed to write analysis CSV '$OutputCsv': $($_.Exception.Message)"
}

$summary =
    $classified |
    Group-Object Classification |
    Sort-Object Name |
    ForEach-Object {
        [pscustomobject]@{
            Classification = $_.Name
            Count = $_.Count
            LargeCount = ($_.Group | Where-Object { $_.LargeFrame -ne 0 }).Count
            AvgEstimatedDeliveryMs = [math]::Round(
                (($_.Group | Measure-Object EstimatedDeliveryUs -Average).Average / 1000.0),
                3)
            AvgUsableSlackMs = [math]::Round(
                (($_.Group | Measure-Object UsableSlackUs -Average).Average / 1000.0),
                3)
        }
    }

$displayDropSummary = $null
if (![string]::IsNullOrWhiteSpace($DiffCsv) -and (Test-Path $DiffCsv)) {
    $diffRows = Import-Csv $DiffCsv
    $guardDropRows =
        @($diffRows |
            Where-Object {
            [double]$_.RaceGuardSuppOn -gt 0 -and
            [double]$_.DisplayedDelta -lt 0
        })
    $displayDropSummary = [pscustomobject]@{
        GuardDisplayedDropRuns = $guardDropRows.Count
        GuardDisplayedDropSuppressedAvg = if ($guardDropRows.Count -gt 0) {
            [math]::Round((($guardDropRows | Measure-Object RaceGuardSuppOn -Average).Average), 3)
        } else { 0 }
        GuardDisplayedDropDeltaAvg = if ($guardDropRows.Count -gt 0) {
            [math]::Round((($guardDropRows | Measure-Object DisplayedDelta -Average).Average), 3)
        } else { 0 }
    }
}

[pscustomobject]@{
    OpportunityCsv = (Resolve-Path $OpportunityCsv).Path
    FrameRecoveryCsv = (Resolve-Path $FrameRecoveryCsv).Path
    OutputCsv = $resolvedOutputCsv
    LateCompletedRows = $classified.Count
}

$summary | Format-Table -AutoSize

$impactSummary =
    $classified |
    Group-Object LateImpactClass |
    Sort-Object Name |
    ForEach-Object {
        [pscustomobject]@{
            LateImpactClass = $_.Name
            Count = $_.Count
            LargeCount = ($_.Group | Where-Object { $_.LargeFrame -ne 0 }).Count
            AvgEstimatedDeliveryMs = [math]::Round(
                (($_.Group | Measure-Object EstimatedDeliveryUs -Average).Average / 1000.0),
                3)
            AvgUsableSlackMs = [math]::Round(
                (($_.Group | Measure-Object UsableSlackUs -Average).Average / 1000.0),
                3)
            AvgDeficitMs = [math]::Round(
                (($_.Group | Measure-Object DeliverySlackDeficitUs -Average).Average / 1000.0),
                3)
        }
    }

if ($impactSummary.Count -gt 0) {
    ""
    "Late repair impact classes:"
    $impactSummary | Format-Table -AutoSize

    $benignCount =
        @($classified |
            Where-Object {
                $_.LateImpactClass -eq "benign-keyframe-completed-early"
            }).Count
    $actionableCount =
        @($classified |
            Where-Object {
                $_.LateImpactClass -eq "deadline-estimate-risk" -or
                $_.LateImpactClass -eq "harmful-or-actionable-late"
            }).Count
    [pscustomobject]@{
        TotalLateCompletedRows = $classified.Count
        BenignCompletedEarlyRows = $benignCount
        AdjustedActionableLateRows = $actionableCount
    } | Format-List
}

$keyframeSlackSummary =
    $classified |
    Where-Object {
        $_.GuardExclusionReason -eq "keyframe-slack-too-tight" -or
        $_.GuardExclusionReason -eq "keyframe-slack-too-wide" -or
        $_.GuardExclusionReason -eq "keyframe-upper-slack-small-frame"
    } |
    Group-Object GuardExclusionReason |
    Sort-Object Name |
    ForEach-Object {
        $deficits = @($_.Group | Select-Object -ExpandProperty DeliverySlackDeficitUs)
        [pscustomobject]@{
            GuardExclusionReason = $_.Name
            Count = $_.Count
            LargeCount = ($_.Group | Where-Object { $_.LargeFrame -ne 0 }).Count
            AvgAckMissingChunks = [math]::Round(
                (($_.Group | Measure-Object AckMissingChunks -Average).Average),
                3)
            AvgEstimatedDeliveryMs = [math]::Round(
                (($_.Group | Measure-Object EstimatedDeliveryUs -Average).Average / 1000.0),
                3)
            AvgSlackMs = [math]::Round(
                (($_.Group | Measure-Object UsableSlackUs -Average).Average / 1000.0),
                3)
            AvgDeficitMs = [math]::Round(
                (($_.Group | Measure-Object DeliverySlackDeficitUs -Average).Average / 1000.0),
                3)
            P50DeficitMs = [math]::Round((Get-Percentile $deficits 50) / 1000.0, 3)
            P95DeficitMs = [math]::Round((Get-Percentile $deficits 95) / 1000.0, 3)
        }
    }

if ($keyframeSlackSummary.Count -gt 0) {
    ""
    "Keyframe slack split:"
    $keyframeSlackSummary | Format-Table -AutoSize
}

$guardExclusionSummary =
    $classified |
    Where-Object { $_.GuardExclusionReason -ne "candidate" } |
    Group-Object GuardExclusionReason |
    Sort-Object Count -Descending |
    ForEach-Object {
        [pscustomobject]@{
            GuardExclusionReason = $_.Name
            Count = $_.Count
            LargeCount = ($_.Group | Where-Object { $_.LargeFrame -ne 0 }).Count
            AvgAckMissingChunks = [math]::Round(
                (($_.Group | Measure-Object AckMissingChunks -Average).Average),
                3)
            AvgSlackMs = [math]::Round(
                (($_.Group | Measure-Object UsableSlackUs -Average).Average / 1000.0),
                3)
        }
    }

if ($guardExclusionSummary.Count -gt 0) {
    ""
    "Guard exclusion reasons:"
    $guardExclusionSummary | Format-Table -AutoSize
}

$largeMissingFecSummary =
    $classified |
    Where-Object { $_.LargeMissingFecClass -ne "not-large-5-13" } |
    Group-Object LargeMissingFecClass |
    Sort-Object Name |
    ForEach-Object {
        [pscustomobject]@{
            LargeMissingFecClass = $_.Name
            Count = $_.Count
            AvgSlackMs = [math]::Round(
                (($_.Group | Measure-Object UsableSlackUs -Average).Average / 1000.0),
                3)
            AvgEstimatedDeliveryMs = [math]::Round(
                (($_.Group | Measure-Object EstimatedDeliveryUs -Average).Average / 1000.0),
                3)
            AvgAckMissingChunks = [math]::Round(
                (($_.Group | Measure-Object AckMissingChunks -Average).Average),
                3)
            AvgFecRepairNeededChunks = [math]::Round(
                (($_.Group | Measure-Object FecRepairNeededChunks -Average).Average),
                3)
        }
    }

if ($largeMissingFecSummary.Count -gt 0) {
    ""
    "Large frame ackMissingChunks 5-13 by FEC likelihood:"
    $largeMissingFecSummary | Format-Table -AutoSize
}

if ($null -ne $displayDropSummary) {
    $displayDropSummary | Format-List
}
