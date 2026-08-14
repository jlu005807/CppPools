"""SDK 异常体系（cluster-cache spec §5.5/§7.2 错误分类矩阵）。

分类语义（客户端重试策略的依据）：
- ClientError        400/其余 4xx：请求本身非法，**永不重试**；
- ConflictError      409：唯一冲突，不重试、交调用方判定（显式键 insert
                     重试撞 409 = 已生效，spec §5.5）；
- ServerError        5xx（503 除外）：后端故障，可重试（限次）；
- UnavailableError   503：池超时/背压，可重试 + 退避；
- NodeUnavailable    网络层（拒连/超时/DNS）：节点不可达 → 拉黑 + attempt 绕行；
- AllNodesBlacklistedError：全黑清空重试一轮后仍失败（§7.2）；
- TopologyError      启动期拓扑拉取全失败（无默认兜底，§7.2）。
"""


class CppPoolsError(Exception):
    """SDK 异常基类。retryable 供 client 层重试决策（矩阵的唯一事实源）。"""

    retryable = False

    def __init__(self, message, status=None, body=None):
        super().__init__(message)
        self.status = status
        self.body = body


class ClientError(CppPoolsError):
    """4xx（409 除外）：请求非法，永不重试。"""


class ConflictError(CppPoolsError):
    """409 唯一冲突：不重试，交调用方。"""


class ServerError(CppPoolsError):
    """5xx（非 503）：可重试。"""

    retryable = True


class UnavailableError(ServerError):
    """503：服务端池超时/背压，可重试 + 退避。"""


class NodeUnavailable(CppPoolsError):
    """网络层不可达（拒连/超时/DNS）：触发拉黑 + attempt 绕行。"""

    retryable = True


class AllNodesBlacklistedError(CppPoolsError):
    """全节点黑名单且清空重试一轮后仍失败。"""


class TopologyError(CppPoolsError):
    """启动期拓扑拉取失败（逐 seed 重试全败）。"""
