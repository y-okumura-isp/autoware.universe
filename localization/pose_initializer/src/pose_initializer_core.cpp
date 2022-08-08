// Copyright 2020 Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pose_initializer/pose_initializer_core.hpp"

#include "pose_initializer/copy_vector_to_array.hpp"

#include <pcl_conversions/pcl_conversions.h>
#ifdef ROS_DISTRO_GALACTIC
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#else
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

double getGroundHeight(const pcl::PointCloud<pcl::PointXYZ>::Ptr pcdmap, const tf2::Vector3 & point)
{
  constexpr double radius = 1.0 * 1.0;
  const double x = point.getX();
  const double y = point.getY();

  double height = INFINITY;
  for (const auto & p : pcdmap->points) {
    const double dx = x - p.x;
    const double dy = y - p.y;
    const double sd = (dx * dx) + (dy * dy);
    if (sd < radius) {
      height = std::min(height, static_cast<double>(p.z));
    }
  }
  return std::isfinite(height) ? height : point.getZ();
}

PoseInitializer::PoseInitializer()
: TildeNode("pose_initializer"), tf2_listener_(tf2_buffer_), map_frame_("map")
{
  enable_gnss_callback_ = this->declare_parameter("enable_gnss_callback", true);

  const std::vector<double> initialpose_particle_covariance =
    this->declare_parameter<std::vector<double>>("initialpose_particle_covariance");
  CopyVectorToArray(initialpose_particle_covariance, initialpose_particle_covariance_);

  const std::vector<double> gnss_particle_covariance =
    this->declare_parameter<std::vector<double>>("gnss_particle_covariance");
  CopyVectorToArray(gnss_particle_covariance, gnss_particle_covariance_);

  const std::vector<double> service_particle_covariance =
    this->declare_parameter<std::vector<double>>("service_particle_covariance");
  CopyVectorToArray(service_particle_covariance, service_particle_covariance_);

  const std::vector<double> output_pose_covariance =
    this->declare_parameter<std::vector<double>>("output_pose_covariance");
  CopyVectorToArray(output_pose_covariance, output_pose_covariance_);

  // We can't use _1 because pcl leaks an alias to boost::placeholders::_1, so it would be ambiguous
  initial_pose_sub_ = this->create_tilde_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "initialpose", 10,
    std::bind(&PoseInitializer::callbackInitialPose, this, std::placeholders::_1));
  map_points_sub_ = this->create_tilde_subscription<sensor_msgs::msg::PointCloud2>(
    "pointcloud_map", rclcpp::QoS{1}.transient_local(),
    std::bind(&PoseInitializer::callbackMapPoints, this, std::placeholders::_1));
  gnss_pose_sub_ = this->create_tilde_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "gnss_pose_cov", 1,
    std::bind(&PoseInitializer::callbackGNSSPoseCov, this, std::placeholders::_1));
  pose_initialization_request_sub_ =
    this->create_tilde_subscription<tier4_localization_msgs::msg::PoseInitializationRequest>(
      "pose_initialization_request", rclcpp::QoS{1}.transient_local(),
      std::bind(&PoseInitializer::callbackPoseInitializationRequest, this, std::placeholders::_1));

  initial_pose_pub_ =
    this->create_tilde_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>("initialpose3d", 10);

  initialize_pose_service_group_ =
    create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  ndt_client_ = this->create_client<tier4_localization_msgs::srv::PoseWithCovarianceStamped>(
    "ndt_align_srv", rmw_qos_profile_services_default, initialize_pose_service_group_);
  while (!ndt_client_->wait_for_service(std::chrono::seconds(1)) && rclcpp::ok()) {
    RCLCPP_INFO(get_logger(), "Waiting for service...");
  }

  initialize_pose_service_ =
    this->create_service<tier4_localization_msgs::srv::PoseWithCovarianceStamped>(
      "service/initialize_pose", std::bind(
                                   &PoseInitializer::serviceInitializePose, this,
                                   std::placeholders::_1, std::placeholders::_2));

  initialize_pose_auto_service_ =
    this->create_service<tier4_external_api_msgs::srv::InitializePoseAuto>(
      "service/initialize_pose_auto", std::bind(
                                        &PoseInitializer::serviceInitializePoseAuto, this,
                                        std::placeholders::_1, std::placeholders::_2));
}

void PoseInitializer::callbackMapPoints(
  sensor_msgs::msg::PointCloud2::ConstSharedPtr map_points_msg_ptr)
{
  std::string map_frame_ = map_points_msg_ptr->header.frame_id;
  map_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*map_points_msg_ptr, *map_ptr_);
}

void PoseInitializer::serviceInitializePose(
  const tier4_localization_msgs::srv::PoseWithCovarianceStamped::Request::SharedPtr req,
  tier4_localization_msgs::srv::PoseWithCovarianceStamped::Response::SharedPtr res)
{
  enable_gnss_callback_ = false;  // get only first topic

  auto add_height_pose_msg_ptr = std::make_shared<geometry_msgs::msg::PoseWithCovarianceStamped>();
  getHeight(req->pose_with_covariance, add_height_pose_msg_ptr);

  add_height_pose_msg_ptr->pose.covariance = service_particle_covariance_;

  res->success = callAlignServiceAndPublishResult(add_height_pose_msg_ptr);
}

