Get-ChildItem Database\*.sql |
    Where-Object { $_.Name -match '^\d+_' } |
    Sort-Object Name |
    ForEach-Object {
        Write-Host "applying $($_.Name)"
        mysql --login-path=projectwar war_world -e "source Database/$($_.Name)"
        if ($LASTEXITCODE -ne 0) { throw "Migration failed: $($_.Name)" }
    }