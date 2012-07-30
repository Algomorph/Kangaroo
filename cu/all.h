#pragma once

#include <cuda_runtime.h>

#include "Image.h"
#include "Volume.h"
#include "Mat.h"
#include "MatUtils.h"

namespace Gpu
{

template<typename To, typename Ti>
void ConvertImage(Image<To> dOut, const Image<Ti> dIn);

template<typename To, typename UpType, typename Ti>
void BoxHalf( Image<To> out, const Image<Ti> in);

template<typename To, typename UpType, typename Ti>
inline void BoxReduce( Image<To> out, Image<Ti> in_temp, Image<To> temp, int level)
{
    const int w = in_temp.w;
    const int h = in_temp.h;

    // in_temp has size (w,h)
    // out has size (w>>l,h>>l)
    // temp has at least size (w/2,h/2)

    Image<Ti>* t[] = {&in_temp, &temp};

    for(int l=0; l < (level-1); ++l ) {
        BoxHalf<To,UpType,Ti>(
            t[(l+1) % 2]->SubImage(w >> (l+1), h >> (l+1) ),
            t[l % 2]->SubImage(w >> l, h >> l)
        );
    }

    BoxHalf<To,UpType,Ti>(out, t[(level+1)%2]->SubImage(w >> (level-1), h >> (level-1) ) );
}

//////////////////////////////////////////////////////

void CreateMatlabLookupTable(Image<float2> lookup,
    float fu, float fv, float u0, float v0, float k1, float k2
);

void CreateMatlabLookupTable(Image<float2> lookup,
    float fu, float fv, float u0, float v0, float k1, float k2, Mat<float,9> H_no
);

//////////////////////////////////////////////////////

void Warp(
    Image<unsigned char> out, const Image<unsigned char> in, const Image<float2> lookup
);

//////////////////////////////////////////////////////

void DenseStereo(
    Image<unsigned char> dDisp, const Image<unsigned char> dCamLeft, const Image<unsigned char> dCamRight, int maxDisp, double acceptThresh
);

//////////////////////////////////////////////////////

void DenseStereoSubpixelRefine(Image<float> dDispOut, const Image<unsigned char> dDisp, const Image<unsigned char> dCamLeft, const Image<unsigned char> dCamRight
);

//////////////////////////////////////////////////////

void DisparityImageCrossSection(
    Image<float4> dScore, Image<unsigned char> dDisp, const Image<unsigned char> dCamLeft, const Image<unsigned char> dCamRight, int y
);

//////////////////////////////////////////////////////

void KinectToVbo( Image<float4> dVbo, const Image<unsigned short> dKinectDepth, double fu, double fv, double u0, double v0);
void KinectToVbo( Image<float4> dVbo, const Image<float> dKinectDepth, double fu, double fv, double u0, double v0);

void DisparityImageToVbo(
    Image<float4> dVbo, const Image<float> dDisp, float baseline, float fu, float fv, float u0, float v0
);

void ColourVbo(Image<uchar4> dId, const Image<float4> dPd, const Image<uchar3> dIc, const Mat<float,3,4> KT_cd );

void NormalsFromVbo(Image<float4> dN, const Image<float4> dV);

//////////////////////////////////////////////////////

void GenerateTriangleStripIndexBuffer( Image<uint2> dIbo);

//////////////////////////////////////////////////////

LeastSquaresSystem<float,6> PoseRefinementFromDepthmap(
    const Image<unsigned char> dImgl,
    const Image<unsigned char> dImgr, const Image<float4> dPr,
    const Mat<float,3,4> KT_lr, float c,
    Image<unsigned char> dWorkspace, Image<float4> dDebug
);

LeastSquaresSystem<float,6> PoseRefinementProjectiveIcpPointPlane(
    const Image<float4> dPl,
    const Image<float4> dPr, const Image<float4> dNr,
    const Mat<float,3,4> KT_lr, const Mat<float,3,4> T_rl, float c,
    Image<unsigned char> dWorkspace, Image<float4> dDebug
);

LeastSquaresSystem<float,2*6> KinectCalibration(
    const Image<float4> dPl, const Image<uchar3> dIl,
    const Image<float4> dPr, const Image<uchar3> dIr,
    const Mat<float,3,4> KcT_cd, const Mat<float,3,4> T_lr,
    float c, Image<unsigned char> dWorkspace, Image<float4> dDebug
);

//////////////////////////////////////////////////////

LeastSquaresSystem<float,3> PlaneFitGN(const Image<float4> dVbo, Mat<float,3,3> Qinv, Mat<float,3> zhat, Image<unsigned char> dWorkspace, Image<float> dErr, float within, float c );

//////////////////////////////////////////////////////

void BilateralFilter(
    Image<float> dOut, const Image<float> dIn, float gs, float gr, uint size
);

void BilateralFilter(
    Image<float> dOut, const Image<unsigned char> dIn, float gs, float gr, uint size
);

void BilateralFilter(
    Image<float> dOut, const Image<unsigned short> dIn, float gs, float gr, uint size
);

void RobustBilateralFilter(
    Image<float> dOut, const Image<unsigned char> dIn, float gs, float gr, float go, uint size
);

//////////////////////////////////////////////////////

void MedianFilter3x3(
    Image<float> dOut, Image<float> dIn
);

void MedianFilter5x5(
    Image<float> dOut, Image<float> dIn
);

//////////////////////////////////////////////////////

void MakeAnaglyth(
    Image<uchar4> anaglyth,
    const Image<unsigned char> left, const Image<unsigned char> right,
    int shift = 0
);

//////////////////////////////////////////////////////

void VboFromHeightMap(Image<float4> dVbo, const Image<float4> dHeightMap);

void InitHeightMap(Image<float4> dHeightMap);

void UpdateHeightMap(Image<float4> dHeightMap, const Image<float4> d3d, const Image<unsigned char> dImage, const Mat<float,3,4> T_hc);

void ColourHeightMap(Image<uchar4> dCbo, const Image<float4> dHeightMap);

//////////////////////////////////////////////////////

struct __align__(8) CostVolElem
{
    int n;
    float sum;
};

void InitCostVolume(Volume<CostVolElem> costvol );

void InitCostVolume(Volume<CostVolElem> dvol, Image<unsigned char> dimgl, Image<unsigned char> dimgr );

void AddToCostVolume(Volume<CostVolElem> vol, const Image<unsigned char> imgv,
    const Image<unsigned char> imgc, Mat<float,3,4> KT_cv,
    float fu, float fv, float u0, float v0,
    float minz, float maxz, int levels
);

void CostVolumeCrossSection(
    Image<float4> dScore, Volume<CostVolElem> dCostVol, int y
);

}
