#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <npp.h>

#include <fiducials/drawing.h>

#include "RpgCameraOpen.h"
#include "ScanlineRectify.h"
#include "CudaImage.h"
#include "kernel.h"

#include <Mvlpp/Mvl.h>
#include <Mvlpp/Cameras.h>

using namespace std;
using namespace pangolin;
using namespace Gpu;

template<typename T, typename Owner>
inline void operator<<(pangolin::GlTextureCudaArray& tex, const Image<T,TargetDevice,Owner>& dImg)
{
    CudaScopedMappedArray arr_tex(tex);
    cudaError_t err = cudaMemcpy2DToArray(*arr_tex, 0, 0, dImg.ptr, dImg.pitch, dImg.w*sizeof(T), dImg.h, cudaMemcpyDeviceToDevice );
    if( err != cudaSuccess ) {
        std::cerr << "cudaMemcpy2DToArray failed: " << err << std::endl;
    }
}

void RenderMesh(GlBufferCudaPtr& ibo, GlBufferCudaPtr& vbo, GlBufferCudaPtr& cbo, int w, int h, bool draw_mesh = true, bool draw_color = true)
{
    if(draw_color) {
        cbo.Bind();
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, 0);
        glEnableClientState(GL_COLOR_ARRAY);
    }

    vbo.Bind();
    glVertexPointer(4, GL_FLOAT, 0, 0);
    glEnableClientState(GL_VERTEX_ARRAY);

    if(draw_mesh) {
        ibo.Bind();
        for( int r=0; r<h-1; ++r) {
            glDrawElements(GL_TRIANGLE_STRIP,2*w, GL_UNSIGNED_INT, (uint*)0 + 2*w*r);
        }
        ibo.Unbind();
    }else{
        glDrawArrays(GL_POINTS, 0, w * h);
    }

    if(draw_color) {
        glDisableClientState(GL_COLOR_ARRAY);
        cbo.Unbind();
    }

    glDisableClientState(GL_VERTEX_ARRAY);
    vbo.Unbind();
}

int main( int /*argc*/, char* argv[] )
{
    // Open video device
//    const std::string cam_uri =
    CameraDevice camera = OpenRpgCamera(
//        "AlliedVision:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/AlliedVisionCam,CamUUID0=5004955,CamUUID1=5004954,ImageBinningX=2,ImageBinningY=2,ImageWidth=694,ImageHeight=518]//"
//        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/CityBlock-Noisy,Channel-0=left.*pgm,Channel-1=right.*pgm,StartFrame=0]//"
        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/xb3,Channel-0=left.*pgm,Channel-1=right.*pgm,StartFrame=0]//"
//        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/20120515/20090822_212628/rect_images,Channel-0=.*left.pnm,Channel-1=.*right.pnm,StartFrame=500]//"
//        "Dvi2Pci:[NumChannels=2,ImageWidth=640,ImageHeight=480,BufferCount=60]//"
    );

