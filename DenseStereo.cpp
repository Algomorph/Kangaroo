#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <Pangolin/glsl.h>
#include <npp.h>

#include <SceneGraph/SceneGraph.h>

#include <fiducials/drawing.h>
#include <fiducials/camera.h>

#include "common/RpgCameraOpen.h"
#include "common/DisplayUtils.h"
#include "common/ScanlineRectify.h"
#include "common/ImageSelect.h"
#include "common/BaseDisplay.h"

#include "cu/all.h"

#include <Mvlpp/Mvl.h>
#include <Mvlpp/Cameras.h>

#include <Node.h>

using namespace std;
using namespace pangolin;
using namespace Gpu;

inline NppiRect GetTopLeftAlignedRegion(int w, int h, int blockx, int blocky)
{
    NppiRect ret;
    ret.width = blockx * (w / blockx);
    ret.height = blocky * (h / blocky);
    ret.x = 0;
    ret.y = 0;
    return ret;
}

inline NppiRect GetCenteredAlignedRegion(int w, int h, int blockx, int blocky)
{
    NppiRect ret;
    ret.width = blockx * (w / blockx);
    ret.height = blocky * (h / blocky);
    ret.x = (w - ret.width) / 2;
    ret.y = (h - ret.height) / 2;
    return ret;
}

inline int GetLevelFromMaxPixels(int w, int h, unsigned long maxpixels)
{
    int level = 0;
    while( (w >> level)*(h >> level) > maxpixels ) {
        ++level;
    }
    return level;
}

inline void LoadPoses(
    vector<Sophus::SE3>& vecT_wh, const std::string& filename, int startframe,
    const Eigen::Matrix4d T_hf, const Eigen::Matrix4d T_fh
) {
    // Parse Ground truth
    ifstream ifs(filename);
    if(ifs.is_open()) {
        Eigen::Matrix<double,1,6> row;
        for(unsigned long lines = 0; lines < 10000;lines++)
        {
            for(int i=0; i<6; ++i ) {
                ifs >> row(i);
            }
            if(lines >= startframe) {
                Sophus::SE3 T_wr( T_hf * mvl::Cart2T(row) * T_fh );
                vecT_wh.push_back(T_wr);
            }
        }

        ifs.close();
    }
}

