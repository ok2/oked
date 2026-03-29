#!/usr/bin/env bash
set -euo pipefail

# sync-upstream.sh — Manage GNU Ed upstream source imports
# Usage: sync-upstream.sh {init|check|update|import VERSION} [--auto]

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FTP_URL="https://ftp.gnu.org/gnu/ed"
VERSION_FILE="${REPO_ROOT}/.ed-version"
KEYRING_CACHE="${REPO_ROOT}/.cache/gnu-keyring.gpg"
UPSTREAM_BRANCH="upstream"

# Colors (if terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; NC=''
fi

info()  { echo -e "${BLUE}==>${NC} $*"; }
ok()    { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==> WARNING:${NC} $*"; }
die()   { echo -e "${RED}==> ERROR:${NC} $*" >&2; exit 1; }

# --- Dependency checks ---

check_deps() {
    local missing=()
    for cmd in curl gpg git lzip tar; do
        command -v "$cmd" >/dev/null || missing+=("$cmd")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        die "Missing dependencies: ${missing[*]}\n    Install with: brew install ${missing[*]}"
    fi
}

# --- Version detection ---

get_latest_version() {
    curl -sfL "${FTP_URL}/" \
        | grep -oE 'ed-[0-9]+\.[0-9]+(\.[0-9]+)?\.tar\.lz"' \
        | sed 's/ed-//; s/\.tar\.lz"//' \
        | sort -V \
        | tail -1
}

get_current_version() {
    if [ -f "$VERSION_FILE" ]; then
        cat "$VERSION_FILE"
    else
        echo ""
    fi
}

# --- Download & verify ---

download_and_verify() {
    local version="$1"
    local tmpdir="$2"
    local tarball="ed-${version}.tar.lz"

    info "Downloading ${tarball}..."
    curl -fSL --progress-bar -o "${tmpdir}/${tarball}" "${FTP_URL}/${tarball}"

    info "Downloading signature..."
    curl -fSL -o "${tmpdir}/${tarball}.sig" "${FTP_URL}/${tarball}.sig"

    # GPG verification
    local gpghome="${tmpdir}/gnupg"
    mkdir -m 700 "${gpghome}"

    # Use cached keyring or download fresh
    if [ -f "$KEYRING_CACHE" ] && [ "$(find "$KEYRING_CACHE" -mtime -30 2>/dev/null)" ]; then
        info "Using cached GNU keyring..."
        gpg --homedir "${gpghome}" --import --quiet "${KEYRING_CACHE}" 2>/dev/null || true
    else
        info "Downloading GNU keyring..."
        mkdir -p "$(dirname "$KEYRING_CACHE")"
        curl -fSL "https://ftp.gnu.org/gnu/gnu-keyring.gpg" -o "${KEYRING_CACHE}"
        gpg --homedir "${gpghome}" --import --quiet "${KEYRING_CACHE}" 2>/dev/null || true
    fi

    info "Verifying GPG signature..."
    if gpg --homedir "${gpghome}" --verify "${tmpdir}/${tarball}.sig" "${tmpdir}/${tarball}" 2>&1; then
        ok "GPG signature verified"
    else
        die "GPG signature verification FAILED for ${tarball}"
    fi
}

# --- Branch operations ---

import_to_upstream() {
    local version="$1"
    local tmpdir="$2"
    local tarball="ed-${version}.tar.lz"
    local is_init="${3:-false}"

    local original_branch
    original_branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")"

    # Extract tarball
    info "Extracting ${tarball}..."
    local extractdir="${tmpdir}/extracted"
    mkdir -p "${extractdir}"
    lzip -dc "${tmpdir}/${tarball}" | tar xf - -C "${extractdir}"

    cd "$REPO_ROOT"

    if [ "$is_init" = "true" ]; then
        # Create orphan upstream branch
        info "Creating orphan branch '${UPSTREAM_BRANCH}'..."
        git checkout --orphan "${UPSTREAM_BRANCH}"
        git rm -rf . 2>/dev/null || true
        rm -rf .cache 2>/dev/null || true
    else
        # Switch to existing upstream branch
        info "Switching to '${UPSTREAM_BRANCH}' branch..."
        git checkout "${UPSTREAM_BRANCH}"
        # Remove all tracked files
        git rm -rf . 2>/dev/null || true
    fi

    # Copy extracted source (strip top-level directory)
    local srcdir="${extractdir}/ed-${version}"
    if [ ! -d "$srcdir" ]; then
        die "Expected directory ${srcdir} not found in tarball"
    fi

    cp -a "${srcdir}"/. "${REPO_ROOT}/"

    # Write version file
    echo "${version}" > "${VERSION_FILE}"

    # Commit
    git add -A
    git commit -m "Import GNU Ed ${version}"

    ok "Imported GNU Ed ${version} on '${UPSTREAM_BRANCH}' branch"

    if [ "$is_init" = "true" ]; then
        # Create main from this commit
        info "Creating 'main' branch from upstream..."
        git branch -f main "${UPSTREAM_BRANCH}"
        git checkout main
        ok "Initialized 'main' branch"
    elif [ -n "$original_branch" ] && [ "$original_branch" != "${UPSTREAM_BRANCH}" ]; then
        git checkout "${original_branch}"
    fi
}

# --- Subcommands ---

cmd_init() {
    cd "$REPO_ROOT"

    # Check if upstream branch already exists
    if git rev-parse --verify "${UPSTREAM_BRANCH}" >/dev/null 2>&1; then
        die "Branch '${UPSTREAM_BRANCH}' already exists. Use 'update' instead."
    fi

    local version
    version="$(get_latest_version)"
    [ -n "$version" ] || die "Could not determine latest version from ${FTP_URL}"
    info "Latest GNU Ed version: ${version}"

    local tmpdir
    tmpdir="$(mktemp -d)"
    trap "rm -rf '${tmpdir}'" EXIT

    download_and_verify "$version" "$tmpdir"
    import_to_upstream "$version" "$tmpdir" true

    ok "Repository initialized with GNU Ed ${version}"
    echo ""
    info "Next steps:"
    echo "  1. Build:  ./configure && make"
    echo "  2. Test:   make check"
    echo "  3. Run:    ./ed --version"
}

cmd_check() {
    local latest current
    latest="$(get_latest_version)"
    current="$(get_current_version)"

    [ -n "$latest" ] || die "Could not determine latest version"

    if [ -z "$current" ]; then
        warn "No version imported yet. Run 'init' first."
        echo "Latest available: ${latest}"
        exit 1
    fi

    if [ "$latest" = "$current" ]; then
        ok "Up to date: GNU Ed ${current}"
        exit 0
    else
        warn "Update available: ${current} -> ${latest}"
        exit 1
    fi
}

cmd_update() {
    local auto=false
    local target_version=""

    while [ $# -gt 0 ]; do
        case "$1" in
            --auto) auto=true; shift ;;
            *) target_version="$1"; shift ;;
        esac
    done

    cd "$REPO_ROOT"

    # Ensure upstream branch exists
    if ! git rev-parse --verify "${UPSTREAM_BRANCH}" >/dev/null 2>&1; then
        die "Branch '${UPSTREAM_BRANCH}' not found. Run 'init' first."
    fi

    # Check for dirty tree
    if ! git diff-index --quiet HEAD -- 2>/dev/null; then
        die "Working tree has uncommitted changes. Commit or stash them first."
    fi

    local version
    if [ -n "$target_version" ]; then
        version="$target_version"
    else
        version="$(get_latest_version)"
    fi
    [ -n "$version" ] || die "Could not determine version"

    local current
    current="$(get_current_version)"
    if [ "$version" = "$current" ]; then
        ok "Already up to date: GNU Ed ${current}"
        return 0
    fi

    info "Updating: ${current:-none} -> ${version}"

    local tmpdir
    tmpdir="$(mktemp -d)"
    trap "rm -rf '${tmpdir}'" EXIT

    download_and_verify "$version" "$tmpdir"
    import_to_upstream "$version" "$tmpdir" false

    # Merge into main
    local current_branch
    current_branch="$(git rev-parse --abbrev-ref HEAD)"

    if [ "$current_branch" != "main" ]; then
        git checkout main
    fi

    if [ "$auto" = true ]; then
        info "Auto-merging '${UPSTREAM_BRANCH}' into 'main'..."
        if git merge "${UPSTREAM_BRANCH}" -m "Merge GNU Ed ${version} from upstream"; then
            ok "Merged GNU Ed ${version} into main"
        else
            die "Merge conflict detected. Resolve manually:\n    git merge --continue  (after resolving conflicts)\n    git merge --abort      (to cancel)"
        fi
    else
        echo ""
        info "Upstream branch updated to GNU Ed ${version}"
        echo "  To merge into main:"
        echo "    git merge ${UPSTREAM_BRANCH}"
        echo ""
        read -rp "Merge now? [Y/n] " answer
        case "${answer:-Y}" in
            [Yy]*)
                if git merge "${UPSTREAM_BRANCH}" -m "Merge GNU Ed ${version} from upstream"; then
                    ok "Merged GNU Ed ${version} into main"
                else
                    warn "Merge conflict detected. Resolve manually:"
                    echo "    git merge --continue  (after resolving conflicts)"
                    echo "    git merge --abort      (to cancel)"
                fi
                ;;
            *)
                info "Skipped merge. Run 'git merge ${UPSTREAM_BRANCH}' when ready."
                ;;
        esac
    fi
}

# --- Main ---

check_deps

case "${1:-}" in
    init)
        cmd_init
        ;;
    check)
        cmd_check
        ;;
    update)
        shift
        cmd_update "$@"
        ;;
    import)
        shift
        [ -n "${1:-}" ] || die "Usage: $0 import VERSION"
        cmd_update "$1"
        ;;
    *)
        echo "Usage: $(basename "$0") {init|check|update|import VERSION} [--auto]"
        echo ""
        echo "Commands:"
        echo "  init              First-time import of GNU Ed into this repository"
        echo "  check             Check if a newer version is available"
        echo "  update [--auto]   Download and import the latest version"
        echo "  import VERSION    Import a specific version"
        exit 1
        ;;
esac
