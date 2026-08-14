param(
    [int]$Port = 8080,
    [int]$Workers = 6,
    [string]$DbHost = "127.0.0.1",
    [int]$DbPort = 3306,
    [string]$DbUser = "root",
    [string]$DbPassword = "123456",
    [string]$DbName = "cpppools",
    [string]$DbTable = "user_scores",
    [int]$PoolSize = 8,
    [string]$MySQLRoot = "C:\Program Files\MySQL\MySQL Server 8.0",
    [string]$ExePath = ""
)

$projectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $exeDefault = Join-Path $projectRoot "build\bin\cpppools_server.exe"
    if (Test-Path $exeDefault) {
        $ExePath = $exeDefault
    } else {
        throw "No server executable found at $exeDefault. Build the project first (cmake --build build)."
    }
}

$mysqlLibDir = Join-Path $MySQLRoot "lib"
$mysqlBinDir = Join-Path $MySQLRoot "bin"

if (-not (Test-Path $mysqlLibDir)) {
    throw "MySQL lib dir not found: $mysqlLibDir"
}
if (-not (Test-Path $mysqlBinDir)) {
    throw "MySQL bin dir not found: $mysqlBinDir"
}

$env:PATH = "$mysqlLibDir;$mysqlBinDir;$env:PATH"
$env:CPPPOOLS_DB_MODE = "mysql"
$env:CPPPOOLS_DB_HOST = $DbHost
$env:CPPPOOLS_DB_PORT = "$DbPort"
$env:CPPPOOLS_DB_USER = $DbUser
$env:CPPPOOLS_DB_PASSWORD = $DbPassword
$env:CPPPOOLS_DB_NAME = $DbName
$env:CPPPOOLS_DB_TABLE = $DbTable
$env:CPPPOOLS_DB_POOL_SIZE = "$PoolSize"

Write-Host "Starting server with MySQL mode..."
Write-Host "exe: $ExePath"
Write-Host ("mysql: {0}:{1}/{2}.{3} pool={4}" -f $DbHost, $DbPort, $DbName, $DbTable, $PoolSize)
Write-Host "url:    http://127.0.0.1:$Port/"
Write-Host "health: http://127.0.0.1:$Port/health"
Write-Host "stats:  http://127.0.0.1:$Port/api/v1/stats"

& $ExePath $Port $Workers