int main( int /*argc*/, char* argv[] )
{
    // Initialise window
    View& container = SetupPangoGL(1024, 768);

    // Initialise CUDA, allowing it to use OpenGL context
    cudaGLSetGLDevice(0);
    size_t cu_mem_start, cu_mem_end, cu_mem_total;
    cudaMemGetInfo( &cu_mem_start, &cu_mem_total );

    // Open video device
//    const std::string cam_uri =
    CameraDevice camera = OpenRpgCamera(
//        "AlliedVision:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/AlliedVisionCam,CamUUID0=5004955,CamUUID1=5004954,ImageBinningX=2,ImageBinningY=2,ImageWidth=694,ImageHeight=518]//"
//        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/CityBlock-Noisy,Channel-0=left.*pgm,Channel-1=right.*pgm,StartFrame=0,BufferSize=120]//"
//        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/xb3,Channel-0=left.*pgm,Channel-1=right.*pgm,StartFrame=0,BufferSize=120]//"
        "FileReader:[NumChannels=2,DataSourceDir=/Users/slovegrove/data/20120515/20090822_212628/rect_images,Channel-0=.*left.pnm,Channel-1=.*right.pnm,StartFrame=500,BufferSize=60]//"
//        "Dvi2Pci:[NumChannels=2,ImageWidth=640,ImageHeight=480,BufferCount=60]//"
    );

//    CameraDevice camera = OpenPangoCamera(
//        "file:[stream=0,fmt=GRAY8]///Users/slovegrove/data/3DCam/DSCF0051.AVI",
//        "file:[stream=1,fmt=GRAY8]///Users/slovegrove/data/3DCam/DSCF0051.AVI"
//    );

    // Capture first image
    std::vector<rpg::ImageWrapper> img;
    camera.Capture(img);

    // native width and height (from camera)
    const unsigned int nw = img[0].width();
    const unsigned int nh = img[0].height();

    // Downsample this image to process less pixels
    const int level = GetLevelFromMaxPixels( nw, nh, 320*240 ); //640*480 );
//    const int level = 4;

    // Find centered image crop which aligns to 16 pixels
    const NppiRect roi = GetCenteredAlignedRegion(nw,nh,16 << level,16 << level);

    // Load Camera intrinsics from file
    mvl::CameraModel camModel[] = {
        camera.GetProperty("DataSourceDir") + "/lcmod.xml",
        camera.GetProperty("DataSourceDir") + "/rcmod.xml"
    };

    for(int i=0; i<2; ++i ) {
        // Adjust to match camera image dimensions
        CamModelScaleToDimensions(camModel[i], img[i].width(), img[i].height() );

        // Adjust to match cropped aligned image
        CamModelCropToRegionOfInterest(camModel[i], roi);

        // Scale to appropriate level
        CamModelScale(camModel[i], 1.0 / (1 << level) );
    }

    const unsigned int w = camModel[0].Width();
    const unsigned int h = camModel[0].Height();

    cout << "Video stream dimensions: " << nw << "x" << nh << endl;
    cout << "Chosen Level: " << level << endl;
    cout << "Processing dimensions: " << w << "x" << h << endl;
    cout << "Offset: " << roi.x << "x" << roi.y << endl;

    // OpenGL's Right Down Forward coordinate systems
    Eigen::Matrix3d RDFvision;RDFvision<< 1,0,0,  0,1,0,  0,0,1;
    Eigen::Matrix3d RDFrobot; RDFrobot << 0,1,0,  0,0, 1,  1,0,0;
    Eigen::Matrix4d T_vis_ro = Eigen::Matrix4d::Identity();
    T_vis_ro.block<3,3>(0,0) = RDFvision.transpose() * RDFrobot;
    Eigen::Matrix4d T_ro_vis = Eigen::Matrix4d::Identity();
    T_ro_vis.block<3,3>(0,0) = RDFrobot.transpose() * RDFvision;

    const Eigen::Matrix3d K = camModel[0].K();
    const Eigen::Matrix3d Kinv = MakeKinv(K);
    const Sophus::SE3 T_rl_orig = T_rlFromCamModelRDF(camModel[0], camModel[1], RDFvision);
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

    // Check we received at least two images
    if(img.size() < 2) {
        std::cerr << "Failed to capture first stereo pair from camera" << std::endl;
        return -1;
    }

    // Load history
    Sophus::SE3 T_wc;
    vector<Sophus::SE3> gtPoseT_wh;
    LoadPoses(gtPoseT_wh, camera.GetProperty("DataSourceDir") + "/pose.txt", camera.GetProperty("StartFrame",0), T_vis_ro, T_ro_vis);

    // Plane Parameters
    // These coordinates need to be below the horizon. This could cause trouble!
    Eigen::Matrix3d U; U << w, 0, w,  h/2, h, h,  1, 1, 1;
    Eigen::Matrix3d Q = -(Kinv * U).transpose();
    Eigen::Matrix3d Qinv = Q.inverse();
    Eigen::Vector3d z; z << 1/5.0, 1/5.0, 1/5.0;
    Eigen::Vector3d n_c = Qinv*z;
    Eigen::Vector3d n_w = project((Eigen::Vector4d)(T_wc.inverse().matrix().transpose() * unproject(n_c)));

    GlTextureCudaArray tex8LeftImg(w,h,GL_LUMINANCE8, false);
    GlTextureCudaArray tex8RightImg(w,h,GL_LUMINANCE8, false);
    GlTextureCudaArray texfDisp(w,h,GL_LUMINANCE32F_ARB, false);
    GlTextureCudaArray texf4Debug(w,h,GL_RGBA_FLOAT32_APPLE, false);

    // Define Camera Render Object (for view / scene browsing)
    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrixRDF_TopLeft(w,h,K(0,0),K(1,1),K(0,2),K(1,2),0.1,1000),
        IdentityMatrix(GlModelViewStack)
    );
    if(!gtPoseT_wh.empty()) {
        s_cam.SetModelViewMatrix(gtPoseT_wh[0].inverse().matrix());
    }

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
    Image<unsigned char, TargetHost, DontManage> hCamImg[] = {{0,nw,nh},{0,nw,nh}};
    Image<unsigned char, TargetDevice, Manage> dCamImg[] = {{w,h},{w,h}};
    Image<float2, TargetDevice, Manage> dLookup[] = {{w,h},{w,h}};
    Image<unsigned char, TargetDevice, Manage> dDispInt(w,h);
    Image<float, TargetDevice, Manage>  dDisp(w,h);
    Image<float, TargetDevice, Manage>  dDispFilt(w,h);
    Image<float4, TargetDevice, Manage>  d3d(w,h);
    Image<unsigned char, TargetDevice,Manage> dScratch(w*sizeof(LeastSquaresSystem<float,6>),h);
    Image<float4, TargetDevice, Manage>  dDebugf4(w,h);
    Image<float, TargetDevice, Manage>  dErr(w,h);

    Sophus::SE3 T_wv;
    Volume<CostVolElem, TargetDevice, Manage>  dCostVol(w,h,80);
    Image<unsigned char, TargetDevice, Manage> dImgv(w,h);


    // heightmap size calculation
    double dHeightMapWidthMeters = 100;
    double dHeightMapHeightMeters = 100;
    double dPixelsPerMeter = 10;
    double w_hm = dHeightMapWidthMeters*dPixelsPerMeter;
    double h_hm = dHeightMapHeightMeters*dPixelsPerMeter;

    // Plane (z=0) to heightmap transform (adjust to pixel units)
    Eigen::Matrix4d eT_hp;
    eT_hp << dPixelsPerMeter, 0, 0, 0,
             0, dPixelsPerMeter, 0, 0,
             0, 0, 1, 0,
             0, 0, 0, 1;

    // Heightmap to world transform (set once we know the plane)
    Eigen::Matrix4d T_hw;

    Image<float4, TargetDevice,Manage> dHeightMap(w_hm, h_hm);
    GlBufferCudaPtr vbo_hm(GlArrayBuffer, w_hm*h_hm*sizeof(float4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo_hm(GlArrayBuffer, w_hm*h_hm*sizeof(uchar4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr ibo_hm(GlElementArrayBuffer, w_hm*h_hm*sizeof(uint2) );
    GlTextureCudaArray tex_hm(w_hm,h_hm,GL_RGBA8);

    //initialize the heightmap
    InitHeightMap(dHeightMap);

    //generate index buffer for heightmap
    {
        CudaScopedMappedPtr var(ibo_hm);
        Gpu::Image<uint2> dIbo((uint2*)*var,w_hm,h_hm);
        GenerateTriangleStripIndexBuffer(dIbo);
    }

    // Temporary image buffers
    const int num_temp = 3;
    Image<unsigned char, TargetDevice, Manage> dTemp[num_temp] = {
        {(uint)roi.width,(uint)roi.height},{(uint)roi.width,(uint)roi.height},{(uint)roi.width,(uint)roi.height}
    };

    // Stereo transformation (post-rectification)
    Sophus::SE3 T_rl = T_rl_orig;

    // Build camera distortion lookup tables
    if(rectify) {
        T_rl = CreateScanlineRectifiedLookupAndT_rl(dLookup[0], dLookup[1], T_rl_orig, K, k1, k2, w, h );
    }

    const double baseline = T_rl.translation().norm();

    {
        cudaMemGetInfo( &cu_mem_end, &cu_mem_total );
        const unsigned bytes_per_mb = 1024*1000;
        cout << "CuTotal: " << cu_mem_total/bytes_per_mb << ", Available: " << cu_mem_end/bytes_per_mb << ", Used: " << (cu_mem_start-cu_mem_end)/bytes_per_mb << endl;
    }

    Var<bool> step("ui.step", false, false);
    Var<bool> run("ui.run", false, true);
    Var<bool> lockToCam("ui.Lock to cam", false, true);
    Var<int> maxDisp("ui.disp",80, 0, 128);
    Var<float> stereoAcceptThresh("ui.2nd Best thresh", 0.99, 0.99, 1.01, false);

    Var<bool> subpix("ui.subpix", true, true);
    Var<bool> fuse("ui.fuse", false, true);
    Var<bool> resetPlane("ui.resetplane", true, false);

    Var<bool> show_mesh("ui.show mesh", true, true);
    Var<bool> show_color("ui.show color", true, true);
    Var<bool> show_history("ui.show history", true, true);
    Var<bool> show_depthmap("ui.show depthmap", true, true);
    Var<bool> show_heightmap("ui.show heightmap", false, true);
    Var<bool> cross_section("ui.Cross Section", true, true);
    Var<bool> pose_refinement("ui.Pose Refinement", false, true);

    Var<bool> applyBilateralFilter("ui.Apply Bilateral Filter", false, true);
    Var<int> bilateralWinSize("ui.size",5, 1, 20);
    Var<float> gs("ui.gs",2, 1E-3, 5);
    Var<float> gr("ui.gr",0.0184, 1E-3, 1);

    Var<int> domedits("ui.median its",10, 1, 10);
    Var<bool> domed5x5("ui.median 5x5", false, true);
    Var<bool> domed3x3("ui.median 3x3", false, true);

    Var<bool> plane_do("ui.Compute Ground Plane", false, true);
    Var<float> plane_within("ui.Plane Within",20, 0.1, 100);
    Var<float> plane_c("ui.Plane c", 0.5, 0.0001, 1);

    Var<bool> costvol_reset("ui.Set Reference", true, false);
    Var<bool> costvol_add("ui.Add to Costvol", true, true);

    pangolin::RegisterKeyPressCallback(' ', [&run](){run = !run;} );
    pangolin::RegisterKeyPressCallback('l', [&lockToCam](){lockToCam = !lockToCam;} );
    pangolin::RegisterKeyPressCallback(PANGO_SPECIAL + GLUT_KEY_RIGHT, [&step](){step=true;} );

    const int N = 5;
    for(int i=0; i<N; ++i ) {
        View& v = CreateDisplay();
        v.SetAspect((double)w/h);
        container.AddDisplay(v);
    }
    View& view3d = CreateDisplay().SetAspect((double)w/h).SetHandler(new Handler3D(s_cam, AxisNone));
    container.AddDisplay(view3d);

    Handler2dImageSelect handler2d(w,h);
    container[0].SetDrawFunction(ActivateDrawTexture(tex8LeftImg, true)).SetHandler(&handler2d);
    container[1].SetDrawFunction(ActivateDrawTexture(tex8RightImg, true)).SetHandler(&handler2d);
    container[2].SetDrawFunction(ActivateDrawTexture(texfDisp, true)).SetHandler(&handler2d);
    container[3].SetDrawFunction(ActivateDrawTexture(texf4Debug, true)).SetHandler(new Handler2dImageSelect(w,h));
    container[4].SetDrawFunction(ActivateDrawTexture(tex_hm, true)).SetHandler(new Handler2dImageSelect(w,h));

    for(unsigned long frame=0; !pangolin::ShouldQuit();)
    {
        const bool go = frame==0 || run || Pushed(step);

        if(go) {
            camera.Capture(img);

            if(frame < gtPoseT_wh.size()) {
                T_wc = gtPoseT_wh[frame];
            }

            frame++;

            /////////////////////////////////////////////////////////////
            // Upload images to device (Warp / Decimate if necessery)
            for(int i=0; i<2; ++i ) {
                hCamImg[i].ptr = img[i].Image.data;

                if(rectify) {
                    dTemp[0].CopyFrom(hCamImg[i].SubImage(roi));

                    if( level != 0 ) {
                        BoxReduce<unsigned char, unsigned int, unsigned char>(dTemp[2].SubImage(w,h), dTemp[0], dTemp[1], level);
                        Warp(dCamImg[i], dTemp[2].SubImage(w,h), dLookup[i]);
                    }else{
                        Warp(dCamImg[i], dTemp[0], dLookup[i]);
                    }
                }else{
                    if( level != 0 ) {
                        dTemp[0].CopyFrom(hCamImg[i].SubImage(roi));
                        BoxReduce<unsigned char, unsigned int, unsigned char>(dCamImg[i], dTemp[0], dTemp[1], level);
                    }else{
                        dCamImg[i].CopyFrom(hCamImg[i].SubImage(roi));
                    }
                }
            }
        }

        if(go || GuiVarHasChanged() ) {
            // Compute dense stereo
            DenseStereo(dDispInt, dCamImg[0], dCamImg[1], maxDisp, stereoAcceptThresh);

            if(subpix) {
                DenseStereoSubpixelRefine(dDisp, dDispInt, dCamImg[0], dCamImg[1]);
            }else{
                ConvertImage<float, unsigned char>(dDisp, dDispInt);
            }

            if(applyBilateralFilter) {
                BilateralFilter(dDispFilt,dDisp,gs,gr,bilateralWinSize);
                dDisp.CopyFrom(dDispFilt);
            }

            for(int i=0; i < domedits; ++i ) {
                if(domed3x3) {
                    MedianFilter3x3(dDisp,dDisp);
                }

                if(domed5x5) {
                    MedianFilter5x5(dDisp,dDisp);
                }
            }

            // Generate point cloud from disparity image
            DisparityImageToVbo(d3d, dDisp, baseline, K(0,0), K(1,1), K(0,2), K(1,2) );

            // we have 3d point data, use this to calculate the heightmap delta

            if(plane_do || resetPlane) {
                // Fit plane
                for(int i=0; i<(resetPlane*100+5); ++i )
                {
                    Gpu::LeastSquaresSystem<float,3> lss = PlaneFitGN(d3d, Qinv, z, dScratch, dErr, plane_within, plane_c);
                    Eigen::FullPivLU<Eigen::MatrixXd> lu_JTJ( (Eigen::Matrix3d)lss.JTJ );
                    Eigen::Matrix<double,Eigen::Dynamic,1> x = -1.0 * lu_JTJ.solve( (Eigen::Matrix<double,3,1>)lss.JTy );
                    if( x.norm() > 1 ) x = x / x.norm();
                    for(int i=0; i<3; ++i ) {
                        z(i) *= exp(x(i));
                    }
                    n_c = Qinv * z;
                    n_w = project((Eigen::Vector4d)(T_wc.inverse().matrix().transpose() * unproject(n_c)));
                }
            }

            if(Pushed(resetPlane) ) {
                Eigen::Matrix4d T_nw = (PlaneBasis_wp(n_c).inverse() * T_wc.inverse()).matrix();
                T_nw.block<2,1>(0,3) += Eigen::Vector2d(dHeightMapWidthMeters/2, dHeightMapHeightMeters /*/2*/);
                T_hw = eT_hp * T_nw;
                InitHeightMap(dHeightMap);
            }

            //calcualte the camera to heightmap transform
            if(fuse)
            {
                Eigen::Matrix<double,3,4> T_hc = (T_hw * T_wc.matrix()).block<3,4>(0,0);

                UpdateHeightMap(dHeightMap,d3d,dCamImg[0],T_hc);

                // Copy point cloud into VBO
                {
                    CudaScopedMappedPtr var(vbo_hm);
                    Gpu::Image<float4> dVbo((float4*)*var,w_hm,h_hm);
                    VboFromHeightMap(dVbo,dHeightMap);
                }

                // Generate CBO
                {
                    CudaScopedMappedPtr var(cbo_hm);
                    Gpu::Image<uchar4> dCbo((uchar4*)*var,w_hm,h_hm);
                    ColourHeightMap(dCbo,dHeightMap);
                    tex_hm << dCbo;
                }
            }

            if(pose_refinement) {
                // attempt to reestimate baseline from depthmap
                // Compute Pose
                Eigen::Matrix<double, 3,4> KT_rl = K * T_rl.matrix3x4();
                Gpu::LeastSquaresSystem<float,6> lss = PoseRefinementFromDepthmap(dCamImg[1], dCamImg[0], d3d, KT_rl, 1E10, dScratch, dDebugf4);
                Eigen::FullPivLU<Eigen::Matrix<double,6,6> > lu_JTJ( (Eigen::Matrix<double,6,6>)lss.JTJ );
                Eigen::Matrix<double,6,1> x = -1.0 * lu_JTJ.solve( (Eigen::Matrix<double,6,1>)lss.JTy );
                cout << "--------------------------------------" << endl;
                cout << ((Eigen::Matrix<double,6,6>)lss.JTJ) << endl << endl;
                cout << ((Eigen::Matrix<double,6,1>)lss.JTy).transpose() << endl << endl;
                cout << x.transpose() << endl;
//                if( x.norm() > 1 ) x = x / x.norm();
                T_rl = T_rl * Sophus::SE3::exp(x);
                texf4Debug << dDebugf4;
            }

            if(costvol_add) {
                const Eigen::Matrix<double,3,4> KT_lv = K * (T_wc.inverse() * T_wv).matrix3x4();
                AddToCostVolume(dCostVol,dImgv, dCamImg[0], KT_lv, K(0,0), K(1,1), K(0,2), K(1,2), 0, 1.0f / baseline, maxDisp);
                const Eigen::Matrix<double,3,4> KT_rv = K * (T_rl*T_wc.inverse() * T_wv).matrix3x4();
                AddToCostVolume(dCostVol,dImgv, dCamImg[1], KT_rv, K(0,0), K(1,1), K(0,2), K(1,2), 0, 1.0f / baseline, maxDisp);
            }

            // Copy point cloud into VBO
            {
                CudaScopedMappedPtr var(vbo);
                Gpu::Image<float4> dVbo((float4*)*var,w,h);
                dVbo.CopyFrom(d3d);
            }

            // Generate CBO
            {
                CudaScopedMappedPtr var(cbo);
                Gpu::Image<uchar4> dCbo((uchar4*)*var,w,h);
                ConvertImage<uchar4,unsigned char>(dCbo, dCamImg[0]);
            }

            // normalise dDisp
            nppiDivC_32f_C1IR(maxDisp,dDisp.ptr,dDisp.pitch,dDisp.Size());

            // Update texture views
            tex8LeftImg << dCamImg[0];
            tex8RightImg << dCamImg[1];
            texfDisp << dDisp;
        }

        if(cross_section) {
            if(0) {
                DisparityImageCrossSection(dDebugf4, dDispInt, dCamImg[0], dCamImg[1], handler2d.GetSelectedPoint(true)[1] + 0.5);
            }else{
                CostVolumeCrossSection(dDebugf4, dCostVol, handler2d.GetSelectedPoint(true)[1] + 0.5);
            }
            texf4Debug << dDebugf4;
        }

        if(Pushed(costvol_reset)) {
            T_wv = T_wc;
            dImgv.CopyFrom(dCamImg[0]);
//            InitCostVolume(dCostVol);
            InitCostVolume(dCostVol,dCamImg[0], dCamImg[1]);
        }

        /////////////////////////////////////////////////////////////
        // Perform drawing
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);

        view3d.ActivateAndScissor(s_cam);
        glEnable(GL_DEPTH_TEST);

        static bool lastLockToCam = lockToCam;
        if( lockToCam != lastLockToCam ) {
            if(lockToCam) {
                const Eigen::Matrix4d T_vc = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wc.matrix();
                s_cam.SetModelViewMatrix(T_vc);
            }else{
                const Eigen::Matrix4d T_vw = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wc.inverse().matrix();
                s_cam.SetModelViewMatrix(T_vw);
            }
            lastLockToCam = lockToCam;
        }

        if(lockToCam) glSetFrameOfReferenceF(T_wc.inverse());

        //draw the global heightmap
        if(show_heightmap)
        {
            //transform the mesh into world coordinates from heightmap coordinates
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glMultMatrix( T_hw.inverse() );
            RenderVbo(ibo_hm,vbo_hm,cbo_hm, w_hm, h_hm, show_mesh, show_color);
            glPopMatrix();
        }

        // Render camera frustum and mesh
        {
            glSetFrameOfReferenceF(T_wc);
            if(show_depthmap) {
                RenderVbo(ibo,vbo,cbo, w, h, show_mesh, show_color);
            }
            glColor3f(1.0,1.0,1.0);
            DrawFrustrum(Kinv,w,h,1.0);
            if(plane_do) {
                // Draw ground plane
                glColor4f(0,1,0,1);
                DrawPlane(n_c,1,100);
            }
            glUnsetFrameOfReference();
        }

        if(show_history) {
            // Draw history
            for(int i=0; i< gtPoseT_wh.size() && i< frame; ++i) {
                DrawAxis(gtPoseT_wh[i]);
            }
        }

        if(lockToCam) glUnsetFrameOfReference();

        glColor4f(1,1,1,1);
        pangolin::RenderViews();
        pangolin::FinishGlutFrame();
    }
}
