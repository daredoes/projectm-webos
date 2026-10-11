#!/bin/sh
# Installs an IPK on a TV, runs the preset benchmark to the end and copies the results here.
#
#   TV_PASSWORD=... scripts/benchmark-tv.sh HOST IPK OUT.tsv [fresh]
#
# "fresh" deletes earlier results on the TV first; otherwise the benchmark resumes. The app
# is relaunched whenever it stops before every preset has a result (standby, the TV's own
# screensaver, someone pressing Back). The Sendspin service keeps the screensaver away while
# it runs, and its earlier keepAwake setting is put back at the end.
set -eu

HOST="$1" IPK="$2" OUT="$3" FRESH="${4:-}"
APP=com.daredoes.projectm
RESULTS=/media/internal/projectm-webos-bench.tsv
PRESETS=/media/developer/apps/usr/palm/applications/$APP/presets
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR"
export SSHPASS="$TV_PASSWORD"
tv() { sshpass -e ssh $OPTS "root@$HOST" "$@"; }
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
tv "rm -f /media/developer/bench.ipk; ${FRESH:+rm -f $RESULTS}"

KEEP_AWAKE=$(tv "luna-send -n 1 luna://com.sendspin.webos.service/status '{}'" | grep -o '"keepAwake": *[a-z]*' | grep -o '[a-z]*$' || echo false)
tv "luna-send -n 1 luna://com.sendspin.webos.service/setKeepAwake '{\"keepAwake\":true}'" >/dev/null
trap 'tv "luna-send -n 1 luna://com.sendspin.webos.service/setKeepAwake \"{\\\"keepAwake\\\":$KEEP_AWAKE}\"" >/dev/null || true' EXIT

TOTAL=$(retry tv "find $PRESETS -name '*.milk' -o -name '*.prjm' | wc -l")
log "$TOTAL presets"
while :; do
  DONE=$(tv "cat $RESULTS 2>/dev/null | wc -l" || echo "")
  if [ -z "$DONE" ]; then
    log "TV unreachable, waiting"
  elif [ "$DONE" -ge "$TOTAL" ]; then
    break
  elif ! tv "ps | grep -q [p]rojectm-webos"; then
    log "$DONE/$TOTAL done, launching benchmark"
    tv "luna-send -n 1 luna://com.webos.applicationManager/launch '{\"id\":\"$APP\",\"params\":{\"benchmark\":true}}'" >/dev/null || true
  else
    log "$DONE/$TOTAL done"
  fi
  sleep 60
done

retry sshpass -e scp $OPTS "root@$HOST:$RESULTS" "$OUT"
tv "grep GL: /tmp/projectm-webos.log" || true
log "results in $OUT"
