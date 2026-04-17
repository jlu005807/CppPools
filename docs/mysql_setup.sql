-- CppPools MySQL 示例初始化脚本
-- 默认数据库与表名对应：
--   CPPPOOLS_DB_NAME=cpppools
--   CPPPOOLS_DB_TABLE=user_scores

CREATE DATABASE IF NOT EXISTS cpppools;
USE cpppools;

CREATE TABLE IF NOT EXISTS user_scores (
    user_id INT PRIMARY KEY,
    score INT NOT NULL,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

INSERT INTO user_scores (user_id, score) VALUES
    (1, 78),
    (2, 85),
    (3, 92),
    (7, 66),
    (8, 88),
    (42, 99)
ON DUPLICATE KEY UPDATE score = VALUES(score);
