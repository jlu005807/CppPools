#pragma once

#include "cpppools/db/value.h"

#include <string>
#include <utility>
#include <vector>

namespace cpppools::db {

// 单列等值条件。column 必须是 PK/UNIQUE（校验管道强制）——
// 这保证每个操作“至多影响一行”，中间件在结构上无法被用来批量改删。
// 多条件 AND / 比较运算符本版不做（扩展位：将来可扩为条件数组）。
struct WhereClause {
    std::string column;
    Value value;  // 不得为 NULL
};

struct SelectRequest {
    std::string table;
    std::vector<std::string> columns;  // 空 = 全部列
    WhereClause where;
};

struct InsertRequest {
    std::string table;
    std::vector<std::pair<std::string, Value>> values;  // 列名 → 值；自增列可缺省
};

struct UpdateRequest {
    std::string table;
    std::vector<std::pair<std::string, Value>> set;
    WhereClause where;
};

struct DeleteRequest {
    std::string table;
    WhereClause where;
};

}  // namespace cpppools::db
