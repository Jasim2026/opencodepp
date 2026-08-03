#!/usr/bin/env bash
# check_hidden_chars.sh -- port of the Go repo's script: reject non-ASCII and
# stray control characters in source files (prompt-injection hygiene, Section 5.5).
# Allowed control chars in text: TAB \t, LF \n, CR \r.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
fails=0

while IFS= read -r -d '' f; do
    if grep -nP '[^\x00-\x7F\x09\x0A\x0D]' "$f" >/dev/null 2>&1; then
        echo "check_hidden_chars: hidden char in $f"
        grep -nP '[^\x00-\x7F\x09\x0A\x0D]' "$f" || true
        fails=1
    fi
done < <(find "$root/src" "$root/include" "$root/tools" "$root/tests" \
    "$root/scripts" -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \
    -o -name '*.cc' -o -name '*.sh' -o -name '*.py' \) -print0)

if [ "$fails" -eq 0 ]; then
    echo "check_hidden_chars: OK"
fi
exit "$fails"
