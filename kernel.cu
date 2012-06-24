#include "kernel.h"

#include "CUDA_SDK/cutil_math.h"
#include <boost/math/common_factor.hpp>

using namespace std;
using namespace boost::math;

//////////////////////////////////////////////////////
// Additions to cutil_math.h
//////////////////////////////////////////////////////

inline __host__ __device__ float3 operator*(float b, uchar3 a)
{
    return make_float3(b * a.x, b * a.y, b * a.z);
}

inline __host__ __device__ float3 operator*(uchar3 a, float b)
{
    return make_float3(b * a.x, b * a.y, b * a.z);
}

//////////////////////////////////////////////////////
// Sampling
//////////////////////////////////////////////////////

// w0, w1, w2, and w3 are the four cubic B-spline basis functions
__host__ __device__
float w0(float a)
{
//    return (1.0f/6.0f)*(-a*a*a + 3.0f*a*a - 3.0f*a + 1.0f);
    return (1.0f/6.0f)*(a*(a*(-a + 3.0f) - 3.0f) + 1.0f);   // optimized
}

__host__ __device__
float w1(float a)
{
//    return (1.0f/6.0f)*(3.0f*a*a*a - 6.0f*a*a + 4.0f);
    return (1.0f/6.0f)*(a*a*(3.0f*a - 6.0f) + 4.0f);
}

__host__ __device__
float w2(float a)
{
//    return (1.0f/6.0f)*(-3.0f*a*a*a + 3.0f*a*a + 3.0f*a + 1.0f);
    return (1.0f/6.0f)*(a*(a*(-3.0f*a + 3.0f) + 3.0f) + 1.0f);
}

__host__ __device__
float w3(float a)
{
    return (1.0f/6.0f)*(a*a*a);
}

// g0 and g1 are the two amplitude functions
__device__ float g0(float a)
{
    return w0(a) + w1(a);
}

__device__ float g1(float a)
{
    return w2(a) + w3(a);
}

