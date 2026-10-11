#!/bin/sh
# Installs an IPK on a TV, runs the preset benchmark to the end and copies the results here.
#
#   TV_PASSWORD=... scripts/benchmark-tv.sh HOST IPK OUT.tsv [fresh]
#
# "fresh" deletes earlier results on the TV first; otherwise the benchmark resumes. The app
# is relaunched whenever results stop coming in before every preset has one (standby, the
# TV's own screensaver, someone pressing Back). The Sendspin service keeps the screensaver away while
# it runs, and its earlier keepAwake setting is put back at the end.
set -eu

HOST="$1" IPK="$2" OUT="$3" FRESH="${4:-}"
APP=com.daredoes.projectm
# The app writes to /media/internal, or to /media/developer where its jail mounts
# /media/internal read-only.
RESULTS="/media/internal/projectm-webos-bench.tsv /media/developer/projectm-webos-bench.tsv"
PRESETS=/media/developer/apps/usr/palm/applications/$APP/presets
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR"
export SSHPASS="$TV_PASSWORD"
tv() { sshpass -e ssh $OPTS "root@$HOST" "$@"; }
# luna-send prints nothing without a PTY on newer firmware.
luna() { sshpass -e ssh -tt $OPTS "root@$HOST" "luna-send -n 1 $1 '$2'" | tr -d '\r'; }
log() { echo "[$(date +%H:%M:%S)] $*"; }

# luna-send -i only streams the install status over a PTY, and busybox timeout takes -t on
# older firmware but not on newer.
install() {
  sshpass -e ssh -tt $OPTS "root@$HOST" "if timeout 1 true 2>/dev/null; then TO='timeout 120'; else TO='timeout -t 120'; fi; \
\$TO luna-send -i -f luna://com.webos.appInstallService/dev/install '{\"id\":\"$APP\",\"ipkUrl\":\"/media/developer/bench.ipk\",\"subscribe\":true}'" \
    | tr -d '\r' | grep -q '"state": "installed"'
}

# The TV drops off the network when busy or in standby; keep trying for up to 10 minutes.
retry() {
  for _ in $(seq 60); do "$@" && return 0; sleep 10; done
  return 1
}

log "installing $IPK on $HOST"
retry sshpass -e scp $OPTS "$IPK" "root@$HOST:/media/developer/bench.ipk"
retry install
tv "rm -f /media/developer/bench.ipk ${FRESH:+$RESULTS}"

KEEP_AWAKE=$(luna luna://com.sendspin.webos.service/status '{}' | grep -o '"keepAwake": *[a-z]*' | grep -o '[a-z]*$' | head -1)
KEEP_AWAKE=${KEEP_AWAKE:-false}
luna luna://com.sendspin.webos.service/setKeepAwake '{"keepAwake":true}' >/dev/null
trap 'luna luna://com.sendspin.webos.service/setKeepAwake "{\"keepAwake\":$KEEP_AWAKE}" >/dev/null || true' EXIT

TOTAL=$(retry tv "find $PRESETS -name '*.milk' -o -name '*.prjm' | wc -l")
log "$TOTAL presets"
# A jailed app doesn't show in the TV's ps, so a run counts as stopped when a minute
# passes without a new result.
launch() { luna luna://com.webos.applicationManager/launch "{\"id\":\"$APP\",\"params\":{\"benchmark\":true}}" >/dev/null || true; }
launch
LAST=-1
while :; do
  sleep 60
  DONE=$(tv "cat $RESULTS 2>/dev/null | wc -l" || echo "")
  if [ -z "$DONE" ]; then
    log "TV unreachable, waiting"
  elif [ "$DONE" -ge "$TOTAL" ]; then
    break
  elif [ "$DONE" -le "$LAST" ]; then
    log "$DONE/$TOTAL done, launching benchmark"
    launch
  else
    log "$DONE/$TOTAL done"
  fi
  LAST=${DONE:-$LAST}
done

retry sh -c "sshpass -e ssh $OPTS root@$HOST 'cat $RESULTS 2>/dev/null' > '$OUT'"
tv "grep GL: /tmp/projectm-webos.log" || true
log "results in $OUT"
