#include <Eigen/Eigen>
#include <Sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>

#include <fiducials/drawing.h>

#include "common/RpgCameraOpen.h"
#include "common/ImageSelect.h"
#include "common/BaseDisplay.h"
#include "common/DisplayUtils.h"

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

    // http://nicolas.burrus.name/index.php/Research/KinectCalibration
    Eigen::Matrix3d Kdepth;
    Kdepth << 5.9421434211923247e+02, 0, 3.3930780975300314e+02,
            0, 5.9104053696870778e+02, 2.4273913761751615e+02,
            0,0,1;

    Eigen::Matrix3d Krgb;
    Krgb << 5.2921508098293293e+02, 0, 3.2894272028759258e+02,
            0, 5.2556393630057437e+02, 2.6748068171871557e+02,
            0,0,1;

    Eigen::Matrix3d R;
    R <<  9.9984628826577793e-01, 1.2635359098409581e-03, -1.7487233004436643e-02,
          -1.4779096108364480e-03, 9.9992385683542895e-01, -1.2251380107679535e-02,
          1.7470421412464927e-02, 1.2275341476520762e-02, 9.9977202419716948e-01;
    Eigen::Vector3d t(1.9985242312092553e-02, -7.4423738761617583e-04, -1.0916736334336222e-02);

    Sophus::SE3 T_dc(R,t);

    Sophus::SE3 T_wd;

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
    Image<uchar3, TargetDevice, Manage> dRgb8(w,h);
    Image<float4, TargetDevice, Manage>  d3d(w,h);

    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    GlTexture texrgb(w,h,GL_RGB8, false);
    GlTextureCudaArray texdepth(w,h,GL_INTENSITY16, false);

    GlBufferCudaPtr vbo(GlArrayBuffer, w*h*sizeof(float4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
    GlBufferCudaPtr cbo(GlArrayBuffer, w*h*sizeof(uchar4), cudaGraphicsMapFlagsWriteDiscard, GL_STREAM_DRAW );
//    GlBufferCudaPtr ibo(GlElementArrayBuffer, w*h*sizeof(uint2) );

    // Create Smart viewports for each camera image that preserve aspect
    const int N = 3;
    for(int i=0; i<N; ++i ) {
        container.AddDisplay(CreateDisplay());
        container[i].SetAspect((double)w/h);
    }
    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrix(w,h,Kdepth(0,0),Kdepth(1,1),Kdepth(0,2),Kdepth(1,2),1E-2,1E3),
        IdentityMatrix(GlModelViewStack)
    );
    View& view3d = CreateDisplay().SetAspect((double)w/h).SetHandler(new Handler3D(s_cam, AxisNone));
    container.AddDisplay(view3d);

    container[0].SetDrawFunction(ActivateDrawTexture(texrgb,true));
    container[1].SetDrawFunction(ActivateDrawTexture(texdepth, true));
    container[1].SetHandler(new Handler2dImageSelect(w,h));

    Var<bool> step("ui.step", false, false);
    Var<bool> run("ui.run", true, true);
    Var<bool> lockToCam("ui.Lock to cam", false, true);

    pangolin::RegisterKeyPressCallback(' ', [&run](){run = !run;} );
    pangolin::RegisterKeyPressCallback('l', [&lockToCam](){lockToCam = !lockToCam;} );
    pangolin::RegisterKeyPressCallback(PANGO_SPECIAL + GLUT_KEY_RIGHT, [&step](){step=true;} );

    for(unsigned long frame=0; !pangolin::ShouldQuit();)
    {        
        const bool go = frame==0 || run || Pushed(step);

        if(go) {
            camera.Capture(img);
            Image<uchar3, TargetHost> hRgb8((uchar3*)img[0].Image.data,w,h);
            dRgb8.CopyFrom(hRgb8);
            dKinect.CopyFrom(Image<unsigned short, TargetHost>((unsigned short*)img[1].Image.data,w,h));

            KinectToVbo(d3d, dKinect, Kdepth(0,0), Kdepth(1,1), Kdepth(0,2), Kdepth(1,2) );

            texrgb.Upload(img[0].Image.data,GL_BGR, GL_UNSIGNED_BYTE);
            texdepth.Upload(img[1].Image.data,GL_LUMINANCE, GL_UNSIGNED_SHORT);

            {
                CudaScopedMappedPtr var(vbo);
                Gpu::Image<float4> dVbo((float4*)*var,w,h);
                dVbo.CopyFrom(d3d);
            }

            {
                CudaScopedMappedPtr var(cbo);
                Gpu::Image<uchar4> dCbo((uchar4*)*var,w,h);
                Eigen::Matrix<double,3,4> KT_cd = Krgb * T_dc.matrix3x4();
                ColourVbo(dCbo, d3d, dRgb8, KT_cd);
            }
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        view3d.ActivateAndScissor(s_cam);
        glEnable(GL_DEPTH_TEST);

        static bool lastLockToCam = lockToCam;
        if( lockToCam != lastLockToCam ) {
            if(lockToCam) {
                const Eigen::Matrix4d T_vc = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wd.matrix();
                s_cam.SetModelViewMatrix(T_vc);
            }else{
                const Eigen::Matrix4d T_vw = (Eigen::Matrix4d)s_cam.GetModelViewMatrix() * T_wd.inverse().matrix();
                s_cam.SetModelViewMatrix(T_vw);
            }
            lastLockToCam = lockToCam;
        }

        if(lockToCam) glSetFrameOfReferenceF(T_wd.inverse());

        {
            glSetFrameOfReferenceF(T_wd);
            {
                glSetFrameOfReferenceF(T_dc);
                glDrawAxis(0.5);
                glUnsetFrameOfReference();
            }

            glDrawAxis(1.0);
            glColor3f(1,1,1);
            RenderVbo(vbo, cbo, w, h);
            glUnsetFrameOfReference();
        }

        if(lockToCam) glUnsetFrameOfReference();

        pangolin::FinishGlutFrame();
    }
}
