# twp_verify.ps1 - twp toolbox NX12 deployment health check
# Run:  powershell -ExecutionPolicy Bypass -File E:\UG\nx_app\twp_verify.ps1
$ErrorActionPreference = 'SilentlyContinue'
$app = 'E:\UG\nx_app\application'
$startup = 'E:\UG\nx_app\startup'
$fail = 0
function Check($name, $ok, $detail) {
    if ($ok) { Write-Host ('[PASS] ' + $name) -ForegroundColor Green }
    else { Write-Host ('[FAIL] ' + $name + '  ->  ' + $detail) -ForegroundColor Red; $script:fail++ }
}

Write-Host '=== twp toolbox NX12 health check ==='

# 1) menu registration file
$men = Join-Path $startup 'twp_toolbox.men'
if (Test-Path $men) {
    $b = [System.IO.File]::ReadAllBytes($men)
    $bom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
    Check 'startup\twp_toolbox.men exists' $true ''
    Check 'no UTF-8 BOM' (-not $bom) 'BOM found - NX12 shows garbage'
    $t = [System.Text.Encoding]::GetEncoding(936).GetString($b)
    Check 'VERSION 120 present' ($t -match 'VERSION 120') ''
    Check 'topbar cascade (BEFORE UG_HELP)' ($t -match 'BEFORE UG_HELP') ''
    Check 'application button' ($t -match 'APPLICATION_BUTTON TWP_TOOLBOX') ''
    Check 'LIBRARIES titleblock_fill' ($t -match 'LIBRARIES titleblock_fill') ''
    Check 'no startup auto-load (no MODIFY statement)' ($t -notmatch 'END_OF_MODIFY') 'startup must not preload DLLs'
} else { Check 'startup\twp_toolbox.men exists' $false 'missing' }

# 2) placeholder
Check 'twp_toolbox_app.men exists' (Test-Path (Join-Path $app 'twp_toolbox_app.men')) ''

# 3) DLLs vs ACTIONS in the menu file
$t = [System.Text.Encoding]::GetEncoding(936).GetString([System.IO.File]::ReadAllBytes($men))
$dllNames = @('NX12_Step1_SheetAndViews','NX12_Step2_SheetPreferences','NX12_Step3_OrdinateDimensions',
              'NX12_Step4_LinearDimensions','NX12_Step5_Centerlines','NX12_Step6_LayerSwitch',
              'NX12_Step7_AppendSuffix','titleblock_fill','dbt_step1','explosion_step1',
              'explosion_step2','balloon_step1')
foreach ($d in $dllNames) {
    $exists = Test-Path (Join-Path $app ($d + '.dll'))
    $inMenu = $t -match [regex]::Escape($d + '.dll')
    Check ('DLL deployed: ' + $d + '.dll') $exists 'not in application dir'
    Check ('ACTIONS references ' + $d + '.dll') $inMenu 'menu does not reference it'
}
Check 'titleblock actions match' ($t -match 'TITLEFILL_APP__fill_date' -and $t -match 'TITLEFILL_APP__fill_partno' -and $t -match 'TITLEFILL_APP__fill_both') ''
Check 'explosion_params.txt deployed' (Test-Path (Join-Path $app 'explosion_params.txt')) ''

# 4) custom dirs mount points (NX12)
$nx12 = 'D:\Program Files\Siemens\NX 12.0\UGII\menus'
foreach ($f in @('custom_dirs.dat','ug_custom_dirs.dat')) {
    $p = Join-Path $nx12 $f
    $hit = $false
    if (Test-Path $p) {
        $lines = Get-Content $p -Encoding Default
        $hit = ($lines | Where-Object { $_.Trim() -eq 'E:\UG\nx_app' }).Count -gt 0
    }
    Check ('mount point ' + $f) $hit ('missing E:\UG\nx_app line in ' + $p)
}

# 5) environment
$u = [Environment]::GetEnvironmentVariable('UGII_BASE_DIR','User')
Check 'user UGII_BASE_DIR points to NX12' ($u -eq 'D:\Program Files\Siemens\NX 12.0') ('current=' + $u)

Write-Host ''
if ($fail -eq 0) { Write-Host 'ALL CHECKS PASSED - restart NX12 and look for the twp toolbox cascade (left of Help).' -ForegroundColor Green }
else { Write-Host ($fail.ToString() + ' check(s) FAILED - see nx_app README section 8.') -ForegroundColor Red }