//    CameraDevice camera = OpenPangoCamera(
//        "file:[stream=0,fmt=GRAY8]///Users/slovegrove/data/3DCam/DSCF0051.AVI",
//        "file:[stream=1,fmt=GRAY8]///Users/slovegrove/data/3DCam/DSCF0051.AVI"
//    );

    mvl::CameraModel camModel[] = {
        camera.GetProperty("DataSourceDir") + "/lcmod.xml",
        camera.GetProperty("DataSourceDir") + "/rcmod.xml"
    };

    Eigen::Matrix3d RDFgl;
    RDFgl << 1,0,0,  0,-1,0,  0,0,-1;

    const Eigen::Matrix3d K = camModel[0].K();
    const Eigen::Matrix3d Kinv = MakeKinv(K);
    const Sophus::SE3 T_rl_orig = T_rlFromCamModelRDF(camModel[0], camModel[1], RDFgl);
    double k1 = 0;
    double k2 = 0;

    if(camModel[0].Type() == MVL_CAMERA_WARPED)
    {
        k1 = camModel[0].GetModel()->warped.kappa1;
        k2 = camModel[0].GetModel()->warped.kappa2;
    }

    const bool rectify = (k1!=0 || k2!=0); // || camModel[0].GetPose().block<3,3>(0,0)
    if(!rectify) {
        cout << "Using pre-rectified images" << endl;
    }

    // Capture first image
    std::vector<rpg::ImageWrapper> img;
    camera.Capture(img);

    // Check we received one or more images
    if(img.empty()) {
        std::cerr << "Failed to capture first image from camera" << std::endl;
        return -1;
    }

    // N cameras, each w*h in dimension, greyscale
    const unsigned int w = img[0].width();
    const unsigned int h = img[0].height();

    // Setup OpenGL Display (based on GLUT)
    pangolin::CreateGlutWindowAndBind(__FILE__,2*w,2*h);
    glewInit();

    // Initialise CUDA, allowing it to use OpenGL context
    cudaGLSetGLDevice(0);

    // Setup default OpenGL parameters
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable (GL_BLEND);
    glEnable (GL_LINE_SMOOTH);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);

    // Tell the base view to arrange its children equally
    const int UI_WIDTH = 180;
    pangolin::CreatePanel("ui")
        .SetBounds(0.0, 1.0, 0.0, Attach::Pix(UI_WIDTH));

    View& container = CreateDisplay()
            .SetBounds(0,1.0, Attach::Pix(UI_WIDTH), 1.0)
            .SetLayout(LayoutEqual);

    const int N = 4;
    for(int i=0; i<N; ++i ) {
        View& disp = CreateDisplay().SetAspect((double)w/h);
        container.AddDisplay(disp);
    }

    // Define Camera Render Object (for view / scene browsing)
    pangolin::OpenGlRenderState s_cam;
    s_cam.Set(ProjectionMatrix(640,480,420,420,320,240,0.1,1000));
    s_cam.Set(IdentityMatrix(GlModelViewStack));
    container[3].SetHandler(new Handler3D(s_cam));

    // Texture we will use to display camera images
    GlTextureCudaArray tex8(w,h,GL_LUMINANCE8);
