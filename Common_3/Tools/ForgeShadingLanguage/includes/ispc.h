#ifndef ISPC_H
#define ISPC_H

#define uint32_t uint32
#define NUM_THREADS(x, y, z)
#define INIT_MAIN

#define MAKE_VECTOR2(T) \
inline T<2> make_##T##2(T x, T y) { \
    T<2> result; \
    result.x = x; \
    result.y = y; \
    return result; \
}

#define MAKE_VECTOR3(T) \
inline T<3> make_##T##3(T x, T y, T z) { \
    T<3> result; \
    result.x = x; \
    result.y = y; \
    result.z = z; \
    return result; \
}

#define MAKE_VECTOR4(T) \
inline T<4> make_##T##4(T x, T y, T z, T w) { \
    T<4> result; \
    result.x = x; \
    result.y = y; \
    result.z = z; \
    result.w = w; \
    return result; \
}

// Generate all vector constructors
MAKE_VECTOR2(int)
MAKE_VECTOR2(uint)
MAKE_VECTOR2(float)
MAKE_VECTOR2(bool)

MAKE_VECTOR3(int)
MAKE_VECTOR3(uint)
MAKE_VECTOR3(float)
MAKE_VECTOR3(bool)

MAKE_VECTOR4(int)
MAKE_VECTOR4(uint)
MAKE_VECTOR4(float)
MAKE_VECTOR4(bool)

// Conversion functions (not macros)
inline float<2> make_float2(uint<2> v) { 
    float<2> result;
    result.x = (float)v.x;
    result.y = (float)v.y;
    return result;
}

inline float<2> make_float2(int<2> v) { 
    float<2> result;
    result.x = (float)v.x;
    result.y = (float)v.y;
    return result;
}

inline int<2> make_int2(float<2> v) { 
    int<2> result;
    result.x = (int)v.x;
    result.y = (int)v.y;
    return result;
}

inline uint<2> make_uint2(float<2> v) { 
    uint<2> result;
    result.x = (uint)v.x;
    result.y = (uint)v.y;
    return result;
}

inline float length(float<2> v) {
    return sqrt(v.x * v.x + v.y * v.y);
}

inline float length(float<3> v) {
    return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline float length(float<4> v) {
    return sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
}



#endif