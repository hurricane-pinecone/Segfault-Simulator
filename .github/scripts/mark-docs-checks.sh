#!/usr/bin/env bash
# Report the master ruleset's required checks as passing on a docs PR's head
# commit. A docs-only PR can't break the code, and pushes made with the
# workflow's token never trigger the `pull_request` workflows, so without this
# the checks would sit forever "waiting for status to be reported".
#
# Usage: mark-docs-checks.sh <commit-sha>   (needs GH_TOKEN and GITHUB_REPOSITORY)
#
# KEEP THIS LIST IN SYNC with the ruleset's required status checks.
set -euo pipefail
sha="$1"
for ctx in test-core sanitize-core test-runtime sanitize-runtime format build build-windows; do
  gh api "repos/$GITHUB_REPOSITORY/statuses/$sha" \
    -f state=success \
    -f context="$ctx" \
    -f description="Skipped for docs-only PR" >/dev/null
done
