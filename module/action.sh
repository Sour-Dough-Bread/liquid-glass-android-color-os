#!/system/bin/sh
# The module's Action button (Magisk 28+, KernelSU, APatch): flips between
# Android's own blur and Liquid Glass. Presets and tuning live in the WebUI and
# in glassctl.
MODDIR=${0%/*}
sh "$MODDIR/glassctl" toggle
echo ""
sh "$MODDIR/glassctl" status
echo ""
echo "Presets and tuning: open the module's WebUI, or"
echo "  sh $MODDIR/glassctl help"
