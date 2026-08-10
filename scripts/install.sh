#!/usr/bin/env sh
#
# sparx installer.
#
#   curl -fsSL https://openschbrid.dev/install.sh | sh
#
# POSIX sh on purpose, not bash: this runs as the very first thing a new user
# does, on machines we do not control (Alpine CI images, minimal Debian, WSL,
# QNX build hosts). /bin/sh is the only interpreter guaranteed present.
#
# Environment overrides:
#   SPARX_VERSION=2.1.0   install a specific version instead of latest
#   SPARX_INSTALL_DIR     where to put the binary (default: see resolve_bindir)
#   SPARX_BASE_URL        artifact host, for mirrors and for testing
#   SPARX_NO_MODIFY_PATH=1  skip the shell-profile PATH hint
#
set -eu

REPO="${SPARX_REPO:-OpenSparX/MasterAgent}"
BASE_URL="${SPARX_BASE_URL:-https://github.com/$REPO/releases}"

# ---- output helpers --------------------------------------------------------
# Colour only when stdout is a TTY. Piping `curl | sh` into a log should not
# produce escape sequences.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RESET='\033[0m'; C_DIM='\033[2m'; C_RED='\033[31m'
    C_GREEN='\033[32m'; C_BOLD='\033[1m'
else
    C_RESET=''; C_DIM=''; C_RED=''; C_GREEN=''; C_BOLD=''
fi

say()  { printf '%b\n' "  $1"; }
dim()  { printf '%b\n' "  ${C_DIM}$1${C_RESET}"; }
ok()   { printf '%b\n' "  ${C_GREEN}✓${C_RESET} $1"; }
die()  { printf '%b\n' "  ${C_RED}✗${C_RESET} $1" >&2; exit 1; }

# ---- platform detection ---------------------------------------------------
# Must produce exactly the triples build_release.sh emits.
detect_target() {
    _os=$(uname -s)
    _arch=$(uname -m)
    case "$_os" in
        Darwin) _os=darwin ;;
        Linux)  _os=linux ;;
        MINGW*|MSYS*|CYGWIN*)
            die "Windows detected. Use WSL, or download the .zip from $BASE_URL" ;;
        *) die "unsupported OS: $_os" ;;
    esac
    case "$_arch" in
        x86_64|amd64) _arch=x64 ;;
        arm64|aarch64) _arch=arm64 ;;
        *) die "unsupported architecture: $_arch" ;;
    esac
    printf '%s-%s' "$_os" "$_arch"
}

# ---- download helper ------------------------------------------------------
# curl and wget differ enough in flags that picking once up front is cleaner
# than branching at each call site. Both are told to fail loudly on HTTP errors
# so a 404 does not get written to disk as a "successful" file.
detect_fetcher() {
    if command -v curl >/dev/null 2>&1; then
        printf 'curl'
    elif command -v wget >/dev/null 2>&1; then
        printf 'wget'
    else
        die "need curl or wget to download"
    fi
}

fetch_to() {
    # fetch_to <url> <dest>
    case "$FETCHER" in
        curl) curl -fsSL --retry 3 --retry-delay 1 -o "$2" "$1" ;;
        wget) wget -q --tries=3 -O "$2" "$1" ;;
    esac
}

fetch_stdout() {
    case "$FETCHER" in
        curl) curl -fsSL --retry 3 --retry-delay 1 "$1" ;;
        wget) wget -q -O - "$1" ;;
    esac
}

# ---- install dir ----------------------------------------------------------
# Preference order is about not needing sudo. A user running `curl | sh` should
# not be prompted for a password, so a writable user-local dir beats /usr/local
# even though the latter is already on PATH.
resolve_bindir() {
    if [ -n "${SPARX_INSTALL_DIR:-}" ]; then
        printf '%s' "$SPARX_INSTALL_DIR"; return
    fi
    for _d in "$HOME/.local/bin" "$HOME/.sparx/bin"; do
        if [ -d "$_d" ] && [ -w "$_d" ]; then printf '%s' "$_d"; return; fi
    done
    if [ -w /usr/local/bin ] 2>/dev/null; then
        printf '/usr/local/bin'; return
    fi
    printf '%s' "$HOME/.sparx/bin"
}

# ---- version resolution ---------------------------------------------------
# Asks the GitHub API for the latest tag. Deliberately does not use
# /releases/latest/download/<file> even though that redirects correctly,
# because we need the resolved version string for the archive name and for the
# "installed X" message.
resolve_version() {
    if [ -n "${SPARX_VERSION:-}" ]; then
        printf '%s' "${SPARX_VERSION#v}"; return
    fi
    _api="${SPARX_API_URL:-https://api.github.com/repos/$REPO/releases/latest}"
    _json=$(fetch_stdout "$_api" 2>/dev/null) || \
        die "could not reach $_api
    Set SPARX_VERSION=<version> to install a specific release instead."
    # sed rather than jq: jq is not present on a stock macOS or minimal Linux.
    _tag=$(printf '%s' "$_json" \
        | tr ',' '\n' \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n 1)
    [ -n "$_tag" ] || die "could not parse latest version from $_api"
    printf '%s' "${_tag#v}"
}

