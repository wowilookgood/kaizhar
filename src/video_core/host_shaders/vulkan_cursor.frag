// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in float shade;
layout (location = 0) out vec4 color;

void main() {
    color = vec4(vec3(shade), 1.0);
}
