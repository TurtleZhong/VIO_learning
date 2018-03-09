#include <iostream>
#include <algorithm>
#include <set>
#include <Eigen/Dense>
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <sensor_msgs/image_encodings.h>
#include <random_numbers/random_numbers.h>

#include <msckf_mono/image_processor.h>
#include <msckf_mono/utils.h>

using namespace std;
using namespace cv;
using namespace Eigen;

namespace msckf_mono
{
ImageProcessor::ImageProcessor(ros::NodeHandle& n) :
    nh(n),
    is_first_img(true),
    //img_transport(n),
    prev_features_ptr(new GridFeatures()),
    curr_features_ptr(new GridFeatures()) {
    return;
}

ImageProcessor::~ImageProcessor()
{
    destroyAllWindows();
    //ROS_INFO("Feature lifetime statistics:");
    //featureLifetimeStatistics();
    return;
}

bool ImageProcessor::loadParameters()
{
    // Camera calibration parameters
    nh.param<string>("cam0/distortion_model",
                     cam0_distortion_model, string("radtan"));

    vector<int> cam0_resolution_temp(2);
    nh.getParam("cam0/resolution", cam0_resolution_temp);
    cam0_resolution[0] = cam0_resolution_temp[0];
    cam0_resolution[1] = cam0_resolution_temp[1];

    vector<double> cam0_intrinsics_temp(4);
    nh.getParam("cam0/intrinsics", cam0_intrinsics_temp);
    cam0_intrinsics[0] = cam0_intrinsics_temp[0];
    cam0_intrinsics[1] = cam0_intrinsics_temp[1];
    cam0_intrinsics[2] = cam0_intrinsics_temp[2];
    cam0_intrinsics[3] = cam0_intrinsics_temp[3];

    vector<double> cam0_distortion_coeffs_temp(4);
    nh.getParam("cam0/distortion_coeffs",
                cam0_distortion_coeffs_temp);
    cam0_distortion_coeffs[0] = cam0_distortion_coeffs_temp[0];
    cam0_distortion_coeffs[1] = cam0_distortion_coeffs_temp[1];
    cam0_distortion_coeffs[2] = cam0_distortion_coeffs_temp[2];
    cam0_distortion_coeffs[3] = cam0_distortion_coeffs_temp[3];

    cv::Mat     T_imu_cam0 = utils::getTransformCV(nh, "cam0/T_cam_imu");
    cv::Matx33d R_imu_cam0(T_imu_cam0(cv::Rect(0,0,3,3)));
    cv::Vec3d   t_imu_cam0 = T_imu_cam0(cv::Rect(3,0,1,3));
    R_cam0_imu = R_imu_cam0.t();
    t_cam0_imu = -R_imu_cam0.t() * t_imu_cam0;

    cv::Mat T_cam0_cam1 = utils::getTransformCV(nh, "cam1/T_cn_cnm1");
    cv::Mat T_imu_cam1 = T_cam0_cam1 * T_imu_cam0;
    cv::Matx33d R_imu_cam1(T_imu_cam1(cv::Rect(0,0,3,3)));
    cv::Vec3d   t_imu_cam1 = T_imu_cam1(cv::Rect(3,0,1,3));
    R_cam1_imu = R_imu_cam1.t();
    t_cam1_imu = -R_imu_cam1.t() * t_imu_cam1;

    // Processor parameters
    nh.param<int>("grid_row", processor_config.grid_row, 4);
    nh.param<int>("grid_col", processor_config.grid_col, 4);
    nh.param<int>("grid_min_feature_num",
                  processor_config.grid_min_feature_num, 2);
    nh.param<int>("grid_max_feature_num",
                  processor_config.grid_max_feature_num, 4);
    nh.param<int>("pyramid_levels",
                  processor_config.pyramid_levels, 3);
    nh.param<int>("patch_size",
                  processor_config.patch_size, 31);
    nh.param<int>("fast_threshold",
                  processor_config.fast_threshold, 20);
    nh.param<int>("max_iteration",
                  processor_config.max_iteration, 30);
    nh.param<double>("track_precision",
                     processor_config.track_precision, 0.01);
    nh.param<double>("ransac_threshold",
                     processor_config.ransac_threshold, 3);
    nh.param<double>("stereo_threshold",
                     processor_config.stereo_threshold, 3);

    ROS_INFO("===========================================");
    ROS_INFO("cam0_resolution: %d, %d",
             cam0_resolution[0], cam0_resolution[1]);
    ROS_INFO("cam0_intrinscs: %f, %f, %f, %f",
             cam0_intrinsics[0], cam0_intrinsics[1],
            cam0_intrinsics[2], cam0_intrinsics[3]);
    ROS_INFO("cam0_distortion_model: %s",
             cam0_distortion_model.c_str());
    ROS_INFO("cam0_distortion_coefficients: %f, %f, %f, %f",
             cam0_distortion_coeffs[0], cam0_distortion_coeffs[1],
            cam0_distortion_coeffs[2], cam0_distortion_coeffs[3]);

    cout << R_imu_cam0 << endl;
    cout << t_imu_cam0.t() << endl;

    ROS_INFO("grid_row: %d",
             processor_config.grid_row);
    ROS_INFO("grid_col: %d",
             processor_config.grid_col);
    ROS_INFO("grid_min_feature_num: %d",
             processor_config.grid_min_feature_num);
    ROS_INFO("grid_max_feature_num: %d",
             processor_config.grid_max_feature_num);
    ROS_INFO("pyramid_levels: %d",
             processor_config.pyramid_levels);
    ROS_INFO("patch_size: %d",
             processor_config.patch_size);
    ROS_INFO("fast_threshold: %d",
             processor_config.fast_threshold);
    ROS_INFO("max_iteration: %d",
             processor_config.max_iteration);
    ROS_INFO("track_precision: %f",
             processor_config.track_precision);
    ROS_INFO("ransac_threshold: %f",
             processor_config.ransac_threshold);
    ROS_INFO("stereo_threshold: %f",
             processor_config.stereo_threshold);
    ROS_INFO("===========================================");
    return true;
}

bool ImageProcessor::createRosIO()
{
    //    feature_pub = nh.advertise<CameraMeasurement>(
    //                "features", 3);
    //    tracking_info_pub = nh.advertise<TrackingInfo>(
    //                "tracking_info", 1);
    //    image_transport::ImageTransport it(nh);
    //    debug_stereo_pub = it.advertise("debug_stereo_image", 1);

    cam0_img_sub = nh.subscribe("cam0_image", 10,
                                &ImageProcessor::monoCallback, this);
    imu_sub = nh.subscribe("imu", 50,
                           &ImageProcessor::imuCallback, this);

    return true;
}

bool ImageProcessor::initialize() {
    if (!loadParameters()) return false;
    ROS_INFO("Finish loading ROS parameters...");

    // Create feature detector.
    detector_ptr = FastFeatureDetector::create(
                processor_config.fast_threshold); //10

    if (!createRosIO()) return false;
    ROS_INFO("Finish creating ROS IO...");

    return true;
}


void ImageProcessor::monoCallback(
        const sensor_msgs::ImageConstPtr &cam0_img)
{
    // Get the current image.
    cam0_curr_img_ptr = cv_bridge::toCvShare(cam0_img,
                                             sensor_msgs::image_encodings::MONO8);
    //    const Mat &curr_cam0_image = cam0_curr_img_ptr->image;
    //    imshow("curr_image", curr_cam0_image);
    //    waitKey(27);

    // Build the image pyramids once since they're used at multiple places
    createImagePyramids();

    if(is_first_img)
    {
        // Detect features in the first frame
        initializeFirstFrame();
        is_first_img = false;
    }
    else
    {
        // Track the feature in the previous image.
        ROS_INFO("Need to track the features");
        trackFeatures();

        drawFeaturesMono();
    }

    // Update the previous image and previous features.
    cam0_prev_img_ptr = cam0_curr_img_ptr;
    prev_features_ptr = curr_features_ptr;
    std::swap(prev_cam0_pyramid_, curr_cam0_pyramid_);

    // Initialize the current features to empty vectors.
    curr_features_ptr.reset(new GridFeatures());
    for (int code = 0; code <
         processor_config.grid_row*processor_config.grid_col; ++code) {
        (*curr_features_ptr)[code] = vector<FeatureMetaData>(0);
    }

}

void ImageProcessor::imuCallback(
        const sensor_msgs::ImuConstPtr& msg)
{
    // Wait for the first image to be set.
    if (is_first_img) return;
    imu_msg_buffer.push_back(*msg);
    return;
}

void ImageProcessor::createImagePyramids()
{
    const Mat& curr_cam0_img = cam0_curr_img_ptr->image;
    buildOpticalFlowPyramid(
                curr_cam0_img, curr_cam0_pyramid_,
                Size(processor_config.patch_size, processor_config.patch_size),
                processor_config.pyramid_levels, true, BORDER_REFLECT_101,
                BORDER_CONSTANT, false);
}

void ImageProcessor::initializeFirstFrame()
{
    // Size of each grid.
    const Mat& img = cam0_curr_img_ptr->image;
    static int grid_height = img.rows / processor_config.grid_row;
    static int grid_width = img.cols / processor_config.grid_col;

    // Detect new features on the frist image.
    vector<KeyPoint> new_features(0);
    detector_ptr->detect(img, new_features); //FastFeatureDetector

    Mat out_img = img.clone();
    cvtColor(out_img, out_img, CV_GRAY2BGR);
    //    drawKeypoints(img, new_features, out_img);
    //    imshow("first_frame", out_img);
    //    ROS_INFO("Feature.size: %d",
    //             new_features.size());
    //    waitKey(0);

    // Find the stereo matched points for the newly
    // detected features.
    vector<cv::Point2f> cam0_points(new_features.size());
    for (int i = 0; i < new_features.size(); ++i)
        cam0_points[i] = new_features[i].pt;

    vector<unsigned char> inlier_markers(cam0_points.size(), 1);
    //    stereoMatch(cam0_points, cam1_points, inlier_markers);


    vector<cv::Point2f> cam0_inliers(0);
    vector<float> response_inliers(0);
    for (int i = 0; i < inlier_markers.size(); ++i)
    {
        if (inlier_markers[i] == 0) continue;
        cam0_inliers.push_back(cam0_points[i]);
        response_inliers.push_back(new_features[i].response);
    }

    // Group the features into grids
    GridFeatures grid_new_features;
    for (int code = 0; code <
         processor_config.grid_row*processor_config.grid_col; ++code)
        grid_new_features[code] = vector<FeatureMetaData>(0);

    for (int i = 0; i < cam0_inliers.size(); ++i) {
        const cv::Point2f& cam0_point = cam0_inliers[i];
        const float& response = response_inliers[i];

        int row = static_cast<int>(cam0_point.y / grid_height);
        int col = static_cast<int>(cam0_point.x / grid_width);
        int code = row*processor_config.grid_col + col;

        FeatureMetaData new_feature;
        new_feature.response = response;
        new_feature.cam0_point = cam0_point;
        grid_new_features[code].push_back(new_feature);
    }

    // Sort the new features in each grid based on its response.
    for (auto& item : grid_new_features)
        std::sort(item.second.begin(), item.second.end(),
                  &ImageProcessor::featureCompareByResponse);

    // Collect new features within each grid with high response.
    for (int code = 0; code <
         processor_config.grid_row*processor_config.grid_col; ++code) {
        vector<FeatureMetaData>& features_this_grid = (*curr_features_ptr)[code];
        vector<FeatureMetaData>& new_features_this_grid = grid_new_features[code];

        for (int k = 0; k < processor_config.grid_min_feature_num &&
             k < new_features_this_grid.size(); ++k) {
            features_this_grid.push_back(new_features_this_grid[k]);
            features_this_grid.back().id = next_feature_id++;
            features_this_grid.back().lifetime = 1;
        }
    }

    for (int code = 0; code < processor_config.grid_row * processor_config.grid_col; ++code)
    {
        vector<FeatureMetaData> &features_this_grid = (*curr_features_ptr)[code];
        for(int i = 0; i < features_this_grid.size(); i++)
        {
            circle(out_img, features_this_grid[i].cam0_point,
                   3, Scalar(0, 255, 0), -1);
        }
    }

    imshow("features", out_img);
    waitKey(27);

    return;
}

void ImageProcessor::trackFeatures()
{
    // Size of grid.
    static int grid_height =
            cam0_curr_img_ptr->image.rows / processor_config.grid_row;
    static int grid_width =
            cam0_curr_img_ptr->image.cols / processor_config.grid_col;

    // Compute a rough relative rotation from pre frame to curr frame.
    Matx33f cam0_R_p_c;
    integrateImuData(cam0_R_p_c);

    // Organize the features in the previous image.
    vector<FeatureIDType> prev_ids(0);
    vector<int> prev_lifetime(0);
    vector<Point2f> prev_cam0_points(0);
    for(const auto& item : (*prev_features_ptr))
    {
        for(const auto& prev_feature : item.second )
        {
            prev_ids.push_back(prev_feature.id);
            prev_lifetime.push_back(prev_feature.lifetime);
            prev_cam0_points.push_back(prev_feature.cam0_point);
        }
    }

    // Number of the features before tracking.
    before_tracking = prev_cam0_points.size();

    if(prev_ids.size() == 0) return;

    // Track features using LK optical flow method.
    vector<Point2f> curr_cam0_points(0);
    vector<unsigned char> track_inliers(0);

    predictFeatureTracking(prev_cam0_points,
                           cam0_R_p_c,
                           cam0_intrinsics,
                           curr_cam0_points);
    calcOpticalFlowPyrLK(
                prev_cam0_pyramid_, curr_cam0_pyramid_,
                prev_cam0_points, curr_cam0_points,
                track_inliers, noArray(),
                Size(processor_config.patch_size, processor_config.patch_size),
                processor_config.pyramid_levels,
                TermCriteria(TermCriteria::COUNT+TermCriteria::EPS,
                             processor_config.max_iteration,
                             processor_config.track_precision),
                cv::OPTFLOW_USE_INITIAL_FLOW);

    // Mark those tracked points out of image region as untracked
    for(int i = 0; i < curr_cam0_points.size(); ++i)
    {
        if(track_inliers[i] == 0) continue;
        if(curr_cam0_points[i].y < 0 ||
                curr_cam0_points[i].y > cam0_curr_img_ptr->image.rows - 1 ||
                curr_cam0_points[i].x < 0 ||
                curr_cam0_points[i].x > cam0_curr_img_ptr->image.cols - 1)
        {
            track_inliers[i] = 0;
        }
    }

    // Collect the tracked points.
    vector<FeatureIDType> prev_tracked_ids(0);
    vector<int> prev_tracked_lifetime(0);
    vector<Point2f> prev_tracked_cam0_points(0);
    vector<Point2f> curr_tracked_cam0_points(0);

    removeUnmarkedElements(
                prev_ids, track_inliers, prev_tracked_ids);
    removeUnmarkedElements(
                prev_lifetime, track_inliers, prev_tracked_lifetime);
    removeUnmarkedElements(
                prev_cam0_points, track_inliers, prev_tracked_cam0_points);
    removeUnmarkedElements(
                curr_cam0_points, track_inliers, curr_tracked_cam0_points);

    // Number of features left after tracking.
    after_tracking = curr_tracked_cam0_points.size();

    // For now we have use LK method to track the features
    // Next we need to use ransac to calc the inliers and outliers.

    // prev frames cam0
    //              |
    //              |ransac
    //              |
    //              v
    // curr frames cam0

    // RANSAC between previous and current images of cam0.
    vector<int> cam0_ransac_inliers(0);
    twoPointRansac(prev_tracked_cam0_points, curr_tracked_cam0_points,
                   cam0_R_p_c, cam0_intrinsics, cam0_distortion_model,
                   cam0_distortion_coeffs, processor_config.ransac_threshold,
                   0.99, cam0_ransac_inliers);

    // Number of features after ransac.
    after_ransac = 0;

    for (int i = 0; i < cam0_ransac_inliers.size(); ++i)
    {
      if (cam0_ransac_inliers[i] == 0) continue;
      int row = static_cast<int>(
          curr_tracked_cam0_points[i].y / grid_height);
      int col = static_cast<int>(
          curr_tracked_cam0_points[i].x / grid_width);
      int code = row*processor_config.grid_col + col;
      (*curr_features_ptr)[code].push_back(FeatureMetaData());

      FeatureMetaData& grid_new_feature = (*curr_features_ptr)[code].back();
      grid_new_feature.id = prev_tracked_ids[i];
      grid_new_feature.lifetime = ++prev_tracked_lifetime[i];
      grid_new_feature.cam0_point = curr_tracked_cam0_points[i];

      ++after_ransac;
    }
    // Compute the tracking rate.
    int prev_feature_num = 0;
    for (const auto& item : *prev_features_ptr)
      prev_feature_num += item.second.size();

    int curr_feature_num = 0;
    for (const auto& item : *curr_features_ptr)
      curr_feature_num += item.second.size();

//    ROS_INFO_THROTTLE(0.5,
//        "\033[0;32m candidates: %d; track: %d; ransac: %d/%d=%f\033[0m",
//        before_tracking, after_tracking,
//        curr_feature_num, prev_feature_num,
//        static_cast<double>(curr_feature_num)/
//        (static_cast<double>(prev_feature_num)+1e-5));
    ROS_INFO("\033[0;32m candidates: %d; track: %d; ransac: %d/%d=%f\033[0m",
        before_tracking, after_tracking,
        curr_feature_num, prev_feature_num,
        static_cast<double>(curr_feature_num)/
        (static_cast<double>(prev_feature_num)+1e-5));
    return;
}

void ImageProcessor::integrateImuData(Matx33f& cam0_R_p_c)
{
    // Find the start and the end limit within the imu msg buffer.
    auto begin_iter = imu_msg_buffer.begin();
    while (begin_iter != imu_msg_buffer.end()) {
        if ((begin_iter->header.stamp-
             cam0_prev_img_ptr->header.stamp).toSec() < -0.01)
            ++begin_iter;
        else
            break;
    }

    auto end_iter = begin_iter;
    while (end_iter != imu_msg_buffer.end()) {
        if ((end_iter->header.stamp-
             cam0_curr_img_ptr->header.stamp).toSec() < 0.005)
            ++end_iter;
        else
            break;
    }

    // Compute the mean angular velocity in the IMU frame.
    Vec3f mean_ang_vel(0.0, 0.0, 0.0);
    for (auto iter = begin_iter; iter < end_iter; ++iter)
        mean_ang_vel += Vec3f(iter->angular_velocity.x,
                              iter->angular_velocity.y, iter->angular_velocity.z);

    if (end_iter-begin_iter > 0)
        mean_ang_vel *= 1.0f / (end_iter-begin_iter);

    // Transform the mean angular velocity from the IMU
    // frame to the cam0 and cam1 frames.
    Vec3f cam0_mean_ang_vel = R_cam0_imu.t() * mean_ang_vel;

    // Compute the relative rotation.
    double dtime = (cam0_curr_img_ptr->header.stamp-
                    cam0_prev_img_ptr->header.stamp).toSec();
    Rodrigues(cam0_mean_ang_vel*dtime, cam0_R_p_c);
    cam0_R_p_c = cam0_R_p_c.t();

    // Delete the useless and used imu messages.
    imu_msg_buffer.erase(imu_msg_buffer.begin(), end_iter);
    return;
}

void ImageProcessor::predictFeatureTracking(
        const vector<cv::Point2f>& input_pts,
        const cv::Matx33f& R_p_c,
        const cv::Vec4d& intrinsics,
        vector<cv::Point2f>& compensated_pts)
{

    // Return directly if there are no input features.
    if (input_pts.size() == 0) {
        compensated_pts.clear();
        return;
    }
    compensated_pts.resize(input_pts.size());

    // Intrinsic matrix.
    cv::Matx33f K(
                intrinsics[0], 0.0, intrinsics[2],
            0.0, intrinsics[1], intrinsics[3],
            0.0, 0.0, 1.0);
    cv::Matx33f H = K * R_p_c * K.inv();

    for (int i = 0; i < input_pts.size(); ++i) {
        cv::Vec3f p1(input_pts[i].x, input_pts[i].y, 1.0f);
        cv::Vec3f p2 = H * p1;
        compensated_pts[i].x = p2[0] / p2[2];
        compensated_pts[i].y = p2[1] / p2[2];
    }

    return;
}

void ImageProcessor::twoPointRansac(
        const vector<Point2f>& pts1, const vector<Point2f>& pts2,
        const cv::Matx33f& R_p_c, const cv::Vec4d& intrinsics,
        const std::string& distortion_model,
        const cv::Vec4d& distortion_coeffs,
        const double& inlier_error,
        const double& success_probability,
        vector<int>& inlier_markers) {

    // Check the size of input point size.
    if (pts1.size() != pts2.size())
        ROS_ERROR("Sets of different size (%lu and %lu) are used...",
                  pts1.size(), pts2.size());

    double norm_pixel_unit = 2.0 / (intrinsics[0]+intrinsics[1]);
    int iter_num = static_cast<int>(
                ceil(log(1-success_probability) / log(1-0.7*0.7)));

    // Initially, mark all points as inliers.
    inlier_markers.clear();
    inlier_markers.resize(pts1.size(), 1);

    // Undistort all the points.
    vector<Point2f> pts1_undistorted(pts1.size());
    vector<Point2f> pts2_undistorted(pts2.size());
    undistortPoints(
                pts1, intrinsics, distortion_model,
                distortion_coeffs, pts1_undistorted);
    undistortPoints(
                pts2, intrinsics, distortion_model,
                distortion_coeffs, pts2_undistorted);

    // Compenstate the points in the previous image with
    // the relative rotation.
    for (auto& pt : pts1_undistorted) {
        Vec3f pt_h(pt.x, pt.y, 1.0f);
        //Vec3f pt_hc = dR * pt_h;
        Vec3f pt_hc = R_p_c * pt_h;
        pt.x = pt_hc[0];
        pt.y = pt_hc[1];
    }

    // Normalize the points to gain numerical stability.
    float scaling_factor = 0.0f;
    rescalePoints(pts1_undistorted, pts2_undistorted, scaling_factor);
    norm_pixel_unit *= scaling_factor;

    // Compute the difference between previous and current points,
    // which will be used frequently later.
    vector<Point2d> pts_diff(pts1_undistorted.size());
    for (int i = 0; i < pts1_undistorted.size(); ++i)
        pts_diff[i] = pts1_undistorted[i] - pts2_undistorted[i];

    // Mark the point pairs with large difference directly.
    // BTW, the mean distance of the rest of the point pairs
    // are computed.
    double mean_pt_distance = 0.0;
    int raw_inlier_cntr = 0;
    for (int i = 0; i < pts_diff.size(); ++i) {
        double distance = sqrt(pts_diff[i].dot(pts_diff[i]));
        // 25 pixel distance is a pretty large tolerance for normal motion.
        // However, to be used with aggressive motion, this tolerance should
        // be increased significantly to match the usage.
        if (distance > 50.0*norm_pixel_unit) {
            inlier_markers[i] = 0;
        } else {
            mean_pt_distance += distance;
            ++raw_inlier_cntr;
        }
    }
    mean_pt_distance /= raw_inlier_cntr;

    // If the current number of inliers is less than 3, just mark
    // all input as outliers. This case can happen with fast
    // rotation where very few features are tracked.
    if (raw_inlier_cntr < 3) {
        for (auto& marker : inlier_markers) marker = 0;
        return;
    }

    // Before doing 2-point RANSAC, we have to check if the motion
    // is degenerated, meaning that there is no translation between
    // the frames, in which case, the model of the RANSAC does not
    // work. If so, the distance between the matched points will
    // be almost 0.
    //if (mean_pt_distance < inlier_error*norm_pixel_unit) {
    if (mean_pt_distance < norm_pixel_unit) {
        //ROS_WARN_THROTTLE(1.0, "Degenerated motion...");
        for (int i = 0; i < pts_diff.size(); ++i) {
            if (inlier_markers[i] == 0) continue;
            if (sqrt(pts_diff[i].dot(pts_diff[i])) >
                    inlier_error*norm_pixel_unit)
                inlier_markers[i] = 0;
        }
        return;
    }

    // In the case of general motion, the RANSAC model can be applied.
    // The three column corresponds to tx, ty, and tz respectively.
    MatrixXd coeff_t(pts_diff.size(), 3);
    for (int i = 0; i < pts_diff.size(); ++i) {
        coeff_t(i, 0) = pts_diff[i].y;
        coeff_t(i, 1) = -pts_diff[i].x;
        coeff_t(i, 2) = pts1_undistorted[i].x*pts2_undistorted[i].y -
                pts1_undistorted[i].y*pts2_undistorted[i].x;
    }

    vector<int> raw_inlier_idx;
    for (int i = 0; i < inlier_markers.size(); ++i) {
        if (inlier_markers[i] != 0)
            raw_inlier_idx.push_back(i);
    }

    vector<int> best_inlier_set;
    double best_error = 1e10;
    random_numbers::RandomNumberGenerator random_gen;

    for (int iter_idx = 0; iter_idx < iter_num; ++iter_idx) {
        // Randomly select two point pairs.
        // Although this is a weird way of selecting two pairs, but it
        // is able to efficiently avoid selecting repetitive pairs.
        int pair_idx1 = raw_inlier_idx[random_gen.uniformInteger(
                    0, raw_inlier_idx.size()-1)];
        int idx_diff = random_gen.uniformInteger(
                    1, raw_inlier_idx.size()-1);
        int pair_idx2 = pair_idx1+idx_diff < raw_inlier_idx.size() ?
                    pair_idx1+idx_diff : pair_idx1+idx_diff-raw_inlier_idx.size();

        // Construct the model;
        Vector2d coeff_tx(coeff_t(pair_idx1, 0), coeff_t(pair_idx2, 0));
        Vector2d coeff_ty(coeff_t(pair_idx1, 1), coeff_t(pair_idx2, 1));
        Vector2d coeff_tz(coeff_t(pair_idx1, 2), coeff_t(pair_idx2, 2));
        vector<double> coeff_l1_norm(3);
        coeff_l1_norm[0] = coeff_tx.lpNorm<1>();
        coeff_l1_norm[1] = coeff_ty.lpNorm<1>();
        coeff_l1_norm[2] = coeff_tz.lpNorm<1>();
        int base_indicator = min_element(coeff_l1_norm.begin(),
                                         coeff_l1_norm.end())-coeff_l1_norm.begin();

        Vector3d model(0.0, 0.0, 0.0);
        if (base_indicator == 0) {
            Matrix2d A;
            A << coeff_ty, coeff_tz;
            Vector2d solution = A.inverse() * (-coeff_tx);
            model(0) = 1.0;
            model(1) = solution(0);
            model(2) = solution(1);
        } else if (base_indicator ==1) {
            Matrix2d A;
            A << coeff_tx, coeff_tz;
            Vector2d solution = A.inverse() * (-coeff_ty);
            model(0) = solution(0);
            model(1) = 1.0;
            model(2) = solution(1);
        } else {
            Matrix2d A;
            A << coeff_tx, coeff_ty;
            Vector2d solution = A.inverse() * (-coeff_tz);
            model(0) = solution(0);
            model(1) = solution(1);
            model(2) = 1.0;
        }

        // Find all the inliers among point pairs.
        VectorXd error = coeff_t * model;

        vector<int> inlier_set;
        for (int i = 0; i < error.rows(); ++i) {
            if (inlier_markers[i] == 0) continue;
            if (std::abs(error(i)) < inlier_error*norm_pixel_unit)
                inlier_set.push_back(i);
        }

        // If the number of inliers is small, the current
        // model is probably wrong.
        if (inlier_set.size() < 0.2*pts1_undistorted.size())
            continue;

        // Refit the model using all of the possible inliers.
        VectorXd coeff_tx_better(inlier_set.size());
        VectorXd coeff_ty_better(inlier_set.size());
        VectorXd coeff_tz_better(inlier_set.size());
        for (int i = 0; i < inlier_set.size(); ++i) {
            coeff_tx_better(i) = coeff_t(inlier_set[i], 0);
            coeff_ty_better(i) = coeff_t(inlier_set[i], 1);
            coeff_tz_better(i) = coeff_t(inlier_set[i], 2);
        }

        Vector3d model_better(0.0, 0.0, 0.0);
        if (base_indicator == 0) {
            MatrixXd A(inlier_set.size(), 2);
            A << coeff_ty_better, coeff_tz_better;
            Vector2d solution =
                    (A.transpose() * A).inverse() * A.transpose() * (-coeff_tx_better);
            model_better(0) = 1.0;
            model_better(1) = solution(0);
            model_better(2) = solution(1);
        } else if (base_indicator ==1) {
            MatrixXd A(inlier_set.size(), 2);
            A << coeff_tx_better, coeff_tz_better;
            Vector2d solution =
                    (A.transpose() * A).inverse() * A.transpose() * (-coeff_ty_better);
            model_better(0) = solution(0);
            model_better(1) = 1.0;
            model_better(2) = solution(1);
        } else {
            MatrixXd A(inlier_set.size(), 2);
            A << coeff_tx_better, coeff_ty_better;
            Vector2d solution =
                    (A.transpose() * A).inverse() * A.transpose() * (-coeff_tz_better);
            model_better(0) = solution(0);
            model_better(1) = solution(1);
            model_better(2) = 1.0;
        }

        // Compute the error and upate the best model if possible.
        VectorXd new_error = coeff_t * model_better;

        double this_error = 0.0;
        for (const auto& inlier_idx : inlier_set)
            this_error += std::abs(new_error(inlier_idx));
        this_error /= inlier_set.size();

        if (inlier_set.size() > best_inlier_set.size()) {
            best_error = this_error;
            best_inlier_set = inlier_set;
        }
    }

    // Fill in the markers.
    inlier_markers.clear();
    inlier_markers.resize(pts1.size(), 0);
    for (const auto& inlier_idx : best_inlier_set)
        inlier_markers[inlier_idx] = 1;

//    printf("inlier ratio: %lu/%lu\n",
//        best_inlier_set.size(), inlier_markers.size());

    return;
}

void ImageProcessor::rescalePoints(
    vector<Point2f>& pts1, vector<Point2f>& pts2,
    float& scaling_factor) {

  scaling_factor = 0.0f;

  for (int i = 0; i < pts1.size(); ++i) {
    scaling_factor += sqrt(pts1[i].dot(pts1[i]));
    scaling_factor += sqrt(pts2[i].dot(pts2[i]));
  }

  scaling_factor = (pts1.size()+pts2.size()) /
    scaling_factor * sqrt(2.0f);

  for (int i = 0; i < pts1.size(); ++i) {
    pts1[i] *= scaling_factor;
    pts2[i] *= scaling_factor;
  }

  return;
}

void ImageProcessor::undistortPoints(
    const vector<cv::Point2f>& pts_in,
    const cv::Vec4d& intrinsics,
    const string& distortion_model,
    const cv::Vec4d& distortion_coeffs,
    vector<cv::Point2f>& pts_out,
    const cv::Matx33d &rectification_matrix,
    const cv::Vec4d &new_intrinsics) {

  if (pts_in.size() == 0) return;

  const cv::Matx33d K(
      intrinsics[0], 0.0, intrinsics[2],
      0.0, intrinsics[1], intrinsics[3],
      0.0, 0.0, 1.0);

  const cv::Matx33d K_new(
      new_intrinsics[0], 0.0, new_intrinsics[2],
      0.0, new_intrinsics[1], new_intrinsics[3],
      0.0, 0.0, 1.0);

  if (distortion_model == "radtan") {
    cv::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                        rectification_matrix, K_new);
  } else if (distortion_model == "equidistant") {
    cv::fisheye::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                                 rectification_matrix, K_new);
  } else {
    ROS_WARN_ONCE("The model %s is unrecognized, use radtan instead...",
                  distortion_model.c_str());
    cv::undistortPoints(pts_in, pts_out, K, distortion_coeffs,
                        rectification_matrix, K_new);
  }

  return;
}

