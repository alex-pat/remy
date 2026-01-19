
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v6.0.2
)
FetchContent_MakeAvailable(ftxui)

# Server
set(REMY_SERVER_TARGET remy-server)
add_executable(${REMY_SERVER_TARGET}
    main-server.cpp
    server.cpp
)

target_link_libraries(${REMY_SERVER_TARGET}
    PRIVATE ftxui::screen
    PRIVATE ftxui::dom
    PRIVATE ftxui::component
    PRIVATE Boost::coroutine
    PRIVATE Boost::program_options
)

# Client
set(REMY_CLIENT_TARGET remy-client)
add_executable(${REMY_CLIENT_TARGET}
    main-client.cpp
    view-client.cpp
    client.cpp
)

target_link_libraries(${REMY_CLIENT_TARGET}
    PRIVATE ftxui::screen
    PRIVATE ftxui::dom
    PRIVATE ftxui::component
    PRIVATE Boost::coroutine
    PRIVATE Boost::program_options
)

install(TARGETS
    ${REMY_SERVER_TARGET}
    ${REMY_CLIENT_TARGET}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
