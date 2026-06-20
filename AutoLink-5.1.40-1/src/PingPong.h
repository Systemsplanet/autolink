// PingPong.h — flat shim at src/PingPong.h. The canonical home of
// this header is src/al/pingpong/PingPongImpl.h (per the Arduino
// Library Spec 1.5 subdir convention). This shim exists so user
// sketches can do either:
//
//   #include "PingPong.h"               // works under arduino-cli 1.5.1
//   #include <PingPong.h>               // works when library.properties lists it
//   #include <al/pingpong/PingPongImpl.h> // works under Arduino IDE 2.x
//
// arduino-cli 1.5.1 does NOT recurse into src/<subdir>/ for include
// resolution, so the subdir path is invisible to it. Arduino IDE 2.x
// (which fully implements the library spec) recurses, so the subdir
// path works there.
//
// The shim just forwards to the canonical file.
#include "al/pingpong/PingPongImpl.h"
