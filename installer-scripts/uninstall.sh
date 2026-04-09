#!/bin/bash
# uninstall.sh — Remove the Numark NS7 DriverKit extension

set -euo pipefail

DEXT_BUNDLE_ID="com.numark.ns7.driverkit"

echo "═══════════════════════════════════════════════════════════"
echo "  Numark NS7 DriverKit Extension — Uninstaller"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Deactivate via systemextensionsctl
if command -v systemextensionsctl &>/dev/null; then
    echo "Deactivating system extension..."
    systemextensionsctl list 2>/dev/null | grep "$DEXT_BUNDLE_ID" && \
        systemextensionsctl uninstall - "$DEXT_BUNDLE_ID" 2>/dev/null || true
fi

# Remove files
for PATH_TO_REMOVE in \
    "/Library/SystemExtensions/${DEXT_BUNDLE_ID}" \
    "/Library/SystemExtensions/NumarkNS7Driver.dext"
do
    if [ -e "$PATH_TO_REMOVE" ]; then
        echo "Removing: $PATH_TO_REMOVE"
        sudo rm -rf "$PATH_TO_REMOVE"
    fi
done

echo ""
echo "✅  Numark NS7 DriverKit extension removed."
echo "    Unplug and replug your NS7 if it was connected."
