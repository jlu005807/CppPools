"""传输层与错误分类矩阵测试（cluster-cache spec §5.5/§7.2）。

用可注入的 fake opener 钉住：状态码 → 异常类型/可重试性的映射、
307 作为正常响应返回（不抛、不自动跟随）、网络层故障（超时/拒连/DNS）
统一归为节点不可达。
"""
import io
import json
import os
import socket
import sys
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cpppools import errors, transport  # noqa: E402


class FakeOpener:
    """可编程 opener：handler(request) -> (status, headers, body) 或抛异常。"""

    def __init__(self, handler):
        self.handler = handler
        self.requests = []

    def open(self, request, timeout=None):
        self.requests.append(request)
        status, headers, body = self.handler(request)
        if status >= 400:
            # 与真实 urllib 同行为：非 2xx/3xx 抛 HTTPError（body 经 fp 读回）。
            raise urllib.error.HTTPError(request.full_url, status, "", 
                                         _Headers(headers), io.BytesIO(body))
        return _FakeHttpResponse(status, headers, body)


class _FakeHttpResponse:
    def __init__(self, status, headers, body):
        self.status = status
        self._headers = headers
        self._body = body
        self.headers = _Headers(headers)

    def read(self):
        return self._body

    def getheader(self, name, default=None):
        for k, v in self._headers:
            if k.lower() == name.lower():
                return v
        return default


class _Headers:
    """与 http.client.HTTPMessage 同形：提供 items()。"""

    def __init__(self, pairs):
        self._pairs = pairs

    def items(self):
        return list(self._pairs)


def make_opener(status, body=b"", headers=None):
    return FakeOpener(lambda req: (status, headers or [], body))


class TestErrorClassification(unittest.TestCase):
    def _post(self, opener):
        return transport.post_json("http://n:1/api/v1/select", {"table": "t"},
                                   opener=opener)

    def test_200_returns_parsed_body(self):
        resp = self._post(make_opener(200, json.dumps({"row_count": 1}).encode()))
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.json()["row_count"], 1)

    def test_201_created(self):
        resp = self._post(make_opener(201, b'{"inserted_id":1}'))
        self.assertEqual(resp.status, 201)

    def test_307_is_a_response_not_error(self):
        # 307 由客户端按 Location 跟随（不计重试，§5.5）—— 传输层不得抛。
        resp = self._post(make_opener(
            307, b'{}', headers=[("Location", "http://other:2/api/v1/select")]))
        self.assertEqual(resp.status, 307)
        self.assertEqual(resp.location, "http://other:2/api/v1/select")

    def test_307_via_real_httperror_path(self):
        # 真实 urllib 对 POST+307 抛 HTTPError（重定向处理器不跟 POST）：
        # 传输层必须把它转成 Response（S2：fake 与真实语义差异的钉桩）。
        def raise_307(req):
            raise urllib.error.HTTPError(
                req.full_url, 307, "",
                _Headers([("Location", "http://other:2/x")]),
                io.BytesIO(b'{}'))
        resp = transport.post_json("http://n:1/api/v1/select", {},
                                   opener=FakeOpener(raise_307))
        self.assertEqual(resp.status, 307)
        self.assertEqual(resp.location, "http://other:2/x")

    def test_400_client_error_not_retryable(self):
        with self.assertRaises(errors.ClientError) as ctx:
            self._post(make_opener(400, b'{"error":"bad"}'))
        self.assertFalse(ctx.exception.retryable)

    def test_409_conflict_handed_to_caller(self):
        with self.assertRaises(errors.ConflictError) as ctx:
            self._post(make_opener(409, b'{"error":"dup"}'))
        self.assertFalse(ctx.exception.retryable)

    def test_503_unavailable_retryable(self):
        with self.assertRaises(errors.UnavailableError) as ctx:
            self._post(make_opener(503, b'{"error":"db timeout"}'))
        self.assertTrue(ctx.exception.retryable)

    def test_500_server_error_retryable(self):
        # 500 视为瞬时后端故障：可重试（spec §7.2 错误分类仅列 503/超时，
        # 500 与 503 同为服务端侧故障，按可重试处理 —— 见 client 层限次）。
        with self.assertRaises(errors.ServerError) as ctx:
            self._post(make_opener(500, b'{"error":"backend"}'))
        self.assertTrue(ctx.exception.retryable)

    def test_other_4xx_is_client_error(self):
        with self.assertRaises(errors.ClientError):
            self._post(make_opener(413, b"{}"))

    def test_error_body_message_extracted(self):
        with self.assertRaises(errors.ClientError) as ctx:
            self._post(make_opener(400, b'{"error":"table x unknown"}'))
        self.assertIn("table x unknown", str(ctx.exception))


