"""The ``cpl`` command-line contract.

The parser, strict JSON input helpers, and remote health probe are implemented
in the initial CLI stages. CRUD adapters and local process management are added
in later stages. Importing this module never constructs an SDK client or does
network I/O.
"""

import argparse
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request

from . import errors, transport


VERSION = "0+dev"
MAX_INPUT_BYTES = 65536


class InputError(ValueError):
    pass


class _StoreOnce(argparse.Action):
    """Reject repeated scalar options instead of silently taking the last."""

    def __call__(self, parser, namespace, values, option_string=None):
        marker = "__cli_set_" + self.dest
        if getattr(namespace, marker, False):
            raise argparse.ArgumentError(self, "option may only be specified once")
        setattr(namespace, self.dest, values)
        setattr(namespace, marker, True)


def _parser(*args, **kwargs):
    kwargs.setdefault("allow_abbrev", False)
    return argparse.ArgumentParser(*args, **kwargs)


def _add_output(parser):
    parser.add_argument("-o", "--output", choices=("table", "json"),
                        default="table", action=_StoreOnce)


def _add_urls(parser):
    parser.add_argument("-u", "--url", dest="urls", action="append",
                        default=None, help="remote seed URL; may be repeated")


def _add_timeout(parser, default=3.0):
    parser.add_argument("-t", "--timeout", type=float, default=default,
                        action=_StoreOnce)


def _add_datasource(parser):
    parser.add_argument("-d", "--datasource", action=_StoreOnce)


def _add_crud_common(parser):
    _add_urls(parser)
    _add_datasource(parser)
    _add_timeout(parser)
    _add_output(parser)


def build_parser():
    parser = _parser(prog="cpl", description="CppPools command-line client")
    parser.add_argument("-V", "--version", action="version", version=VERSION)
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    start = sub.add_parser("start", aliases=[], allow_abbrev=False,
                           help="start a local CppPools node")
    start.add_argument("-c", "--config", required=True, action=_StoreOnce)
    start.add_argument("-n", "--name", default="default", action=_StoreOnce)
    start.add_argument("-b", "--binary", action=_StoreOnce)
    start.add_argument("-W", "--workdir", action=_StoreOnce)
    start.add_argument("-R", "--state-dir", action=_StoreOnce)
    start.add_argument("-l", "--log-file", action=_StoreOnce)
    start.add_argument("-t", "--timeout", type=float, default=15.0,
                       action=_StoreOnce)
    start.add_argument("-F", "--foreground", action="store_true")
    _add_output(start)

    stop = sub.add_parser("stop", aliases=[], allow_abbrev=False,
                          help="stop a local CppPools node")
    stop.add_argument("-n", "--name", default="default", action=_StoreOnce)
    stop.add_argument("-R", "--state-dir", action=_StoreOnce)
    stop.add_argument("-t", "--timeout", type=float, default=10.0,
                      action=_StoreOnce)
    stop.add_argument("-f", "--force", action="store_true")
    _add_output(stop)

    status = sub.add_parser("status", aliases=["st"], allow_abbrev=False,
                            help="show a local node status")
    status.set_defaults(command="status")
    status.add_argument("-n", "--name", default="default", action=_StoreOnce)
    status.add_argument("-R", "--state-dir", action=_StoreOnce)
    _add_urls(status)
    _add_timeout(status)
    _add_output(status)

    health = sub.add_parser("health", aliases=["h"], allow_abbrev=False,
                            help="probe a remote /health endpoint")
    health.set_defaults(command="health")
    _add_urls(health)
    _add_timeout(health)
    _add_output(health)

    select = sub.add_parser("select", aliases=["get"], allow_abbrev=False,
                            help="select one row")
    select.set_defaults(command="select")
    select.add_argument("table")
    select.add_argument("-w", "--where", required=True, action=_StoreOnce)
    select.add_argument("-C", "--column", dest="columns", action="append")
    _add_crud_common(select)

    insert = sub.add_parser("insert", aliases=["add"], allow_abbrev=False,
                            help="insert one row")
    insert.set_defaults(command="insert")
    insert.add_argument("table")
    insert.add_argument("-D", "--data", required=True, action=_StoreOnce)
    _add_crud_common(insert)

    update = sub.add_parser("update", aliases=["set"], allow_abbrev=False,
                            help="update one row")
    update.set_defaults(command="update")
    update.add_argument("table")
    update.add_argument("-w", "--where", required=True, action=_StoreOnce)
    update.add_argument("-D", "--data", required=True, action=_StoreOnce)
    _add_crud_common(update)

    delete = sub.add_parser("delete", aliases=["del"], allow_abbrev=False,
                            help="delete one row")
    delete.set_defaults(command="delete")
    delete.add_argument("table")
    delete.add_argument("-w", "--where", required=True, action=_StoreOnce)
    _add_crud_common(delete)
    return parser


