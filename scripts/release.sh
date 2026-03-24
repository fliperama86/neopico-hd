#!/usr/bin/env bash

set -euo pipefail

# Release helper:
# - Validates repo state
# - Creates an annotated semver tag
# - Pushes the tag to origin (triggers tag-based GitHub Release workflow)

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
    cat <<'EOF'
Usage:
  ./scripts/release.sh [vX.Y.Z]
  ./scripts/release.sh --help

Behavior:
  - If no tag is provided, auto-bumps PATCH from latest v* tag.
  - Requires a clean working tree.
  - Creates an annotated tag and pushes it to origin.

Examples:
  ./scripts/release.sh
  ./scripts/release.sh v1.4.0
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Error: not inside a git repository."
    exit 1
fi

# Require clean tree to avoid accidental releases from uncommitted changes.
if [[ -n "$(git status --porcelain)" ]]; then
    echo "Error: working tree is not clean."
    echo "Commit/stash changes before running release."
    git status --short
    exit 1
fi

is_valid_tag() {
    [[ "$1" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]
}

latest_tag="$(git tag --sort=-v:refname | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1 || true)"
if [[ -z "$latest_tag" ]]; then
    latest_tag="v0.0.0"
fi

if [[ -n "${1:-}" ]]; then
    new_tag="$1"
    if ! is_valid_tag "$new_tag"; then
        echo "Error: invalid tag '$new_tag'. Expected format: vX.Y.Z"
        exit 1
    fi
else
    version="${latest_tag#v}"
    IFS='.' read -r major minor patch <<<"$version"
    patch=$((patch + 1))
    new_tag="v${major}.${minor}.${patch}"
fi

if git rev-parse -q --verify "refs/tags/${new_tag}" >/dev/null; then
    echo "Error: tag '${new_tag}' already exists locally."
    exit 1
fi

if [[ -n "$(git ls-remote --tags origin "refs/tags/${new_tag}")" ]]; then
    echo "Error: tag '${new_tag}' already exists on origin."
    exit 1
fi

current_branch="$(git rev-parse --abbrev-ref HEAD)"
echo "Current branch: ${current_branch}"
echo "Latest version : ${latest_tag}"
echo "New version    : ${new_tag}"
echo ""
echo "Creating annotated tag..."
git tag -a "${new_tag}" -m "Release ${new_tag}"

echo "Pushing tag to origin..."
git push origin "${new_tag}"

echo ""
echo "Release tag pushed: ${new_tag}"
echo "GitHub Actions will publish the release from this tag."
