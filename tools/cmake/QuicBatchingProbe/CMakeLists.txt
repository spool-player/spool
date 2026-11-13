cmake_minimum_required(VERSION 3.20)
project(SpoolQuicBatchingProbe C)
# Probe the exact curl configuration and target headers used by this build.
# No target executable is run, so this is equally valid for cross builds.
foreach(required STREAMING_CURL_SOURCE STREAMING_CURL_BUILD STREAMING_INSTALLED_PC)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${STREAMING_CURL_SOURCE}/lib;${STREAMING_CURL_SOURCE}/include;${STREAMING_CURL_BUILD}/lib")
set(CMAKE_REQUIRED_DEFINITIONS -DHAVE_CONFIG_H -DBUILDING_LIBCURL -D_GNU_SOURCE)
unset(STREAMING_QUIC_BATCHING CACHE)
check_c_source_compiles([=[
#include "curl_setup.h"
#ifdef HAVE_NETINET_UDP_H
#include <netinet/udp.h>
#endif
#if !defined(__linux__) || !defined(USE_NGTCP2) || !defined(USE_NGHTTP3) || !defined(UDP_GRO) || !defined(UDP_SEGMENT) || !defined(HAVE_SENDMMSG) || !defined(HAVE_SENDMSG)
#error Curl QUIC packet batching is not compiled on this target
#endif
int main(void) {
    struct mmsghdr packets[1];
    return recvmmsg(-1, packets, 1, 0, 0) + UDP_GRO + UDP_SEGMENT;
}
]=] STREAMING_QUIC_BATCHING)
if(STREAMING_QUIC_BATCHING)
    set(value 1)
else()
    set(value 0)
    message(STATUS "quic-batching-unavailable: target curl uses the portable packet path")
endif()
file(READ "${STREAMING_INSTALLED_PC}" metadata)
string(REGEX REPLACE "\nspool_quic_batching=[^\n]*" "" metadata "${metadata}")
file(WRITE "${STREAMING_INSTALLED_PC}" "${metadata}\nspool_quic_batching=${value}\n")
