# Load with: source /path/to/cpppools/completions/cpl.bash
_cpl_complete() {
    local current="${COMP_WORDS[COMP_CWORD]}" options candidate
    COMPREPLY=()
    local commands='start stop status st health h select get insert add update set delete del'
    if (( COMP_CWORD == 1 )); then
        options="$commands -h --help -V --version"
    else
        case "${COMP_WORDS[1]}" in
            start) options='-h --help -c --config -n --name -b --binary -W --workdir -R --state-dir -l --log-file -t --timeout -F --foreground -o --output' ;;
            stop) options='-h --help -n --name -R --state-dir -t --timeout -f --force -o --output' ;;
            status|st) options='-h --help -n --name -R --state-dir -u --url -t --timeout -o --output' ;;
            health|h) options='-h --help -u --url -t --timeout -o --output' ;;
            select|get) options='-h --help -w --where -C --column -u --url -d --datasource -t --timeout -o --output' ;;
            insert|add) options='-h --help -D --data -u --url -d --datasource -t --timeout -o --output' ;;
            update|set) options='-h --help -w --where -D --data -u --url -d --datasource -t --timeout -o --output' ;;
            delete|del) options='-h --help -w --where -u --url -d --datasource -t --timeout -o --output' ;;
        esac
        case "${COMP_WORDS[COMP_CWORD-1]}" in
            -o|--output) options='table json' ;;
            -c|--config|-b|--binary|-W|--workdir|-R|--state-dir|-l|--log-file)
                while IFS= read -r candidate; do COMPREPLY+=("$candidate"); done < <(compgen -f -- "$current")
                return ;;
        esac
    fi
    while IFS= read -r candidate; do COMPREPLY+=("$candidate"); done < <(compgen -W "$options" -- "$current")
}
complete -F _cpl_complete cpl
