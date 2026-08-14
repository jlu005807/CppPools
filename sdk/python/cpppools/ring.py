"""一致性哈希环（cluster-cache spec §3.1/§3.5）。

建环期使用（构造完成后只读）—— 客户端按 spec §7.2 "构建新环后原子
替换引用"：读者持有旧环引用不受影响，永不阻塞。

口径与服务端 HashRing 一致：哈希升序、查询取顺时针第一个 vnode
（bisect_left，越界回绕首元素）、vnode 哈希冲突按 (node_id, index)
字典序确定性 tie-break（不依赖插入顺序）。
"""
import bisect

from .hashing import murmur3_32, vnode_key


class HashRing:
    def __init__(self):
        self._hashes = []                 # 升序的 vnode 哈希
        self._slots = {}                  # 哈希 → node_id（碰撞已 tie-break）
        self._pairs = {}                  # 哈希 → (node_id, index)：tie-break 比较用
        self._nodes = {}                  # node_id → vnode 数（调试/对账用）

    def add_node(self, node_id, vnode_count):
        """添加节点：生成 vnode_count 个 vnode 并入环。<=0 忽略。"""
        if vnode_count <= 0:
            return
        for index in range(vnode_count):
            h = murmur3_32(vnode_key(node_id, index))
            if h in self._slots:
                # 碰撞 tie-break：保留 (node_id, index) 字典序最小者
                #（与 C++ 端同口径，不依赖插入顺序）。
                if (node_id, index) < self._pairs[h]:
                    self._pairs[h] = (node_id, index)
                    self._slots[h] = node_id
            else:
                self._hashes.append(h)
                self._slots[h] = node_id
                self._pairs[h] = (node_id, index)
        self._nodes[node_id] = self._nodes.get(node_id, 0) + vnode_count
        self._hashes.sort()

    def owner(self, hash_input):
        """哈希输入（bytes/str，未编码原文）的归属节点。空环抛 ValueError。"""
        if not self._hashes:
            raise ValueError("empty ring")
        h = murmur3_32(hash_input)
        i = bisect.bisect_left(self._hashes, h)
        if i >= len(self._hashes):
            i = 0  # 回绕到环首
        return self._slots[self._hashes[i]]

    def vnode_hashes(self):
        """全部 vnode 哈希（升序）：测试/诊断用。"""
        return list(self._hashes)
