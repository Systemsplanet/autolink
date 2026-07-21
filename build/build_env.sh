#!/bin/bash
# build_env.sh — set up an ESP32 Arduino build environment that
# matches ArduinoDroid's build target (esp32:esp32@3.3.5, board
# esp32:esp32:firebeetle32 for DFRobot FireBeetle-ESP32).
#
# Installs arduino-cli (latest stable), clang-format, and the
# Espressif esp32 Arduino core. Idempotent: re-running on an
# already-set-up env is a no-op.
#
# clang-format is needed by build/pretty_print.py, which the
# top-level Makefile runs as a pre-flight for `make test` /
# `make all`. pretty_print.py has its own pip-install fallback
# for sandboxes; installing clang-format here makes the
# one-shot env script the canonical entry point.
#
# After this script runs, the verify_build sketch can be compiled
# with ./build/verify_build.sh.

set -e

ARDUINO_CLI_VERSION="${ARDUINO_CLI_VERSION:-1.5.1}"
ESP32_CORE_VERSION="${ESP32_CORE_VERSION:-3.3.5}"
ESP32_FQBN="${ESP32_FQBN:-esp32:esp32:firebeetle32}"
CLANG_FORMAT_VERSION="${CLANG_FORMAT_VERSION:-18}"

echo "Target: arduino-cli $ARDUINO_CLI_VERSION + esp32:esp32@$ESP32_CORE_VERSION + $ESP32_FQBN + clang-format $CLANG_FORMAT_VERSION"

# 1. Install arduino-cli if missing
if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "  installing arduino-cli ..."
    cd /tmp
    if [ "$ARDUINO_CLI_VERSION" = "latest" ]; then
        curl -sL -o arduino-cli.tar.gz \
            "https://github.com/arduino/arduino-cli/releases/latest/download/arduino-cli_Linux_64bit.tar.gz"
    else
        curl -sL -o arduino-cli.tar.gz \
            "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz"
    fi
    tar -xzf arduino-cli.tar.gz
    mv arduino-cli /usr/local/bin/arduino-cli
    chmod +x /usr/local/bin/arduino-cli
    echo "  installed: $(arduino-cli version | head -1)"
else
    echo "  arduino-cli already installed: $(arduino-cli version | head -1)"
fi

# 2. Install clang-format if missing. apt-get first (the
# GitHub Actions Ubuntu channel); pip with
# --break-system-packages as the fallback for stripped
# containers where the apt repo doesn't carry the package.
# Idempotent: the apt branch is a no-op if already installed,
# and pip refuses to re-install without --upgrade.
if ! command -v clang-format >/dev/null 2>&1; then
    echo "  installing clang-format ..."
    if command -v apt-get >/dev/null 2>&1; then
        if command -v sudo >/dev/null 2>&1; then
            sudo -n apt-get install -y "clang-format-$CLANG_FORMAT_VERSION" \
                || sudo -n apt-get install -y clang-format || true
        else
            apt-get install -y "clang-format-$CLANG_FORMAT_VERSION" \
                || apt-get install -y clang-format || true
        fi
    fi
    if ! command -v clang-format >/dev/null 2>&1; then
        # PEP 668-managed distros (Debian 12+, Ubuntu 23.04+)
        # need --break-system-packages for system-wide pip.
        pip3 install --break-system-packages clang-format \
            || pip3 install --user --break-system-packages clang-format \
            || pip3 install --user clang-format || true
        # A --user install running as root may have placed
        # the wrapper off-PATH; prepend ~/.local/bin so
        # subsequent steps (and pretty_print.py) find it.
        if [ -x "$HOME/.local/bin/clang-format" ]; then
            export PATH="$HOME/.local/bin:$PATH"
        fi
    fi
    if command -v clang-format >/dev/null 2>&1; then
        echo "  installed: $(clang-format --version | head -1)"
    else
        echo "  WARN: clang-format install failed; build/pretty_print.py will retry at run time"
    fi
else
    echo "  clang-format already installed: $(clang-format --version | head -1)"
fi

# 3. Add Espressif package index (idempotent)
arduino-cli config set board_manager.additional_urls \
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json \
    2>/dev/null || true

# 4. Install esp32 core if missing
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32@$ESP32_CORE_VERSION"; then
    echo "  installing esp32:esp32@$ESP32_CORE_VERSION (this can take a few minutes) ..."
    arduino-cli core install "esp32:esp32@$ESP32_CORE_VERSION"
else
    echo "  esp32:esp32@$ESP32_CORE_VERSION already installed"
fi

# 5. Verify
echo ""
echo "Ready. To verify a build:"
echo "  $ESP32_FQBN"
echo ""
echo "  arduino-cli compile --fqbn $ESP32_FQBN \\"
echo "    --library $PWD \\"
echo "    build/verify_build.ino"
echo ""
echo "Or use the wrapper: ./build/verify_build.sh"
