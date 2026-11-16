#!/usr/bin/env python3
"""List the hosts a Chromium net log (--log-net-log) shows network activity
for, and fail when any is not allowed: the CEF backend must make no request
of its own (docs/backends.md, "No network requests of its own").

  scripts/netlog-hosts.py <netlog.json> [--allow HOST]... [--expect HOST]...

A host counts when the log has a request to it (a URL request, a preconnect
or a stream job), a DNS lookup of it, or a socket connect to it (the system
resolver's name servers, port 53, are judged by the names looked up). Loopback
(localhost, *.localhost, 127.0.0.0/8, ::1) is always allowed; --allow adds a
host. Exits 1 when a host is not allowed, or when an --expect host (the
app's own request, proving the log captured the run) is missing.

Secure DNS must be off, whatever the machine's resolver: Chrome's automatic
mode upgrades the system resolver to its DNS-over-HTTPS server when that
resolver is a known provider's, and then probes and queries it. Exits 1 when
the log has a DoH request (DOH_URL_REQUEST), or when the host resolver's last
configuration (DNS_CONFIG_CHANGED) has a secure DNS mode other than Off: on a
machine whose resolver no provider runs, automatic mode makes no request but
still shows in that configuration. A log cut
short (the process ended before Chromium closed it) is read up to its last
complete event.
"""

import ipaddress
import json
import re
import sys
from urllib.parse import urlsplit

# Event parameters that name where traffic goes.
URL_KEYS = ("url", "original_url", "location", "destination",
            "logical_destination")
HOST_KEYS = ("host", "hostname")
ADDRESS_KEYS = ("address", "address_list")
NETWORK_SCHEMES = ("http", "https", "ws", "wss", "ftp")


def load(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass
    # Cut short: keep every complete event line, then close the document.
    # Chromium writes one event per line, each ending in ",".
    lines = text.splitlines()
    while lines:
        candidate = "\n".join(lines).rstrip().rstrip(",") + "]}"
        try:
            return json.loads(candidate)
        except json.JSONDecodeError:
            lines.pop()
    raise SystemExit(f"{path}: not a net log")


def host_of_url(value):
    if not isinstance(value, str) or "://" not in value:
        return None
    try:
        parts = urlsplit(value)
    except ValueError:
        return None
    if parts.scheme.lower() not in NETWORK_SCHEMES:
        return None  # data:, blob:, the app's own schemes, chrome:, ...
    return parts.hostname


def host_of(key, value):
    if key in URL_KEYS:
        return host_of_url(value)
    if not isinstance(value, str) or not value:
        return None
    if "://" in value:  # the host resolver logs "https://name:443" forms
        return host_of_url(value)
    # "name:port", "[v6]:port", a bare name or address.
    m = re.fullmatch(r"\[([^\]]+)\](?::\d+)?", value)
    if m:
        return m.group(1)
    if value.count(":") == 1:
        return value.split(":", 1)[0]
    return value


# Events whose address is this machine's own end of a socket.
LOCAL_EVENTS = ("UDP_LOCAL_ADDRESS",)
# A UDP socket's connect sends nothing: Chromium connects one to a public
# address only to learn its route (the host resolver's IPv6 reachability
# probe). Such a socket counts once it sends (UDP_BYTES_SENT).
UDP_SOURCES = ("UDP_SOCKET", "UDP_CLIENT_SOCKET")
UDP_SENT = "UDP_BYTES_SENT"


def is_dns_server(value):
    # The system resolver's name servers: the lookups they answer are judged
    # by the names looked up (HOST_RESOLVER_* / DNS_TRANSACTION events).
    return isinstance(value, str) and value.endswith(":53")


def hosts_in(event_type, params):
    for key, value in params.items():
        if key in URL_KEYS or key in HOST_KEYS:
            host = host_of(key, value)
            if host:
                yield host.lower()
        # Addresses count where a socket connects to them, not where a DNS
        # answer lists them (the name looked up is judged instead).
        elif key in ADDRESS_KEYS and "CONNECT" in event_type:
            values = value if isinstance(value, list) else [value]
            for v in values:
                if is_dns_server(v):
                    continue
                host = host_of(key, v)
                if host:
                    yield host.lower()


DOH_REQUEST = "DOH_URL_REQUEST"
DNS_CONFIG = "DNS_CONFIG_CHANGED"


def secure_dns_problems(log, types, events):
    """What the log shows of secure DNS being on (empty when it is off)."""
    modes = {v: k for k, v in log["constants"].get("secureDnsMode", {}).items()}
    problems = []
    doh = sum(1 for e in events if types.get(e.get("type")) == DOH_REQUEST)
    if doh:
        problems.append(f"{doh} DNS-over-HTTPS request event(s) ({DOH_REQUEST})")
    last = None
    for event in events:
        params = event.get("params")
        if (types.get(event.get("type")) == DNS_CONFIG
                and isinstance(params, dict) and "secure_dns_mode" in params):
            last = params
    if last is not None:
        mode = modes.get(last["secure_dns_mode"], last["secure_dns_mode"])
        if mode != "Off":
            servers = [s.get("template") for s in
                       (last.get("doh_config") or {}).get("servers", [])
                       if isinstance(s, dict)]
            problems.append(f"the resolver's last secure DNS mode is {mode} "
                            f"(DoH servers: {servers or 'none'})")
    return problems


def is_loopback(host):
    host = host.strip("[]").rstrip(".")
    if host == "localhost" or host.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def main(argv):
    allow, expect, paths = set(), set(), []
    args = iter(argv)
    for arg in args:
        if arg == "--allow":
            allow.add(next(args).lower())
        elif arg == "--expect":
            expect.add(next(args).lower())
        else:
            paths.append(arg)
    if len(paths) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    log = load(paths[0])
    types = {v: k for k, v in log["constants"]["logEventTypes"].items()}
    sources = {v: k for k, v in log["constants"]["logSourceType"].items()}
    events = log.get("events", [])

    def source(event):
        src = event.get("source") or {}
        return sources.get(src.get("type")), src.get("id")

    sending = {source(e) for e in events if types.get(e.get("type")) == UDP_SENT}
    seen = {}
    for event in events:
        params = event.get("params")
        if not isinstance(params, dict):
            continue
        if types.get(event.get("type")) in LOCAL_EVENTS:
            continue
        if source(event)[0] in UDP_SOURCES and source(event) not in sending:
            continue
        for host in hosts_in(types.get(event.get("type"), "?"), params):
            seen.setdefault(host, set()).add(types.get(event.get("type"), "?"))
    bad = sorted(h for h in seen if not is_loopback(h) and h not in allow)
    secure_dns = secure_dns_problems(log, types, events)
    for problem in secure_dns:
        print(f"  SECURE DNS {problem}")
    for host in sorted(seen):
        tag = "UNEXPECTED" if host in bad else "ok"
        print(f"  {tag:<10} {host}  ({', '.join(sorted(seen[host]))})")
    missing = sorted(h for h in expect if h not in seen)
    for host in missing:
        print(f"  MISSING    {host} (the log should show the app's request)")
    print(f"{len(events)} events, {len(seen)} hosts, "
          f"{len(bad)} unexpected")
    return 1 if bad or missing or secure_dns else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
