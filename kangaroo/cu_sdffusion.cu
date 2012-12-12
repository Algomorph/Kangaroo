#include "MatUtils.h"
#include "Image.h"
#include "Sdf.h"
#include "BoundedVolume.h"
#include "launch_utils.h"
#include "ImageKeyframe.h"

namespace Gpu
{

//////////////////////////////////////////////////////
// Truncated SDF Fusion
//////////////////////////////////////////////////////

__global__ void KernSdfFuse(BoundedVolume<SDF_t> vol, Image<float> depth, Image<float4> normals, Mat<float,3,4> T_cw, ImageIntrinsics K, float trunc_dist, float max_w, float mincostheta )
{
    const int x = blockIdx.x*blockDim.x + threadIdx.x;
    const int y = blockIdx.y*blockDim.y + threadIdx.y;
    const int z = blockIdx.z*blockDim.z + threadIdx.z;

    const float3 P_w = vol.VoxelPositionInUnits(x,y,z);
    const float3 P_c = T_cw * P_w;
    const float2 p_c = K.Project(P_c);

    if( depth.InBounds(p_c, 2) )
    {
        const float vd = P_c.z;
//        const float md = depth.GetNearestNeighbour(p_c);
//        const float3 mdn = make_float3(normals.GetNearestNeighbour(p_c));

        const float md = depth.GetBilinear<float>(p_c);
        const float3 mdn = make_float3(normals.GetBilinear<float4>(p_c));

        const float costheta = dot(mdn, P_c) / -length(P_c);
        const float sd = costheta * (md - vd);
        const float w = costheta * 1.0f/vd;

        if(sd <= -trunc_dist) {
            // Further than truncation distance from surface
            // We do nothing.
        }else{
//        }else if(sd < 5*trunc_dist) {
            if(isfinite(md) && isfinite(w) && costheta > mincostheta ) {
                SDF_t sdf( clamp(sd,-trunc_dist,trunc_dist) , w);
                sdf += vol(x,y,z);
//                sdf.Clamp(-trunc_dist, trunc_dist);
                sdf.LimitWeight(max_w);
                vol(x,y,z) = sdf;
            }
        }
    }
 }

using namespace std;
void SdfFuse(BoundedVolume<SDF_t> vol, Image<float> depth, Image<float4> norm, Mat<float,3,4> T_cw, ImageIntrinsics K, float trunc_dist, float max_w, float mincostheta )
{
    dim3 blockDim(8,8,8);
    dim3 gridDim(vol.w / blockDim.x, vol.h / blockDim.y, vol.d / blockDim.z);
    KernSdfFuse<<<gridDim,blockDim>>>(vol, depth, norm, T_cw, K, trunc_dist, max_w, mincostheta);
    GpuCheckErrors();
}

//////////////////////////////////////////////////////
// Reset SDF
//////////////////////////////////////////////////////

__global__ void KernSdfReset(BoundedVolume<SDF_t> vol, float trunc_dist)
{
    const int x = blockIdx.x*blockDim.x + threadIdx.x;
    const int y = blockIdx.y*blockDim.y + threadIdx.y;
    const int z = blockIdx.z*blockDim.z + threadIdx.z;

    vol(x,y,z) = SDF_t(0.0/0.0, 0);
}

void SdfReset(BoundedVolume<SDF_t> vol, float trunc_dist)
{
    dim3 blockDim(8,8,8);
    dim3 gridDim(vol.w / blockDim.x, vol.h / blockDim.y, vol.d / blockDim.z);
    KernSdfReset<<<gridDim,blockDim>>>(vol, trunc_dist);
    GpuCheckErrors();
}

//////////////////////////////////////////////////////
// Create SDF representation of sphere
//////////////////////////////////////////////////////

__global__ void KernSdfSphere(BoundedVolume<SDF_t> vol, float3 center, float r)
{
    const int x = blockIdx.x*blockDim.x + threadIdx.x;
    const int y = blockIdx.y*blockDim.y + threadIdx.y;
    const int z = blockIdx.z*blockDim.z + threadIdx.z;

    const float3 pos = vol.VoxelPositionInUnits(x,y,z);
    const float dist = length(pos - center);
    const float sdf = dist - r;

    vol(x,y,z) = SDF_t(sdf);
}

void SdfSphere(BoundedVolume<SDF_t> vol, float3 center, float r)
{
    dim3 blockDim(8,8,8);
    dim3 gridDim(vol.w / blockDim.x, vol.h / blockDim.y, vol.d / blockDim.z);

    KernSdfSphere<<<gridDim,blockDim>>>(vol, center, r);
    GpuCheckErrors();
}


}
