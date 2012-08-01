#include <Eigen/Eigen>
#include <Sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>

#include "common/ViconTracker.h"

#include <fiducials/drawing.h>

#include "common/RpgCameraOpen.h"
#include "common/ImageSelect.h"
#include "common/BaseDisplay.h"
#include "common/DisplayUtils.h"
#include "common/HeightmapFusion.h"

#include "cu/all.h"
#include "cu/Image.h"

using namespace std;
using namespace pangolin;
using namespace Gpu;

int main( int /*argc*/, char* argv[] )
{
    // Initialise window
    View& container = SetupPangoGL(1024, 768);
    cudaGLSetGLDevice(0);

    // Open video device
    CameraDevice camera = OpenRpgCamera("Kinect://");

    // Open Vicon
    ViconTracking tracker("KINECT","192.168.10.1");

    // http://nicolas.burrus.name/index.php/Research/KinectCalibration
    Eigen::Matrix3d Kdepth;
    Kdepth << 5.9421434211923247e+02, 0, 3.3930780975300314e+02,
            0, 5.9104053696870778e+02, 2.4273913761751615e+02,
            0,0,1;

    Eigen::Matrix3d Krgb;
    Krgb << 5.2921508098293293e+02, 0, 3.2894272028759258e+02,
            0, 5.2556393630057437e+02, 2.6748068171871557e+02,
            0,0,1;

    // Vicon to Camera
    Eigen::Matrix3d RDFvision;RDFvision<< 1,0,0,  0,1,0,   0,0,1;
    Eigen::Matrix3d RDFvicon; RDFvicon << 0,-1,0,  0,0,-1,  1,0,0;
    Sophus::SE3 T_cv = Sophus::SE3(Sophus::SO3(RDFvision.transpose() * RDFvicon), Eigen::Vector3d::Zero() );

    // Camera (rgb) to depth
    Eigen::Matrix3d R_dc;
    R_dc <<  9.9984628826577793e-01, 1.2635359098409581e-03, -1.7487233004436643e-02,
          -1.4779096108364480e-03, 9.9992385683542895e-01, -1.2251380107679535e-02,
          1.7470421412464927e-02, 1.2275341476520762e-02, 9.9977202419716948e-01;
    Eigen::Vector3d c_d(1.9985242312092553e-02, -7.4423738761617583e-04, -1.0916736334336222e-02);
    Sophus::SE3 T_cd = Sophus::SE3(R_dc,c_d).inverse();

    // Reference (depth) to world
    Sophus::SE3 T_wr;

    // Reference (depth) to live (depth)
    Sophus::SE3 T_lr;

    // Capture first image
    std::vector<rpg::ImageWrapper> img;
    camera.Capture(img);

    // Check we received one or more images
    if(img.empty()) {
        std::cerr << "Failed to capture first image from camera" << std::endl;
        return -1;
    }

    const int w = img[0].width();
    const int h = img[0].height();

    Image<unsigned short, TargetDevice, Manage> dKinect(w,h);
    Image<float, TargetDevice, Manage> dKinectf(w,h);
    Image<uchar3, TargetDevice, Manage>  dI(w,h);
    Image<float4, TargetDevice, Manage>  dV(w,h);
    Image<float4, TargetDevice, Manage>  dN(w,h);
    Image<uchar3, TargetDevice, Manage>  dIr(w,h);
    Image<float4, TargetDevice, Manage>  dVr(w,h);
    Image<float4, TargetDevice, Manage>  dNr(w,h);
    Image<float4, TargetDevice, Manage>  dDebug(w,h);
    Image<unsigned char, TargetDevice,Manage> dScratch(w*sizeof(LeastSquaresSystem<float,12>),h);

    HeightmapFusion hm(100,100,10);

    GlBufferCudaPtr vbo_hm(GlArrayBuffer, hm.Pixels()*sizeof(float4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo_hm(GlArrayBuffer, hm.Pixels()*sizeof(uchar4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr ibo_hm(GlElementArrayBuffer, hm.Pixels()*sizeof(uint2) );

    //generate index buffer for heightmap
    {
        CudaScopedMappedPtr var(ibo_hm);
        Gpu::Image<uint2> dIbo((uint2*)*var,hm.WidthPixels(),hm.HeightPixels());
        GenerateTriangleStripIndexBuffer(dIbo);
    }

    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    GlTexture texrgb(w,h,GL_RGB8, false);
    GlTextureCudaArray texdepth(w,h,GL_INTENSITY16, false);
    GlTextureCudaArray texnorm(w,h,GL_RGBA32F_ARB, false);
    GlTextureCudaArray texdebug(w,h,GL_RGBA32F_ARB, false);

    GlBufferCudaPtr vbo(GlArrayBuffer, w*h*sizeof(float4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo(GlArrayBuffer, w*h*sizeof(uchar4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
//    GlBufferCudaPtr ibo(GlElementArrayBuffer, w*h*sizeof(uint2) );

    // Create Smart viewports for each camera image that preserve aspect
    const int N = 4;
    for(int i=0; i<N; ++i ) {
        container.AddDisplay(CreateDisplay());
        container[i].SetAspect((double)w/h);
    }
    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrixRDF_TopLeft(w,h,Kdepth(0,0),Kdepth(1,1),Kdepth(0,2),Kdepth(1,2),1E-2,1E3),
        ModelViewLookAtRDF(0,5,5,0,0,0,0,0,1)
    );
    View& view3d = CreateDisplay().SetAspect((double)w/h).SetHandler(new Handler3D(s_cam, AxisNone));
    container.AddDisplay(view3d);

    container[0].SetDrawFunction(ActivateDrawTexture(texrgb,true));

    Handler2dImageSelect prop_depth(w,h);
    prop_depth.SetPixelScale(10.0f);
    container[1].SetDrawFunction(ActivateDrawTexture(texdepth, true));
    container[1].SetHandler(&prop_depth);

    Handler2dImageSelect prop_debug(w,h);
    container[2].SetDrawFunction(ActivateDrawTexture(texdebug, true));
    container[2].SetHandler(&prop_debug);

    Var<bool> step("ui.step", false, false);
    Var<bool> run("ui.run", true, true);
    Var<bool> lockToCam("ui.Lock to cam", false, true);

    Var<bool> applyBilateralFilter("ui.Apply Bilateral Filter", true, true);
    Var<int> bilateralWinSize("ui.size",5, 1, 20);
    Var<float> gs("ui.gs",5, 1E-3, 10);
    Var<float> gr("ui.gr",100, 1E-3, 100);

    Var<bool> bundle("ui.Bundle", false, true);
    Var<bool> pose_refinement("ui.Pose Refinement", false, true);
    Var<bool> pose_update("ui.Pose Update", false, true);
    Var<bool> calib_update("ui.Calib Update", false, true);
    Var<float> icp_c("ui.icp c",0.5, 1E-3, 1);
    Var<float> img_c("ui.img c",10, 1, 1E3);

    Var<bool> save_ref("ui.Save Reference", true, false);
    Var<bool> fuse("ui.Fuse Heightmap", false, true);
    Var<bool> resetHeightmap("ui.Reset Heightmap", true, false);
    Var<bool> show_heightmap("ui.show heightmap", false, true);
    Var<bool> show_mesh("ui.show mesh", true, true);

    pangolin::RegisterKeyPressCallback(' ', [&run](){run = !run;} );
    pangolin::RegisterKeyPressCallback('l', [&lockToCam](){lockToCam = !lockToCam;} );
    pangolin::RegisterKeyPressCallback(PANGO_SPECIAL + GLUT_KEY_RIGHT, [&step](){step=true;} );

    for(unsigned long frame=0; !pangolin::ShouldQuit();)
    {        
        const bool go = frame==0 || run || Pushed(step);

        if(go) {
            camera.Capture(img);
            Image<uchar3, TargetHost> hRgb8((uchar3*)img[0].Image.data,w,h);
            dI.CopyFrom(hRgb8);
            dKinect.CopyFrom(Image<unsigned short, TargetHost>((unsigned short*)img[1].Image.data,w,h));

            if(applyBilateralFilter) {
                BilateralFilter(dKinectf,dKinect,gs,gr,bilateralWinSize);
            }else{
                ConvertImage<float,unsigned short>(dKinectf, dKinect);
            }

            KinectToVbo(dV, dKinectf, Kdepth(0,0), Kdepth(1,1), Kdepth(0,2), Kdepth(1,2) );
            NormalsFromVbo(dN, dV);

            if(bundle) {
                const Eigen::Matrix<double, 3,4> mKcT_cd = Krgb * T_cd.matrix3x4();
                const Eigen::Matrix<double, 3,4> mT_lr = T_lr.matrix3x4();
                Gpu::LeastSquaresSystem<float,2*6> lss = KinectCalibration(dV, dI, dVr, dIr, mKcT_cd, mT_lr, img_c, dScratch, dDebug);
                Eigen::FullPivLU<Eigen::Matrix<double,12,12> > lu_JTJ( (Eigen::Matrix<double,12,12>)lss.JTJ );
                Eigen::Matrix<double,12,1> x = -1.0 * lu_JTJ.solve( (Eigen::Matrix<double,12,1>)lss.JTy );
//                cout << "-----------------------------------------------" << endl;
//                cout << (Eigen::Matrix<double,12,12>)lss.JTJ << endl;
//                cout << x.transpose() << endl;
                if(calib_update) {
                    T_cd = T_cd * Sophus::SE3::exp(x.segment<6>(0));
                }
                if(pose_update) {
                    T_lr = T_lr * Sophus::SE3::exp(x.segment<6>(6));
                }
                cout << lss.sqErr / lss.obs << endl;
                texdebug << dDebug;
            }

            if(tracker.IsConnected())
            {
                T_lr = T_cd.inverse() * T_cv * tracker.T_wf.inverse() * T_wr;
            }

            if(pose_refinement) {
                for(int i=0; i<2; ++i ) {
                    const Eigen::Matrix<double, 3,4> mKT_lr = Kdepth * T_lr.matrix3x4();
                    const Eigen::Matrix<double, 3,4> mT_rl = T_lr.inverse().matrix3x4();
                    Gpu::LeastSquaresSystem<float,6> lss = PoseRefinementProjectiveIcpPointPlane(
                        dV, dVr, dNr, mKT_lr, mT_rl, icp_c, dScratch, dDebug
                    );
                    Eigen::FullPivLU<Eigen::Matrix<double,6,6> > lu_JTJ( (Eigen::Matrix<double,6,6>)lss.JTJ );
                    Eigen::Matrix<double,6,1> x = -1.0 * lu_JTJ.solve( (Eigen::Matrix<double,6,1>)lss.JTy );
                    if(pose_update) {
                        T_lr = T_lr * Sophus::SE3::exp(x);
                    }
                }
                texdebug << dDebug;
            }

            if(fuse)
            {
//                hm.Fuse(d3d, dCamImg[0], T_wc);
                hm.Fuse(dV, T_wr);
                hm.GenerateVbo(vbo_hm);
//                hm.GenerateCbo(cbo_hm);
            }

            if(Pushed(resetHeightmap) ) {
                Eigen::Matrix4d T_nw = Eigen::Matrix4d::Identity();
                T_nw.block<2,1>(0,3) += Eigen::Vector2d(hm.WidthMeters()/2, hm.HeightMeters() /*/2*/);
                hm.Init(T_nw);
            }

            if(Pushed(save_ref)) {
                dIr.CopyFrom(dI);
                dVr.CopyFrom(dV);
                dNr.CopyFrom(dN);
                T_lr = Sophus::SE3();
            }

            texrgb.Upload(img[0].Image.data,GL_BGR, GL_UNSIGNED_BYTE);
            texdepth.Upload(img[1].Image.data,GL_LUMINANCE, GL_UNSIGNED_SHORT);
            texnorm << dN;

            {
                CudaScopedMappedPtr var(vbo);
                Gpu::Image<float4> dVbo((float4*)*var,w,h);
                dVbo.CopyFrom(dV);
            }

            {
                CudaScopedMappedPtr var(cbo);
                Gpu::Image<uchar4> dCbo((uchar4*)*var,w,h);
                Eigen::Matrix<double,3,4> KT_cd = Krgb * T_cd.matrix3x4();
                ColourVbo(dCbo, dV, dI, KT_cd);
            }
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        container[3].Activate();
        glColor3f(1,1,1);
        GlSlUtilities::Scale(0.5,0.5);
        texnorm.RenderToViewportFlipY();
        GlSlUtilities::UseNone();

        view3d.ActivateAndScissor(s_cam);
        glEnable(GL_DEPTH_TEST);

        static bool lastLockToCam = lockToCam;
        if( lockToCam != lastLockToCam ) {
            if(lockToCam) {
                const Eigen::Matrix4d T_vc = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wr.matrix();
                s_cam.SetModelViewMatrix(T_vc);
            }else{
                const Eigen::Matrix4d T_vw = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wr.inverse().matrix();
                s_cam.SetModelViewMatrix(T_vw);
            }
            lastLockToCam = lockToCam;
        }

        if(lockToCam) glSetFrameOfReferenceF(T_wr.inverse());

        //draw the global heightmap
        if(show_heightmap) {
            glPushMatrix();
            glMultMatrix( hm.T_hw().inverse() );
//            RenderVbo(ibo_hm,vbo_hm,cbo_hm, hm.WidthPixels(), hm.HeightPixels(), show_mesh, show_color);
            RenderVboIbo(vbo_hm,ibo_hm, hm.WidthPixels(), hm.HeightPixels(), show_mesh);
            glPopMatrix();
        }

        {
            glSetFrameOfReferenceF(T_wr);
            {
//                glSetFrameOfReferenceF(T_rc);
//                glDrawAxis(0.5);
//                glUnsetFrameOfReference();

                glSetFrameOfReferenceF(T_lr.inverse());
                glDrawAxis(0.2);
                glColor3f(1,1,1);
                RenderVboCbo(vbo, cbo, w, h);
                glUnsetFrameOfReference();

                glDrawAxis(0.2);
            }
            glUnsetFrameOfReference();

            glColor3f(0.8,0.8,0.8);
            glDraw_z0(1.0,5);

            glSetFrameOfReferenceF(tracker.T_wf);
            {
                glDrawAxis(0.2);
            }
            glUnsetFrameOfReference();
        }

        if(lockToCam) glUnsetFrameOfReference();

        glColor3f(1,1,1);
        pangolin::FinishGlutFrame();
    }
}
