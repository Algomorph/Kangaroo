#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <Pangolin/glsl.h>
#include <npp.h>

#include <SceneGraph/SceneGraph.h>
#include <SceneGraph/GLVbo.h>
#include "common/GLCameraHistory.h"

#include <fiducials/drawing.h>
#include <fiducials/camera.h>

#include "common/RpgCameraOpen.h"
#include "common/DisplayUtils.h"
#include "common/ScanlineRectify.h"
#include "common/ImageSelect.h"
#include "common/BaseDisplay.h"
#include "common/HeightmapFusion.h"
#include "common/CameraModelPyramid.h"
#include "common/LoadPosesFromFile.h"

#include <kangaroo/kangaroo.h>

#include <Node.h>

using namespace std;
using namespace pangolin;
using namespace Gpu;

int main( int argc, char* argv[] )
{
    // Initialise window
    View& container = SetupPangoGL(1024, 768);

    // Initialise CUDA, allowing it to use OpenGL context
    if( cudaGLSetGLDevice(0) != cudaSuccess ) {
        cerr << "Unable to get CUDA Device" << endl;
        return -1;
    }
    const unsigned bytes_per_mb = 1024*1000;
    size_t cu_mem_start, cu_mem_end, cu_mem_total;
    cudaMemGetInfo( &cu_mem_start, &cu_mem_total );
    cout << cu_mem_start/bytes_per_mb << " MB Video Memory Available." << endl;
    if( cu_mem_start < (100 * bytes_per_mb) ) {
        cerr << "Not enough memory to proceed." << endl;
        return -1;
    }
    glClearColor(1,1,1,0);

    // Open video device
    CameraDevice video = OpenRpgCamera(argc,argv);

    // Capture first image
    std::vector<rpg::ImageWrapper> img;
    video.Capture(img);

    // native width and height (from camera)
    const unsigned int nw = img[0].width();
    const unsigned int nh = img[0].height();

    // Downsample this image to process less pixels
    const int max_levels = 6;
    const int level = GetLevelFromMaxPixels( nw, nh, 640*480 );
//    const int level = 4;
    assert(level <= max_levels);

    // Find centered image crop which aligns to 16 pixels at given level
    const NppiRect roi = GetCenteredAlignedRegion(nw,nh,16 << level,16 << level);

    // Load Camera intrinsics from file
    CameraModelPyramid cam[] = {
        video.GetProperty("DataSourceDir") + "/lcmod.xml",
        video.GetProperty("DataSourceDir") + "/rcmod.xml"
    };

    for(int i=0; i<2; ++i ) {
        // Adjust to match camera image dimensions
        CamModelScaleToDimensions(cam[i], nw, nh );

        // Adjust to match cropped aligned image
        CamModelCropToRegionOfInterest(cam[i], roi);

        cam[i].PopulatePyramid(max_levels);
    }

    const unsigned int w = roi.width;
    const unsigned int h = roi.height;
    const unsigned int lw = w >> level;
    const unsigned int lh = h >> level;

    const Eigen::Matrix3d& K0 = cam[0].K();
    const Eigen::Matrix3d& Kl = cam[0].K(level);

    cout << "Video stream dimensions: " << nw << "x" << nh << endl;
    cout << "Chosen Level: " << level << endl;
    cout << "Processing dimensions: " << lw << "x" << lh << endl;
    cout << "Offset: " << roi.x << "x" << roi.y << endl;

    // OpenGL's Right Down Forward coordinate systems
    Eigen::Matrix3d RDFvision;RDFvision<< 1,0,0,  0,1,0,  0,0,1;
    Eigen::Matrix3d RDFrobot; RDFrobot << 0,1,0,  0,0, 1,  1,0,0;
    Eigen::Matrix4d T_vis_ro = Eigen::Matrix4d::Identity();
    T_vis_ro.block<3,3>(0,0) = RDFvision.transpose() * RDFrobot;
    Eigen::Matrix4d T_ro_vis = Eigen::Matrix4d::Identity();
    T_ro_vis.block<3,3>(0,0) = RDFrobot.transpose() * RDFvision;

    const Sophus::SE3 T_rl_orig = T_rlFromCamModelRDF(cam[0], cam[1], RDFvision);
    double k1 = 0;
    double k2 = 0;

    if(cam[0].Type() == MVL_CAMERA_WARPED)
    {
        k1 = cam[0].GetModel()->warped.kappa1;
        k2 = cam[0].GetModel()->warped.kappa2;
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
    LoadPosesFromFile(gtPoseT_wh, video.GetProperty("DataSourceDir") + "/pose.txt", video.GetProperty("StartFrame",0), T_vis_ro, T_ro_vis);

    // Plane Parameters
    // These coordinates need to be below the horizon. This could cause trouble!
    Eigen::Matrix3d U; U << w, 0, w,  h/2, h, h,  1, 1, 1;
    Eigen::Matrix3d Q = -(cam[0].Kinv() * U).transpose();
    Eigen::Matrix3d Qinv = Q.inverse();
    Eigen::Vector3d z; z << 1/5.0, 1/5.0, 1/5.0;
    Eigen::Vector3d n_c = Qinv*z;
    Eigen::Vector3d n_w = project((Eigen::Vector4d)(T_wc.inverse().matrix().transpose() * unproject(n_c)));

    // Define Camera Render Object (for view / scene browsing)
    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrixRDF_TopLeft(w,h,K0(0,0),K0(1,1),K0(0,2),K0(1,2),0.1,10000),
        IdentityMatrix(GlModelViewStack)
    );
    if(!gtPoseT_wh.empty()) {
        s_cam.SetModelViewMatrix(gtPoseT_wh[0].inverse().matrix());
    }

    GlBufferCudaPtr vbo(GlArrayBuffer, lw,lh,GL_FLOAT, 4, cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo(GlArrayBuffer, lw,lh,GL_UNSIGNED_BYTE, 4, cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr ibo(GlElementArrayBuffer, lw,lh,GL_UNSIGNED_INT, 2 );

    // Generate Index Buffer Object for rendering mesh
    {
        CudaScopedMappedPtr var(ibo);
        Gpu::Image<uint2> dIbo((uint2*)*var,lw,lh);
        GenerateTriangleStripIndexBuffer(dIbo);
    }


    // Allocate Camera Images on device for processing
    Image<unsigned char, TargetHost, DontManage> hCamImg[] = {{0,nw,nh},{0,nw,nh}};
    Image<float2, TargetDevice, Manage> dLookup[] = {{w,h},{w,h}};

    Pyramid<unsigned char, max_levels, TargetDevice, Manage> dCamImg[] = {{w,h},{w,h}};

    Image<unsigned char, TargetDevice, Manage> dDispInt(lw,lh);
    Image<float, TargetDevice, Manage>  dDisp(lw,lh);
    Image<float, TargetDevice, Manage>  dDispFilt(lw,lh);
    Image<float4, TargetDevice, Manage>  d3d(lw,lh);
    Image<float4, TargetDevice, Manage>  dN(lw,lh);
    Image<unsigned char, TargetDevice,Manage> dScratch(lw*sizeof(LeastSquaresSystem<float,6>),lh);
    Image<float4, TargetDevice, Manage>  dDebugf4(lw,lh);
    Image<float, TargetDevice, Manage>  dErr(lw,lh);


    Sophus::SE3 T_wv;
    Volume<CostVolElem, TargetDevice, Manage>  dCostVol(lw,lh,80);
    Image<unsigned char, TargetDevice, Manage> dImgv(lw,lh);

//    HeightmapFusion hm(800,800,2);
//    HeightmapFusion hm(200,200,10);
    HeightmapFusion hm(100,100,10);
    const bool center_y = false;

    GlBufferCudaPtr vbo_hm(GlArrayBuffer, hm.WidthPixels(), hm.HeightPixels(), GL_FLOAT, 4, cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo_hm(GlArrayBuffer, hm.WidthPixels(), hm.HeightPixels(), GL_UNSIGNED_BYTE, 4, cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr ibo_hm(GlElementArrayBuffer, hm.WidthPixels(), hm.HeightPixels(), GL_UNSIGNED_INT, 2 );

    //generate index buffer for heightmap
    {
        CudaScopedMappedPtr var(ibo_hm);
        Gpu::Image<uint2> dIbo((uint2*)*var,hm.WidthPixels(),hm.HeightPixels());
        GenerateTriangleStripIndexBuffer(dIbo);
    }

    // Stereo transformation (post-rectification)
    Sophus::SE3 T_rl = T_rl_orig;

    // Build camera distortion lookup tables
    if(rectify) {
        T_rl = CreateScanlineRectifiedLookupAndT_rl(dLookup[0], dLookup[1], T_rl_orig, K0, k1, k2, w, h );
    }

    const double baseline = T_rl.translation().norm();

    {
        cudaMemGetInfo( &cu_mem_end, &cu_mem_total );
        cout << "CuTotal: " << cu_mem_total/bytes_per_mb << ", Available: " << cu_mem_end/bytes_per_mb << ", Used: " << (cu_mem_start-cu_mem_end)/bytes_per_mb << endl;
    }

    Var<bool> step("ui.step", false, false);
    Var<bool> run("ui.run", false, true);
    Var<bool> lockToCam("ui.Lock to cam", false, true);
    Var<int> show_level("ui.show level",0, 0, max_levels-1);

    Var<float> maxDisp("ui.disp",80, 0, 128);
    Var<float> dispStep("ui.disp step",1, 0.1, 1);
    Var<int> scoreRad("ui.score rad",1, 0, 7 );
    Var<bool> scoreNormed("ui.score normed",true, true);

    Var<float> stereoAcceptThresh("ui.2nd Best thresh", 0, 0, 1, false);

    Var<bool> subpix("ui.subpix", false, true);
    Var<bool> reverse_check("ui.reverse_check", false, true);

    Var<bool> fuse("ui.fuse", false, true);
    Var<bool> resetPlane("ui.resetplane", true, false);
    Var<bool> save_hm("ui.save heightmap", false, false);

//    Var<bool> draw_frustrum("ui.show frustrum", false, true);
//    Var<bool> show_mesh("ui.show mesh", true, true);
//    Var<bool> show_color("ui.show color", true, true);
    Var<bool> show_history("ui.show history", true, true);
    Var<bool> show_depthmap("ui.show depthmap", true, true);
    Var<bool> show_heightmap("ui.show heightmap", false, true);
    Var<bool> cross_section("ui.Cross Section", false, true);

    Var<bool> applyBilateralFilter("ui.Apply Bilateral Filter", false, true);
    Var<int> bilateralWinSize("ui.size",5, 1, 20);
    Var<float> gs("ui.gs",2, 1E-3, 5);
    Var<float> gr("ui.gr",0.0184, 1E-3, 1);

    Var<int> domedits("ui.median its",1, 1, 10);
    Var<bool> domed9x9("ui.median 9x9", false, true);
    Var<bool> domed7x7("ui.median 7x7", false, true);
    Var<bool> domed5x5("ui.median 5x5", false, true);
    Var<bool> domed3x3("ui.median 3x3", false, true);
    Var<int> medi("ui.medi",12, 0, 24);

    Var<float> filtgradthresh("ui.filt grad thresh", 0, 0, 20);

    Var<bool> plane_do("ui.Compute Ground Plane", false, true);
    Var<float> plane_within("ui.Plane Within",20, 0.1, 100);
    Var<float> plane_c("ui.Plane c", 0.5, 0.0001, 1);

    Var<bool> costvol_reset("ui.Set Reference", true, false);
    Var<bool> costvol_add("ui.Add to Costvol", false, true);

    pangolin::RegisterKeyPressCallback(' ', [&run](){run = !run;} );
    pangolin::RegisterKeyPressCallback('l', [&lockToCam](){lockToCam = !lockToCam;} );
    pangolin::RegisterKeyPressCallback(PANGO_SPECIAL + GLUT_KEY_RIGHT, [&step](){step=true;} );
    pangolin::RegisterKeyPressCallback('~', [&container](){static bool showpanel=true; showpanel = !showpanel; if(showpanel) { container.SetBounds(0,1,Attach::Pix(180), 1); }else{ container.SetBounds(0,1,0, 1); } Display("ui").Show(showpanel); } );
    pangolin::RegisterKeyPressCallback('1', [&container](){container[0].ToggleShow();} );
    pangolin::RegisterKeyPressCallback('2', [&container](){container[1].ToggleShow();} );
    pangolin::RegisterKeyPressCallback('3', [&container](){container[2].ToggleShow();} );
    pangolin::RegisterKeyPressCallback('4', [&container](){container[3].ToggleShow();} );
    pangolin::RegisterKeyPressCallback('$', [&container](){container[3].SaveRenderNow("screenshot",4);} );

    Handler2dImageSelect handler2d(lw,lh);
    ActivateDrawPyramid<unsigned char,max_levels> adleft(dCamImg[0],GL_LUMINANCE8, false, true);
    ActivateDrawImage<float> adDisp(dDisp,GL_LUMINANCE32F_ARB, false, true);
    ActivateDrawImage<float4> adDebug(dDebugf4,GL_RGBA_FLOAT32_APPLE, false, true);

    SceneGraph::GLSceneGraph graph;
    SceneGraph::GLVbo glvbo(&vbo,&ibo,&cbo);
    SceneGraph::GLVbo glhmvbo(&vbo_hm,&ibo_hm,&cbo_hm);
    SceneGraph::GLGrid glGroundPlane;
    SceneGraph::GLCameraHistory history;
    history.LoadFromAbsoluteCartesianFile(video.GetProperty("DataSourceDir") + "/pose.txt", video.GetProperty("StartFrame",0), T_vis_ro, T_ro_vis);
    graph.AddChild(&glvbo);
    glvbo.AddChild(&glGroundPlane);
    graph.AddChild(&glhmvbo);
    graph.AddChild(&history);

    SetupContainer(container, 4, (float)w/h);
    container[0].SetDrawFunction(boost::ref(adleft)).SetHandler(&handler2d);
    container[1].SetDrawFunction(boost::ref(adDisp)).SetHandler(&handler2d);
    container[2].SetDrawFunction(boost::ref(adDebug)).SetHandler(new Handler2dImageSelect(lw,lh));
    container[3].SetDrawFunction(SceneGraph::ActivateDrawFunctor(graph, s_cam));
    container[3].SetHandler( new Handler3D(s_cam, AxisNone) );

    for(unsigned long frame=0; !pangolin::ShouldQuit();)
    {
        const bool go = frame==0 || run || Pushed(step);

        if(go) {
            video.Capture(img);

            if(frame < gtPoseT_wh.size()) {
                T_wc = gtPoseT_wh[frame];
            }

            frame++;

            /////////////////////////////////////////////////////////////
            // Upload images to device (Warp / Decimate if necessery)
            for(int i=0; i<2; ++i ) {
                hCamImg[i].ptr = img[i].Image.data;

                if(rectify) {
//                    dTemp[0].CopyFrom(hCamImg[i].SubImage(roi));

//                    if( level != 0 ) {
//                        BoxReduce<unsigned char, unsigned int, unsigned char>(dTemp[2].SubImage(w,h), dTemp[0], dTemp[1], level);
//                        Warp(dCamImg[i], dTemp[2].SubImage(w,h), dLookup[i]);
//                    }else{
//                        Warp(dCamImg[i], dTemp[0], dLookup[i]);
//                    }
                }else{
                    dCamImg[i][0].CopyFrom(hCamImg[i].SubImage(roi));
                    BoxReduce<unsigned char, max_levels, unsigned int>(dCamImg[i]);
                }
            }
        }

        if(go || GuiVarHasChanged() ) {

            if(dispStep == 1 )
            {
                // Compute dense stereo
                DenseStereo<unsigned char,unsigned char>(dDispInt, dCamImg[0][level], dCamImg[1][level], maxDisp, stereoAcceptThresh, scoreRad);

                if(reverse_check) {
                    // Replace with better LeftRightCheck
//                    ReverseCheck(dDispInt, dCamImg[0][level], dCamImg[1][level] );
                }

                if(subpix) {
                    DenseStereoSubpixelRefine(dDisp, dDispInt, dCamImg[0][level], dCamImg[1][level]);
                }else{
                    ConvertImage<float, unsigned char>(dDisp, dDispInt);
                }
            }else{
                DenseStereoSubpix(dDisp, dCamImg[0][level], dCamImg[1][level], maxDisp, dispStep, stereoAcceptThresh, scoreRad, scoreNormed);
            }

            for(int i=0; i < domedits; ++i ) {
                if(domed9x9) MedianFilterRejectNegative9x9(dDisp,dDisp, medi);
                if(domed7x7) MedianFilterRejectNegative7x7(dDisp,dDisp, medi);
                if(domed5x5) MedianFilterRejectNegative5x5(dDisp,dDisp, medi);
                if(domed3x3) MedianFilter3x3(dDisp,dDisp);
            }

            if(filtgradthresh > 0) {
                FilterDispGrad(dDisp, dDisp, filtgradthresh);
            }

            if(applyBilateralFilter) {
                BilateralFilter(dDispFilt,dDisp,gs,gr,bilateralWinSize);
                dDisp.CopyFrom(dDispFilt);
            }

            // Generate point cloud from disparity image
            DisparityImageToVbo(d3d, dDisp, baseline, Kl(0,0), Kl(1,1), Kl(0,2), Kl(1,2) );

            if(container[2].IsShown()) {
                NormalsFromVbo(dN, d3d);
                dDebugf4.CopyFrom(dN);
            }

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
//                T_nw.block<2,1>(0,3) += Eigen::Vector2d(hm.WidthMeters()/2, hm.HeightMeters() /*/2*/);
                T_nw.block<2,1>(0,3) += Eigen::Vector2d(hm.WidthMeters()/2, hm.HeightMeters() / (center_y ? 2 : 1) );
                hm.Init(T_nw);
            }

            //calcualte the camera to heightmap transform
            if(fuse) {
                hm.Fuse(d3d, dCamImg[0][level], T_wc);
                hm.GenerateVbo(vbo_hm);
                hm.GenerateCbo(cbo_hm);
            }

            if(costvol_add) {
                const Eigen::Matrix<double,3,4> KT_lv = Kl * (T_wc.inverse() * T_wv).matrix3x4();
                AddToCostVolume(dCostVol,dImgv, dCamImg[0][level], KT_lv, Kl(0,0), Kl(1,1), Kl(0,2), Kl(1,2), 1.0f / baseline, 0, maxDisp);
//                const Eigen::Matrix<double,3,4> KT_rv = K * (T_rl*T_wc.inverse() * T_wv).matrix3x4();
//                AddToCostVolume(dCostVol,dImgv, dCamImg[1][level], KT_rv, K(0,0), K(1,1), K(0,2), K(1,2), 0, 1.0f / baseline, maxDisp);
            }

            if(container[3].IsShown()) {
                // Copy point cloud into VBO
                {
                    CudaScopedMappedPtr var(vbo);
                    Gpu::Image<float4> dVbo((float4*)*var,lw,lh);
                    dVbo.CopyFrom(d3d);
                }

                // Generate CBO
                {
                    CudaScopedMappedPtr var(cbo);
                    Gpu::Image<uchar4> dCbo((uchar4*)*var,lw,lh);
                    ConvertImage<uchar4,unsigned char>(dCbo, dCamImg[0][level]);
                }
            }

            // normalise dDisp
            nppiDivC_32f_C1IR(maxDisp,dDisp.ptr,dDisp.pitch,dDisp.Size());

            // Update texture views
            adleft.SetLevel(show_level);
        }

        if(cross_section) {
            if(1) {
                DisparityImageCrossSection(dDebugf4, dDispInt, dCamImg[0][level], dCamImg[1][level], handler2d.GetSelectedPoint(true)[1] + 0.5);
            }else{
                CostVolumeCrossSection(dDebugf4, dCostVol, handler2d.GetSelectedPoint(true)[1] + 0.5);
            }
        }else{
//            dDebugf4.CopyFrom(dN);
        }

        if(Pushed(costvol_reset)) {
            T_wv = T_wc;
            dImgv.CopyFrom(dCamImg[0][level]);
            InitCostVolume(dCostVol);
//            InitCostVolume(dCostVol,dCamImg[0][level], dCamImg[1][level]);
        }

        if(Pushed(save_hm)) {
            hm.SaveModel("test");
        }

        /////////////////////////////////////////////////////////////
        // Setup Drawing

        s_cam.Follow(T_wc.matrix(), lockToCam);

        glvbo.SetPose(T_wc.matrix());
        glvbo.SetVisible(show_depthmap);

        glGroundPlane.SetPose(PlaneBasis_wp(n_c).matrix());
        glGroundPlane.SetVisible(plane_do);

        glhmvbo.SetPose((Eigen::Matrix4d)hm.T_hw().inverse());
        glhmvbo.SetVisible(show_heightmap);

        history.SetNumberToShow(frame);
        history.SetVisible(show_history);

        /////////////////////////////////////////////////////////////
        // Draw

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);

        pangolin::FinishGlutFrame();
    }
}
