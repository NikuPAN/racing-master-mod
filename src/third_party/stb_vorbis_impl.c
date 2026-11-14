/* The one translation unit that holds stb_vorbis's implementation (pull API only). Compiled as C
 * with NDEBUG so no assert() can end the game. See build\build.ps1. */
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
