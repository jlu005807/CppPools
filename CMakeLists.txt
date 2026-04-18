cmake_minimum_required(VERSION 3.16)
project(CppPools LANGUAGES CXX)

# =========================
# 基础编译配置
# =========================
# 使用 C++17，并关闭编译器方言扩展，保证跨平台一致性。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 是否启用真实 MySQL 连接池：
# OFF: 使用 mock 模式
# ON : 编译 MySQL 分支并链接 MySQL 客户端库
option(CPPPOOLS_USE_MYSQL "Enable real MySQL-backed DB pool" OFF)

# Windows 下用于记录 MySQL 运行时 DLL 路径，构建后复制到可执行目录。
set(MYSQL_RUNTIME_DLL "")
set(MYSQL_RUNTIME_BIN_DIR "")

# =========================
# 输出目录统一
# =========================
# 最终产物统一输出到 output/ 目录，便于运行与提交。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)

# 多配置生成器（如 VS）也统一输出到同一目录。
foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG_TYPE} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/lib)
endforeach()

enable_testing()

# =========================
# 核心库：thread_pool
# =========================
add_library(thread_pool STATIC
    src/thread_pool/thread_pool.cpp
)
# PUBLIC: 链接该库的目标会自动继承 include 头文件目录。
target_include_directories(thread_pool PUBLIC include)

# =========================
# 核心库：db_pool
# =========================
add_library(db_pool STATIC
    src/db_pool/mock_db_connection.cpp
    src/db_pool/db_connection_pool.cpp
)
target_include_directories(db_pool PUBLIC include)

# =========================
# 可选：MySQL 支持
# =========================
if (CPPPOOLS_USE_MYSQL)
    target_compile_definitions(db_pool PUBLIC CPPPOOLS_USE_MYSQL=1)

    # 查找 MySQL/MariaDB 头文件目录。
    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql.h mysql/mysql.h
        HINTS
            ENV MYSQL_ROOT
            ENV MYSQL_DIR
            ENV MARIADB_ROOT
            ENV MARIADB_DIR
        PATHS
            "C:/Program Files/MySQL/MySQL Server 8.0/include"
            "C:/Program Files/MariaDB/include"
        PATH_SUFFIXES mysql include
    )

    # 查找 MySQL 库文件。
    # 关键：MinGW 下优先 libmysql，避免误选 mysqlclient.lib（MSVC 静态库，常导致 ld 链接失败）。
    find_library(MYSQL_LIBRARY
        NAMES libmysql mariadb mysqlclient
        HINTS
            ENV MYSQL_ROOT
            ENV MYSQL_DIR
            ENV MARIADB_ROOT
            ENV MARIADB_DIR
        PATHS
            "C:/Program Files/MySQL/MySQL Server 8.0/lib"
            "C:/Program Files/MariaDB/lib"
            "C:/Program Files/MariaDB/lib/mariadb"
    )

    if (MINGW AND MYSQL_LIBRARY MATCHES "mysqlclient\\.lib$")
        message(FATAL_ERROR
            "Detected mysqlclient.lib with MinGW, which is usually incompatible. "
            "Please use libmysql.lib (import library) instead, or set -DMYSQL_LIBRARY to libmysql.lib.")
    endif()

    if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "CPPPOOLS_USE_MYSQL=ON but MySQL client headers/libs were not found. Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY.")
    endif()

    message(STATUS "MySQL include: ${MYSQL_INCLUDE_DIR}")
    message(STATUS "MySQL library: ${MYSQL_LIBRARY}")

    target_include_directories(db_pool PUBLIC ${MYSQL_INCLUDE_DIR})
    target_link_libraries(db_pool PUBLIC ${MYSQL_LIBRARY})

    if (WIN32)
        get_filename_component(MYSQL_LIBRARY_DIR "${MYSQL_LIBRARY}" DIRECTORY)
        get_filename_component(MYSQL_ROOT_DIR "${MYSQL_LIBRARY_DIR}" DIRECTORY)
        set(MYSQL_RUNTIME_BIN_DIR "${MYSQL_ROOT_DIR}/bin")

        if (EXISTS "${MYSQL_LIBRARY_DIR}/libmysql.dll")
            set(MYSQL_RUNTIME_DLL "${MYSQL_LIBRARY_DIR}/libmysql.dll")
        elseif (EXISTS "${MYSQL_LIBRARY_DIR}/libmariadb.dll")
            set(MYSQL_RUNTIME_DLL "${MYSQL_LIBRARY_DIR}/libmariadb.dll")
        endif()
    endif()
endif()

# =========================
# Demo：线程池基础示例
# =========================
add_executable(demo_thread_pool demo/main.cpp)
target_link_libraries(demo_thread_pool PRIVATE thread_pool)

# =========================
# Demo：三池联动命令行示例
# =========================
add_executable(demo_multi_pools demo/multi_pools_demo.cpp)
target_link_libraries(demo_multi_pools PRIVATE thread_pool db_pool)
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    add_custom_command(TARGET demo_multi_pools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:demo_multi_pools>"
    )
endif()

# =========================
# 测试：线程池
# =========================
add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE thread_pool)
add_test(NAME test_thread_pool COMMAND $<TARGET_FILE:test_thread_pool>)

# =========================
# Demo：HTTP 服务端
# =========================
add_executable(demo_server
    demo/server_main.cpp
    src/server/http_server.cpp
)
target_link_libraries(demo_server PRIVATE thread_pool db_pool)
if (WIN32)
    target_link_libraries(demo_server PRIVATE ws2_32)
endif()
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    add_custom_command(TARGET demo_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:demo_server>"
    )
endif()
if (CPPPOOLS_USE_MYSQL)
    # 兼容文件名：在 MySQL 模式下额外生成 demo_server_mysql(.exe)
    add_custom_command(TARGET demo_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:demo_server>"
            "$<TARGET_FILE_DIR:demo_server>/demo_server_mysql${CMAKE_EXECUTABLE_SUFFIX}"
    )
    # 兼容目标：可直接执行 cmake --build build --target demo_server_mysql
    add_custom_target(demo_server_mysql DEPENDS demo_server)
endif()

# =========================
# 测试：对象池 + 连接池
# =========================
add_executable(test_pools tests/test_pools.cpp)
target_link_libraries(test_pools PRIVATE db_pool)
add_test(NAME test_pools COMMAND $<TARGET_FILE:test_pools>)
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    add_custom_command(TARGET test_pools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:test_pools>"
    )
endif()
