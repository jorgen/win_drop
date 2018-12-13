add_executable(pexip_drop_server main.cpp
                                 win_global.h
                                 server.h
                                 server.cpp
                                 deserializer.h)

set_target_properties(pexip_drop_server PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
set_target_properties(pexip_drop_server PROPERTIES COMPILE_DEFINITIONS "_CONSOLE;UNICODE")

target_link_libraries(pexip_drop_server Ws2_32)
