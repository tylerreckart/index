#!/usr/bin/env bash
set -euo pipefail

# ─────────────────────────────────────────────
# Claudius installer
# Usage: ./install.sh
# ─────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${CLAUDIUS_PREFIX:-/usr/local}"
BUILD_DIR="${SCRIPT_DIR}/build"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

RED='\033[0;31m'
GREEN='\033[0;32m'
DIM='\033[2m'
RESET='\033[0m'

info()  { echo -e "${GREEN}▸${RESET} $*"; }
dim()   { echo -e "${DIM}  $*${RESET}"; }
fail()  { echo -e "${RED}✗${RESET} $*" >&2; exit 1; }

# ── 1. Dependencies ──────────────────────────

if command -v brew &>/dev/null; then
    info "Installing dependencies via Homebrew..."
    brew bundle --file="${SCRIPT_DIR}/Brewfile" --no-lock --quiet
    OPENSSL_ROOT="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl)"
    CMAKE_SSL_FLAG="-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT}"
elif [[ -f /etc/debian_version ]]; then
    info "Debian/Ubuntu detected — checking system packages..."
    MISSING=()
    dpkg -s cmake      &>/dev/null || MISSING+=(cmake)
    dpkg -s libssl-dev  &>/dev/null || MISSING+=(libssl-dev)
    dpkg -s g++        &>/dev/null || MISSING+=(g++)
    if [[ ${#MISSING[@]} -gt 0 ]]; then
        info "Installing: ${MISSING[*]}"
        sudo apt-get update -qq
        sudo apt-get install -y -qq "${MISSING[@]}"
    fi
    CMAKE_SSL_FLAG=""
elif [[ -f /etc/redhat-release ]]; then
    info "RHEL/Fedora detected — checking system packages..."
    MISSING=()
    rpm -q cmake       &>/dev/null || MISSING+=(cmake)
    rpm -q openssl-devel &>/dev/null || MISSING+=(openssl-devel)
    rpm -q gcc-c++     &>/dev/null || MISSING+=(gcc-c++)
    if [[ ${#MISSING[@]} -gt 0 ]]; then
        info "Installing: ${MISSING[*]}"
        sudo dnf install -y "${MISSING[@]}"
    fi
    CMAKE_SSL_FLAG=""
else
    info "Unknown platform — assuming cmake, openssl, and a C++20 compiler are present."
    CMAKE_SSL_FLAG=""
fi

# ── 2. Build ─────────────────────────────────

info "Building claudius..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    ${CMAKE_SSL_FLAG} \
    -Wno-dev \
    2>&1 | sed 's/^/  /'

make -j"${JOBS}" 2>&1 | sed 's/^/  /'

# ── 3. Install ───────────────────────────────

info "Installing to ${PREFIX}/bin/claudius ..."

# Ensure bin dir exists
mkdir -p "${PREFIX}/bin" 2>/dev/null || sudo mkdir -p "${PREFIX}/bin"

install_file() {
    local src="$1" dst="$2"
    if [[ -w "${PREFIX}/bin" ]]; then
        cp "$src" "$dst"
    else
        dim "Requires sudo for ${PREFIX}/bin"
        sudo cp "$src" "$dst"
    fi
}

install_file "${BUILD_DIR}/claudius" "${PREFIX}/bin/claudius"
install_file "${SCRIPT_DIR}/scripts/claudius-cli.sh" "${PREFIX}/bin/claudius-cli"
chmod +x "${PREFIX}/bin/claudius-cli" 2>/dev/null || sudo chmod +x "${PREFIX}/bin/claudius-cli"

# ── 4. Init config (if first install) ────────

CLAUDIUS_DIR="${HOME}/.claudius"
if [[ ! -d "${CLAUDIUS_DIR}" ]]; then
    info "First install — initializing ~/.claudius/ ..."
    "${PREFIX}/bin/claudius" --init
else
    dim "~/.claudius/ exists — skipping init (run 'claudius --init' to reinitialize)"
fi

# ── 5. API key prompt ────────────────────────

if [[ -z "${ANTHROPIC_API_KEY:-}" ]] && [[ ! -f "${CLAUDIUS_DIR}/api_key" ]]; then
    echo ""
    info "No API key found."
    echo "  Set it now or later:"
    echo ""
    echo "    export ANTHROPIC_API_KEY=\"sk-ant-...\""
    echo "    # or"
    echo "    echo 'sk-ant-...' > ~/.claudius/api_key"
    echo ""
fi

# ── 6. Done ──────────────────────────────────

echo ""
info "Claudius installed."
echo ""
echo "  claudius              Interactive REPL"
echo "  claudius --serve      Start TCP server (port 9077)"
echo "  claudius --send <agent> <msg>"
echo "  claudius --help       Full usage"
echo ""
echo "  claudius-cli <host> <port> <token>    Remote client"
echo ""