# ---- main -----------------------------------------------------------------
main() {
    printf '\n'
    printf '%b\n' "  ${C_BOLD}sparx installer${C_RESET}"
    printf '\n'

    FETCHER=$(detect_fetcher)
    TARGET=$(detect_target)
    dim "platform: $TARGET"

    VERSION=$(resolve_version)
    dim "version:  $VERSION"

    BINDIR=$(resolve_bindir)
    dim "install:  $BINDIR/sparx"
    printf '\n'

    ARCHIVE="sparx-$VERSION-$TARGET.tar.gz"
    URL="$BASE_URL/download/v$VERSION/$ARCHIVE"

    # mktemp -d is portable; the trap covers the error paths too so a failed
    # install does not leave a multi-megabyte tarball in /tmp.
    TMP=$(mktemp -d 2>/dev/null || mktemp -d -t sparx)
    trap 'rm -rf "$TMP"' EXIT INT TERM

    say "downloading $ARCHIVE"
    fetch_to "$URL" "$TMP/$ARCHIVE" || die "download failed: $URL
    Check that version $VERSION publishes a $TARGET build."

    # Checksum verification. Treated as required-if-published: a missing
    # .sha256 warns rather than fails (older releases may not have one), but a
    # present-and-mismatched one is always fatal.
    if fetch_to "$URL.sha256" "$TMP/$ARCHIVE.sha256" 2>/dev/null; then
        _want=$(cut -d' ' -f1 < "$TMP/$ARCHIVE.sha256")
        if command -v sha256sum >/dev/null 2>&1; then
            _got=$(sha256sum "$TMP/$ARCHIVE" | cut -d' ' -f1)
        elif command -v shasum >/dev/null 2>&1; then
            _got=$(shasum -a 256 "$TMP/$ARCHIVE" | cut -d' ' -f1)
        else
            _got=""
        fi
        if [ -z "$_got" ]; then
            dim "checksum: skipped (no sha256 tool)"
        elif [ "$_want" = "$_got" ]; then
            ok "checksum verified"
        else
            die "checksum MISMATCH
    expected: $_want
    actual:   $_got
    Do not use this download."
        fi
    else
        dim "checksum: not published for this release"
    fi

    tar -xzf "$TMP/$ARCHIVE" -C "$TMP" || die "could not extract $ARCHIVE"
    _src="$TMP/sparx-$VERSION-$TARGET/bin/sparx"
    [ -f "$_src" ] || die "archive layout unexpected: no bin/sparx inside"

    mkdir -p "$BINDIR" || die "could not create $BINDIR"
    # Install to a temp name in the target dir then rename: an atomic swap, so
    # re-running the installer cannot leave a half-written binary if the copy
    # is interrupted, and works while the old sparx is running.
    cp "$_src" "$BINDIR/.sparx.new" || die "could not write to $BINDIR"
    chmod 755 "$BINDIR/.sparx.new"
    mv -f "$BINDIR/.sparx.new" "$BINDIR/sparx"
    ok "installed to $BINDIR/sparx"

    # Verify the thing actually runs before claiming success. Catches an
    # arch mismatch (Rosetta, wrong triple) immediately instead of at first use.
    if ! "$BINDIR/sparx" version >/dev/null 2>&1; then
        die "installed binary will not execute on this machine.
    This usually means the $TARGET build does not match your CPU."
    fi

    printf '\n'
    # PATH guidance. Detecting rather than editing rc files: silently rewriting
    # someone's shell profile from a piped-curl script is worse than telling
    # them one line to add.
    case ":$PATH:" in
        *":$BINDIR:"*)
            ok "$BINDIR is on your PATH"
            printf '\n'
            say "${C_BOLD}get started${C_RESET}"
            dim "sparx init my-agent && cd my-agent"
            dim "sparx doctor"
            ;;
        *)
            if [ -z "${SPARX_NO_MODIFY_PATH:-}" ]; then
                say "${C_BOLD}one more step${C_RESET} — $BINDIR is not on your PATH:"
                printf '\n'
                case "${SHELL:-}" in
                    */zsh)  _rc="~/.zshrc" ;;
                    */bash) _rc="~/.bashrc" ;;
                    */fish) _rc="~/.config/fish/config.fish" ;;
                    *)      _rc="your shell profile" ;;
                esac
                if [ "${_rc}" = "~/.config/fish/config.fish" ]; then
                    dim "echo 'fish_add_path $BINDIR' >> $_rc"
                else
                    dim "echo 'export PATH=\"$BINDIR:\$PATH\"' >> $_rc"
                fi
                dim "then restart your shell, or run: export PATH=\"$BINDIR:\$PATH\""
            fi
            ;;
    esac
    printf '\n'
    dim "docs: https://github.com/$REPO"
    printf '\n'
}

main "$@"
