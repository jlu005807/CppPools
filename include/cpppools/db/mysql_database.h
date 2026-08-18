#pragma once

#include "cpppools/config/config.h"
#include "cpppools/db/database_factory.h"

namespace cpppools::db {

// MySQL 后端入口（工厂一步完成：连接 + utf8mb4 + schema 自省 + 构造）。
//
// 失败按值返回（db 为空 + error 非空），不抛异常：
// - host/database 为空（配置层已校验，这里为防御）
// - mysql_real_connect 失败（拒绝访问 / 库不存在 / 网络不通）
// - 设置字符集 / 连接超时失败
// - schema 自省失败（见 src/db/mysql_database.cpp 的自省口径）
// - 出现 Value 无法表达的列类型（BLOB / JSON / BIT / 空间类型等，error 指明 table.column）
//
// 仅当编译时启用 CPPPOOLS_USE_MYSQL 时有实现；否则该符号在工厂里不会被注册。
// 自省与 CRUD 口径见 docs/superpowers/specs/2026-08-15-mysql-backend-design.md。
DatabaseCreateResult make_mysql_database(const config::DataSourceConfig& cfg);

// 把 "mysql" 类型注册进工厂注册表（幂等：重复注册返回 false 无害）。
// 由 main 显式调用而非全局静态初始化 —— 避免静态初始化顺序依赖。
// 仅当编译时启用 CPPPOOLS_USE_MYSQL 时调用；无 MySQL 支持时不应被调用。
void register_mysql_backend();

}  // namespace cpppools::db
