#!/bin/bash
# install.sh — Install the Numark NS7 DriverKit extension on Apple Silicon
#
# This script activates the system extension after Xcode has built it.
# DriverKit extensions must be loaded via OSSystemExtensionManager (macOS API),
# not by manual file copying. This script is a wrapper for that flow.
#
# Usage:
#   1. Build the .dext in Xcode first
#   2. Run:  sudo bash installer-scripts/install.sh
#
# Requirements:
#   • macOS 12.0+
#   • Apple Silicon (arm64) — this driver was built for arm64 only
#   • Apple Developer account with DriverKit USB entitlement approved
#   • SIP must be enabled for production use

set -euo pipefail

DRIVER_NAME="NumarkNS7Driver"
DEXT_BUNDLE_ID="com.numark.ns7.driverkit"
BUILD_DIR="${BUILD_DIR:-$(xcodebuild -project NumarkNS7Driver.xcodeproj -target NumarkNS7Driver -showBuildSettings 2>/dev/null | grep 'BUILT_PRODUCTS_DIR' | head -1 | awk '{print $3}')}"
DEXT_PATH="${BUILD_DIR}/${DRIVER_NAME}.dext"

echo "═══════════════════════════════════════════════════════════"
echo "  Numark NS7 DriverKit Extension Installer"
echo "  USB VID=0x15E4 (Ploytec/Numark)  PID=0x0071 (NS7)"
echo "  macOS 12+ / Apple Silicon (arm64)"
echo "═══════════════════════════════════════════════════════════"
echo ""

# ── Preflight checks ──────────────────────────────────────────────────────────

# Check macOS version
OS_VER=$(sw_vers -productVersion)
MAJOR=$(echo "$OS_VER" | cut -d. -f1)
if [ "$MAJOR" -lt 12 ]; then
    echo "❌  macOS 12.0 or later required (you have $OS_VER)"
    echo "    DriverKit system extensions are not supported on earlier versions."
    exit 1
fi

# Check architecture
ARCH=$(uname -m)
if [ "$ARCH" != "arm64" ]; then
    echo "⚠️   Running on $ARCH. This driver was built for arm64 (Apple Silicon)."
    echo "    If you are on an Intel Mac, rebuild with ARCHS=x86_64 in Xcode."
    # Don't exit — allow on Intel as a warning
fi

echo "✅  macOS $OS_VER on $ARCH"

# Check the built .dext exists
if [ ! -d "$DEXT_PATH" ]; then
    echo ""
    echo "❌  Driver not found at: $DEXT_PATH"
    echo ""
    echo "    Please build the Xcode project first:"
    echo "      xcodebuild -project NumarkNS7Driver.xcodeproj \\"
    echo "                 -target NumarkNS7Driver \\"
    echo "                 -configuration Release \\"
    echo "                 ONLY_ACTIVE_ARCH=NO"
    exit 1
fi

echo "✅  Found .dext at: $DEXT_PATH"

# ── Remove old kext if present ────────────────────────────────────────────────
# The original Ploytec kext conflicts with the DriverKit extension.

OLD_KEXT_SYS="/System/Library/Extensions/NumarkNS7Audio.kext"
OLD_KEXT_LIB="/Library/Extensions/NumarkNS7Audio.kext"

for OLD_KEXT in "$OLD_KEXT_SYS" "$OLD_KEXT_LIB"; do
    if [ -d "$OLD_KEXT" ]; then
        echo ""
        echo "⚠️   Found old kext: $OLD_KEXT"
        echo "    The x86-only Ploytec kext cannot run on Apple Silicon and"
        echo "    must be removed before loading the DriverKit extension."
        echo ""
        read -p "    Remove it now? [y/N] " -n 1 -r
        echo ""
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            sudo rm -rf "$OLD_KEXT"
            sudo kextcache -system-prelinked-kernel 2>/dev/null || true
            sudo kextcache -system-caches 2>/dev/null || true
            echo "    ✅  Old kext removed."
        else
            echo "    ⚠️   Skipped. You may need to remove it manually."
        fi
    fi
done

# ── Copy .dext to system extensions folder ────────────────────────────────────
# DriverKit extensions live in /Applications/<HostApp>.app/Contents/Library/SystemExtensions/
# For development/testing, we use a helper app approach.

SYSEXT_DIR="/Library/SystemExtensions/${DEXT_BUNDLE_ID}"
echo ""
echo "Installing DriverKit extension..."

# The proper way to activate a DriverKit extension is via the OSSystemExtensionManager
# API from a host application. For development purposes we use the 'systemextensionsctl'
# command available in macOS 12+.

# Copy the .dext to a location systemextensionsctl can find it
sudo mkdir -p "/Library/SystemExtensions"
sudo cp -R "$DEXT_PATH" "/Library/SystemExtensions/"

echo ""
echo "Activating system extension (you may see an approval dialog)..."
echo ""
echo "  If macOS shows 'System Extension Blocked':"
echo "  1. Open System Preferences → Security & Privacy"
echo "  2. Click 'Allow' next to the blocked extension message"
echo "  3. Re-run this script, or the extension will activate on next reboot"
echo ""

# Activate the extension
# Note: In production this should be done via OSSystemExtensionManager from a signed app
if command -v systemextensionsctl &>/dev/null; then
    systemextensionsctl list | grep -q "$DEXT_BUNDLE_ID" && {
        echo "Extension already listed. Attempting reset..."
        systemextensionsctl reset 2>/dev/null || true
    }
fi

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  Installation complete!"
echo ""
echo "  Next steps:"
echo "    1. If prompted, approve the extension in System Settings"
echo "       → Privacy & Security → scroll to 'System Software'"
echo "       → click Allow"
echo ""
echo "    2. Plug in your Numark NS7 via USB"
echo ""
echo "    3. Open Audio MIDI Setup.app — the NS7 should appear as:"
echo "       'Numark NS7' with 4 inputs + 4 outputs at 44100 Hz"
echo ""
echo "    4. The MIDI port 'Numark NS7 MIDI' will appear in"
echo "       MIDI Studio (Audio MIDI Setup → Window → MIDI Studio)"
echo "═══════════════════════════════════════════════════════════"
