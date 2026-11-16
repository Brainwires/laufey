#!/usr/bin/env bash
#
# Offline tests for scripts/netlog-hosts.py: each case writes a small net log
# (the shape Chromium's --log-net-log writes) and checks the script's exit
# status and the line it prints. No network, no browser.
#
#   scripts/test-netlog-hosts.sh
set -euo pipefail
cd "$(dirname "$0")/.."

py=$(command -v python3 || command -v python)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

# netlog <events...>: a net log holding the given event objects (JSON).
netlog() {
  local file=$tmp/netlog.json sep="" e
  {
    printf '{"constants": {"logEventTypes": {"URL_REQUEST_START_JOB": 1,'
    printf ' "DNS_CONFIG_CHANGED": 2, "DOH_URL_REQUEST": 3, "TCP_CONNECT": 4},'
    printf ' "logSourceType": {"URL_REQUEST": 1, "NONE": 0},'
    printf ' "secureDnsMode": {"Off": 0, "Automatic": 1, "Secure": 2}},\n'
    printf '"events": ['
    for e in "$@"; do
      printf '%s\n%s' "$sep" "$e"
      sep=,
    done
    printf ']}\n'
  } >"$file"
  echo "$file"
}

APP='{"type": 1, "time": "1", "source": {"type": 1, "id": 1}, "params": {"url": "http://127.0.0.1:4000/echo"}}'
OFF='{"type": 2, "time": "0", "source": {"type": 0, "id": 0}, "params": {"secure_dns_mode": 0, "doh_config": {"servers": []}}}'
AUTO='{"type": 2, "time": "0", "source": {"type": 0, "id": 0}, "params": {"secure_dns_mode": 1, "doh_config": {"servers": [{"template": "https://dns.google/dns-query{?dns}"}]}}}'
AUTO_NONE='{"type": 2, "time": "0", "source": {"type": 0, "id": 0}, "params": {"secure_dns_mode": 1, "doh_config": {"servers": []}}}'
DOH='{"type": 3, "time": "5000", "source": {"type": 1, "id": 2}, "params": {}}'
GOOGLE='{"type": 1, "time": "2", "source": {"type": 1, "id": 3}, "params": {"url": "https://dns.google/dns-query"}}'

# expect <name> <exit status> <output pattern> <events...>
expect() {
  local name=$1 want=$2 pattern=$3 out status=0
  shift 3
  out=$("$py" -I scripts/netlog-hosts.py "$(netlog "$@")" --expect 127.0.0.1) || status=$?
  if [ "$status" != "$want" ] || ! grep -Eq -- "$pattern" <<<"$out"; then
    echo "FAIL $name: exit $status (want $want), output:"
    echo "$out" | sed 's/^/    /'
    failures=$((failures + 1))
  else
    echo "ok   $name"
  fi
}

expect "loopback only, secure DNS off" 0 "0 unexpected" "$OFF" "$APP"
expect "no resolver configuration logged" 0 "0 unexpected" "$APP"
expect "secure DNS automatic with a provider's DoH server" 1 \
  "SECURE DNS the resolver's last secure DNS mode is Automatic" "$OFF" "$AUTO" "$APP"
expect "secure DNS automatic with no DoH server (any resolver)" 1 \
  "last secure DNS mode is Automatic \(DoH servers: none\)" "$AUTO_NONE" "$APP"
expect "turned off before any lookup" 0 "0 unexpected" "$AUTO" "$OFF" "$APP"
expect "a DoH request" 1 "SECURE DNS 1 DNS-over-HTTPS request" "$OFF" "$APP" "$DOH"
expect "a DoH provider's host" 1 "UNEXPECTED dns.google" "$OFF" "$APP" "$GOOGLE"
expect "the app's request missing" 1 "MISSING +127.0.0.1" "$OFF"

if [ "$failures" -gt 0 ]; then
  echo "$failures case(s) failed"
  exit 1
fi
echo "all cases passed"
