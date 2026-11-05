# QuestLHSync install: lhsight reads the tracking cameras of a Pico 4
DEV=$(getprop ro.product.device)
case "$DEV" in
  PICOA8110) ui_print "- Pico 4 ($DEV)" ;;
  *) ui_print "! this is $DEV, not a Pico 4 (PICOA8110): lhsight may not find its cameras" ;;
esac
[ -f /mnt/vendor/persist/pvr/mav0/cam0/sensor.yaml ] || ui_print "! no camera calibration in /mnt/vendor/persist/pvr/mav0"
set_perm "$MODPATH/lhsyncd" 0 0 0755
set_perm "$MODPATH/frida-inject" 0 0 0755
ui_print "- lhsyncd starts at boot. To start it now:"
ui_print "  su -c 'setsid sh $MODPATH/service.sh </dev/null >/dev/null 2>&1 &'"
ui_print "- log: /dev/.questlhsync/lhsyncd.log"