//    GlTextureCudaArray texrgba8(w,h,GL_RGBA8);
    GlTextureCudaArray texf(w,h,GL_LUMINANCE32F_ARB);

    GlBufferCudaPtr vbo(GlArrayBuffer, w*h*sizeof(float4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo(GlArrayBuffer, w*h*sizeof(uchar4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr ibo(GlElementArrayBuffer, w*h*sizeof(uint2) );

    // Generate Index Buffer Object for rendering mesh
    {
        CudaScopedMappedPtr var(ibo);
        Gpu::Image<uint2> dIbo((uint2*)*var,w,h);
        GenerateTriangleStripIndexBuffer(dIbo);
    }

    // Allocate Camera Images on device for processing
    Image<unsigned char, TargetDevice, Manage> dCamImgDist[] = {{w,h},{w,h}};
    Image<unsigned char, TargetDevice, Manage> dCamImg[] = {{w,h},{w,h}};
    Image<uchar4, TargetDevice, Manage> dCamColor(w,h);
    Image<float2, TargetDevice, Manage> dLookup[] = {{w,h},{w,h}};
    Image<uchar4, TargetDevice, Manage> d3d(w,h);
    Image<unsigned char, TargetDevice, Manage> dDispInt(w,h);
    Image<float, TargetDevice, Manage>  dDisp(w,h);
//    Image<float4, TargetDevice, Manage>  dVbo(w,h);
    Image<float, TargetDevice, Manage>  dDispFilt(w,h);

    // Camera Parameters
//    Eigen::VectorXd camParamsVec(6);
//    camParamsVec << 0.558526, 0.747774, 0.484397, 0.494393, -0.249261, 0.0825967;

    // Stereo transformation (post-rectification)
    Sophus::SE3 T_rl = T_rl_orig;

    // Build camera distortion lookup tables
    if(rectify)
    {
//        // Actual Original Stereo configuration
//        Eigen::Matrix3d mR_rl_orig;
//        mR_rl_orig << 0.999995,   0.00188482,  -0.00251896,
//                -0.0018812,     0.999997,   0.00144025,
//                0.00252166,  -0.00143551,     0.999996;

//        Eigen::Vector3d l_r_orig;
//        l_r_orig <<    -0.203528, -0.000750334, 0.00403201;

//        const Sophus::SO3 R_rl_orig = Sophus::SO3(mR_rl_orig);
//        const Sophus::SE3 T_rl_orig = Sophus::SE3(R_rl_orig, l_r_orig);

        T_rl = CreateScanlineRectifiedLookupAndT_rl(
                    dLookup[0], dLookup[1], T_rl_orig,
                    K, k1, k2, w, h
                    );
    }

    const double baseline = T_rl.translation().norm();

    Var<bool> run("ui.run", true, true);
    Var<int> maxDisp("ui.disp",55, 0, 64);
    Var<bool> subpix("ui.subpix", true, true);
    Var<bool> show_mesh("ui.show mesh", true, true);
    Var<bool> show_color("ui.show color", true, true);

    Var<bool> applyBilateralFilter("ui.Apply Bilateral Filter", false, true);
    Var<int> bilateralWinSize("ui.size",5, 1, 20);
    Var<float> gs("ui.gs",2, 1E-3, 5);
    Var<float> gr("ui.gr",0.0184, 1E-3, 1);

    for(unsigned long frame=0; !pangolin::ShouldQuit(); ++frame)
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);

        if(run) {
            camera.Capture(img);

            /////////////////////////////////////////////////////////////
            // Upload images to device
            for(int i=0; i<2; ++i ) {
                if(rectify) {
                    dCamImgDist[i].MemcpyFromHost(img[i].Image.data );
                    Warp(dCamImg[i],dCamImgDist[i],dLookup[i]);
                }else{
                    dCamImg[i].MemcpyFromHost(img[i].Image.data );
                }
            }
        }

        if(run || GuiVarHasChanged() ) {
            ConvertImage<uchar4,unsigned char>(dCamColor, dCamImg[0]);
            DenseStereo(dDispInt, dCamImg[0], dCamImg[1], maxDisp);

            if(subpix) {
                DenseStereoSubpixelRefine(dDisp, dDispInt, dCamImg[0], dCamImg[1]);
            }else{
                ConvertImage<float, unsigned char>(dDisp, dDispInt);
            }

            if(applyBilateralFilter) {
                BilateralFilter(dDispFilt,dDisp,gs,gr,bilateralWinSize);
                dDisp.CopyFrom(dDispFilt);
            }

            // Generate VBO
            {
                CudaScopedMappedPtr var(vbo);
                Gpu::Image<float4> dVbo((float4*)*var,w,h);
                DisparityImageToVbo(dVbo, dDisp, baseline, K(0,0), K(1,1), K(0,2), K(1,2) );
            }

            // Generate CBO
            {
                CudaScopedMappedPtr var(cbo);
                cudaMemcpy2D(*var, w*sizeof(uchar4), dCamColor.ptr, dCamColor.pitch, w*sizeof(uchar4), h, cudaMemcpyDeviceToDevice);
            }

            // normalise dDisp
            nppiDivC_32f_C1IR(maxDisp,dDisp.ptr,dDisp.pitch,dDisp.Size());
        }

        /////////////////////////////////////////////////////////////
        // Perform drawing
        // Draw Stereo images
        for(int i=0; i<2; ++i ) {
            container[i].Activate();
            tex8 << dCamImg[i];
            tex8.RenderToViewportFlipY();
        }

        container[2].Activate();
        texf << dDisp;
        texf.RenderToViewportFlipY();

        container[3].ActivateAndScissor(s_cam);
        glEnable(GL_DEPTH_TEST);

        glDrawAxis(1.0);
        glDrawFrustrum(Kinv,w,h,-1.0);

        // Render Mesh
        glColor3f(1.0,1.0,1.0);
        RenderMesh(ibo,vbo,cbo, w, h, show_mesh, show_color);

        pangolin::RenderViews();
        pangolin::FinishGlutFrame();
    }
}
