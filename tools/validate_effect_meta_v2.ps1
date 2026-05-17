param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]

$metaRoot = Join-Path $Root 'effects_meta'
$requiredRootFields = @(
    'schema_version',
    'effect_id',
    'name',
    'category',
    'role',
    'performance_group',
    'quality_cost',
    'requires_input',
    'description',
    'controls',
    'audio_link'
)
$requiredControlFields = @(
    'param',
    'label',
    'type',
    'default',
    'min',
    'max',
    'step',
    'group',
    'basic',
    'advanced',
    'audio_target',
    'save'
)
$allowedRoles = @('generator', 'filter', 'postprocess', 'overlay', 'flash', 'utility')
$allowedQualityCosts = @('low', 'medium', 'high', 'extreme')
$allowedControlGroups = @('Main', 'Transform', 'AudioLink', 'Color', 'Texture/Input', 'Glitch', 'Performance')

function Test-JsonProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return $null -ne $Object.PSObject.Properties[$Name]
}

function Add-Failure {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $failures.Add("${RelativePath}: ${Message}")
}

if (-not (Test-Path -LiteralPath $metaRoot)) {
    $failures.Add('Missing directory: effects_meta')
} else {
    $files = Get-ChildItem -LiteralPath $metaRoot -Recurse -Filter '*.json' | Sort-Object FullName

    foreach ($file in $files) {
        $relativePath = $file.FullName.Substring($Root.TrimEnd('\').Length + 1)
        try {
            $json = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        } catch {
            Add-Failure $relativePath "Invalid JSON: $($_.Exception.Message)"
            continue
        }

        if (-not (Test-JsonProperty $json 'schema_version')) {
            continue
        }

        if ($json.schema_version -ne 2) {
            Add-Failure $relativePath "Unsupported schema_version '$($json.schema_version)'. Expected 2."
            continue
        }

        foreach ($field in $requiredRootFields) {
            if (-not (Test-JsonProperty $json $field)) {
                Add-Failure $relativePath "Missing required root field '$field'."
            }
        }

        if ((Test-JsonProperty $json 'role') -and $json.role -notin $allowedRoles) {
            Add-Failure $relativePath "Invalid role '$($json.role)'."
        }

        if ((Test-JsonProperty $json 'quality_cost') -and $json.quality_cost -notin $allowedQualityCosts) {
            Add-Failure $relativePath "Invalid quality_cost '$($json.quality_cost)'."
        }

        if ((Test-JsonProperty $json 'controls') -and $json.controls -isnot [array]) {
            Add-Failure $relativePath "Root field 'controls' must be an array."
            continue
        }

        if (-not (Test-JsonProperty $json 'controls')) {
            continue
        }

        for ($i = 0; $i -lt $json.controls.Count; $i++) {
            $control = $json.controls[$i]
            $controlName = "controls[$i]"

            foreach ($field in $requiredControlFields) {
                if (-not (Test-JsonProperty $control $field)) {
                    Add-Failure $relativePath "$controlName missing required field '$field'."
                }
            }

            if ((Test-JsonProperty $control 'group') -and $control.group -notin $allowedControlGroups) {
                Add-Failure $relativePath "$controlName has invalid group '$($control.group)'."
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'Effect metadata validation failed:' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'Effect metadata validation passed' -ForegroundColor Green
