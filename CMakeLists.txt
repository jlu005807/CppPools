cmake_minimum_required(VERSION 3.16)
project(CppPools VERSION 0.3.0 LANGUAGES CXX)

# =========================
# 基础编译配置
# =========================
# 使用 C++17，并关闭编译器方言扩展，保证跨平台一致性。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 单配置生成器默认既不带优化也不带 NDEBUG。
# 显式给个默认值，避免有人忘记指定时行为随生成器漂移。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# 产物统一输出到 build/ 之外的 bin / lib，便于运行与打包。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

enable_testing()

# 是否启用真实 MySQL 后端：
# OFF: mock 后端（零外部依赖，供开发与测试使用）
# ON : 编译 MySQL 分支并链接 MySQL 客户端库（生产定位）
option(CPPPOOLS_USE_MYSQL "Enable real MySQL-backed DB pool" OFF)
option(CPPPOOLS_USE_REDIS "Enable Redis cache transport" OFF)

# =========================
# 编译告警
# =========================
# 关键：路由分发依赖 -Wswitch 把「新增 RouteId 却忘记接 handler」拦成编译错误。
# 不开这些开关的话，漏接的路由会静默返回 200 + 空响应体。
function(cpppools_set_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /we4062)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wshadow -Wpedantic
            -Wconversion -Wsign-conversion
            -Wold-style-cast -Wcast-align
            -Wnon-virtual-dtor -Woverloaded-virtual
            -Wdouble-promotion -Wunused
            # 下面两项升级为错误：它们对应真实的正确性风险，而非风格问题。
            -Werror=switch          # 漏接路由分支
            -Werror=return-type     # 缺少返回值
        )
    endif()
endfunction()

# =========================
# 核心库：cpppools
# =========================
# 单节点三级池化 API 服务的全部实现，按三个模块组织：
#
#   pool/   通用池化原语（ThreadPool / ObjectPool）  namespace cpppools::pool
#   db/     数据库后端与连接池                        namespace cpppools::db
#   server/ HTTP 接入层                              namespace cpppools::server
#
# 依赖方向：server -> {pool, db}，pool 与 db 互不依赖。
#
# 头文件分层：
#   include/cpppools/<模块>/  对外接口
#   src/<模块>/               实现 + 私有头文件
# http_util / route_table / crud_api 只在 src/server/ 下，属实现细节。
#
# 抽成库而非直接塞进可执行文件，是为了让 test/ 能链接并直接调用内部函数——
# 否则解析与路由逻辑将无法被单元测试触达。
add_library(cpppools STATIC
    src/config/config.cpp
    src/pool/thread_pool.cpp
    src/db/database_factory.cpp
    src/db/db_connection_pool.cpp
    src/db/sqlite_database.cpp
    src/db/mysql_database.cpp
    src/db/datasource_registry.cpp
    src/server/http_util.cpp
    src/server/api_response.cpp
    src/server/route_table.cpp
    src/server/crud_api.cpp
    src/server/http_server.cpp
    src/server/hash_ring.cpp
    src/server/ownership_router.cpp
)
if (CPPPOOLS_USE_REDIS)
    find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h REQUIRED)
    find_library(HIREDIS_LIBRARY NAMES hiredis REQUIRED)
    target_sources(cpppools PRIVATE src/cache/redis_transport.cpp)
    target_compile_definitions(cpppools PUBLIC CPPPOOLS_USE_REDIS=1)
    target_include_directories(cpppools SYSTEM PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(cpppools PUBLIC ${HIREDIS_LIBRARY})
endif()
target_include_directories(cpppools
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(cpppools PUBLIC Threads::Threads SQLite::SQLite3)

# =========================
# 第三方依赖：nlohmann/json（vendored 单头文件，见 third_party/README.md）
# =========================
# INTERFACE 库：它没有可编译的产物，只贡献一个 include 目录。
# 用法：在 .cpp 里 #include <nlohmann/json.hpp>。
#
# 约束：**只在 .cpp 里 include，不要出现在任何公开头文件**。
# json.hpp 约 2.4 万行，进了公开头文件会让每个包含它的 TU 都重复编译一遍。
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(cpppools PRIVATE nlohmann_json)
# 版本号由构建系统注入，避免在源码里硬编码（供 /api/v1/get-config 使用）。
target_compile_definitions(cpppools PUBLIC CPPPOOLS_VERSION="${PROJECT_VERSION}")
if (WIN32)
    target_link_libraries(cpppools PUBLIC ws2_32)
endif()
cpppools_set_warnings(cpppools)

# =========================
# 可选：MySQL 后端
# =========================
set(MYSQL_RUNTIME_DLL "")
if (CPPPOOLS_USE_MYSQL)
    target_compile_definitions(cpppools PUBLIC CPPPOOLS_USE_MYSQL=1)

    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql.h mysql/mysql.h
        HINTS ENV MYSQL_ROOT ENV MYSQL_DIR ENV MARIADB_ROOT ENV MARIADB_DIR
        PATHS
            "C:/Program Files/MySQL/MySQL Server 8.0/include"
            "C:/Program Files/MariaDB/include"
            /usr/include /usr/local/include /opt/homebrew/opt/mysql-client/include
        PATH_SUFFIXES mysql include
    )

    # 关键：MinGW 下优先 libmysql，避免误选 mysqlclient.lib（MSVC 静态库，常导致 ld 链接失败）。
    find_library(MYSQL_LIBRARY
        NAMES libmysql mariadb mysqlclient
        HINTS ENV MYSQL_ROOT ENV MYSQL_DIR ENV MARIADB_ROOT ENV MARIADB_DIR
        PATHS
            "C:/Program Files/MySQL/MySQL Server 8.0/lib"
            "C:/Program Files/MariaDB/lib"
            "C:/Program Files/MariaDB/lib/mariadb"
            /usr/lib /usr/local/lib /opt/homebrew/opt/mysql-client/lib
    )

    if (MINGW AND MYSQL_LIBRARY MATCHES "mysqlclient\\.lib$")
        message(FATAL_ERROR
            "Detected mysqlclient.lib with MinGW, which is usually incompatible. "
            "Please use libmysql.lib (import library) instead, or set -DMYSQL_LIBRARY to libmysql.lib.")
    endif()

    if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "CPPPOOLS_USE_MYSQL=ON but MySQL client headers/libs were not found. "
                            "Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY.")
    endif()

    message(STATUS "MySQL include: ${MYSQL_INCLUDE_DIR}")
    message(STATUS "MySQL library: ${MYSQL_LIBRARY}")

    # SYSTEM：MySQL 官方头含 old-style cast 等，按系统头处理抑制其告警，
    # 避免污染项目 -Wall -Wextra 的零告警纪律。
    # PRIVATE：仅 cpppools 库自身需要；测试不直接 #include <mysql.h>。
    target_include_directories(cpppools SYSTEM PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(cpppools PUBLIC ${MYSQL_LIBRARY})

    if (WIN32)
        get_filename_component(MYSQL_LIBRARY_DIR "${MYSQL_LIBRARY}" DIRECTORY)
        if (EXISTS "${MYSQL_LIBRARY_DIR}/libmysql.dll")
            set(MYSQL_RUNTIME_DLL "${MYSQL_LIBRARY_DIR}/libmysql.dll")
        elseif (EXISTS "${MYSQL_LIBRARY_DIR}/libmariadb.dll")
            set(MYSQL_RUNTIME_DLL "${MYSQL_LIBRARY_DIR}/libmariadb.dll")
        endif()
    endif()
endif()

# 构建后把 MySQL 运行时 DLL 复制到可执行目录（仅 Windows + MySQL 模式）。
function(cpppools_copy_mysql_dll target)
    if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
        )
    endif()
