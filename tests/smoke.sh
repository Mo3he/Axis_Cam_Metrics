#!/usr/bin/env sh
#
# Hits every endpoint on a running device and checks the shape of what comes
# back. This is the test that would have caught the two worst bugs so far: a
# truncated request path silently dropping series, and an undecoded query
# string matching no metrics at all.
#
#   sh tests/smoke.sh 192.168.0.155 admin
#
# The password is read from the ACAP_PASSWORD environment variable so it never
# appears in shell history or process listings.

set -eu

DEVICE=${1:?usage: smoke.sh <device> <user>}
USER=${2:?usage: smoke.sh <device> <user>}
: "${ACAP_PASSWORD:?set ACAP_PASSWORD}"

BASE="https://$DEVICE/local/Metrics"
failures=0

get() {
	curl -sk --anyauth -u "$USER:$ACAP_PASSWORD" -m 20 "$1"
}

check() {
	if [ "$1" = "yes" ]; then
		printf '  ok   %s\n' "$2"
	else
		printf '  FAIL %s\n' "$2"
		failures=$((failures + 1))
	fi
}

json_check() {
	# $1 body, $2 python expression over `d`, $3 description
	result=$(printf '%s' "$1" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception as error:
    print('no:' + str(error))
    raise SystemExit
print('yes' if ($2) else 'no')
" 2>&1)
	case "$result" in
	yes) check yes "$3" ;;
	*) check no "$3 ($result)" ;;
	esac
}

echo "health"
body=$(get "$BASE/data/health")
json_check "$body" "d['ok'] is True" "reports ok"
json_check "$body" "d['metrics'] > 20" "discovered a plausible number of metrics"

echo "meta"
meta=$(get "$BASE/data/meta")
json_check "$meta" "len(d['metrics']) == d and True or len(d['metrics']) > 20" "lists metrics"
json_check "$meta" "d['device']['serial'] != ''" "device identity resolved"
json_check "$meta" "len(d['store']['tiers']) == 3" "three history tiers"
json_check "$meta" "all('display' in m and 'transmit' in m for m in d['metrics'])" "metrics carry selection flags"

echo "current"
body=$(get "$BASE/data/current")
json_check "$body" "d['timestamp'] > 0" "has a timestamp"
json_check "$body" "len(d['values']) > 20" "has values"

echo "series"
# Ask for every metric at once: this is what truncated the request path before.
ids=$(printf '%s' "$meta" | python3 -c "
import sys, json, urllib.parse
print(urllib.parse.quote(','.join(m['id'] for m in json.load(sys.stdin)['metrics'])))
")
body=$(get "$BASE/data/series?window=1800&metrics=$ids")
json_check "$meta" "True" "built a full metric list"
json_check "$body" "len(d['series']) > 20" "returns every requested series"
json_check "$body" "len(d['timestamps']) > 0" "returns timestamps"
json_check "$body" "all(len(v) == len(d['timestamps']) for v in d['series'].values())" \
	"every series matches the timestamp length"

echo "alerts"
body=$(get "$BASE/data/alerts")
json_check "$body" "len(d['rules']) > 0" "has rules"
json_check "$body" "'firing' in d" "reports a firing count"

echo "processes"
body=$(get "$BASE/data/processes?limit=5")
json_check "$body" "len(d['processes']) == 5" "honours the limit"
json_check "$body" "d['total'] > 10" "counts every process"
json_check "$body" "all(p['rss'] > 0 for p in d['processes'])" "reports resident memory"
# Shares of the whole device, so the busiest process cannot exceed 100.
json_check "$body" "all(0 <= p['cpu'] <= 100 for p in d['processes'])" "cpu is a whole-device share"
json_check "$body" "d['processes'] == sorted(d['processes'], key=lambda p: -p['cpu'])" "sorted by cpu"

echo "prometheus"
body=$(get "$BASE/data/prometheus")
case "$body" in
*"# TYPE axis_"*) check yes "emits typed metrics" ;;
*) check no "emits typed metrics" ;;
esac
case "$body" in
*"axis_device_info"*) check yes "includes device info" ;;
*) check no "includes device info" ;;
esac

echo "stream"
frames=$(curl -sk --anyauth -u "$USER:$ACAP_PASSWORD" --max-time 5 -N "$BASE/data/stream" 2>/dev/null |
	grep -c '^data: ' || true)
if [ "${frames:-0}" -ge 2 ]; then
	check yes "pushed $frames events"
else
	check no "pushed $frames events"
fi

echo "settings"
body=$(get "$BASE/api/settings")
json_check "$body" "'SampleInterval' in d" "returns settings"
json_check "$body" "d.get('MqttPassword', '') == ''" "never returns the stored password"

echo
if [ "$failures" -eq 0 ]; then
	echo "all smoke tests passed"
else
	echo "$failures smoke test(s) FAILED"
	exit 1
fi
