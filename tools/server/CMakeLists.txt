include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-decision.cpp
    server-decision.h
    server-context.cpp
    server-context.h
    server-realtime.cpp
    server-realtime.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd llama-decision ${CMAKE_THREAD_LIBS_INIT})

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server-connect.cpp
    server-connect.h
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
    server-models-sched.cpp
    server-models-sched.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

add_dependencies(${TARGET} llama-ui-assets)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

# research module (POC, see research/README.md)

option(LLAMA_SERVER_RESEARCH "llama-server: build the research module (hypertoken observation, SQLite store)" OFF)

if (LLAMA_SERVER_RESEARCH)
    include(FetchContent)
    FetchContent_Declare(research_sqlite3
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(research_sqlite3)

    add_library(research-sqlite3 STATIC ${research_sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(research-sqlite3 PUBLIC ${research_sqlite3_SOURCE_DIR})
    target_compile_definitions(research-sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
    set_target_properties(research-sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (MSVC)
        target_compile_options(research-sqlite3 PRIVATE /w)
    else()
        target_compile_options(research-sqlite3 PRIVATE -w)
    endif()

    target_sources(server-context PRIVATE
        research/research-math.cpp
        research/research-math.h
        research/research-compile.cpp
        research/research-compile.h
        research/research-store.cpp
        research/research-store.h
        research/research-capture.cpp
        research/research-capture.h
        research/research-api.cpp
        research/research-api.h
    )
    target_compile_definitions(server-context PUBLIC LLAMA_SERVER_RESEARCH)
    target_link_libraries(server-context PUBLIC research-sqlite3 vendor::hash)
endif()
