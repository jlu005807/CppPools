"""HTTP 传输层（cluster-cache spec §7）。

零依赖（urllib）。职责边界：只做"一次请求"与错误分类，不做重试/
路由/跟随 —— 那些是 client 层的策略。

口径：
- 307 作为**正常 Response 返回**（不抛、不自动跟随）：urllib 的重定向
  处理器对 POST 本就不跟随，HTTPError(307) 在此转成 Response 交 client；
- 400/其余 4xx → ClientError；409 → ConflictError；503 → UnavailableError；
  其余 5xx → ServerError；
- 拒连/超时/DNS → NodeUnavailable（client 据此拉黑 + 绕行）。

已知限制（计划 D2）：urllib 无连接复用，每请求新建 TCP 连接。spec §7.2
的 requests.Session keep-alive 留待引入 requests 的 v1.1。
"""
import http.client
import json
import socket
import urllib.error
import urllib.request

from . import errors

DEFAULT_TIMEOUT = 3.0  # spec §7.1 read_timeout：须 > 服务端 db_acquire_timeout


class Response:
    """一次 HTTP 应答的最小视图。"""

    def __init__(self, status, headers, body):
        self.status = status
        # 头名小写归一：调用方不关心大小写。
        self.headers = {k.lower(): v for k, v in headers}
        self.body = body

    def json(self):
        if not self.body:
            return {}
        return json.loads(self.body.decode("utf-8", "replace"))

    @property
    def location(self):
        return self.headers.get("location")

    @property
    def server_epoch(self):
        # X-CppPools-Epoch 回显（§5.4）：不存在 → None。
        return self.headers.get("x-cpppools-epoch")


def _extract_message(body):
    """错误 body 里提 message：服务端错误响应是 {"code","message"} 或
    {"error"}（历史端点）；提不到就回退原文。"""
    try:
        doc = json.loads(body.decode("utf-8", "replace"))
        if isinstance(doc, dict):
            for key in ("message", "error"):
                if isinstance(doc.get(key), str) and doc[key]:
                    return doc[key]
    except (ValueError, UnicodeDecodeError):
        pass
    text = body.decode("utf-8", "replace") if body else ""
    return text[:200]


def request(method, url, payload=None, headers=None, opener=None,
            timeout=DEFAULT_TIMEOUT):
    """一次 HTTP 请求（GET/POST）。成功/307 返回 Response；其余按矩阵抛。

    opener 可注入（测试用 fake）；timeout 覆盖 connect+read 两阶段
    （urllib 单一超时参数的限制；§7.1 的两段式超时留待 v1.1）。
    """
    opener = opener or urllib.request.build_opener()
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for name, value in (headers or {}).items():
        req.add_header(name, value)
    try:
        resp = opener.open(req, timeout=timeout)
        body = resp.read()
        return Response(resp.status, resp.headers.items(), body)
    except urllib.error.HTTPError as e:
        body = e.read()
        resp_headers = e.headers.items() if e.headers is not None else []
        if e.code == 307:
            return Response(307, resp_headers, body)
        message = _extract_message(body) or ("HTTP %d" % e.code)
        if e.code == 409:
            raise errors.ConflictError(message, e.code, body) from None
        if e.code == 503:
            raise errors.UnavailableError(message, e.code, body) from None
        if 500 <= e.code < 600:
            raise errors.ServerError(message, e.code, body) from None
        raise errors.ClientError(message, e.code, body) from None
    except (urllib.error.URLError, socket.timeout, TimeoutError,
            ConnectionError, http.client.HTTPException, OSError) as e:
        # URLError 包住拒连/DNS；socket.timeout 在部分路径直接冒出；
        # HTTPException 包住 IncompleteRead/BadStatusLine —— 节点响应到一半
        # 断开（kill/崩溃的典型形态），同样归"节点不可达"，不得裸穿透
        #（否则绕过调用方的 CppPoolsError 捕获面与拉黑绕行机制）。
        raise errors.NodeUnavailable(
            "node unreachable: %s: %s" % (url, e)) from None


def post_json(url, payload, headers=None, opener=None, timeout=DEFAULT_TIMEOUT):
    """POST JSON 一次（request 的便捷包装，历史接口保留）。"""
    return request("POST", url, payload, headers, opener, timeout)
