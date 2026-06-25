// Flat shim — arduino-cli 1.5.1 (ArduinoDroid) adds
// src/ to the include path but does NOT recurse. The
// canonical public header lives at include/AutoLink.h;
// this file makes the flat name work.
#include "../include/AutoLink.h"
