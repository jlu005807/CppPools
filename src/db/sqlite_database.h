#pragma once

#include "cpppools/config/config.h"
#include "cpppools/db/database_factory.h"

namespace cpppools::db {

// SQLite 后端入口（工厂一步完成：连接 + 自省 + 构造）。
//
// 失败按值返回（db 为空 + error 非空），不抛异常：
// - 路径为空 / 打开失败（READWRITE 不带 CREATE：库文件必须已存在）
// - schema 自省失败
// - 出现 Value 无法表达的 BLOB 亲和列（error 指明 table.column）
//
// sqlite_path 支持 URI（以 "file:" 开头自动启用 SQLITE_OPEN_URI，
// 如 file::memory:?cache=shared —— 测试与 :memory: 共享场景用）。
DatabaseCreateResult make_sqlite_database(const config::DataSourceConfig& cfg);

// 把 "sqlite" 类型注册进工厂注册表（幂等：重复注册返回 false 无害）。
// 由 main 显式调用而非全局静态初始化 —— 避免静态初始化顺序依赖。
void register_sqlite_backend();

}  // namespace cpppools::db