vector<cv::Point2f> ImageProcessor::distortPoints(
    const vector<cv::Point2f>& pts_in,
    const cv::Vec4d& intrinsics,
    const string& distortion_model,
    const cv::Vec4d& distortion_coeffs) {

  const cv::Matx33d K(intrinsics[0], 0.0, intrinsics[2],
                      0.0, intrinsics[1], intrinsics[3],
                      0.0, 0.0, 1.0);

  vector<cv::Point2f> pts_out;
  if (distortion_model == "radtan") {
    vector<cv::Point3f> homogenous_pts;
    cv::convertPointsToHomogeneous(pts_in, homogenous_pts);
    cv::projectPoints(homogenous_pts, cv::Vec3d::zeros(), cv::Vec3d::zeros(), K,
                      distortion_coeffs, pts_out);
  } else if (distortion_model == "equidistant") {
    cv::fisheye::distortPoints(pts_in, pts_out, K, distortion_coeffs);
  } else {
    ROS_WARN_ONCE("The model %s is unrecognized, using radtan instead...",
                  distortion_model.c_str());
    vector<cv::Point3f> homogenous_pts;
    cv::convertPointsToHomogeneous(pts_in, homogenous_pts);
    cv::projectPoints(homogenous_pts, cv::Vec3d::zeros(), cv::Vec3d::zeros(), K,
                      distortion_coeffs, pts_out);
  }

  return pts_out;
}

