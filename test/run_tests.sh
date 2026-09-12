#!/usr/bin/env bash
# Builds and runs every PulseCoAP host-side test suite. No Arduino
# toolchain required — just a host g++/clang++ with C++11.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
STD="${STD:-c++11}"
FLAGS=(-std="$STD" -Wall -Wextra -Isrc)

build_and_run() {
    local name="$1"; shift
    echo "==> $name"
    "$CXX" "${FLAGS[@]}" "$@" -o "/tmp/pulsecoap_$name"
    "/tmp/pulsecoap_$name"
    echo
}

build_and_run test_message_codec \
    test/test_message_codec.cpp src/PulseCoAPMessage.cpp

build_and_run test_transaction_pool \
    test/test_transaction_pool.cpp src/PulseCoAPTransaction.cpp

build_and_run test_client_server_integration \
    test/test_client_server_integration.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

build_and_run test_blockwise \
    -DPULSECOAP_ENABLE_BLOCKWISE=1 \
    -DPULSECOAP_BLOCK_SZX=2 \
    -DPULSECOAP_MAX_MSG_SIZE=512 \
    -DPULSECOAP_BLOCK1_MAX_BODY=1024 \
    -DPULSECOAP_BLOCK2_MAX_BODY=1024 \
    -DPULSECOAP_MAX_BLOCK1_SESSIONS=2 \
    -DPULSECOAP_MAX_TRANSACTIONS=4 \
    test/test_blockwise.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

build_and_run test_path_templates \
    -DPULSECOAP_MAX_RESOURCES=12 \
    test/test_path_templates.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

build_and_run test_multi_observe \
    -DPULSECOAP_MAX_TRANSACTIONS=6 \
    -DPULSECOAP_MAX_RESOURCES=12 \
    -DPULSECOAP_MAX_OBSERVERS=8 \
    test/test_multi_observe.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

build_and_run test_posix_transport \
    test/test_posix_transport.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

build_and_run test_multicast_discover \
    -DPULSECOAP_DISCOVER_TIMEOUT_MS=100 \
    test/test_multicast_discover.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp

echo "All PulseCoAP test suites passed."
