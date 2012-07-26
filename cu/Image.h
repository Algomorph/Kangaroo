#ifndef CUDAIMAGE_H
#define CUDAIMAGE_H

#include <iostream>
#include <assert.h>
#include <boost/static_assert.hpp>

#include <cuda_runtime.h>

#define HAVE_THRUST
#ifdef HAVE_THRUST
#include <thrust/device_vector.h>
#endif // HAVE_THRUST

#define HAVE_NPP
#ifdef HAVE_NPP
#include <npp.h>
#endif // HAVE_NPP

#include "Mat.h"
#include "sampling.h"

namespace Gpu
{

struct TargetHost
{
    template<typename T> inline static
    void AllocatePitchedMem(T** hostPtr, size_t *pitch, size_t w, size_t h){
        *pitch = w*sizeof(T);
        *hostPtr = (T*)malloc(*pitch * h);
    }

    template<typename T> inline static
    void DeallocatePitchedMem(T* hostPtr){
        free(hostPtr);
    }
};

struct TargetDevice
{
    template<typename T> inline static
    void AllocatePitchedMem(T** devPtr, size_t *pitch, size_t w, size_t h){
        cudaMallocPitch(devPtr,pitch,w*sizeof(T),h);
    }

    template<typename T> inline static
    void DeallocatePitchedMem(T* devPtr){
        cudaFree(devPtr);
    }
};

template<typename TargetTo, typename TargetFrom>
cudaMemcpyKind TargetCopyKind();

template<> inline cudaMemcpyKind TargetCopyKind<TargetHost,TargetHost>() { return cudaMemcpyHostToHost;}
template<> inline cudaMemcpyKind TargetCopyKind<TargetDevice,TargetHost>() { return cudaMemcpyHostToDevice;}
template<> inline cudaMemcpyKind TargetCopyKind<TargetHost,TargetDevice>() { return cudaMemcpyDeviceToHost;}
template<> inline cudaMemcpyKind TargetCopyKind<TargetDevice,TargetDevice>() { return cudaMemcpyDeviceToDevice;}

#ifdef HAVE_THRUST
template<typename T, typename Target> struct ThrustType;
template<typename T> struct ThrustType<T,TargetHost> { typedef T* Ptr; };
template<typename T> struct ThrustType<T,TargetDevice> { typedef thrust::device_ptr<T> Ptr; };
#endif // HAVE_THRUST

struct Manage
{
    inline static __host__
    void AllocateCheck()
    {
    }

    inline static __host__ __device__
    void AssignmentCheck()
    {
        assert(0);
        exit(-1);
    }

    template<typename T, typename Target> inline static __host__
    void Cleanup(T* ptr)
    {
        if(ptr) {
            Target::template DeallocatePitchedMem<T>(ptr);
        }
    }
};

struct DontManage
{
    inline static __host__
    void AllocateCheck()
    {
        std::cerr << "Image that doesn't own data should not call this constructor" << std::endl;
        assert(0);
        exit(-1);
    }

    inline static __host__ __device__
    void AssignmentCheck()
    {
    }


    template<typename T, typename Target> inline static __device__ __host__
    void Cleanup(T* ptr)
    {
    }
};

//! Return v clamped to interval [vmin,vmax]
template<typename T> __host__ __device__
inline T clamp(T vmin, T vmax, T v) {
    return v < vmin ? vmin : (vmax < v ? vmax : v);
}


//! Simple templated pitched image type for use with Cuda
//! Type encapsulates ptr, pitch, width and height
//! Instantiate Image<T,Target,ManagementAllocDealloc> to handle memory allocation
template<typename T, typename Target = TargetDevice, typename Management = DontManage>
struct Image {

    inline __device__ __host__
    ~Image()
    {
        Management::template Cleanup<T,Target>(ptr);
    }

    template<typename ManagementCopyFrom> inline __host__ __device__
    Image( const Image<T,Target,ManagementCopyFrom>& img )
        : ptr(img.ptr), pitch(img.pitch), w(img.w), h(img.h)
    {
        Management::AssignmentCheck();
    }

    inline __host__
    Image()
        :w(0), h(0), pitch(0), ptr(0)
    {
    }