void ImageProcessor::drawFeaturesMono()
{
    // Colors for different features.
    Scalar tracked(0, 255, 0);
    Scalar new_feature(0, 255, 255);

    static int grid_height =
            cam0_curr_img_ptr->image.rows / processor_config.grid_row;
    static int grid_width =
            cam0_curr_img_ptr->image.cols / processor_config.grid_col;

    // Create an output image.
    int img_height = cam0_curr_img_ptr->image.rows;
    int img_width = cam0_curr_img_ptr->image.cols;
    Mat out_img(img_height, img_width, CV_8UC3);
    cvtColor(cam0_curr_img_ptr->image, out_img, CV_GRAY2RGB);

    // Draw grids on the image.
    for (int i = 1; i < processor_config.grid_row; ++i) {
        Point pt1(0, i*grid_height);
        Point pt2(img_width, i*grid_height);
        line(out_img, pt1, pt2, Scalar(255, 0, 0));
    }
    for (int i = 1; i < processor_config.grid_col; ++i) {
        Point pt1(i*grid_width, 0);
        Point pt2(i*grid_width, img_height);
        line(out_img, pt1, pt2, Scalar(255, 0, 0));
    }

    // Collect features ids in the previous frame.
    vector<FeatureIDType> prev_ids(0);
    for (const auto& grid_features : *prev_features_ptr)
        for (const auto& feature : grid_features.second)
            prev_ids.push_back(feature.id);

    // Collect feature points in the previous frame.
    map<FeatureIDType, Point2f> prev_points;
    for (const auto& grid_features : *prev_features_ptr)
        for (const auto& feature : grid_features.second)
            prev_points[feature.id] = feature.cam0_point;

    // Collect feature points in the current frame.
    map<FeatureIDType, Point2f> curr_points;
    for (const auto& grid_features : *curr_features_ptr)
        for (const auto& feature : grid_features.second)
            curr_points[feature.id] = feature.cam0_point;

    // Draw tracked features.
    for (const auto& id : prev_ids) {
        if (prev_points.find(id) != prev_points.end() &&
                curr_points.find(id) != curr_points.end()) {
            cv::Point2f prev_pt = prev_points[id];
            cv::Point2f curr_pt = curr_points[id];
            circle(out_img, curr_pt, 3, tracked, -1);
            line(out_img, prev_pt, curr_pt, tracked, 1);

            prev_points.erase(id);
            curr_points.erase(id);
        }
    }

    // Draw new features.
    for (const auto& new_curr_point : curr_points) {
        cv::Point2f pt = new_curr_point.second;
        circle(out_img, pt, 3, new_feature, -1);
    }

    imshow("Feature", out_img);
    waitKey(27);
}




}

