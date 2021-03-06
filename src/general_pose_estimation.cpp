#include <iostream>
#include <pcl/point_types.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/point_cloud.h>
#include <pcl/correspondence.h>
#include <pcl/features/shot_omp.h>
#include <pcl/features/board.h>
#include <pcl/recognition/cg/hough_3d.h>
#include <pcl/recognition/cg/geometric_consistency.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/kdtree/impl/kdtree_flann.hpp>
#include <pcl/common/transforms.h>
#include <pcl/console/parse.h>
#include <pcl/registration/icp.h>
#include <Eigen/StdVector>
#include <libgen.h>
#include <string>

typedef pcl::PointXYZ PointT;
typedef pcl::PointCloud<PointT> PointCloudT;
typedef pcl::Normal NormalT;
typedef pcl::ReferenceFrame RFT;
typedef pcl::SHOT352 DescriptorT;

enum PointGenType
{
    RANDOM=0,
    FROMPCD,
    FROMKINECT
};

void printTransformMatrix(Eigen::Matrix4f transformation)
{
    printf ("            | %6.3f %6.3f %6.3f | \n", transformation(0,0), transformation(0,1), transformation(0,2));
    printf ("        R = | %6.3f %6.3f %6.3f | \n", transformation(1,0), transformation(1,1), transformation(1,2));
    printf ("            | %6.3f %6.3f %6.3f | \n", transformation(2,0), transformation(2,1), transformation(2,2));
    printf ("\n");
    printf ("        t = < %0.3f, %0.3f, %0.3f >\n", transformation(0,3), transformation(1,3), transformation(2,3));
}

//Acquire point Clouds from pcd file or from kinect,
// or randomly generation;
class PointCloudGrabber
{
private:
    PointCloudT::Ptr sourceCloud;
public:
    std::string pcdPath;
    PointCloudGrabber():sourceCloud(new PointCloudT){}

    ~PointCloudGrabber(){}

    PointCloudT::Ptr getPointCloud(PointGenType type)
    {
        clearSourceCloud();

        if(type==RANDOM)
        {
            //Todo
        }
        else if(type==FROMPCD)
        {
            if(pcl::io::loadPCDFile(pcdPath,*sourceCloud)<0)
            {
                std::cerr<<"Load pcd file "<<pcdPath<<" failed!"<<std::endl;
            }
        }
        else if(type==FROMKINECT)
        {
            //Todo
        }

        return sourceCloud;
    }

protected:
    void clearSourceCloud()
    {
        sourceCloud->width=0;
        sourceCloud->height=0;
        sourceCloud->points.resize(0);
    }

};

struct corrsRes
{
    int corrsSize;
    int clusterCorrsSize;
    int cluster_index;
};
// This class can estimate the accurate pose of
// an object from a scene point cloud;
// The process pipeline includes:(1)Preprocess(downsample,filter)
// (2)segmentation; (3)recognition of target object;
// and (4)estimate the transformation by ICP algorithm;
class generalPoseEstimator
{
private:
    PointCloudT::Ptr objectCloud;
    PointCloudT::Ptr sceneCloud;
    PointCloudT::Ptr preProcessedObjectCloud;
    PointCloudT::Ptr preProcessedSceneCloud;
    PointCloudT::Ptr recognizedCloud;
    pcl::PointCloud<pcl::Normal>::Ptr objectNormals;
    std::vector<PointCloudT,Eigen::aligned_allocator<PointCloudT> > cluster_pointclouds;
    pcl::PointCloud<DescriptorT>::Ptr object_descriptors;
    Eigen::Matrix4f recog_transformation;
    Eigen::Matrix4f precise_transformation;
    PointCloudT::Ptr cloud_rmplane;
    corrsRes _corrsRes;
    std::vector<Eigen::Matrix4f, Eigen::aligned_allocator<Eigen::Matrix4f> > rototranslations;

    bool use_hough_=true;
    float model_ss_=0.01f;
    float scene_ss_=0.03f;
    float rf_rad_=0.015f;
    float descr_rad_=0.02f;
    float cg_size_=0.01f;
    float cg_thresh_=5.0f;

public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    //parameters
    float pass_Zmax = 5.0f; //pass through filter
    float pass_Zmin = 0.0f; //pass through filter
    float dsLeafSize = 0.005f; //down sample leaf size


    //member functions
    generalPoseEstimator():
    objectCloud(new PointCloudT),
    sceneCloud(new PointCloudT),
    preProcessedObjectCloud(new PointCloudT),
    preProcessedSceneCloud(new PointCloudT),
    recognizedCloud(new PointCloudT),
    objectNormals(new pcl::PointCloud<pcl::Normal>),
    object_descriptors(new pcl::PointCloud<DescriptorT> ()),
    cloud_rmplane(new PointCloudT)
    {
        _corrsRes.corrsSize=0;
        _corrsRes.clusterCorrsSize=0;
        _corrsRes.cluster_index=0;
        recog_transformation=Eigen::Matrix4f::Identity();
        precise_transformation=Eigen::Matrix4f::Identity();
    }

