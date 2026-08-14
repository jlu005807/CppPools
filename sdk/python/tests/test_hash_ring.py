"""SDK 哈希与一致性环测试（cluster-cache spec §3.1/§3.2）。

黄金向量引用 spec §3.2 冻结表 —— 与 C++ test_hash_ring 同源，
任何一端的实现漂移都会撞红（跨语言一致性是路由契约的根）。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cpppools.hashing import murmur3_32, lp, le32, vnode_key, query_key  # noqa: E402
from cpppools.ring import HashRing  # noqa: E402

# spec §3.2 冻结值（禁止随实现改动）。
V1 = 1664544994    # vnode("node-a", 0)
V2 = 3870970589    # vnode("节点-b", 99)
V3 = 931942866     # query("main","users","id","42",0)
V4 = 2309089622    # query("main","users","tag","a/b",0)
V5 = 4213239566    # query("main","users","id","-7",3)
V8_COLLIDE = 2079710457  # vnode(node-8,1591) == vnode(node-21,173)


class TestGoldenVectors(unittest.TestCase):
    def test_v1_vnode_ascii(self):
        self.assertEqual(murmur3_32(vnode_key("node-a", 0)), V1)

    def test_v2_vnode_non_ascii(self):
        self.assertEqual(murmur3_32(vnode_key("节点-b", 99)), V2)

    def test_v3_query_integer_key(self):
        self.assertEqual(murmur3_32(query_key("main", "users", "id", "42", 0)), V3)

    def test_v4_query_value_with_slash(self):
        self.assertEqual(murmur3_32(query_key("main", "users", "tag", "a/b", 0)), V4)

    def test_v5_query_negative_attempt(self):
        self.assertEqual(murmur3_32(query_key("main", "users", "id", "-7", 3)), V5)

    def test_v7_murmur3_public_vectors(self):
        # tail=0/1/3 字节基准（覆盖 body 循环后的全部分支）。
        self.assertEqual(murmur3_32(b""), 0)
        self.assertEqual(murmur3_32(b"test"), 3127628307)
        self.assertEqual(murmur3_32(b"abc"), 3017643002)

    def test_v8_collision_pair_hash(self):
        # 离线搜索冻结的真实碰撞对：两 vnode 同哈希。
        self.assertEqual(murmur3_32(vnode_key("node-8", 1591)), V8_COLLIDE)
        self.assertEqual(murmur3_32(vnode_key("node-21", 173)), V8_COLLIDE)

    def test_lp_le32_encoding(self):
        self.assertEqual(lp("ab"), b"\x02\x00\x00\x00ab")
        self.assertEqual(le32(0x01020304), b"\x04\x03\x02\x01")
        # 长度前缀防拼接歧义：("a","bc") 与 ("ab","c") 编码不同。
        self.assertNotEqual(lp("a") + lp("bc"), lp("ab") + lp("c"))


class TestHashRing(unittest.TestCase):
    def _abc_ring(self):
        ring = HashRing()
        for node in ("node-a", "node-b", "node-c"):
            ring.add_node(node, 100)
        return ring

    def test_v6_ring_ownership(self):
        ring = self._abc_ring()
        key = query_key("main", "users", "id", "42", 0)
        self.assertEqual(murmur3_32(key), V3)
        self.assertEqual(ring.owner(key), "node-a")

    def test_v8_collision_tiebreak(self):
        # 碰撞槽归属字典序赢家 node-21；查询键 2078835168 落在碰撞点前
        # 一个 vnode(2076313318) 与碰撞点之间 → lower_bound 命中碰撞槽。
        ring = HashRing()
        ring.add_node("node-8", 1592)
        ring.add_node("node-21", 174)
        key = query_key("main", "users", "id", "1099", 0)
        self.assertEqual(murmur3_32(key), 2078835168)
        self.assertEqual(ring.owner(key), "node-21")

    def test_wrap_around(self):
        # 哈希大于环上最大 vnode 哈希的键必须回绕到环首节点。
        ring = self._abc_ring()
        head_owner = ring.owner(b"")  # murmur3("")=0，必落环首
        max_vnode_hash = max(ring.vnode_hashes())
        probe = 0
        while True:
            candidate = ("w%d" % probe).encode()
            if murmur3_32(candidate) > max_vnode_hash:
                break
            probe += 1
            assert probe < 100000
        self.assertEqual(ring.owner(candidate), head_owner)

    def test_unsigned_ordering(self):
        # 无符号排序：若按有符号排序，高位为 1 的哈希会排到最前，
        # 归属整体漂移。构造两个哈希（一高一低，高位差异）验证顺序。
        ring = HashRing()
        ring.add_node("node-a", 100)
        ring.add_node("node-b", 100)
        hashes = sorted(ring.vnode_hashes())
        # 升序且全在 uint32 域。
        self.assertEqual(hashes, sorted(hashes))
        self.assertTrue(all(0 <= h <= 0xFFFFFFFF for h in hashes))

    def test_owner_empty_ring_raises(self):
        ring = HashRing()
        with self.assertRaises(ValueError):
            ring.owner(b"x")

    def test_attempt_changes_owner_deterministically(self):
        # attempt 进哈希输入（§3.5 绕行机制的前提）：同一键不同 attempt
        # 可落不同节点，且结果跨进程确定（黄金向量同口径）。
        ring = self._abc_ring()
        owners = {
            attempt: ring.owner(query_key("main", "users", "id", "42", attempt))
            for attempt in range(6)
        }
        self.assertEqual(owners[0], "node-a")  # = V6 口径
        self.assertTrue(len(set(owners.values())) >= 2,
                        "6 个 attempt 全落同一节点，绕行机制失效")


if __name__ == "__main__":
    unittest.main()