    inline __host__
    Image(unsigned int w, unsigned int h)
        :w(w), h(h)
    {
        Management::AllocateCheck();
        Target::template AllocatePitchedMem<T>(&ptr,&pitch,w,h);
    }

    inline __device__ __host__
    Image(T* ptr)
        :ptr(ptr), pitch(0), w(0), h(0)
    {
    }

    inline __device__ __host__
    Image(T* ptr, size_t w)
        :ptr(ptr), pitch(sizeof(T)*w), w(w), h(0)
    {
    }

    inline __device__ __host__
    Image(T* ptr, size_t w, size_t h)
        :ptr(ptr), pitch(sizeof(T)*w), w(w), h(h)
    {
    }

    inline __device__ __host__
    Image(T* ptr, size_t w, size_t h, size_t pitch)
        :ptr(ptr), pitch(pitch), w(w), h(h)
    {
    }

    template<typename TargetFrom, typename ManagementFrom>
    inline __host__
    void CopyFrom(const Image<T,TargetFrom,ManagementFrom>& img)
    {
        cudaMemcpy2D(ptr,pitch,img.ptr,img.pitch, std::min(img.w,w)*sizeof(T), std::min(img.h,h), TargetCopyKind<Target,TargetFrom>() );
    }

    inline  __device__ __host__
    T* RowPtr(size_t y)
    {
        return (T*)((unsigned char*)(ptr) + y*pitch);
    }

    inline  __device__ __host__
    const T* RowPtr(size_t y) const
    {
        return (T*)((unsigned char*)(ptr) + y*pitch);
    }

    inline  __device__ __host__
    T& operator()(size_t x, size_t y)
    {
        return RowPtr(y)[x];
    }

    inline  __device__ __host__
    const T& operator()(size_t x, size_t y) const
    {
        return RowPtr(y)[x];
    }

    inline  __device__ __host__
    T& operator[](size_t ix)
    {
        return ptr[ix];
    }

    inline  __device__ __host__
    const T& operator[](size_t ix) const
    {
        return ptr[ix];
    }

    inline  __device__ __host__
    const T& Get(int x, int y) const
    {
        return RowPtr(y)[x];
    }

    inline  __device__ __host__
    const T& GetWithClampedRange(int x, int y) const
    {
        x = Gpu::clamp<int>(0,w-1,x);
        y = Gpu::clamp<int>(0,h-1,y);
        return RowPtr(y)[x];
    }

    template<typename TR>
    inline __device__ __host__
    TR GetBilinear(float u, float v) const
    {
        const float ix = floorf(u);
        const float iy = floorf(v);
        const float fx = u - ix;
        const float fy = v - iy;

        const T* bl = RowPtr(iy)  + (size_t)ix;
        const T* tl = RowPtr(iy+1)+ (size_t)ix;

        return lerp(
            lerp( bl[0], bl[1], fx ),
            lerp( tl[0], tl[1], fx ),
            fy
        );
    }

    inline __device__ __host__
    T GetNearestNeighbour(float u, float v) const
    {
        return Get(u+0.5, v+0.5);
    }

    template<typename TR>
    inline __device__ __host__
    TR GetCentralDiffDx(int x, int y) const
    {
        const T* row = RowPtr(y);
        return ((TR)row[x+1] - (TR)row[x-1]) / (TR)2;
    }

    template<typename TR>
    inline __device__ __host__
    TR GetCentralDiffDy(int x, int y) const
    {
        return ((TR)Get(x,y+1) - (TR)Get(x,y-1)) / (TR)2;
    }

    template<typename TR>
    inline __device__ __host__
    Mat<TR,1,2> GetCentralDiff(float px, float py) const
    {
        // TODO: Make more efficient by expanding GetCentralDiff calls
        const int ix = floor(px);
        const int iy = floor(py);
        const float fx = px - ix;
        const float fy = py - iy;

        const int b = py;   const int l = px;
        const int t = py+1; const int r = px+1;

        TR tldx = GetCentralDiffDx<TR>(l,t);
        TR trdx = GetCentralDiffDx<TR>(r,t);
        TR bldx = GetCentralDiffDx<TR>(l,b);
        TR brdx = GetCentralDiffDx<TR>(r,b);
        TR tldy = GetCentralDiffDy<TR>(l,t);
        TR trdy = GetCentralDiffDy<TR>(r,t);
        TR bldy = GetCentralDiffDy<TR>(l,b);
        TR brdy = GetCentralDiffDy<TR>(r,b);

        Mat<TR,1,2> res;
        res(0) = lerp(lerp(bldx,brdx,fx), lerp(tldx,trdx,fx), fy);
        res(1) = lerp(lerp(bldy,brdy,fx), lerp(tldy,trdy,fx), fy);
        return res;
    }