void PoseInitializer::callbackInitialPose(
  geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr pose_cov_msg_ptr)
{
  enable_gnss_callback_ = false;  // get only first topic

  auto add_height_pose_msg_ptr = std::make_shared<geometry_msgs::msg::PoseWithCovarianceStamped>();
  getHeight(*pose_cov_msg_ptr, add_height_pose_msg_ptr);

  add_height_pose_msg_ptr->pose.covariance = initialpose_particle_covariance_;

  callAlignServiceAndPublishResult(add_height_pose_msg_ptr);
}

// NOTE Still not usable callback
void PoseInitializer::callbackGNSSPoseCov(
  geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr pose_cov_msg_ptr)
{
  if (!enable_gnss_callback_) {
    return;
  }

  // TODO(YamatoAndo) check service is available

  auto add_height_pose_msg_ptr = std::make_shared<geometry_msgs::msg::PoseWithCovarianceStamped>();
  getHeight(*pose_cov_msg_ptr, add_height_pose_msg_ptr);

  add_height_pose_msg_ptr->pose.covariance = gnss_particle_covariance_;

  callAlignServiceAndPublishResult(add_height_pose_msg_ptr);
}

void PoseInitializer::serviceInitializePoseAuto(
  const tier4_external_api_msgs::srv::InitializePoseAuto::Request::SharedPtr req,
  tier4_external_api_msgs::srv::InitializePoseAuto::Response::SharedPtr res)
{
  (void)req;

  RCLCPP_INFO(this->get_logger(), "Called Pose Initialize Service");
  enable_gnss_callback_ = true;
  res->status = tier4_api_utils::response_success();
}

void PoseInitializer::callbackPoseInitializationRequest(
  const tier4_localization_msgs::msg::PoseInitializationRequest::ConstSharedPtr request_msg_ptr)
{
  RCLCPP_INFO(this->get_logger(), "Called Pose Initialize");
  enable_gnss_callback_ = request_msg_ptr->data;
}

bool PoseInitializer::getHeight(
  const geometry_msgs::msg::PoseWithCovarianceStamped & input_pose_msg,
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr output_pose_msg_ptr)
{
  std::string fixed_frame = input_pose_msg.header.frame_id;
  tf2::Vector3 point(
    input_pose_msg.pose.pose.position.x, input_pose_msg.pose.pose.position.y,
    input_pose_msg.pose.pose.position.z);

  if (map_ptr_) {
    tf2::Transform transform{tf2::Quaternion{}, tf2::Vector3{}};
    try {
      const auto stamped = tf2_buffer_.lookupTransform(map_frame_, fixed_frame, tf2::TimePointZero);
      tf2::fromMsg(stamped.transform, transform);
    } catch (tf2::TransformException & exception) {
      RCLCPP_WARN_STREAM(get_logger(), "failed to lookup transform: " << exception.what());
    }

    point = transform * point;
    point.setZ(getGroundHeight(map_ptr_, point));
    point = transform.inverse() * point;
  }

  *output_pose_msg_ptr = input_pose_msg;
  output_pose_msg_ptr->pose.pose.position.x = point.getX();
  output_pose_msg_ptr->pose.pose.position.y = point.getY();
  output_pose_msg_ptr->pose.pose.position.z = point.getZ();

  return true;
}

bool PoseInitializer::callAlignServiceAndPublishResult(
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr input_pose_msg)
{
  if (request_id_ != response_id_) {
    RCLCPP_ERROR(get_logger(), "Did not receive response for previous NDT Align Server call");
    return false;
  }
  auto req = std::make_shared<tier4_localization_msgs::srv::PoseWithCovarianceStamped::Request>();
  req->pose_with_covariance = *input_pose_msg;
  req->seq = ++request_id_;

  RCLCPP_INFO(get_logger(), "call NDT Align Server");
  auto result = ndt_client_->async_send_request(req).get();

  if (!result->success) {
    RCLCPP_INFO(get_logger(), "failed NDT Align Server");
    response_id_ = result->seq;
    return false;
  }

  RCLCPP_INFO(get_logger(), "called NDT Align Server");
  response_id_ = result->seq;
  // NOTE temporary cov
  geometry_msgs::msg::PoseWithCovarianceStamped & pose_with_cov = result->pose_with_covariance;
  pose_with_cov.pose.covariance[0] = 1.0;
  pose_with_cov.pose.covariance[1 * 6 + 1] = 1.0;
  pose_with_cov.pose.covariance[2 * 6 + 2] = 0.01;
  pose_with_cov.pose.covariance[3 * 6 + 3] = 0.01;
  pose_with_cov.pose.covariance[4 * 6 + 4] = 0.01;
  pose_with_cov.pose.covariance[5 * 6 + 5] = 0.2;
  initial_pose_pub_->publish(pose_with_cov);
  enable_gnss_callback_ = false;

  return true;
}
