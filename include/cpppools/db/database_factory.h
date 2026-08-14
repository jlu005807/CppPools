#pragma once

#include "cpppools/config/config.h"
#include "cpppools/db/database.h"

#include <memory>
#include <string>

namespace cpppools::db {

// 创建结果：成功时 db 非空、error 为空；失败时 db 为空、error 非空。
struct DatabaseCreateResult {
    std::unique_ptr<Database> db;
    std::string error;
};

// 后端工厂函数：按数据源配置创建一个 Database 实例。
// 约定：**不得抛异常**（项目约定：不用异常做控制流），
// 失败通过返回值表达（db 为空 + error 非空）。
using DatabaseFactoryFn = DatabaseCreateResult (*)(const config::DataSourceConfig&);

// 类型注册表：type 字符串（"sqlite" / "mysql" / ...）→ 工厂函数。
//
// 扩展新后端 = 实现一个 Database 子类 + 注册一行，
// 无需改动基类、路由、HTTP 层或配置解析。
bool register_database_type(const std::string& type, DatabaseFactoryFn fn);
bool unregister_database_type(const std::string& type);

// 按配置的 type 创建实例。未知类型 / 工厂失败均返回带 error 的结果。
DatabaseCreateResult create_database(const config::DataSourceConfig& cfg);

}  // namespace cpppools::db
