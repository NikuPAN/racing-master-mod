/* The one translation unit that holds miniaudio's implementation. Compiled as C at /W3 with
 * NDEBUG (without NDEBUG about 1,400 assert() calls would stay live and end the game with a
 * message box on a malformed file). See build\build.ps1. */
#include "../music/miniaudio_config.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
