"""The ``cpl`` command-line contract.

Stage 0 deliberately contains only parsing and dispatch scaffolding.  Remote
health/CRUD adapters are added in later stages; importing this module must not
construct an SDK client or perform network I/O.
"""

import argparse
import sys


VERSION = "0+dev"


class _StoreOnce(argparse.Action):
    """Reject repeated scalar options instead of silently taking the last."""

    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            raise argparse.ArgumentError(self, "option may only be specified once")
        setattr(namespace, self.dest, values)


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
    print("command %s is not implemented in this stage" % command,
          file=sys.stderr)
    return 6


if __name__ == "__main__":
    raise SystemExit(main())
