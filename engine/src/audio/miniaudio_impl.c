/* Single translation unit that instantiates the miniaudio implementation.
 * No other file should define MINIAUDIO_IMPLEMENTATION.
 *
 * WAVREC_HAVE_ASIO is defined by CMake when the Steinberg ASIO SDK is present;
 * CMake also injects MA_ENABLE_ASIO into the compile definitions in that case. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