    ~generalPoseEstimator(){}

    void setInputSceneCloud(PointCloudT::Ptr scene_cloud)
    {
        sceneCloud=scene_cloud;
    }

    void setInputObjectCloud(PointCloudT::Ptr object_cloud)
    {
        objectCloud=object_cloud;
    }

    void estimate()
    {
        preProcessCloud();
        segmentClusters();
        int idx=0;
        for (int i=0;i<cluster_pointclouds.size(); ++i)
        {
            PointCloudT::Ptr scene_cluster(new PointCloudT);
            scene_cluster=static_cast<PointCloudT>(cluster_pointclouds[i]).makeShared();
            int corrs_size=recognizeObject(preProcessedObjectCloud,objectNormals,scene_cluster);
            if(_corrsRes.corrsSize<corrs_size && _corrsRes.clusterCorrsSize!=0)
            {
                _corrsRes.corrsSize=corrs_size;
                _corrsRes.cluster_index=idx;
                recog_transformation=rototranslations[0].block<4,4>(0,0);
            }

            idx++;
        }

        recognizedCloud=(cluster_pointclouds[_corrsRes.cluster_index]).makeShared();
        PointCloudT::Ptr tmpCloud(new PointCloudT);
        pcl::transformPointCloud(*preProcessedObjectCloud,*tmpCloud,recog_transformation);
        precisePoseEstimate(recognizedCloud,tmpCloud);

        //Transform Info
        printf ("Recognization Estimation:\n");
        printTransformMatrix(recog_transformation);

        //Transform Info
        precise_transformation=recog_transformation*precise_transformation;
        printf ("Precise Estimation:\n");
        printTransformMatrix(precise_transformation);

        //Visualization
        pcl::visualization::PCLVisualizer viewer_scene("scene");
        pcl::visualization::PCLVisualizer viewer_object("object");
        pcl::visualization::PCLVisualizer viewer_recog("Recognized cluster");

        viewer_scene.addPointCloud(preProcessedSceneCloud,"scene");
        viewer_object.addPointCloud(preProcessedObjectCloud,"object");
        viewer_recog.addPointCloud((cluster_pointclouds[_corrsRes.cluster_index]).makeShared(),"recog");

        while(!viewer_scene.wasStopped())
        {
            viewer_scene.spinOnce();
            viewer_object.spinOnce();
            viewer_recog.spinOnce();
        }
    }

protected:
    void preProcessCloud(void)
    {
        PointCloudT::Ptr filteredObjectCloud(new PointCloudT);
        PointCloudT::Ptr filteredSceneCloud(new PointCloudT);
        //pass through filter
        pcl::PassThrough<PointT> pass;
        pass.setFilterFieldName("z");
        pass.setFilterLimits(pass_Zmin,pass_Zmax);
        pass.setInputCloud(objectCloud);
        pass.filter(*filteredObjectCloud);
        std::cout<<"After pass through filtering, the size of object points is "<<filteredObjectCloud->points.size()<<std::endl;
        pass.setInputCloud(sceneCloud);
        pass.filter(*filteredSceneCloud);
        std::cout<<"After pass through filtering, the size of scene points is "<<filteredSceneCloud->points.size()<<std::endl;

        //Downsample
        pcl::VoxelGrid<PointT> grid;
        grid.setLeafSize(dsLeafSize,dsLeafSize,dsLeafSize);
        PointCloudT::Ptr tempCloud1(new PointCloudT);
        PointCloudT::Ptr tempCloud2(new PointCloudT);
        grid.setInputCloud(filteredObjectCloud);
        grid.filter(*tempCloud1);
        filteredObjectCloud=tempCloud1;
        std::cout<<"After down sampling, the size of object points is "<<filteredObjectCloud->points.size()<<std::endl;
        grid.setInputCloud(filteredSceneCloud);
        grid.filter(*tempCloud2);
        filteredSceneCloud=tempCloud2;
        std::cout<<"After down sampling, the size of scene points is "<<filteredSceneCloud->points.size()<<std::endl;

        //Compute Normals
        pcl::NormalEstimationOMP<PointT, pcl::Normal> norm_est;
        norm_est.setKSearch (10);
        norm_est.setInputCloud (filteredObjectCloud);
        norm_est.compute (*objectNormals);

        preProcessedObjectCloud=filteredObjectCloud;
        preProcessedSceneCloud=filteredSceneCloud;
    }

    void segmentClusters(void)
    {
        //Segment the planes
        pcl::SACSegmentation<PointT> seg;
        pcl::PointIndices::Ptr inliers(new pcl::PointIndices);
        pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);

