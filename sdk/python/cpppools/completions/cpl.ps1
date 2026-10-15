# Load with: . /path/to/cpppools/completions/cpl.ps1
Register-ArgumentCompleter -Native -CommandName cpl,cpl.exe -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)
    $elements = @($commandAst.CommandElements | Where-Object { $_.Extent.StartOffset -lt $cursorPosition })
    $options = 'start stop status st health h select get insert add update set delete del -h --help -V --version'
    if ($elements.Count -gt 2 -or ($elements.Count -eq 2 -and $wordToComplete -eq '')) {
        switch -Regex ($elements[1].Extent.Text) {
            '^start$' { $options = '-h --help -c --config -n --name -b --binary -W --workdir -R --state-dir -l --log-file -t --timeout -F --foreground -o --output' }
            '^stop$' { $options = '-h --help -n --name -R --state-dir -t --timeout -f --force -o --output' }
            '^(status|st)$' { $options = '-h --help -n --name -R --state-dir -u --url -t --timeout -o --output' }
            '^(health|h)$' { $options = '-h --help -u --url -t --timeout -o --output' }
            '^(select|get)$' { $options = '-h --help -w --where -C --column -u --url -d --datasource -t --timeout -o --output' }
            '^(insert|add)$' { $options = '-h --help -D --data -u --url -d --datasource -t --timeout -o --output' }
            '^(update|set)$' { $options = '-h --help -w --where -D --data -u --url -d --datasource -t --timeout -o --output' }
            '^(delete|del)$' { $options = '-h --help -w --where -u --url -d --datasource -t --timeout -o --output' }
        }
        $previous = if ($wordToComplete) { $elements[-2].Extent.Text } else { $elements[-1].Extent.Text }
        if ($previous -in '-o', '--output') { $options = 'table json' }
    }
    foreach ($option in ($options -split ' ')) {
        if ($option.StartsWith($wordToComplete, [System.StringComparison]::Ordinal)) {
            [System.Management.Automation.CompletionResult]::new($option, $option, 'ParameterName', $option)
        }
    }
}
