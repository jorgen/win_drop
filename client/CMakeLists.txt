add_executable(pexip_drop_client main.cpp
                                 win_global.h
                                 client.h
                                 client.cpp
								 sha1.h
								 sha1.c
                                 serializer.h)

set_target_properties(pexip_drop_client PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
set_target_properties(pexip_drop_client PROPERTIES COMPILE_DEFINITIONS "_CONSOLE;UNICODE")

target_link_libraries(pexip_drop_client Ws2_32)
