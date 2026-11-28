set(SPOOLET_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/automation")
add_executable(spoolet
    "${SPOOLET_SOURCE_DIR}/SpooletMain.cpp"
    "${SPOOLET_SOURCE_DIR}/LocalControlProtocol.cpp"
    "${SPOOLET_SOURCE_DIR}/LocalControlProtocol.h")
target_link_libraries(spoolet PRIVATE Qt6::Core Qt6::Network)
qt_add_resources(spoolet remote_discovery PREFIX "/automation"
    BASE "${SPOOLET_SOURCE_DIR}" FILES "${SPOOLET_SOURCE_DIR}/RemoteDiscovery.sh")
