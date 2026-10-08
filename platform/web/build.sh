#!/bin/sh
# Builds the web platform with Emscripten into build-web/ (mf.js + mf.wasm).
# Needs emcc on the PATH (brew install emscripten). Serve the repository root with
# platform/web/tools/serve.py and open /platform/web/app/ (see platform/web/README.md).
set -e
cd "$(dirname "$0")/../.."
OUT=build-web
mkdir -p "$OUT"

# The core without what needs blocking threads (the thread scheduler, the segment recorder and
# the camera preview): the web runs every stage on one thread (CooperativeScheduler).
CORE=$(ls core/src/*.cpp | grep -v -e thread_scheduler -e segment_recorder -e live_preview)

em++ -std=c++17 -O2 -pthread -Wall -Wno-unused-parameter \
  -Icore/include -Iplatform/web/include -Iplatform/web/src \
  $CORE platform/web/src/mp4_demuxer.cpp platform/web/src/mp4_muxer.cpp platform/web/src/web_platform.cpp platform/web/src/web_api.cpp \
  --js-library platform/web/src/library_mf.js --js-library platform/web/src/library_mf_compositor.js \
  -pthread -sWASM_WORKERS -sAUDIO_WORKLET -sOFFSCREENCANVAS_SUPPORT \
  -sMODULARIZE -sEXPORT_NAME=createMediaFramework -sENVIRONMENT=web \
  -sINITIAL_MEMORY=536870912 -sSTACK_SIZE=1048576 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToNewUTF8 \
  -o "$OUT/mf.js"
echo "$OUT/mf.js"
