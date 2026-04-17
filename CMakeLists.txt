cmake_minimum_required(VERSION 3.16)
project(CppPools LANGUAGES CXX)

# =========================
# 基础编译配置
# =========================
# 统一使用 C++17，且不启用编译器私有扩展（保证跨平台一致性）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 是否启用真实 MySQL 连接池：
# OFF = 使用 mock 连接，ON = 编译并链接 MySQL 客户端库。
option(CPPPOOLS_USE_MYSQL "Enable real MySQL-backed DB pool" OFF)

# Windows 下如果启用 MySQL，会在构建后把运行时 DLL 拷贝到可执行目录。
set(MYSQL_RUNTIME_DLL "")
set(MYSQL_RUNTIME_BIN_DIR "")

# =========================
# 输出目录规范化
# =========================
# 把最终产物统一放进 output/，便于演示与发布：
# - 可执行文件 -> output/bin
# - 动态库/静态库 -> output/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)

# 多配置生成器（如 Visual Studio）会按配置区分输出目录，
# 这里统一重定向到同一 output 目录，避免 Debug/Release 分散。
foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG_TYPE} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/output/lib)
endforeach()

# 开启 CTest 测试支持（配合 add_test 使用）。
enable_testing()

# =========================
# 核心库目标：thread_pool
# =========================
# 线程池实现打包为可复用静态库，供 demo 和测试链接。
add_library(thread_pool STATIC
    src/thread_pool/thread_pool.cpp
)
# PUBLIC 表示：链接 thread_pool 的目标会自动继承 include 头文件路径。
target_include_directories(thread_pool PUBLIC include)

# =========================
# 核心库目标：db_pool
# =========================
# 连接池与 mock/real DB 访问实现统一打包到 db_pool。
add_library(db_pool STATIC
    src/db_pool/mock_db_connection.cpp
    src/db_pool/db_connection_pool.cpp
)
target_include_directories(db_pool PUBLIC include)

# =========================
# 可选：启用 MySQL 实现
# =========================
if (CPPPOOLS_USE_MYSQL)
    # 给 db_pool 传播宏，控制源码中 MySQL 分支编译。
    target_compile_definitions(db_pool PUBLIC CPPPOOLS_USE_MYSQL=1)

    # 查找 MySQL/MariaDB 头文件和库文件。
    # 可通过以下方式显式指定：
    #   -DMYSQL_INCLUDE_DIR=<path>
    #   -DMYSQL_LIBRARY=<path>
    # 或设置环境变量：
    #   MYSQL_ROOT / MYSQL_DIR / MARIADB_ROOT / MARIADB_DIR
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
    find_library(MYSQL_LIBRARY
        NAMES mysqlclient libmysql mariadb
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

    # 依赖找不到就立即停止配置，避免后续编译阶段才报错。
    if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR "CPPPOOLS_USE_MYSQL=ON but MySQL client headers/libs were not found. Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY.")
    endif()

    # 打印路径，便于排查链接问题。
    message(STATUS "MySQL include: ${MYSQL_INCLUDE_DIR}")
    message(STATUS "MySQL library: ${MYSQL_LIBRARY}")

    # 把 MySQL 头文件与库链接到 db_pool，并向下游传播。
    target_include_directories(db_pool PUBLIC ${MYSQL_INCLUDE_DIR})
    target_link_libraries(db_pool PUBLIC ${MYSQL_LIBRARY})

    # Windows 额外处理：定位运行时 DLL，后续拷贝到 exe 目录。
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
# Demo 目标：线程池基础示例
# =========================
add_executable(demo_thread_pool demo/main.cpp)
target_link_libraries(demo_thread_pool PRIVATE thread_pool)

# =========================
# Demo 目标：三池联动命令行示例
# =========================
add_executable(demo_multi_pools demo/multi_pools_demo.cpp)
target_link_libraries(demo_multi_pools PRIVATE thread_pool db_pool)
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    # MySQL 模式下，确保 demo_multi_pools 运行时可找到 DLL。
    add_custom_command(TARGET demo_multi_pools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:demo_multi_pools>"
    )
endif()

# =========================
# 测试目标：线程池
# =========================
add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE thread_pool)
add_test(NAME test_thread_pool COMMAND $<TARGET_FILE:test_thread_pool>)

# =========================
# Demo 目标：HTTP 服务端
# =========================
add_executable(demo_server
    demo/server_main.cpp
    src/server/http_server.cpp
)
target_link_libraries(demo_server PRIVATE thread_pool db_pool)
if (WIN32)
    # WinSock 链接库（Windows 网络编程必需）。
    target_link_libraries(demo_server PRIVATE ws2_32)
endif()
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    # MySQL 模式下，确保 demo_server 运行时可找到 DLL。
    add_custom_command(TARGET demo_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:demo_server>"
    )
endif()
if (CPPPOOLS_USE_MYSQL)
    # 保持主目标名 demo_server 不变，同时生成兼容文件名 demo_server_mysql(.exe)。
    add_custom_command(TARGET demo_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:demo_server>"
            "$<TARGET_FILE_DIR:demo_server>/demo_server_mysql${CMAKE_EXECUTABLE_SUFFIX}"
    )
    # 提供标准目标入口，便于直接构建：
    #   cmake --build build --target demo_server_mysql
    add_custom_target(demo_server_mysql DEPENDS demo_server)
endif()

# =========================
# 测试目标：对象池 + 连接池
# =========================
add_executable(test_pools tests/test_pools.cpp)
target_link_libraries(test_pools PRIVATE db_pool)
add_test(NAME test_pools COMMAND $<TARGET_FILE:test_pools>)
if (CPPPOOLS_USE_MYSQL AND WIN32 AND MYSQL_RUNTIME_DLL)
    # MySQL 模式下，确保 test_pools 运行时可找到 DLL。
    add_custom_command(TARGET test_pools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:test_pools>"
    )
endif()