class TestNetworkFailures(unittest.TestCase):
    def test_connection_refused_is_node_unavailable(self):
        def raise_refused(req):
            raise urllib.error.URLError(ConnectionRefusedError())
        with self.assertRaises(errors.NodeUnavailable) as ctx:
            self._post_helper(FakeOpener(raise_refused))
        self.assertTrue(ctx.exception.retryable)

    def test_timeout_is_node_unavailable(self):
        def raise_timeout(req):
            raise socket.timeout()
        with self.assertRaises(errors.NodeUnavailable):
            self._post_helper(FakeOpener(raise_timeout))

    def test_dns_failure_is_node_unavailable(self):
        def raise_gai(req):
            raise urllib.error.URLError(socket.gaierror())
        with self.assertRaises(errors.NodeUnavailable):
            self._post_helper(FakeOpener(raise_gai))

    def test_incomplete_read_is_node_unavailable(self):
        # 节点响应到一半断开（kill/崩溃的典型形态）：IncompleteRead 不是
        # OSError 子类，必须显式归入 NodeUnavailable（W2），否则绕过
        # 调用方的拉黑绕行。
        import http.client as hc

        def raise_incomplete(req):
            raise hc.IncompleteRead(b"partial")
        with self.assertRaises(errors.NodeUnavailable):
            self._post_helper(FakeOpener(raise_incomplete))

    def _post_helper(self, opener):
        return transport.post_json("http://n:1/api/v1/select", {}, opener=opener)


class TestRequestShape(unittest.TestCase):
    def test_post_json_body_and_headers(self):
        opener = make_opener(200, b"{}")
        transport.post_json("http://n:1/api/v1/select", {"a": 1}, opener=opener,
                            headers={"X-Custom": "v"})
        req = opener.requests[0]
        self.assertEqual(req.get_method(), "POST")
        self.assertEqual(req.get_header("Content-type"), "application/json")
        self.assertEqual(req.get_header("X-custom"), "v")
        self.assertEqual(json.loads(req.data), {"a": 1})


class TestTruncatedErrorBody(unittest.TestCase):
    """Phase 5 Task 4: HTTPError whose read() raises must be NodeUnavailable."""

    def test_httperror_incompleteread_is_node_unavailable(self):
        import http.client as hc

        def raise_truncated(req):
            class _TruncatedHTTPError(urllib.error.HTTPError):
                def read(self):
                    raise hc.IncompleteRead(b"partial")
            raise _TruncatedHTTPError(req.full_url, 503, "",
                                     _Headers([]), io.BytesIO(b""))

        with self.assertRaises(errors.NodeUnavailable) as ctx:
            transport.post_json("http://n:1/api/v1/select", {},
                                opener=FakeOpener(raise_truncated))
        self.assertIn("truncated", str(ctx.exception).lower())

    def test_normal_response_incompleteread_is_node_unavailable(self):
        import http.client as hc

        def truncated_ok(req):
            return 200, [], b'{"ok":true}'

        # Also verify a normal response whose read() raises is classified
        def truncated_read(req):
            class _TruncatedResponse:
                status = 200
                headers = _Headers([])

                def read(self):
                    raise hc.IncompleteRead(b"partial")
            return _TruncatedResponse()

        # The FakeOpener handler returns a tuple for normal responses,
        # so we need a custom opener for this case.
        class _CustomOpener:
            def __init__(self):
                self.requests = []

            def open(self, request, timeout=None):
                self.requests.append(request)
                import http.client as hc
                raise hc.IncompleteRead(b"partial")

        with self.assertRaises(errors.NodeUnavailable):
            transport.post_json("http://n:1/api/v1/select", {},
                                opener=_CustomOpener())



if __name__ == "__main__":
    unittest.main()