endfunction()

# =========================
# 可执行程序
# =========================
add_executable(cpppools_server src/main.cpp)
target_link_libraries(cpppools_server PRIVATE cpppools)
cpppools_set_warnings(cpppools_server)
cpppools_copy_mysql_dll(cpppools_server)

# =========================
# 测试
# =========================
# 测试全部基于 assert，因此必须确保 NDEBUG 不生效——否则在 Release 配置下
# 所有断言会被消除，测试会退化成空 main 并全部“通过”，形成假绿。
#
# 测试额外加 -Isrc：需要直接调用 src/server/ 下 http_util / route_table / crud_api
# 这些私有头文件里的函数。这是测试相对普通消费者的合理特权。
function(cpppools_add_test name)
    add_executable(${name} test/${name}.cpp)
    target_link_libraries(${name} PRIVATE cpppools)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
    )
    cpppools_set_warnings(${name})
    cpppools_copy_mysql_dll(${name})
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

cpppools_add_test(test_thread_pool)
cpppools_add_test(test_object_pool)
cpppools_add_test(test_db_pool)
cpppools_add_test(test_http_util)
cpppools_add_test(test_metrics)
target_link_libraries(test_metrics PRIVATE nlohmann_json)
cpppools_add_test(test_http_server)
cpppools_add_test(test_config)
cpppools_add_test(test_schema_validator)
cpppools_add_test(test_statement_cache)
cpppools_add_test(test_database_conformance)
cpppools_add_test(test_routing)
cpppools_add_test(test_crud_api)
cpppools_add_test(test_hash_ring)
cpppools_add_test(test_cluster_api)
cpppools_add_test(test_row_cache)
cpppools_add_test(test_caching_database)
cpppools_add_test(test_cache_metrics)
cpppools_add_test(test_ownership)
if (CPPPOOLS_USE_REDIS)
    cpppools_add_test(test_redis_transport)
endif()

# MySQL 自省专项测试：仅在启用 MySQL 后端时编译运行。
# 连接信息来自环境变量（CPPPOOLS_TEST_MYSQL_HOST/PORT/USER/PASSWORD），
# 未配置则测试内自行跳过（spec §十五.3）。
if (CPPPOOLS_USE_MYSQL)
    cpppools_add_test(test_mysql_introspection)
    cpppools_add_test(test_mysql_crud)
    # These tests intentionally exercise the real client API.  Keep the
    # client headers private to the tests instead of leaking them through the
    # public cpppools target interface.
    target_include_directories(test_mysql_introspection SYSTEM PRIVATE ${MYSQL_INCLUDE_DIR})
    target_include_directories(test_mysql_crud SYSTEM PRIVATE ${MYSQL_INCLUDE_DIR})
endif()

# =========================
# 构建摘要
# =========================
if (CPPPOOLS_USE_MYSQL)
    set(CPPPOOLS_BACKEND_DESC "mysql")
else()
    set(CPPPOOLS_BACKEND_DESC "mock")
endif()
message(STATUS "CppPools ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  db backend : ${CPPPOOLS_BACKEND_DESC}")
message(STATUS "  output dir : ${CMAKE_BINARY_DIR}/bin")
