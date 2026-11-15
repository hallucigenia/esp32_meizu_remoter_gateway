#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${MEIZU_CJSON_DIR:?Set MEIZU_CJSON_DIR to ESP-IDF 4.3.7 components/json/cJSON}"
MEIZU_TEST_BUILD=$(mktemp -d)
trap 'rm -rf "$MEIZU_TEST_BUILD"' EXIT
export ASAN_OPTIONS="${ASAN_OPTIONS:-alloc_dealloc_mismatch=1:detect_leaks=0}"
${CC:-cc} -std=c99 -g -Wno-deprecated-declarations -fsanitize=address,undefined -I "$MEIZU_CJSON_DIR" -c "$MEIZU_CJSON_DIR/cJSON.c" -o "$MEIZU_TEST_BUILD/cjson.o"
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -g -Wno-deprecated-declarations -fsanitize=address,undefined -I main main/IRProtocol.cpp tests/ir_protocol_test.cpp -o "$MEIZU_TEST_BUILD/ir"
"$MEIZU_TEST_BUILD/ir"
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -g -Wno-deprecated-declarations -fsanitize=address,undefined -I tests/stubs -I main -I "$MEIZU_CJSON_DIR" main/BLEAddress.cpp main/RemoterInfo.cpp tests/state_test.cpp "$MEIZU_TEST_BUILD/cjson.o" -o "$MEIZU_TEST_BUILD/state"
"$MEIZU_TEST_BUILD/state"
${CC:-cc} -std=c99 -Wall -Wextra -Werror -g -Wno-deprecated-declarations -fsanitize=address,undefined -I tests/stubs -I main -c main/wifi_station.c -o "$MEIZU_TEST_BUILD/wifi.o"
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -g -Wno-deprecated-declarations -fsanitize=address,undefined -I tests/stubs -I main tests/wifi_station_test.cpp "$MEIZU_TEST_BUILD/wifi.o" -o "$MEIZU_TEST_BUILD/wifi"
"$MEIZU_TEST_BUILD/wifi"
