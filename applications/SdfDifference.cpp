#include <Eigen/Eigen>
#include <sophus/se3.h>

#include <pangolin/pangolin.h>
#include <pangolin/glcuda.h>
#include <pangolin/glvbo.h>

#include <CVars/CVar.h>

#include <SceneGraph/SceneGraph.h>

#include "common/ViconTracker.h"

#include <fiducials/drawing.h>

#include "common/RpgCameraOpen.h"
#include "common/ImageSelect.h"
#include "common/BaseDisplayCuda.h"
#include "common/DisplayUtils.h"
#include "common/ViconTracker.h"
#include "common/PoseGraph.h"
#include "common/GLPoseGraph.h"
#include "common/Handler3dGpuDepth.h"
#include "common/SavePPM.h"
#include "common/SaveGIL.h"
//#include "common/SaveMeshlab.h"
#include "common/CVarHelpers.h"

#include "MarchingCubes.h"

#include <kangaroo/kangaroo.h>
#include <kangaroo/variational.h>
#include <kangaroo/BoundedVolume.h>

using namespace std;
using namespace pangolin;

int main( int argc, char* argv[] )
{
    // Initialise window
    View& container = SetupPangoGLWithCuda(1024, 768);
    SceneGraph::GLSceneGraph::ApplyPreferredGlSettings();

    // Open video device
    const int w = 640;
    const int h = 480;

    const Gpu::ImageIntrinsics K(
        570.342,
        570.342,
        w/2.0 - 0.5, h/2.0 - 0.5
    );
    const float volrad = 0.6;

    Gpu::BoundingBox reset_bb(make_float3(-volrad,-volrad,0.5), make_float3(volrad,volrad,0.5+2*volrad));
//    Gpu::BoundingBox reset_bb(make_float3(-volrad,-volrad,-volrad), make_float3(volrad,volrad,volrad));

    CVarUtils::AttachCVar<Gpu::BoundingBox>("BoundingBox", &reset_bb);

    Gpu::Image<float,  Gpu::TargetDevice, Gpu::Manage> ray_i(w,h);
    Gpu::Image<float,  Gpu::TargetDevice, Gpu::Manage> ray_d(w,h);
    Gpu::Image<float4, Gpu::TargetDevice, Gpu::Manage> ray_n(w,h);
    Gpu::BoundedVolume<Gpu::SDF_t, Gpu::TargetDevice, Gpu::Manage> vol;
    Gpu::BoundedVolume<Gpu::SDF_t, Gpu::TargetDevice, Gpu::Manage> vol2;
    
    LoadPXM("save.vol", vol);
    LoadPXM("save2.vol", vol2);
    vol.bbox = reset_bb;
    vol2.bbox = reset_bb;    

    SceneGraph::GLSceneGraph glgraph;
    SceneGraph::GLAxisAlignedBox glboxvol;

    glboxvol.SetBounds(Gpu::ToEigen(vol.bbox.Min()), Gpu::ToEigen(vol.bbox.Max()) );
    glgraph.AddChild(&glboxvol);

    pangolin::OpenGlRenderState s_cam(
        ProjectionMatrixRDF_TopLeft(w,h,K.fu,K.fv,K.u0,K.v0,0.1,1000),
        ModelViewLookAtRDF(0,0,-2,0,0,0,0,-1,0)
    );

    Var<float> trunc_dist_factor("ui.trunc vol factor",2, 1, 4);

    ActivateDrawImage<float> adrayimg(ray_i, GL_LUMINANCE32F_ARB, true, true);

    Handler3DGpuDepth rayhandler(ray_d, s_cam, AxisNone);
    SetupContainer(container, 2, (float)w/h);
    container[0].SetDrawFunction(boost::ref(adrayimg))
                .SetHandler(&rayhandler);
    container[1].SetDrawFunction(SceneGraph::ActivateDrawFunctor(glgraph, s_cam))
                .SetHandler( new Handler3D(s_cam, AxisNone) );

    while(!pangolin::ShouldQuit())
    {
        const float trunc_dist = trunc_dist_factor*length(vol.VoxelSizeUnits());

        Sophus::SE3 T_vw(s_cam.GetModelViewMatrix());
        const Gpu::BoundingBox roi(T_vw.inverse().matrix3x4(), w, h, K, 0, 50);
        Gpu::BoundedVolume<Gpu::SDF_t> work_vol = vol.SubBoundingVolume( roi );
        Gpu::BoundedVolume<Gpu::SDF_t> work_vol2 = vol2.SubBoundingVolume( roi );
        if(work_vol.IsValid()) {
            Gpu::RaycastSdf(ray_d, ray_n, ray_i, work_vol, T_vw.inverse().matrix3x4(), K, 0.1, 50, trunc_dist, true );
            Gpu::SdfDistance(ray_i, ray_d, work_vol2, T_vw.inverse().matrix3x4(), K, trunc_dist);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1,1,1);
        pangolin::FinishGlutFrame();
    }
}
