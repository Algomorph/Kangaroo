#pragma once

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <Eigen/Eigen>
#include <Sophus/se3.h>

#include "../cu/Image.h"
#include "../cu/all.h"

class HeightmapFusion
{
public:
    HeightmapFusion(
        double HeightMapWidthMeters, double HeightMapHeightMeters,
        double PixelsPerMeter
    )
        : wm(HeightMapWidthMeters), hm(HeightMapHeightMeters),
          wp(wm*PixelsPerMeter), hp(hm*PixelsPerMeter),
          min_height(0.02f), max_height(2.0f),
          dHeightMap(wp, hp)
    {
        eT_hp << PixelsPerMeter, 0, 0, 0,
                 0, PixelsPerMeter, 0, 0,
                 0, 0, 1, 0,
                 0, 0, 0, 1;
    }

    // Initialise with World to Plane coordinates.
    void Init(Eigen::Matrix4d T_pw)
    {
        eT_hw = eT_hp * T_pw;
        Gpu::InitHeightMap(dHeightMap);
    }

    void Fuse(Gpu::Image<float4> d3d, const Sophus::SE3& T_wc)
    {
        Eigen::Matrix<double,3,4> T_hc = (eT_hw * T_wc.matrix()).block<3,4>(0,0);
        Gpu::UpdateHeightMap(dHeightMap,d3d,Gpu::Image<unsigned char>(),T_hc, min_height, max_height);
    }

    void Fuse(Gpu::Image<float4> d3d, Gpu::Image<unsigned char> dImg, const Sophus::SE3& T_wc)
    {
        Eigen::Matrix<double,3,4> T_hc = (eT_hw * T_wc.matrix()).block<3,4>(0,0);
        Gpu::UpdateHeightMap(dHeightMap,d3d,dImg,T_hc, min_height, max_height);
    }

    void GenerateVbo(pangolin::GlBufferCudaPtr& vbo)
    {
        pangolin::CudaScopedMappedPtr var(vbo);
        Gpu::Image<float4> dVbo((float4*)*var,wp,hp);
        Gpu::VboFromHeightMap(dVbo,dHeightMap);
    }

    void GenerateCbo(pangolin::GlBufferCudaPtr& cbo)
    {
        pangolin::CudaScopedMappedPtr var(cbo);
        Gpu::Image<uchar4> dCbo((uchar4*)*var,wp,hp);
        Gpu::ColourHeightMap(dCbo,dHeightMap);
    }

    template<typename T>
    static T noNans(T in, T nanval = 0)
    {
        return std::isfinite(in) ? in : nanval;
    }

    static float4 noNans(float4 in)
    {
        return make_float4(noNans(in.x),noNans(in.y),noNans(in.z),noNans(in.w));
    }

    void SaveHeightmap(std::string heightfile, std::string imagefile)
    {
        Gpu::Image<float4, Gpu::TargetDevice, Gpu::Manage> dVbo(wp,hp);
        Gpu::Image<unsigned char, Gpu::TargetDevice, Gpu::Manage> dImg(wp,hp);

        Eigen::Matrix<double,3,4> eT_wh = eT_hw.inverse().block<3,4>(0,0);
        Gpu::GenerateWorldVboAndImageFromHeightmap(dVbo, dImg, dHeightMap, eT_wh );

        const int32_t width = wp;
        const int32_t height = hp;
        Gpu::Image<float4, Gpu::TargetHost, Gpu::Manage> hVbo(wp,hp);
        Gpu::Image<unsigned char, Gpu::TargetHost, Gpu::Manage> hImg(wp,hp);
        hImg.CopyFrom(dImg);
        hVbo.CopyFrom(dVbo);

        std::cout << "Saving to file (" << width << "x" << height << ")" << std::endl;

        std::ofstream hof(heightfile, std::ios::binary);
        hof.write((char*)&width,  sizeof(int32_t));
        hof.write((char*)&height, sizeof(int32_t));
        for(unsigned r=0; r < hp; ++r) {
            for(unsigned c=0; c < wp; ++c) {
                const float4 P = noNans(hVbo(r,c));
                hof.write((char*)&P, sizeof(float)*3);
            }
        }
        hof.close();

        std::ofstream iof(imagefile, std::ios::binary);
        iof.write((char*)&width,  sizeof(int32_t));
        iof.write((char*)&height, sizeof(int32_t));
        for(unsigned r=0; r < hp; ++r) {
            for(unsigned c=0; c < wp; ++c) {
                const float p = hImg(r,c);
                const uchar3 pc = make_uchar3(p,p,p);
                iof.write((char*)& pc, sizeof(uchar3));
            }
        }
        iof.close();

        std::cout << "Done" << std::endl;
    }

    Gpu::Image<float4> GetHeightMap()
    {
        return dHeightMap;
    }

    Eigen::Matrix4d T_hw()
    {
        return eT_hw;
    }

    int WidthPixels() { return wp; }
    int HeightPixels() { return hp; }

    double WidthMeters() { return wm; }
    double HeightMeters() { return hm; }

    unsigned long Pixels() { return wp * hp; }

protected:
    // Width / Height in meters
    double wm;
    double hm;

    // Width / Height in pixels
    int wp;
    int hp;

    float min_height;
    float max_height;

    // Plane (z=0) to heightmap transform (adjust to pixel units)
    Eigen::Matrix4d eT_hp;

    // Heightmap to world transform (set once we know the plane)
    Eigen::Matrix4d eT_hw;

    Gpu::Image<float4,Gpu::TargetDevice,Gpu::Manage> dHeightMap;
};
