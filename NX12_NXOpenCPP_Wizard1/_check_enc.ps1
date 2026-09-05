 = [System.IO.File]::ReadAllBytes('D:\Program Files\Siemens\NX 12.0\UGII\NXAttributeCatalog.xml')
Write-Host ('Byte0={0} Byte1={1} Byte2={2}' -f [0],[1],[2])
Write-Host ('FileSize={0}' -f .Length)
if ([0] -eq 239 -and [1] -eq 187 -and [2] -eq 191) { Write-Host 'ENCODING: UTF-8 with BOM' } else { Write-Host 'ENCODING: No BOM (plain UTF-8 or other)' }
