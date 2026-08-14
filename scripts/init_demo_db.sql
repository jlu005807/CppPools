-- CppPools 演示库初始化脚本。
--
-- 中间件**不建表**（Database 接口没有 DDL 方法是类型级保证），
-- 因此演示库由本脚本预置：scripts/run_server.sh 在库文件不存在时执行它。
--
-- 过渡期数据模型：score 四端点使用（期 5 通用 CRUD 端点取代后，
-- 表结构由使用方自行定义，本脚本仅服务演示）。
CREATE TABLE IF NOT EXISTS user_scores(
    user_id INTEGER PRIMARY KEY,
    score INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