        seg.setOptimizeCoefficients(true);
        seg.setModelType(pcl::SACMODEL_PLANE);
        seg.setMethodType(pcl::SAC_RANSAC);
        seg.setMaxIterations(100);
        seg.setDistanceThreshold(0.02);
        seg.setInputCloud(preProcessedSceneCloud);
        seg.segment(*inliers,*coefficients);

        //Extract
        pcl::ExtractIndices<PointT> extract;
        extract.setInputCloud(preProcessedSceneCloud);
        extract.setIndices(inliers);
        extract.setNegative(true);
        extract.filter(*cloud_rmplane);
        std::cout<<"removed plane of scene point cloud"<<std::endl;

        //Euclidean cluster extraction
        pcl::search::KdTree<PointT>::Ptr tree (new pcl::search::KdTree<PointT>);
        tree->setInputCloud (cloud_rmplane);
        std::vector<pcl::PointIndices> cluster_indices;
        pcl::EuclideanClusterExtraction<PointT> ec;
        ec.setClusterTolerance (0.02); // 2cm
        ec.setMinClusterSize (10);
        ec.setMaxClusterSize (45000);
        ec.setSearchMethod (tree);
        ec.setInputCloud (cloud_rmplane);
        ec.extract (cluster_indices);
        std::cout<<"segmentation results: totally "<<cluster_indices.size()<<" clusters"<<std::endl;
        
        int j = 0;
        for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it)
        {
          PointCloudT::Ptr cloud_cluster (new PointCloudT);
          for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); ++pit)
          {
            cloud_cluster->points.push_back (cloud_rmplane->points[*pit]); //*
          }
          std::cout<<"Cluster "<<j<<" has "<<cloud_cluster->points.size ()<<" points"<<std::endl;
          cloud_cluster->width = cloud_cluster->points.size ();
          cloud_cluster->height = 1;
          cloud_cluster->is_dense = true;
          //cluster_pointclouds.insert(j,*cloud_cluster);
          cluster_pointclouds.push_back(*cloud_cluster);
          j++;
        }
    }

    //Recognize the target object from a cluster of point clouds, and estimate the transformation
    int recognizeObject(PointCloudT::Ptr objectCloud,pcl::PointCloud<NormalT>::Ptr objectNormals,PointCloudT::Ptr sceneCloud)
    {
        pcl::PointCloud<NormalT>::Ptr scene_normals(new pcl::PointCloud<NormalT>());
        //Compute Normals
        pcl::NormalEstimationOMP<PointT, pcl::Normal> norm_est;
        norm_est.setKSearch (10);
        norm_est.setInputCloud (sceneCloud);
        norm_est.compute (*scene_normals);

        //  Compute Descriptor for keypoints
        pcl::SHOTEstimationOMP<PointT, NormalT, DescriptorT> descr_est;
        descr_est.setRadiusSearch (descr_rad_);

        descr_est.setInputCloud (objectCloud);
        descr_est.setInputNormals (objectNormals);
        descr_est.setSearchSurface (objectCloud);
        descr_est.compute (*object_descriptors);

        pcl::PointCloud<DescriptorT>::Ptr scene_descriptors(new pcl::PointCloud<DescriptorT>());
        descr_est.setInputCloud (sceneCloud);
        descr_est.setInputNormals (scene_normals);
        descr_est.setSearchSurface (sceneCloud);
        descr_est.compute (*scene_descriptors);

        //  Find Model-Scene Correspondences with KdTree
        pcl::CorrespondencesPtr object_scene_corrs (new pcl::Correspondences ());

        pcl::KdTreeFLANN<DescriptorT> match_search;
        match_search.setInputCloud (object_descriptors);

        //  For each scene keypoint descriptor, find nearest neighbor into the model keypoints descriptor cloud and add it to the correspondences vector.
        for (size_t i = 0; i < scene_descriptors->size (); ++i)
        {
          std::vector<int> neigh_indices (1);
          std::vector<float> neigh_sqr_dists (1);
          if (!pcl_isfinite (scene_descriptors->at (i).descriptor[0])) //skipping NaNs
          {
            continue;
          }
          int found_neighs = match_search.nearestKSearch (scene_descriptors->at (i), 1, neigh_indices, neigh_sqr_dists);
          if(found_neighs == 1 && neigh_sqr_dists[0] < 0.25f) //  add match only if the squared descriptor distance is less than 0.25 (SHOT descriptor distances are between 0 and 1 by design)
          {
            pcl::Correspondence corr (neigh_indices[0], static_cast<int> (i), neigh_sqr_dists[0]);
            object_scene_corrs->push_back (corr);
          }
        }
        std::cout << "Correspondences found: " << object_scene_corrs->size () << std::endl;

        //  Actual Clustering
        std::vector<pcl::Correspondences> clustered_corrs;

        //  Using Hough3D
        if (use_hough_)
        {
          //  Compute (Keypoints) Reference Frames only for Hough
          pcl::PointCloud<RFT>::Ptr object_rf (new pcl::PointCloud<RFT> ());
          pcl::PointCloud<RFT>::Ptr scene_rf (new pcl::PointCloud<RFT> ());

          pcl::BOARDLocalReferenceFrameEstimation<PointT, NormalT, RFT> rf_est;
          rf_est.setFindHoles (true);
          rf_est.setRadiusSearch (rf_rad_);

          rf_est.setInputCloud (objectCloud);
          rf_est.setInputNormals (objectNormals);
          rf_est.setSearchSurface (objectCloud);
          rf_est.compute (*object_rf);

          rf_est.setInputCloud (sceneCloud);
          rf_est.setInputNormals (scene_normals);
          rf_est.setSearchSurface (sceneCloud);
          rf_est.compute (*scene_rf);

          //  Clustering
          pcl::Hough3DGrouping<PointT, PointT, RFT, RFT> clusterer;
          clusterer.setHoughBinSize (cg_size_);
          clusterer.setHoughThreshold (cg_thresh_);
          clusterer.setUseInterpolation (true);
          clusterer.setUseDistanceWeight (false);

          clusterer.setInputCloud (objectCloud);
          clusterer.setInputRf (object_rf);
          clusterer.setSceneCloud (sceneCloud);
          clusterer.setSceneRf (scene_rf);
          clusterer.setModelSceneCorrespondences (object_scene_corrs);

          //clusterer.cluster (clustered_corrs);
          clusterer.recognize (rototranslations, clustered_corrs);
        }
        else // Using GeometricConsistency
        {
          pcl::GeometricConsistencyGrouping<PointT, PointT> gc_clusterer;
          gc_clusterer.setGCSize (cg_size_);
          gc_clusterer.setGCThreshold (cg_thresh_);

          gc_clusterer.setInputCloud (objectCloud);
          gc_clusterer.setSceneCloud (sceneCloud);
          gc_clusterer.setModelSceneCorrespondences (object_scene_corrs);

          //gc_clusterer.cluster (clustered_corrs);
          gc_clusterer.recognize (rototranslations, clustered_corrs);
        }

        _corrsRes.clusterCorrsSize=clustered_corrs.size();
        std::cout<<"correspondence size: "<<clustered_corrs.size()<<std::endl;

        return object_scene_corrs->size();
    }

    void precisePoseEstimate(PointCloudT::Ptr targetCloud, PointCloudT::Ptr sourceCloud)
    {
        pcl::IterativeClosestPoint<PointT,PointT> icp;
        icp.setMaximumIterations(5000);
        icp.setInputSource(sourceCloud);
        icp.setInputTarget(targetCloud);
        icp.align(*sourceCloud);
        precise_transformation=icp.getFinalTransformation();
    }

};

