#!/usr/bin/env bash
# Authenticate the unchanged, pinned Entware SDK action's GHCR image pull.
# No PAT, compilation retry, anonymous fallback or credential artifact.
set +x
set -euo pipefail

: "${GHCR_TOKEN:?GHCR_TOKEN must be supplied by the workflow}"
: "${GITHUB_ACTOR:?GITHUB_ACTOR must be supplied by the workflow}"
: "${DOCKER_CONFIG:?DOCKER_CONFIG must point to the job-private directory}"

umask 077
mkdir -p -- "$DOCKER_CONFIG"
chmod 700 -- "$DOCKER_CONFIG"

for attempt in 1 2 3; do
    # Supply the token over stdin, never in argv or a printed command.
    if printf '%s' "$GHCR_TOKEN" |
        timeout 45s docker login ghcr.io --username "$GITHUB_ACTOR" --password-stdin; then
        exit 0
    fi
    # Do not reuse a partial login result on a subsequent attempt.
    rm -f -- "$DOCKER_CONFIG/config.json"
    if [ "$attempt" -lt 3 ]; then
        printf 'GHCR login failed (attempt %s/3); retrying.\n' "$attempt" >&2
        sleep "$((attempt * 2))"
    fi
done
printf 'GHCR authentication failed after 3 attempts; refusing to start the SDK.\n' >&2
exit 1
