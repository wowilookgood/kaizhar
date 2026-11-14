// Enough of GLSL in C++ to run the emulator's fragment shaders on the host (difftest.py turns
// swizzles into calls: v.rgb -> v.rgb()). Textures and lookup tables are hashes of what is
// asked: deterministic, the same for every shader.
#pragma once
#include <cmath>
#include <cstdint>

typedef unsigned int uint;

struct vec2 {
    union { float x, r; };
    union { float y, g; };
    vec2() : x(0), y(0) {}
    explicit vec2(float v) : x(v), y(v) {}
    vec2(float a, float b) : x(a), y(b) {}
    explicit vec2(struct ivec2 v);
    vec2 yy() const { return {y, y}; }
    vec2 xy() const { return *this; }
};
struct vec3;
struct ref3 {
    float &a, &b, &c;
    ref3& operator=(const vec3& v);
    ref3& operator=(const ref3& v);
    ref3& operator+=(const vec3& v);
    operator vec3() const;
};
struct vec3 {
    union { float x, r; };
    union { float y, g; };
    union { float z, b; };
    vec3() : x(0), y(0), z(0) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float b_, float c) : x(a), y(b_), z(c) {}
    vec3(vec2 v, float c) : x(v.x), y(v.y), z(c) {}
    float& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    ref3 rgb() { return {x, y, z}; }
    vec3 rrr() const { return {x, x, x}; }
    vec3 xyz() const { return *this; }
    vec2 xy() const { return {x, y}; }
};
inline ref3& ref3::operator=(const vec3& v) { a = v.x; b = v.y; c = v.z; return *this; }
inline ref3& ref3::operator=(const ref3& v) { const float p = v.a, q = v.b, s = v.c; a = p; b = q; c = s; return *this; }
inline ref3& ref3::operator+=(const vec3& v) { a += v.x; b += v.y; c += v.z; return *this; }
inline ref3::operator vec3() const { return {a, b, c}; }
struct vec4 {
    union { float x, r; };
    union { float y, g; };
    union { float z, b; };
    union { float w, a; };
    vec4() : x(0), y(0), z(0), w(0) {}
    explicit vec4(float v) : x(v), y(v), z(v), w(v) {}
    vec4(float p, float q, float s, float t) : x(p), y(q), z(s), w(t) {}
    vec4(vec3 v, float t) : x(v.x), y(v.y), z(v.z), w(t) {}
    float& operator[](int i) { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; }
    ref3 rgb() { return {x, y, z}; }
    vec3 xyz() const { return {x, y, z}; }
    vec3 aaa() const { return {w, w, w}; }
    vec3 rrr() const { return {x, x, x}; }
    vec3 ggg() const { return {y, y, y}; }
    vec3 bbb() const { return {z, z, z}; }
    vec2 rg() const { return {x, y}; }
    vec2 xy() const { return {x, y}; }
};
struct ivec2 {
    int x, y;
    ivec2() : x(0), y(0) {}
    ivec2(int a, int b) : x(a), y(b) {}
};
inline vec2::vec2(ivec2 v) : x(static_cast<float>(v.x)), y(static_cast<float>(v.y)) {}
struct ivec4 {
    int x, y, z, w;
    int& operator[](int i) { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; }
};
struct uvec2 {
    uint x, y;
    uvec2(uint a, uint b) : x(a), y(b) {}
};

#define VEC_OPS(T, N, ...) \
    inline T operator+(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] + (&b.x)[i]; return r; } \
    inline T operator-(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] - (&b.x)[i]; return r; } \
    inline T operator*(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] * (&b.x)[i]; return r; } \
    inline T operator/(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] / (&b.x)[i]; return r; } \
    inline T operator*(T a, float b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] * b; return r; } \
    inline T operator*(float b, T a) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = b * (&a.x)[i]; return r; } \
    inline T operator/(T a, float b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] / b; return r; } \
    inline T operator+(T a, float b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] + b; return r; } \
    inline T operator-(T a, float b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] - b; return r; } \
    inline T operator-(T a) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = -(&a.x)[i]; return r; } \
    inline T& operator+=(T& a, T b) { a = a + b; return a; } \
    inline T& operator*=(T& a, T b) { a = a * b; return a; } \
    inline T& operator*=(T& a, float b) { a = a * b; return a; } \
    inline T& operator/=(T& a, float b) { a = a / b; return a; } \
    inline T clamp(T v, T lo, T hi) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::fmin(std::fmax((&v.x)[i], (&lo.x)[i]), (&hi.x)[i]); return r; } \
    inline T min(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::fmin((&a.x)[i], (&b.x)[i]); return r; } \
    inline T max(T a, T b) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::fmax((&a.x)[i], (&b.x)[i]); return r; } \
    inline T abs(T a) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::fabs((&a.x)[i]); return r; } \
    inline T floor(T a) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::floor((&a.x)[i]); return r; } \
    inline T round(T a) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = std::round((&a.x)[i]); return r; } \
    inline T mix(T a, T b, T t) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] * (1.0f - (&t.x)[i]) + (&b.x)[i] * (&t.x)[i]; return r; } \
    inline T mix(T a, T b, float t) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] * (1.0f - t) + (&b.x)[i] * t; return r; } \
    inline T fma(T a, T b, T c) { T r; for (int i = 0; i < N; i++) (&r.x)[i] = (&a.x)[i] * (&b.x)[i] + (&c.x)[i]; return r; } \
    inline float dot(T a, T b) { float s = 0; for (int i = 0; i < N; i++) s += (&a.x)[i] * (&b.x)[i]; return s; } \
    inline float length(T a) { return std::sqrt(dot(a, a)); } \
    inline T normalize(T a) { return a / length(a); }
VEC_OPS(vec2, 2)
VEC_OPS(vec3, 3)
VEC_OPS(vec4, 4)

inline vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float clamp(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
inline float min(float a, float b) { return std::fmin(a, b); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline int max(int a, int b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
// (abs, floor, round, sqrt, log2 and fma of a float are the C library's)
inline float fract(float a) { return a - std::floor(a); }
inline float sign(float a) { return a > 0 ? 1.0f : a < 0 ? -1.0f : 0.0f; }
inline float mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }
inline vec2 dFdx(vec2) { return vec2(0.01f); }
inline vec2 dFdy(vec2) { return vec2(0.02f); }

struct sampler2D { int id; };
struct samplerBuffer { int id; };
inline uint32_t hash32(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}
inline float unit(uint32_t h) { return (h & 0xFFFF) / 65535.0f; }
inline uint32_t bits(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
inline vec4 texel(int id, vec2 uv) {
    const uint32_t h = hash32(bits(uv.x) * 31u + bits(uv.y) * 131u + static_cast<uint32_t>(id) * 7919u);
    return {unit(h), unit(hash32(h + 1)), unit(hash32(h + 2)), unit(hash32(h + 3))};
}
inline vec4 textureLod(sampler2D s, vec2 uv, float) { return texel(s.id, uv); }
inline vec4 texture(sampler2D s, vec2 uv) { return texel(s.id, uv); }
inline vec4 textureProj(sampler2D s, vec3 uvw) { return texel(s.id, vec2(uvw.x / uvw.z, uvw.y / uvw.z)); }
inline ivec2 textureSize(sampler2D, int) { return {64, 32}; }
inline vec4 texelFetch(samplerBuffer s, int index) {
    const uint32_t h = hash32(static_cast<uint32_t>(index) * 2654435761u + static_cast<uint32_t>(s.id));
    return {unit(h), (unit(hash32(h + 1)) - 0.5f) * 0.05f, unit(hash32(h + 2)), unit(hash32(h + 3))};
}
