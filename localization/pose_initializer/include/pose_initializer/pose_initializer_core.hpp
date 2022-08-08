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

#ifndef POSE_INITIALIZER__POSE_INITIALIZER_CORE_HPP_
#define POSE_INITIALIZER__POSE_INITIALIZER_CORE_HPP_

#include <rclcpp/rclcpp.hpp>
#include <tier4_api_utils/tier4_api_utils.hpp>

#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <tier4_external_api_msgs/srv/initialize_pose_auto.hpp>
#include <tier4_localization_msgs/msg/pose_initialization_request.hpp>
#include <tier4_localization_msgs/srv/pose_with_covariance_stamped.hpp>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <tf2/transform_datatypes.h>
#include <tf2_ros/transform_listener.h>

#include <memory>
#include <string>

#include "tilde/tilde_publisher.hpp"
#include "tilde/tilde_node.hpp"

class PoseInitializer : public tilde::TildeNode
{
public:
  PoseInitializer();

private:
  void callbackMapPoints(sensor_msgs::msg::PointCloud2::ConstSharedPtr map_points_msg_ptr);
  void serviceInitializePose(
    const tier4_localization_msgs::srv::PoseWithCovarianceStamped::Request::SharedPtr req,
    tier4_localization_msgs::srv::PoseWithCovarianceStamped::Response::SharedPtr res);
  void serviceInitializePoseAuto(
    const tier4_external_api_msgs::srv::InitializePoseAuto::Request::SharedPtr req,
    tier4_external_api_msgs::srv::InitializePoseAuto::Response::SharedPtr res);
  void callbackInitialPose(
    geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr pose_cov_msg_ptr);
  void callbackGNSSPoseCov(
    geometry_msgs::msg::PoseWithCovarianceStamped::ConstSharedPtr pose_cov_msg_ptr);
  void callbackPoseInitializationRequest(
    const tier4_localization_msgs::msg::PoseInitializationRequest::ConstSharedPtr
      request_msg_ptr);  // NOLINT

  bool getHeight(
    const geometry_msgs::msg::PoseWithCovarianceStamped & input_pose_msg,
    const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr output_pose_msg_ptr);
  bool callAlignServiceAndPublishResult(
    const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);

  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr gnss_pose_sub_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr map_points_sub_;

  // TODO(Takagi, Isamu): deprecated
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_sub_;
  rclcpp::Subscription<tier4_localization_msgs::msg::PoseInitializationRequest>::SharedPtr
    pose_initialization_request_sub_;

  tilde::TildePublisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_pub_;

  rclcpp::Client<tier4_localization_msgs::srv::PoseWithCovarianceStamped>::SharedPtr ndt_client_;

  rclcpp::CallbackGroup::SharedPtr initialize_pose_service_group_;
  rclcpp::Service<tier4_localization_msgs::srv::PoseWithCovarianceStamped>::SharedPtr
    initialize_pose_service_;
  rclcpp::Service<tier4_external_api_msgs::srv::InitializePoseAuto>::SharedPtr
    initialize_pose_auto_service_;

  tf2::BufferCore tf2_buffer_;
  tf2_ros::TransformListener tf2_listener_;

  pcl::PointCloud<pcl::PointXYZ>::Ptr map_ptr_;
  std::string map_frame_;

  // With the currently available facilities for calling a service, there is no
  // easy way of detecting whether an answer was received within a reasonable
  // amount of time. So, as a sanity check, we check whether a response for the
  // previous request was received when a new request is sent.
  uint32_t request_id_ = 0;
  uint32_t response_id_ = 0;

  bool enable_gnss_callback_;
  std::array<double, 36> initialpose_particle_covariance_;
  std::array<double, 36> gnss_particle_covariance_;
  std::array<double, 36> service_particle_covariance_;
  std::array<double, 36> output_pose_covariance_;
};

#endif  // POSE_INITIALIZER__POSE_INITIALIZER_CORE_HPP_
