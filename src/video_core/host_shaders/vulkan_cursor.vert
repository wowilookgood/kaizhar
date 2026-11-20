// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

// (Kaizhar) x, y, and a shade from black (0) to white (1)
layout (location = 0) in vec3 vert_position;
layout (location = 0) out float shade;

void main() {
    gl_Position = vec4(vert_position.xy, 0.0, 1.0);
    shade = vert_position.z;
}
