#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <npp.h>

#include <fiducials/drawing.h>

#include "common/RpgCameraOpen.h"
#include "common/DisplayUtils.h"
#include "common/BaseDisplayCuda.h"
#include "common/ImageSelect.h"

#include <kangaroo/kangaroo.h>
#include <kangaroo/variational.h>

#include <Mvlpp/Mvl.h>
#include <Mvlpp/Cameras.h>

using namespace std;
using namespace pangolin;

int main( int argc, char* argv[] )
{
    // Open video device
    CameraDevice video = OpenRpgCamera(argc,argv,1);

    // Capture first image
    std::vector<rpg::ImageWrapper> images;
    video.Capture(images);

    // Image dimensions
    const unsigned int w = images[0].width();
    const unsigned int h = images[0].height();

    // Initialise window
    View& container = SetupPangoGLWithCuda(180+2*w, h,180);
    SetupContainer(container, 3, (float)w/h);

    // Allocate Camera Images on device for processing
    Gpu::Image<unsigned char, Gpu::TargetDevice, Gpu::Manage> img(w,h);
    Gpu::Image<float, Gpu::TargetDevice, Gpu::Manage> imgg(w,h);
    Gpu::Image<float, Gpu::TargetDevice, Gpu::Manage> imgu(w,h);
    Gpu::Image<float2, Gpu::TargetDevice, Gpu::Manage> imgp(w,h);
    Gpu::Image<unsigned char, Gpu::TargetDevice, Gpu::Manage> scratch(w,h);

    ActivateDrawImage<float> adg(imgg, GL_LUMINANCE32F_ARB, false, true);
    ActivateDrawImage<float> adu(imgu, GL_LUMINANCE32F_ARB, false, true);

    container[0].SetDrawFunction(boost::ref(adg));
    container[1].SetDrawFunction(boost::ref(adu));

    Var<bool> nextImage("ui.step", false, false);
    Var<bool> go("ui.go", false, false);

    Var<float> sigma("ui.sigma", 0.01, 0, 1);
    Var<float> tau("ui.tau", 0.01, 0, 1);
    Var<float> lamda("ui.lamda", 0.01, 0, 1);

    for(unsigned long frame=0; !pangolin::ShouldQuit(); ++frame)
    {
        const bool reset = (frame==0) || Pushed(nextImage);

        if(reset) {
            video.Capture(images);
            img.MemcpyFromHost(images[0].Image.data );
            Gpu::ElementwiseScaleBias<float,unsigned char,float>(imgg, img, 1.0f/255.0f);
            imgu.CopyFrom(imgg);
            imgp.Memset(0);
        }

        if(go) {
            Gpu::DenoisingRof_pAscent(imgp,imgu,sigma,scratch);
        }

        /////////////////////////////////////////////////////////////
        // Perform drawing
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);

        pangolin::FinishGlutFrame();
    }
}