def _reject_constant(value):
    raise InputError("non-finite JSON number: %s" % value)


def _parse_int(value):
    number = int(value)
    if number < -(1 << 63) or number > (1 << 63) - 1:
        raise InputError("integer is outside signed int64 range")
    return number


def _parse_float(value):
    number = float(value)
    if not math.isfinite(number):
        raise InputError("non-finite JSON number")
    return number


def _pairs_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InputError("duplicate JSON key: %s" % key)
        result[key] = value
    return result


def _load_json_bytes(data):
    if len(data) > MAX_INPUT_BYTES:
        raise InputError("JSON input exceeds 65536 bytes")
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise InputError("JSON input is not valid UTF-8: %s" % exc) from None
    try:
        return json.loads(text, object_pairs_hook=_pairs_no_duplicates,
                          parse_constant=_reject_constant,
                          parse_int=_parse_int, parse_float=_parse_float)
    except InputError:
        raise
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise InputError("invalid JSON: %s" % exc) from None


def parse_json_source(source, stdin=None):
    if source == "-":
        stream = stdin if stdin is not None else getattr(sys.stdin, "buffer", sys.stdin)
        data = stream.read(MAX_INPUT_BYTES + 1)
        if isinstance(data, str):
            data = data.encode("utf-8")
    elif source.startswith("@"):
        try:
            with open(source[1:], "rb") as handle:
                data = handle.read(MAX_INPUT_BYTES + 1)
        except OSError as exc:
            raise InputError("cannot read JSON file: %s" % exc) from None
    else:
        data = source.encode("utf-8")
    return _load_json_bytes(data)


def health_urls(args):
    urls = args.urls
    if not urls:
        urls = [os.environ.get("CPL_URL", "http://127.0.0.1:8080")]
    for url in urls:
        try:
            parsed = urllib.parse.urlsplit(url)
            hostname = parsed.hostname
            port = parsed.port
        except ValueError:
            raise InputError("invalid URL origin: %s" % url) from None
        if (parsed.scheme not in ("http", "https") or not hostname or
                any(character.isspace() for character in hostname) or
                parsed.username is not None or parsed.password is not None or
                parsed.path not in ("", "/") or parsed.query or parsed.fragment or
                (port is not None and not 1 <= port <= 65535)):
            raise InputError("invalid URL origin: %s" % url)
    return [url.rstrip("/") for url in urls]


def validate_health_args(args):
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        raise InputError("timeout must be a finite positive number")
    return health_urls(args)


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _health_opener():
    return urllib.request.build_opener(_NoRedirectHandler())


def run_health(args):
    results = []
    for base in validate_health_args(args):
        started = time.monotonic()
        item = {"url": base, "ok": False, "http_status": None,
                "body": None, "elapsed_ms": 0.0, "error": None}
        try:
            response = transport.request("GET", base + "/health",
                                         opener=_health_opener(),
                                         timeout=args.timeout)
            item["http_status"] = response.status
            item["body"] = response.body.decode("utf-8", "replace")
            item["ok"] = response.status == 200
            if not item["ok"]:
                item["error"] = "HTTP %d" % response.status
        except errors.CppPoolsError as exc:
            item["error"] = str(exc)
            item["http_status"] = getattr(exc, "status", None)
        finally:
            item["elapsed_ms"] = round((time.monotonic() - started) * 1000, 3)
        results.append(item)
    if args.output == "json":
        print(json.dumps({"results": results}, ensure_ascii=False,
                         separators=(",", ":")))
    else:
        for item in results:
            state = "OK" if item["ok"] else "FAIL"
            print("%s %s %s" % (state, item["url"],
                                item["body"] or item["error"] or ""))
    for item in results:
        if not item["ok"]:
            print("health probe failed for %s: %s" %
                  (item["url"], item["error"] or "non-200 response"),
                  file=sys.stderr)
    if any(item["ok"] for item in results) and all(item["ok"] for item in results):
        return 0
    if any(item["error"] and item["http_status"] is None for item in results):
        return 3
    if any(item["http_status"] is not None and item["http_status"] >= 500
           for item in results):
        return 5
    return 4


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help(sys.stderr)
        return 2
    if args.command in {"status", "st"}:
        command = "status"
    elif args.command in {"health", "h"}:
        command = "health"
    elif args.command in {"select", "get"}:
        command = "select"
    elif args.command in {"insert", "add"}:
        command = "insert"
    elif args.command in {"update", "set"}:
        command = "update"
    elif args.command in {"delete", "del"}:
        command = "delete"
    else:
        command = args.command
    if command == "health":
        try:
            return run_health(args)
        except InputError as exc:
            print(str(exc), file=sys.stderr)
            return 2
    print("command %s is not implemented in this stage" % command,
          file=sys.stderr)
    return 6


if __name__ == "__main__":
    raise SystemExit(main())