// h0 and h1 are the two offset functions
__device__ float h0(float a)
{
    // note +0.5 offset to compensate for CUDA linear filtering convention
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

__device__ float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

// filter 4 values using cubic splines
template<typename R, typename T>
__device__
R cubicFilter(float x, T c0, T c1, T c2, T c3)
{
    R r;
    r = c0 * w0(x);
    r += c1 * w1(x);
    r += c2 * w2(x);
    r += c3 * w3(x);
    return r;
}

// Catmull-Rom interpolation

__host__ __device__
float catrom_w0(float a)
{
    //return -0.5f*a + a*a - 0.5f*a*a*a;
    return a*(-0.5f + a*(1.0f - 0.5f*a));
}

__host__ __device__
float catrom_w1(float a)
{
    //return 1.0f - 2.5f*a*a + 1.5f*a*a*a;
    return 1.0f + a*a*(-2.5f + 1.5f*a);
}

__host__ __device__
float catrom_w2(float a)
{
    //return 0.5f*a + 2.0f*a*a - 1.5f*a*a*a;
    return a*(0.5f + a*(2.0f - 1.5f*a));
}

__host__ __device__
float catrom_w3(float a)
{
    //return -0.5f*a*a + 0.5f*a*a*a;
    return a*a*(-0.5f + 0.5f*a);
}

template<typename R, typename T>
__device__
R catRomFilter(float x, T c0, T c1, T c2, T c3)
{
    R r;
    r = c0 * catrom_w0(x);
    r += c1 * catrom_w1(x);
    r += c2 * catrom_w2(x);
    r += c3 * catrom_w3(x);
    return r;
}

template<typename R, typename T>
__device__ R nearestneighbour(const T* img, int stride, float x, float y)
{
  const int xi = floor(x);
  const int yi = floor(y);
  return img[xi + stride*yi];
}

template<typename R, typename T>
__device__ R bilinear(const T* img, int stride, float x, float y)
{
  const float px = x - 0.5f;
  const float py = y - 0.5f;

//  if( 0.0 <= px && px < w-1.0 && 0.0 <= py && py < h-1.0 ) {
    const float ix = floorf(px);
    const float iy = floorf(py);
    const float fx = px - ix;
    const float fy = py - iy;
    const int idx = (int)ix + (int)iy*stride;

    return lerp(
      lerp( img[idx], img[idx+1], fx ),
      lerp( img[idx+stride], img[idx+stride+1], fx ),
      fy
    );
//  }else{
//    return nearestneighbour(img,stride,w,h,x,y);
//  }
}

template<typename R, typename T>
__device__ R bicubic(const T* img, int stride, float x, float y)
{
  const float px = x-0.5f;
  const float py = y-0.5f;

//  if( 1.0 <= px && px < w-2.0 && 1.0 <= py && py < h-2.0 ) {
    const int ix = floor(px);
    const int iy = floor(py);
    const float fx = px - ix;
    const float fy = py - iy;
    const int idx = ((int)ix) + ((int)iy)*stride;

    return cubicFilter<R,R>(
          fy,
          cubicFilter<R,T>(fx, img[idx-stride-1], img[idx-stride], img[idx-stride+1], img[idx-stride+2]),
          cubicFilter<R,T>(fx, img[idx-1], img[idx], img[idx+1], img[idx+2]),
          cubicFilter<R,T>(fx, img[idx+stride-1], img[idx+stride], img[idx+stride+1], img[idx+stride+2]),
          cubicFilter<R,T>(fx, img[idx+2*stride-1], img[idx+2*stride], img[idx+2*stride+1], img[idx+2*stride+2])
    );
//  }else{
//    return nearestneighbour(img,stride,w,h,x,y);
//  }
}

template<typename R, typename T>
__device__ R catrom(const T* img, uint stride, float x, float y)
{
  const float px = x-0.5f;
  const float py = y-0.5f;

//  if( 1.0 <= px && px < w-2.0 && 1.0 <= py && py < h-2.0 ) {
    const int ix = floor(px);
    const int iy = floor(py);
    const float fx = px - ix;
    const float fy = py - iy;
    const uint idx = ((uint)ix) + ((uint)iy)*stride;
    const uint stride2 = 2 *stride;

    return catRomFilter<R,R>(
          fy,
          catRomFilter<R,T>(fx, img[idx-stride-1], img[idx-stride], img[idx-stride+1], img[idx-stride+2]),
          catRomFilter<R,T>(fx, img[idx-1], img[idx], img[idx+1], img[idx+2]),
          catRomFilter<R,T>(fx, img[idx+stride-1], img[idx+stride], img[idx+stride+1], img[idx+stride+2]),
          catRomFilter<R,T>(fx, img[idx+stride2-1], img[idx+stride2], img[idx+stride2+1], img[idx+stride2+2])
    );
//  }else{
//    return nearestneighbour<R,T>(img,stride,x,y);
//  }
}

__global__ void  resample_kernal(
    float4* out, int ostride, int ow, int oh,
    float4* in,  int istride, int iw, int ih,
    int resample_type
) {
    const int x = blockIdx.x*blockDim.x + threadIdx.x;
    const int y = blockIdx.y*blockDim.y + threadIdx.y;
    const int index = y*ostride + x;

    const float xf = ((x+0.5) / (float)ow) * (float)iw;
    const float yf = ((y+0.5) / (float)oh) * (float)ih;

    if( 1.5 <= xf && xf < iw-2.5 && 1.5 <= yf && yf < ih-2.5 ) {
      if( resample_type == 1 ) {
        out[index] = bilinear<float4,float4>(in,istride,xf,yf);
      }else if( resample_type == 2 ) {
        out[index] = bicubic<float4,float4>(in,istride,xf,yf);
      }else if( resample_type == 3 ) {
        out[index] = catrom<float4,float4>(in,istride,xf,yf);
      }else{
        out[index] = nearestneighbour<float4,float4>(in,istride,xf,yf);
      }
    }
}


void resample(
    float4* out, int ostride, int ow, int oh,
    float4* in,  int istride, int iw, int ih,
    int resample_type
) {
  dim3 blockdim(boost::math::gcd<unsigned>(ow,16), boost::math::gcd<unsigned>(oh,16), 1);
  dim3 griddim( ow / blockdim.x, oh / blockdim.y);
  resample_kernal<<<griddim,blockdim>>>(out,ostride,ow,oh,in,istride,iw,ih, resample_type);
}

//////////////////////////////////////////////////////
// Image warping
//////////////////////////////////////////////////////

namespace Gpu
{

//! Utility for attempting to estimate safe block/grid dimensions from working image dimensions
//! These are not necesserily optimal. Far from it.
template<typename T>
inline void InitDimFromOutputImage(dim3& blockDim, dim3& gridDim, const Image<T>& image, int blockx = 16, int blocky = 16)
{
    blockDim = dim3(boost::math::gcd<unsigned>(image.w,blockx), boost::math::gcd<unsigned>(image.h,blocky), 1);
    gridDim =  dim3( image.w / blockDim.x, image.h / blockDim.y, 1);
}

__global__ void KernCreateMatlabLookupTable(
    Image<LookupWeights> lookup, float fu, float fv, float u0, float v0, float k1, float k2
) {

}

void CreateMatlabLookupTable(
    Image<LookupWeights> lookup, float fu, float fv, float u0, float v0, float k1, float k2
) {
    dim3 blockDim, gridDim;
    InitDimFromOutputImage(blockDim,gridDim, lookup);
    KernCreateMatlabLookupTable<<<gridDim,blockDim>>>(lookup,fu,fv,u0,v0,k1,k2);
}

__global__ void KernMakeAnaglyth(Image<uchar4> anaglyth, const Image<uchar1> left, const Image<uchar1> right)
{
    const int x = blockIdx.x*blockDim.x + threadIdx.x;
    const int y = blockIdx.y*blockDim.y + threadIdx.y;

    const unsigned char leftI  = left(x,y).x;
    const unsigned char rightI = right(x,y).x;

    anaglyth(x,y) = make_uchar4(leftI, 0, rightI,255);
}

void MakeAnaglyth(Image<uchar4> anaglyth, const Image<uchar1> left, const Image<uchar1> right)
{
    dim3 blockDim, gridDim;
    InitDimFromOutputImage(blockDim,gridDim, anaglyth);
    KernMakeAnaglyth<<<gridDim,blockDim>>>(anaglyth, left, right);
}

}
