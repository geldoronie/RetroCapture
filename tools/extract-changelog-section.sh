#!/bin/bash
# Extracts a single version section from CHANGELOG.md and prints it to stdout.
# Used to feed GitHub Releases without maintaining a duplicate RELEASE_NOTES file.
#
# Usage:
#   tools/extract-changelog-section.sh 0.6.0-alpha
#
# Typical release flow:
#   gh release create 0.6.0-alpha dist/RetroCapture-* dist/SHA256SUMS \
#       --title "0.6.0-alpha" \
#       --notes-file <(tools/extract-changelog-section.sh 0.6.0-alpha) \
#       --prerelease

set -eu

if [ $# -lt 1 ]; then
    echo "Usage: $0 <version>" >&2
    echo "Example: $0 0.6.0-alpha" >&2
    exit 1
fi

VERSION="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CHANGELOG="$REPO_ROOT/CHANGELOG.md"

if [ ! -f "$CHANGELOG" ]; then
    echo "Error: $CHANGELOG not found" >&2
    exit 1
fi

# Print from `## [<version>]` (inclusive) until the next `## [` (exclusive),
# then strip the trailing horizontal-rule separator and blank lines.
awk -v v="$VERSION" '
    $0 ~ "^## \\[" v "\\]"          { in_section = 1; print; next }
    in_section && /^## \[/          { exit }
    in_section                      { print }
' "$CHANGELOG" | awk '
    # Buffer the lines so we can drop trailing --- and blank lines at the end.
    { lines[NR] = $0 }
    END {
        # Find the last non-blank, non---separator line.
        last = NR
        while (last > 0 && (lines[last] == "" || lines[last] ~ /^-{3,}$/)) {
            last--
        }
        for (i = 1; i <= last; i++) print lines[i]
    }
'

# If awk produced nothing, the version section was not found.
# (We re-run a quick check rather than capturing the awk output to keep this
# script streaming-friendly for shell process substitution.)
if ! grep -q "^## \[${VERSION}\]" "$CHANGELOG"; then
    echo "Error: no section '## [${VERSION}]' found in $CHANGELOG" >&2
    exit 2
fi
