"""哈希与编码原语（cluster-cache spec §3.1）。

与服务端 src/server/hash_ring.cpp 逐字节同口径，由 spec §3.2 黄金向量
（tests/test_hash_ring.py）钉死 —— 两端建环必须逐项一致，否则同一键
在客户端与服务端选出不同 owner，单 owner 契约整体失效。

关键口径：
- murmur3 x86 32-bit，seed=0，**无符号** 32 位输出（Python int 天然无符号域，
  全程 & 0xFFFFFFFF）；
- 字符串一律 UTF-8；哈希输入用 LP 长度前缀编码（禁止分隔符拼接）。
"""
import struct

_MASK = 0xFFFFFFFF


def _rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & _MASK


def _fmix(h):
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & _MASK
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & _MASK
    h ^= h >> 16
    return h


def murmur3_32(data, seed=0):
    """murmur3 x86 32-bit reference 实现（Austin Appleby, public domain）。

    等价 mmh3.hash(data, seed, signed=False)。入参 bytes 或 str（str 先 UTF-8）。
    """
    if isinstance(data, str):
        data = data.encode("utf-8")
    length = len(data)
    h = seed & _MASK
    c1, c2, c3 = 0xCC9E2D51, 0x1B873593, 0xE6546B64

    nblocks = length // 4
    for i in range(nblocks):
        k = struct.unpack_from("<I", data, i * 4)[0]
        k = (k * c1) & _MASK
        k = _rotl(k, 15)
        k = (k * c2) & _MASK
        h ^= k
        h = _rotl(h, 13)
        h = (h * 5 + c3) & _MASK

    # tail：剩余 1-3 字节。
    tail = data[nblocks * 4:]
    k1 = 0
    if len(tail) >= 3:
        k1 ^= tail[2] << 16
    if len(tail) >= 2:
        k1 ^= tail[1] << 8
    if len(tail) >= 1:
        k1 ^= tail[0]
        k1 = (k1 * c1) & _MASK
        k1 = _rotl(k1, 15)
        k1 = (k1 * c2) & _MASK
        h ^= k1

    h ^= length
    return _fmix(h)


def le32(value):
    """uint32 小端编码（attempt / vnode index / LP 长度共用）。"""
    return struct.pack("<I", value & _MASK)


def lp(s):
    """长度前缀编码：LE32(len) ‖ utf8(s)。禁止分隔符拼接（§3.1）。"""
    b = s.encode("utf-8") if isinstance(s, str) else s
    return struct.pack("<I", len(b)) + b


def vnode_key(node_id, index):
    """vnode 哈希输入：LP(node_id) ‖ LE32(index)。"""
    return lp(node_id) + le32(index)


def query_key(datasource, table, column, value, attempt):
    """查询键哈希输入：LP(ds) ‖ LP(table) ‖ LP(column) ‖ LP(value) ‖ LE32(attempt)。

    value 必须是已串化的文本（int → 十进制，与服务端
    value_to_routing_string 同口径；REAL/NULL 键不参与路由，到不了这里）。
    """
    return lp(datasource) + lp(table) + lp(column) + lp(value) + le32(attempt)