int
main(int argc, char** argv)
{
    double theta=M_PI/4;

    std::string scenePath,objectPath;
    std::string baseDir=dirname(argv[0]);
    scenePath=baseDir+"/pcd/milk_cartoon_all_small_clorox.pcd";
    objectPath=baseDir+"/pcd/milk.pcd";
    //scene
    PointCloudGrabber grabber1;
    PointCloudT::Ptr sceneCloud(new PointCloudT);
    std::cout<<"before grabbing cloud"<<std::endl;
    grabber1.pcdPath=scenePath;
    sceneCloud=grabber1.getPointCloud(FROMPCD);
    std::cout<<"grab scene cloud"<<std::endl;
    //Transform the scene to test
    Eigen::Matrix4f transformation_matrix=Eigen::Matrix4f::Identity();
    transformation_matrix(0,0)=cos(theta);
    transformation_matrix(0,1)=-sin(theta);
    transformation_matrix(1,0)=sin(theta);
    transformation_matrix(1,1)=cos(theta);
    transformation_matrix(2,3)=0.4;
    PointCloudT::Ptr trans_sceneCloud(new PointCloudT);
    pcl::transformPointCloud(*sceneCloud,*trans_sceneCloud,transformation_matrix);
    sceneCloud=trans_sceneCloud;
    //Transform Info
    printf ("Real transformation:\n");
    printTransformMatrix(transformation_matrix);

    //object
    PointCloudGrabber grabber2;
    PointCloudT::Ptr objectCloud(new PointCloudT);
    grabber2.pcdPath=objectPath;
    objectCloud=grabber2.getPointCloud(FROMPCD);

    generalPoseEstimator estimator;
    estimator.setInputSceneCloud(sceneCloud);
    estimator.setInputObjectCloud(objectCloud);
    estimator.estimate();

    return(0);
}
