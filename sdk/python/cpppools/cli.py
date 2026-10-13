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
# Kept as a lazy binding: importing the CLI (including ``--help``) must not
# initialise a ClusterClient or perform topology/network work.  Tests and
# embedders may replace this binding with a compatible client implementation.
ClusterClient = None
LocalNodeManager = None


class InputError(ValueError):
    pass


class ProtocolError(ValueError):
    """A successful HTTP response did not satisfy the CRUD response shape."""


def _error_document(kind, message, exit_code, status=None):
    return {"kind": kind, "message": str(message), "exit_code": exit_code,
            "http_status": status}


def _write_runtime_error(kind, exc, exit_code):
    print(json.dumps(_error_document(kind, str(exc), exit_code,
                                     getattr(exc, "status", None)),
                     ensure_ascii=False, separators=(",", ":")),
          file=sys.stderr)


def _get_cluster_client():
    global ClusterClient
    if ClusterClient is None:
        from .client import ClusterClient as client_type
        ClusterClient = client_type
    return ClusterClient


def _get_local_manager():
    global LocalNodeManager
    if LocalNodeManager is None:
        from .local_node import LocalNodeManager as manager_type
        LocalNodeManager = manager_type
    return LocalNodeManager


def _default_state_root():
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                            "CppPools", "cpl", "instances")
    return os.path.join(os.environ.get("XDG_STATE_HOME",
                                      os.path.expanduser("~/.local/state")),
                        "cpl", "instances")


def _local_manager(args):
    return _get_local_manager()(args.state_dir or _default_state_root(), args.name)


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


def _is_scalar(value):
    return value is None or isinstance(value, (str, int, float, bool))


def _parse_where(source):
    value = parse_json_source(source)
    if (not isinstance(value, dict) or set(value) != {"column", "value"}
            or not isinstance(value["column"], str)
            or not value["column"].strip()
            or value["value"] is None
            or not _is_scalar(value["value"])):
        raise InputError("input where must contain a non-empty column and scalar value")
    return value


def _parse_values(source, label):
    value = parse_json_source(source)
    if not isinstance(value, dict) or not value:
        raise InputError("input %s must be a non-empty JSON object" % label)
    for key, item in value.items():
        if not isinstance(key, str) or not key.strip() or not _is_scalar(item):
            raise InputError("input %s must contain only scalar column values" % label)
    return value


def _validate_crud_args(args):
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        raise InputError("timeout must be a finite positive number")
    if not isinstance(args.table, str) or not args.table.strip():
        raise InputError("input table must not be empty")
    if args.datasource is not None and not args.datasource.strip():
        raise InputError("input datasource must not be empty")
    seeds = health_urls(args)
    if args.command in ("select", "update", "delete"):
        where = _parse_where(args.where)
    else:
        where = None
    if args.command in ("insert", "update"):
        values = _parse_values(args.data, "data")
    else:
        values = None
    columns = args.columns if args.command == "select" else None
    if columns is not None and any(not isinstance(c, str) or not c.strip()
                                   for c in columns):
        raise InputError("input columns must be non-empty strings")
    if args.command == "select" and columns is not None and not columns:
        raise InputError("input columns must not be empty")
    if args.command == "select":
        payload = {"table": args.table, "where": where}
        if columns is not None:
            payload["columns"] = columns
    elif args.command == "insert":
        payload = {"table": args.table, "values": values}
    elif args.command == "update":
        payload = {"table": args.table, "set": values, "where": where}
    else:
        payload = {"table": args.table, "where": where}
    if args.datasource:
        payload["datasource"] = args.datasource
    # Match transport.request's serializer, including its default escaping
    # and separators.  A 64 KiB body is accepted; one byte more is not.
    if len(json.dumps(payload).encode("utf-8")) > MAX_INPUT_BYTES:
        raise InputError("request payload exceeds 65536 bytes")
    return seeds, where, values, columns, payload


