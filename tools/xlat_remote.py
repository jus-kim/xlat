#!/usr/bin/env python3
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Command-line client for the XLAT serial remote-control protocol."""

import argparse
import datetime as dt
import json
import math
import re
import shlex
import statistics
import sys
import time

try:
    import serial
except ImportError:
    print("error: pyserial is required (python -m pip install pyserial)", file=sys.stderr)
    raise SystemExit(2)


CSV_RE = re.compile(r"^(\d+);(\d+);(\d+);(\d+)$")


class RemoteError(RuntimeError):
    pass


def scalar(value):
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    return value


def parse_fields(tokens):
    result = {}
    for token in tokens:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = scalar(value)
    return result


def parse_protocol_line(line):
    try:
        tokens = shlex.split(line)
    except ValueError as exc:
        raise RemoteError(f"malformed protocol line: {line!r}") from exc
    return tokens, parse_fields(tokens[2:])


class XlatRemote:
    def __init__(self, port):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 1_000_000
        self.serial.bytesize = serial.EIGHTBITS
        self.serial.parity = serial.PARITY_NONE
        self.serial.stopbits = serial.STOPBITS_ONE
        self.serial.dsrdtr = False
        self.serial.rtscts = False
        self.serial.timeout = 0.05
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.rx_buffer = b""
        self.boots_seen = 0

    def close(self):
        self.serial.close()

    def write_command(self, command):
        self.serial.write((command + "\n").encode("ascii"))

    def read_line(self, deadline):
        # Only ever returns whole lines: a read timeout must not split one in two.
        while True:
            if b"\n" in self.rx_buffer:
                raw, self.rx_buffer = self.rx_buffer.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip()
                if line.startswith("@event boot "):
                    self.boots_seen += 1
                return line
            if time.monotonic() >= deadline:
                raise RemoteError("timed out waiting for the XLAT device")
            self.rx_buffer += self.serial.read(max(1, self.serial.in_waiting))

    def request(self, command, timeout=5.0):
        name = command.split()[0]
        self.write_command(command)
        deadline = time.monotonic() + timeout
        data_lines = []
        while True:
            line = self.read_line(deadline)
            if not line.startswith("@"):
                continue
            if line.startswith("@d ") or line.startswith("@ep "):
                data_lines.append(line)
                continue
            if line.startswith("@err "):
                tokens = line.split(maxsplit=3)
                if len(tokens) >= 3 and tokens[1] == name:
                    raise RemoteError(line)
                continue
            if line.startswith("@ok "):
                tokens, fields = parse_protocol_line(line)
                if len(tokens) >= 2 and tokens[1] == name:
                    return fields, data_lines

    def wait_for_boot(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            line = self.read_line(deadline)
            if line.startswith("@event boot "):
                _, fields = parse_protocol_line(line)
                return fields

    def usb(self, timeout=5.0):
        fields, lines = self.request("usb", timeout)
        endpoints = []
        for line in lines:
            tokens = shlex.split(line)
            endpoints.append(parse_fields(tokens[1:]))
        fields["endpoints"] = endpoints
        return fields

    def reset_and_wait(self, timeout, settle=0.0):
        # The firmware reboots itself if USB enumeration hangs, so stay silent-tolerant here:
        # a command can go unanswered for a few seconds, and more than one boot can appear.
        deadline = time.monotonic() + timeout
        self.boots_seen = 0
        while time.monotonic() < deadline:
            try:
                self.request("reset", timeout=1.0)
                break
            except RemoteError:
                continue
        while time.monotonic() < deadline:
            try:
                info = self.usb(1.0)
            except RemoteError:
                continue
            if self.boots_seen and info.get("ready") == 1 and (info.get("button_bits", 0) or info.get("motion_bits", 0)):
                if self.boots_seen > 1:
                    print(f"warning: {self.boots_seen} boots during reset (enumeration hung and recovered)", file=sys.stderr)
                info["boots_seen"] = self.boots_seen
                time.sleep(settle)
                return info
            time.sleep(0.2)
        raise RemoteError("timed out waiting for USB readiness after reset")

    def dump_chunk(self, start, count):
        last_problem = ""
        for _ in range(3):
            fields, lines = self.request(f"dump {start} {count}", timeout=30.0)
            values = []
            for line in lines:
                parts = line.split()
                values.extend(int(value) for value in parts[2:])
            expected_sum = fields.get("sum")
            if len(values) == fields.get("n") == count and (sum(values) & 0xFFFFFFFF) == expected_sum:
                return values, True
            last_problem = f"dump chunk {start}:{start + count} failed length/sum validation"
        print(f"warning: {last_problem}", file=sys.stderr)
        return values, False


def print_json(value):
    print(json.dumps(value, separators=(",", ":"), sort_keys=True))


def simple_command(remote, args):
    if args.command == "reset":
        print_json(remote.reset_and_wait(args.timeout))
        return 0
    if args.command == "usb":
        print_json(remote.usb())
        return 0
    if args.command == "start":
        fields, _ = remote.request(f"start {args.n}")
    elif args.command == "dump":
        command = "dump"
        if args.start is not None:
            command += f" {args.start}"
            if args.count is not None:
                command += f" {args.count}"
        elif args.count is not None:
            command += f" 0 {args.count}"
        fields, lines = remote.request(command, timeout=30.0)
        values = []
        for line in lines:
            values.extend(int(value) for value in line.split()[2:])
        print_json(values)
        return 0
    elif args.command == "set":
        fields, _ = remote.request(f"set {args.key} {args.value}")
    else:
        fields, _ = remote.request(args.command)
    print_json(fields)
    return 0


def apply_run_settings(remote, args):
    settings = [
        ("edge", args.edge),
        ("bias", args.bias),
        ("mode", args.mode),
        ("debounce_ms", args.debounce_ms),
        ("interval_ms", args.interval_ms),
        ("level", args.level),
        ("output", args.output),
        ("release_holdoff_ms", args.release_holdoff_ms),
    ]
    for key, value in settings:
        if value is not None:
            remote.request(f"set {key} {value}")
    fields, _ = remote.request("get")
    return fields


def run_command(remote, args):
    started_at = dt.datetime.now(dt.timezone.utc).isoformat()
    boots_seen = 0
    if args.reset:
        boots_seen = remote.reset_and_wait(30.0, args.settle)["boots_seen"]
    ping, _ = remote.request("ping")
    settings = apply_run_settings(remote, args)
    usb = remote.usb()
    if usb.get("ready") != 1:
        raise RemoteError("USB device is not ready")
    remote.request("clear")
    remote.request(f"start {args.n}")

    interval = int(settings["interval_ms"])
    timeout = args.timeout if args.timeout is not None else args.n * (interval + 40) / 1000.0 + 30.0
    deadline = time.monotonic() + timeout
    csv_rows = []
    done = None
    while done is None:
        line = remote.read_line(deadline)
        match = CSV_RE.fullmatch(line)
        if match:
            row = tuple(int(value) for value in match.groups())
            csv_rows.append(row)
            if row[0] % 100 == 0:
                print(f"{row[0]} samples", file=sys.stderr)
        elif line.startswith("@event done "):
            _, done = parse_protocol_line(line)

    status, _ = remote.request("status")
    samples = []
    chunk_integrity = True
    buffered = int(status.get("buffered", 0))
    for start in range(0, buffered, 512):
        count = min(512, buffered - start)
        values, valid = remote.dump_chunk(start, count)
        samples.extend(values)
        chunk_integrity &= valid

    count = int(status["count"])
    firmware_sum = int(status["sum"])
    checks = {
        "dump_chunks": chunk_integrity,
        "dump_count": len(samples) == count,
        "dump_sum": sum(samples) == firmware_sum,
        "csv_status": False,
        "csv_samples": True,
    }
    if csv_rows:
        last = csv_rows[-1]
        checks["csv_status"] = last == (
            count, int(status["last"]), int(status["avg"]), int(status["stdev"])
        )
        for csv_count, latency, _, _ in csv_rows:
            if 1 <= csv_count <= len(samples) and samples[csv_count - 1] != latency:
                checks["csv_samples"] = False
                break

    mean_exact = statistics.fmean(samples) if samples else 0.0
    stdev_exact = statistics.pstdev(samples) if samples else 0.0
    ended_at = dt.datetime.now(dt.timezone.utc).isoformat()
    result = {
        "requested": int(done.get("requested", args.n)),
        "triggers": int(status["triggers"]),
        "count": count,
        "avg_us": int(status["avg"]),
        "stdev_us": int(status["stdev"]),
        "min_us": int(status["min"]),
        "max_us": int(status["max"]),
        "last_us": int(status["last"]),
        "mean_exact_us": mean_exact,
        "stdev_exact_us": stdev_exact,
        "samples": samples,
        "settings": settings,
        "usb": usb,
        "fw": ping.get("fw", ""),
        "boots_during_reset": boots_seen,
        "checks": checks,
        "started_at": started_at,
        "ended_at": ended_at,
    }
    encoded = json.dumps(result, separators=(",", ":"), sort_keys=True)
    print(encoded)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as output:
            output.write(encoded + "\n")

    dump_failed = not (checks["dump_chunks"] and checks["dump_count"] and checks["dump_sum"])
    return 1 if count != args.n or dump_failed else 0


def listen_command(remote, args):
    deadline = math.inf if args.seconds is None else time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        try:
            line = remote.read_line(deadline)
        except RemoteError:
            break
        timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
        print(f"{timestamp} {line}")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM4")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("ping", "status", "clear", "stop", "get", "usb"):
        subparsers.add_parser(name)

    reset = subparsers.add_parser("reset")
    reset.add_argument("--timeout", type=float, default=30.0)
    start = subparsers.add_parser("start")
    start.add_argument("-n", type=int, default=1000)
    dump = subparsers.add_parser("dump")
    dump.add_argument("--start", type=int)
    dump.add_argument("--count", type=int)
    setting = subparsers.add_parser("set")
    setting.add_argument("key")
    setting.add_argument("value")

    run = subparsers.add_parser("run")
    run.add_argument("-n", type=int, default=1000)
    run.add_argument("--reset", action="store_true")
    run.add_argument("--settle", type=float, default=2.0,
                     help="seconds to wait after the device is ready following --reset")
    run.add_argument("--edge", choices=("falling", "rising"))
    run.add_argument("--bias", choices=("none", "up", "down"))
    run.add_argument("--mode", choices=("click", "motion", "key"))
    run.add_argument("--debounce-ms", type=int)
    run.add_argument("--interval-ms", type=int)
    run.add_argument("--level", choices=("high", "low"))
    run.add_argument("--output", type=int, choices=(6, 11))
    run.add_argument("--release-holdoff-ms", type=int, choices=(0, 20, 50, 100, 200),
                     help="1.5.1 and later: ignore trigger edges this long after a USB release (0 = off)")
    run.add_argument("--out")
    run.add_argument("--timeout", type=float)

    listen = subparsers.add_parser("listen")
    listen.add_argument("--seconds", type=float)
    return parser


def main():
    args = build_parser().parse_args()
    remote = None
    try:
        remote = XlatRemote(args.port)
        if args.command == "run":
            return run_command(remote, args)
        if args.command == "listen":
            return listen_command(remote, args)
        return simple_command(remote, args)
    except (RemoteError, serial.SerialException, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        if remote is not None:
            remote.close()


if __name__ == "__main__":
    raise SystemExit(main())
