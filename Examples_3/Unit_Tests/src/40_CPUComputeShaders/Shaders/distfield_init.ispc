//--------------------------------------
// Generated from Forge Shading Language
//--------------------------------------

#define STAGE_COMP
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

inline float dot(float<2> v1, float<2> v2) {
    return v1.x * v2.x + v1.y * v2.y;
}

inline float dot(float<3> v1, float<3> v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}

inline float dot(float<4> v1, float<4> v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z + v1.w * v2.w;
}

inline float length(float<2> v) {
    return sqrt(dot(v, v));
}

inline float length(float<3> v) {
    return sqrt(dot(v, v));
}

inline float length(float<4> v) {
    return sqrt(dot(v, v));
}

#endif









struct ComputeTestData {
    uint32_t width;
    uint32_t height;
// line 7
};

struct DistanceFieldParams {
    uint width;
    uint height;
    float threshold;
// line 13
};

uint getPosIndex(int<2> pos, int<2> dims)
{
    return (uint)(pos.y * dims.x + pos.x);
}

NUM_THREADS(16, 16, 1)
void DISTFIELD_INIT_impl(uniform const float gInputBuffer[],uniform int gSeedBuffer[],uniform const DistanceFieldParams& gParams,uint<3> DTid)
{
    INIT_MAIN;

    int<2> pos = make_int2(DTid.xy);
    int<2> dims = make_int2(gParams.width, gParams.height);

    if (pos.x >= dims.x || pos.y >= dims.y)
        return;

    uint idx = getPosIndex(pos, dims);
    float centerValue = gInputBuffer[idx];
    bool isInside = centerValue > gParams.threshold;


    bool isBoundary = false;
    if (pos.x > 0 && pos.x < dims.x - 1 && pos.y > 0 && pos.y < dims.y - 1)
    {
        int leftIndex = getPosIndex(pos + make_int2(-1, 0), dims);
        float left = gInputBuffer[leftIndex];
        int rightIndex = getPosIndex(pos + make_int2(1, 0), dims);
        float right = gInputBuffer[rightIndex];
        int upIndex = getPosIndex(pos + make_int2(0, -1), dims);
        float up = gInputBuffer[upIndex];
        int downIndex = getPosIndex(pos + make_int2(0, 1), dims);
        float down = gInputBuffer[downIndex];

        bool leftIn = left > gParams.threshold;
        bool rightIn = right > gParams.threshold;
        bool upIn = up > gParams.threshold;
        bool downIn = down > gParams.threshold;

        isBoundary = (leftIn != isInside || rightIn != isInside ||
                     upIn != isInside || downIn != isInside);
    }


    uint seedIdx = idx * 2;
    if (isBoundary)
    {
        gSeedBuffer[seedIdx] = pos.x;
        gSeedBuffer[seedIdx + 1] = pos.y;
    }
    else
    {
        gSeedBuffer[seedIdx] = -1;
        gSeedBuffer[seedIdx + 1] = -1;
    }

    return;
}

export void DISTFIELD_INIT(uniform const float gInputBuffer_arg[],uniform int gSeedBuffer_arg[],uniform const DistanceFieldParams& gParams_arg,uniform int dispatch_x, uniform int dispatch_y, uniform int dispatch_z) {
    foreach_tiled (z = 0 ... dispatch_z, y = 0 ... dispatch_y, x = 0 ... dispatch_x) {
        uint<3> DTid = { x,y,z };
        DISTFIELD_INIT_impl(gInputBuffer_arg, gSeedBuffer_arg, gParams_arg, DTid);
    }
}
