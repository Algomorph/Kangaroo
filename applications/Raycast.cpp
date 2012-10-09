#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <npp.h>

#include <SceneGraph/SceneGraph.h>

#include "common/RpgCameraOpen.h"
#include "common/DisplayUtils.h"
#include "common/BaseDisplayCuda.h"
#include "common/ImageSelect.h"

#include <kangaroo/kangaroo.h>

using namespace std;
using namespace pangolin;

int main( int argc, char* argv[] )
{
    const unsigned int w = 512;
    const unsigned int h = 512;
    const float u0 = w /2;
    const float v0 = h /2;
    const float fu = 500;
    const float fv = 500;

    // Initialise window
    View& container = SetupPangoGLWithCuda(2*w, h);
    SceneGraph::GLSceneGraph::ApplyPreferredGlSettings();

    Var<float> near("ui.near",1, 0, 10);
    Var<float> far("ui.far",10, 0, 10);

    // Allocate Camera Images on device for processing
    Gpu::Image<float, Gpu::TargetDevice, Gpu::Manage> img(w,h);
    Gpu::Volume<Gpu::SDF_t, Gpu::TargetDevice, Gpu::Manage> vol(128,128,128);
    ActivateDrawImage<float> adg(img, GL_LUMINANCE32F_ARB, true, true);

    SceneGraph::GLSceneGraph graph;
    SceneGraph::GLAxis glaxis;
    SceneGraph::GLAxisAlignedBox glbox;
    graph.AddChild(&glaxis);
    graph.AddChild(&glbox);

    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrixRDF_TopLeft(w,h, fu,fv, u0,v0, 1E-2,1E3),
        ModelViewLookAtRDF(0,0,0,0,0,1,0,-1,0)
    );

    Handler2dImageSelect handler2d(w,h);
    SetupContainer(container, 2, (float)w/h);
    container[0].SetDrawFunction(boost::ref(adg)).SetHandler(&handler2d);
    container[1].SetDrawFunction(SceneGraph::ActivateDrawFunctor(graph, s_cam))
                .SetHandler( new SceneGraph::HandlerSceneGraph(graph, s_cam, AxisNone) );

    Gpu::SDFSphere(vol, make_float3(vol.w/2,vol.h/2,vol.d/2), vol.w/2.2 );

    for(unsigned long frame=0; !pangolin::ShouldQuit(); ++frame)
    {
        Sophus::SE3 T_cw(s_cam.GetModelViewMatrix());

        {
            const float3 boxmin = make_float3(-1,-1,-1);
            const float3 boxmax = make_float3(1,1,1);
            Gpu::Raycast(img, vol, boxmin, boxmax, T_cw.inverse().matrix3x4(), fu, fv, u0, v0, near, far );
        }

        /////////////////////////////////////////////////////////////
        // Perform drawing
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);

        pangolin::FinishGlutFrame();
    }
}
