
if(NOT BUILD_SERVER_ONLY)
  include(FetchContent)
  set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v7.0.3
  )
  FetchContent_MakeAvailable(ftxui)
endif()

# Server
set(REMY_SERVER_TARGET remy-server)
add_executable(${REMY_SERVER_TARGET}
    main-server.cpp
    server.cpp
)

target_link_libraries(${REMY_SERVER_TARGET}
    PRIVATE Boost::coroutine
    PRIVATE Boost::program_options
)

if(WIN32)
  target_link_libraries(${REMY_SERVER_TARGET}
    PRIVATE ws2_32
    PRIVATE mswsock
  )
endif()

set(INSTALL_TARGETS ${REMY_SERVER_TARGET})

# Client
if(NOT BUILD_SERVER_ONLY)
  set(REMY_CLIENT_TARGET remy-client)
  add_executable(${REMY_CLIENT_TARGET}
      main-client.cpp
      view-client.cpp
      client.cpp
      sys-linux.cpp
  )

  target_link_libraries(${REMY_CLIENT_TARGET}
      PRIVATE ftxui::screen
      PRIVATE ftxui::dom
      PRIVATE ftxui::component
      PRIVATE Boost::coroutine
      PRIVATE Boost::program_options
  )
  list(APPEND INSTALL_TARGETS ${REMY_CLIENT_TARGET})
endif()

install(TARGETS
    ${INSTALL_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
