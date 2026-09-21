//
// Apricot Renderer — C++ backend implementation
// Implements aprendscene.h using GLM for math and SpudGPU for GPU calls.
//

#include "render/aprenderer.h"
#include "aprend_internal.hpp"

extern "C" {
void aprenderer_cmd(
    aprend_command_list list,
    APREND_COMMAND cmd) {}
} // Extern "C"
