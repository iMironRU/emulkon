# serve.ps1 — минимальный TCP-HTTP для раздачи файлов из $Dir в тестовую ВМ (без прав администратора, TcpListener).
param([string]$Dir, [string]$IP = "172.27.144.1", [int]$Port = 8099, [int]$Minutes = 60)
$ErrorActionPreference = "Stop"
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse($IP), $Port)
$listener.Start()
$until = (Get-Date).AddMinutes($Minutes)
"listening http://${IP}:$Port/ dir=$Dir"
while ((Get-Date) -lt $until) {
    if (-not $listener.Pending()) { Start-Sleep -Milliseconds 200; continue }
    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream()
        $reader = [IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ($line -match '^GET /(\S*)') {
            $name = [Uri]::UnescapeDataString($Matches[1])
            $path = Join-Path $Dir $name
            $w = [IO.BinaryWriter]::new($stream)
            if ($name -and (Test-Path $path -PathType Leaf) -and -not $name.Contains("..")) {
                $bytes = [IO.File]::ReadAllBytes($path)
                $hdr = "HTTP/1.1 200 OK`r`nContent-Type: application/octet-stream`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n"
                $w.Write([Text.Encoding]::ASCII.GetBytes($hdr)); $w.Write($bytes)
                "200 $name ($($bytes.Length))"
            } else {
                $body = [Text.Encoding]::ASCII.GetBytes("not found")
                $w.Write([Text.Encoding]::ASCII.GetBytes("HTTP/1.1 404 Not Found`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n")); $w.Write($body)
                "404 $name"
            }
            $w.Flush()
        }
    } catch { "err: $_" } finally { $client.Close() }
}
$listener.Stop()
"stopped"
