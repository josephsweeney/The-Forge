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

struct RootConstantData {
 uint stepIndex;
// line 10
};

uint getPosIndex(int<2> pos, int<2> dims)
{
    return (uint)(pos.y * dims.x + pos.x);
}

NUM_THREADS(16, 16, 1)
void DISTFIELD_FLOOD_impl(uniform const float gFloodInputBuffer[],uniform float gFloodOutputBuffer[],uniform int gFloodSeedBufferIn[],uniform int gFloodSeedBufferOut[],uniform const DistanceFieldParams& gFloodParams,uniform const RootConstantData& RootConstant, uint<3> DTid)
{
    INIT_MAIN;

    int<2> pos = make_int2(DTid.xy);
    int<2> dims = make_int2(gFloodParams.width, gFloodParams.height);

    if (pos.x >= dims.x || pos.y >= dims.y)
        return;

    uint idx = getPosIndex(pos, dims);
    int jumpSize = (int)(1) << RootConstant.stepIndex;

    uint seedIdx = idx * 2;
    int<2> closestSeed = make_int2(gFloodSeedBufferIn[seedIdx], gFloodSeedBufferIn[seedIdx + 1]);
    float minDistSq = 1e18;


    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            int<2> samplePos = pos + make_int2(dx, dy) * jumpSize;

            if (samplePos.x < 0 || samplePos.x >= dims.x ||
                samplePos.y < 0 || samplePos.y >= dims.y)
                continue;

            uint sampleSeedIdx = getPosIndex(samplePos, dims) * 2;
            int<2> seed = make_int2(gFloodSeedBufferIn[sampleSeedIdx], gFloodSeedBufferIn[sampleSeedIdx + 1]);

            if (seed.x != -1)
            {
                float<2> offset = make_float2(pos - seed);
                float distSq = dot(offset, offset);
                if (distSq < minDistSq)
                {
                    minDistSq = distSq;
                    closestSeed = seed;
                }
            }
        }
    }


    gFloodSeedBufferOut[seedIdx] = closestSeed.x;
    gFloodSeedBufferOut[seedIdx + 1] = closestSeed.y;


    if (jumpSize == 1)
    {
        float sign = gFloodInputBuffer[idx] > gFloodParams.threshold ? -1.0 : 1.0;
        gFloodOutputBuffer[idx] = (closestSeed.x != -1) ? sign * sqrt(minDistSq) : sign * 1e9;
    }

    return;
}

#define NUM_X_THREADS 16
#define NUM_Y_THREADS 16
#define NUM_Z_THREADS 1

task void DISTFIELD_FLOOD_task(uniform const float gFloodInputBuffer[],
                              uniform float gFloodOutputBuffer[],
                              uniform int gFloodSeedBufferIn[],
                              uniform int gFloodSeedBufferOut[],
                              uniform const DistanceFieldParams& gFloodParams,
                              uniform const RootConstantData& RootConstant,
                              uniform int dispatch_x, 
                              uniform int dispatch_y, 
                              uniform int dispatch_z) {
    // Calculate rows per task, dividing work only in Y dimension for better coherency
    uniform int rows_per_task = (dispatch_y + taskCount - 1) / taskCount;
    uniform int start_y = taskIndex * rows_per_task;
    uniform int end_y = min(start_y + rows_per_task, dispatch_y);
    
    // Process full width and depth for better vectorization
    foreach_tiled(z = 0 ... dispatch_z,
                 y = start_y ... end_y,
                 x = 0 ... dispatch_x) {
        uint<3> DTid = { x, y, z };
        DISTFIELD_FLOOD_impl(gFloodInputBuffer, gFloodOutputBuffer, 
                           gFloodSeedBufferIn, gFloodSeedBufferOut, 
                           gFloodParams, RootConstant, DTid);
    }
}

export void DISTFIELD_FLOOD(uniform const float gFloodInputBuffer_arg[],
                          uniform float gFloodOutputBuffer_arg[],
                          uniform int gFloodSeedBufferIn_arg[],
                          uniform int gFloodSeedBufferOut_arg[],
                          uniform const DistanceFieldParams& gFloodParams_arg,
                          uniform const RootConstantData& RootConstant_arg,
                          uniform int dispatch_x, 
                          uniform int dispatch_y, 
                          uniform int dispatch_z) {
    // Launch tasks just in Y dimension for better cache coherency
    launch[NUM_Y_THREADS] 
        DISTFIELD_FLOOD_task(gFloodInputBuffer_arg, 
                            gFloodOutputBuffer_arg,
                            gFloodSeedBufferIn_arg, 
                            gFloodSeedBufferOut_arg,
                            gFloodParams_arg,
                            RootConstant_arg,
                            dispatch_x,
                            dispatch_y,
                            dispatch_z);
    sync;
}