    inline  __device__ __host__
    bool InBounds(int x, int y) const
    {
        return 0 <= x && x < w && 0 <= y && y < h;
    }

    inline  __device__ __host__
    bool InBounds(float x, float y, float border) const
    {
        return border <= x && x < (w-border) && border <= y && y < (h-border);
    }

    template <typename DT>
    inline __host__
    void MemcpyFromHost(DT* hptr, size_t hpitch )
    {
        cudaMemcpy2D( (void*)ptr, pitch, hptr, hpitch, w*sizeof(T), h, cudaMemcpyHostToDevice );
    }

    template <typename DT>
    inline __host__
    void MemcpyFromHost(DT* ptr )
    {
        MemcpyFromHost(ptr, w*sizeof(T) );
    }

    inline __device__ __host__
    const Image<T,Target,DontManage> SubImage(int x, int y, int width, int height) const
    {
        assert( (x+width) <= w && (y+height) <= h);
        return Image<T,Target,DontManage>( RowPtr(y)+x, width, height, pitch);
    }

    inline __device__ __host__
    Image<T,Target,DontManage> SubImage(int x, int y, int width, int height)
    {
        assert( (x+width) <= w && (y+height) <= h);
        return Image<T,Target,DontManage>( RowPtr(y)+x, width, height, pitch);
    }

    inline __device__ __host__
    Image<T,Target,DontManage> Row(int y) const
    {
        return SubImage(0,y,w,1);
    }

    inline __device__ __host__
    Image<T,Target,DontManage> Col(int x) const
    {
        return SubImage(x,0,1,h);
    }

    inline __device__ __host__
    Image<T,Target,DontManage> SubImage(int width, int height)
    {
        assert(width <= w && height <= h);
        return Image<T,Target,DontManage>(ptr, width, height, pitch);
    }

    //! Ignore this images pitch - just return new image of
    //! size w x h which uses this memory
    template<typename TP>
    inline __device__ __host__
    Image<TP,Target,DontManage> PackedImage(int width, int height)
    {
        assert(width*height*sizeof(TP) <= h*pitch );
        return Image<TP,Target,DontManage>((TP*)ptr, width, height, width*sizeof(TP) );
    }

#ifdef HAVE_NPP
    inline __device__ __host__
    Image<T,Target,DontManage> SubImage(const NppiRect& region)
    {
        return Image<T,Target,DontManage>(RowPtr(region.y)+region.x, region.width, region.height, pitch);
    }

    inline __device__ __host__
    Image<T,Target,DontManage> SubImage(const NppiSize& size)
    {
        return Image<T,Target,DontManage>(ptr, size.width,size.height, pitch);
    }

    inline __host__
    const NppiSize Size() const
    {
        NppiSize ret = {(int)w,(int)h};
        return ret;
    }

    inline __host__
    const NppiRect Rect() const
    {
        NppiRect ret = {0,0,w,h};
        return ret;
    }
#endif

#ifdef HAVE_THRUST
    inline __device__ __host__
    typename Gpu::ThrustType<T,Target>::Ptr begin() {
        return (typename Gpu::ThrustType<T,Target>::Ptr)(ptr);
    }

    inline __device__ __host__
    typename Gpu::ThrustType<T,Target>::Ptr end() {
        return (typename Gpu::ThrustType<T,Target>::Ptr)( RowPtr(h-1) + w );
    }

    inline __host__
    void Fill(T val) {
        thrust::fill(begin(), end(), val);
    }

#endif

    T* ptr;
    size_t pitch;
    size_t w;
    size_t h;
};

}

#endif // CUDAIMAGE_H