def _validate_crud_result(command, result):
    if not isinstance(result, dict):
        raise ProtocolError("CRUD response must be a JSON object")
    if command == "select":
        rows = result.get("rows")
        columns = result.get("columns")
        count = result.get("row_count")
        if (not isinstance(rows, list) or not isinstance(columns, list)
                or isinstance(count, bool) or not isinstance(count, int)
                or count < 0 or count != len(rows)
                or any(not isinstance(c, str) for c in columns)
                or any(not isinstance(row, list) or len(row) != len(columns)
                       for row in rows)):
            raise ProtocolError("select response has invalid rows/columns shape")
        return result
    affected = result.get("affected_rows")
    if isinstance(affected, bool) or not isinstance(affected, int) or affected < 0:
        raise ProtocolError("CRUD response has invalid affected_rows")
    if command == "insert" and "inserted_id" not in result:
        raise ProtocolError("insert response is missing inserted_id")
    return result


def _print_crud_result(command, result, output):
    if output == "json":
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return
    if command == "select":
        print("\t".join(result["columns"]))
        for row in result["rows"]:
            print("\t".join("" if value is None else str(value) for value in row))
    elif command == "insert":
        print("affected_rows=%s\tinserted_id=%s" %
              (result["affected_rows"], result.get("inserted_id")))
    else:
        print("affected_rows=%s" % result["affected_rows"])


def run_crud(args):
    seeds, where, values, columns, _payload = _validate_crud_args(args)
    try:
        client = _get_cluster_client()(seeds, read_timeout=args.timeout)
        if args.command == "select":
            result = client.select(args.table, where, columns=columns,
                                   datasource=args.datasource)
        elif args.command == "insert":
            result = client.insert(args.table, values,
                                   datasource=args.datasource)
        elif args.command == "update":
            result = client.update(args.table, values, where,
                                   datasource=args.datasource)
        else:
            result = client.delete(args.table, where,
                                   datasource=args.datasource)
        result = _validate_crud_result(args.command, result)
    except ProtocolError as exc:
        _write_runtime_error("protocol_error", exc, 5)
        return 5
    except (ValueError, TypeError, KeyError, AttributeError, IndexError) as exc:
        # JSON decoding and response-shape failures are not safe to retry: a
        # write may already have committed before its response was truncated.
        _write_runtime_error("protocol_error", exc, 5)
        return 5
    except errors.NodeUnavailable as exc:
        _write_runtime_error("node_unavailable", exc, 3)
        return 3
    except errors.TopologyError as exc:
        _write_runtime_error("topology_error", exc, 3)
        return 3
    except errors.AllNodesBlacklistedError as exc:
        _write_runtime_error("node_unavailable", exc, 3)
        return 3
    except errors.ConflictError as exc:
        _write_runtime_error("client_error", exc, 4)
        return 4
    except errors.ClientError as exc:
        _write_runtime_error("client_error", exc, 4)
        return 4
    except errors.ServerError as exc:
        _write_runtime_error("server_error", exc, 5)
        return 5
    _print_crud_result(args.command, result, args.output)
    return 0


def _print_local_result(result, output):
    if output == "json":
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return
    print("%s %s endpoint=%s pid=%s" %
          (result.get("state", "unknown"), result.get("name", ""),
           result.get("endpoint") or "-", result.get("pid") or "-"))


def _run_local(args, operation):
    try:
        manager = _local_manager(args)
        if operation == "status":
            result = manager.status()
        elif operation == "start":
            result = manager.start(config=args.config, binary=args.binary,
                                   workdir=args.workdir, log_file=args.log_file,
                                   timeout=args.timeout, foreground=args.foreground)
        else:
            result = manager.stop(timeout=args.timeout, force=args.force)
        _print_local_result(result, args.output)
        return 0
    except Exception as exc:
        code = getattr(exc, "code", 6)
        kind = getattr(exc, "kind", "start_error" if operation == "start"
                        else "local_state_error")
        _write_runtime_error(kind, exc, code)
        return code


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
    if command in {"select", "insert", "update", "delete"}:
        try:
            return run_crud(args)
        except InputError as exc:
            print(str(exc), file=sys.stderr)
            return 2
    if command in {"start", "stop", "status"}:
        return _run_local(args, command)
    print("command %s is not implemented in this stage" % command,
          file=sys.stderr)
    return 6


if __name__ == "__main__":
    raise SystemExit(main())
