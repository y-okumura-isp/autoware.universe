// Copyright 2020 Tier IV, Inc.
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
#ifndef OBSTACLE_AVOIDANCE_PLANNER__NODE_HPP_
#define OBSTACLE_AVOIDANCE_PLANNER__NODE_HPP_

#include "motion_utils/trajectory/trajectory.hpp"
#include "obstacle_avoidance_planner/common_structs.hpp"
#include "obstacle_avoidance_planner/costmap_generator.hpp"
#include "obstacle_avoidance_planner/eb_path_optimizer.hpp"
#include "obstacle_avoidance_planner/mpt_optimizer.hpp"
#include "opencv2/core.hpp"
#include "rclcpp/clock.hpp"
#include "rclcpp/rclcpp.hpp"
#include "tier4_autoware_utils/ros/self_pose_listener.hpp"
#include "tier4_autoware_utils/system/stop_watch.hpp"

#include "autoware_auto_perception_msgs/msg/predicted_objects.hpp"
#include "autoware_auto_planning_msgs/msg/path.hpp"
#include "autoware_auto_planning_msgs/msg/path_point.hpp"
#include "autoware_auto_planning_msgs/msg/trajectory.hpp"
#include "autoware_auto_planning_msgs/msg/trajectory_point.hpp"
#include "geometry_msgs/msg/point.hpp"
#include "geometry_msgs/msg/pose.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "nav_msgs/msg/map_meta_data.hpp"
#include "nav_msgs/msg/occupancy_grid.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "tier4_debug_msgs/msg/string_stamped.hpp"
#include "tier4_planning_msgs/msg/enable_avoidance.hpp"
#include "visualization_msgs/msg/marker_array.hpp"

#include "boost/optional.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "tilde/tilde_publisher.hpp"
#include "tilde/tilde_node.hpp"

namespace
{
template <typename T>
boost::optional<geometry_msgs::msg::Pose> lerpPose(
  const T & points, const geometry_msgs::msg::Point & target_pos, const size_t closest_seg_idx)
{
  constexpr double epsilon = 1e-6;

  const double closest_to_target_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, target_pos);
  const double seg_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, closest_seg_idx + 1);

  const auto & closest_pose = points[closest_seg_idx].pose;
  const auto & next_pose = points[closest_seg_idx + 1].pose;

  geometry_msgs::msg::Pose interpolated_pose;
  if (std::abs(seg_dist) < epsilon) {
    interpolated_pose.position.x = next_pose.position.x;
    interpolated_pose.position.y = next_pose.position.y;
    interpolated_pose.position.z = next_pose.position.z;
    interpolated_pose.orientation = next_pose.orientation;
  } else {
    const double ratio = closest_to_target_dist / seg_dist;
    if (ratio < 0 || 1 < ratio) {
      return {};
    }

    interpolated_pose.position.x =
      interpolation::lerp(closest_pose.position.x, next_pose.position.x, ratio);
    interpolated_pose.position.y =
      interpolation::lerp(closest_pose.position.y, next_pose.position.y, ratio);
    interpolated_pose.position.z =
      interpolation::lerp(closest_pose.position.z, next_pose.position.z, ratio);

    const double closest_yaw = tf2::getYaw(closest_pose.orientation);
    const double next_yaw = tf2::getYaw(next_pose.orientation);
    const double interpolated_yaw = interpolation::lerp(closest_yaw, next_yaw, ratio);
    interpolated_pose.orientation = tier4_autoware_utils::createQuaternionFromYaw(interpolated_yaw);
  }
  return interpolated_pose;
}

template <typename T>
double lerpTwistX(
  const T & points, const geometry_msgs::msg::Point & target_pos, const size_t closest_seg_idx)
{
  if (points.size() == 1) {
    return points.at(0).longitudinal_velocity_mps;
  }

  constexpr double epsilon = 1e-6;

  const double closest_to_target_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, target_pos);
  const double seg_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, closest_seg_idx + 1);

  const double closest_vel = points[closest_seg_idx].longitudinal_velocity_mps;
  const double next_vel = points[closest_seg_idx + 1].longitudinal_velocity_mps;

  if (std::abs(seg_dist) < epsilon) {
    return next_vel;
  }

  const double ratio = std::min(1.0, std::max(0.0, closest_to_target_dist / seg_dist));
  return interpolation::lerp(closest_vel, next_vel, ratio);
}

template <typename T>
double lerpPoseZ(
  const T & points, const geometry_msgs::msg::Point & target_pos, const size_t closest_seg_idx)
{
  if (points.size() == 1) {
    return points.at(0).pose.position.z;
  }

  constexpr double epsilon = 1e-6;

  const double closest_to_target_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, target_pos);
  const double seg_dist =
    motion_utils::calcSignedArcLength(points, closest_seg_idx, closest_seg_idx + 1);

  const double closest_z = points[closest_seg_idx].pose.position.z;
  const double next_z = points[closest_seg_idx + 1].pose.position.z;

  return std::abs(seg_dist) < epsilon
           ? next_z
           : interpolation::lerp(closest_z, next_z, closest_to_target_dist / seg_dist);
}
}  // namespace

class ObstacleAvoidancePlanner : public tilde::TildeNode
{
private:
  OnSetParametersCallbackHandle::SharedPtr set_param_res_;
  rclcpp::Clock logger_ros_clock_;
  int eb_solved_count_;
  bool is_driving_forward_{true};

  bool is_publishing_debug_visualization_marker_;
  bool is_publishing_area_with_objects_;
  bool is_publishing_object_clearance_map_;
  bool is_publishing_clearance_map_;
  bool is_showing_debug_info_;
  bool is_showing_calculation_time_;
  bool is_stopping_if_outside_drivable_area_;
  bool enable_avoidance_;
  bool enable_pre_smoothing_;
  bool skip_optimization_;
  bool reset_prev_optimization_;

  // vehicle circles info for for mpt constraints
  std::string vehicle_circle_method_;
  int vehicle_circle_num_for_calculation_;
  std::vector<double> vehicle_circle_radius_ratios_;

  // params for replan
  double max_path_shape_change_dist_for_replan_;
  double max_ego_moving_dist_for_replan_;
  double max_delta_time_sec_for_replan_;

  // logic
  std::unique_ptr<CostmapGenerator> costmap_generator_ptr_;
  std::unique_ptr<EBPathOptimizer> eb_path_optimizer_ptr_;
  std::unique_ptr<MPTOptimizer> mpt_optimizer_ptr_;

  // params
  TrajectoryParam traj_param_;
  EBParam eb_param_;
  VehicleParam vehicle_param_;
  MPTParam mpt_param_;
  int mpt_visualize_sampling_num_;

  // debug
  mutable std::shared_ptr<DebugData> debug_data_ptr_;
  mutable tier4_autoware_utils::StopWatch<
    std::chrono::milliseconds, std::chrono::microseconds, std::chrono::steady_clock>
    stop_watch_;

  geometry_msgs::msg::Pose current_ego_pose_;
  std::unique_ptr<geometry_msgs::msg::TwistStamped> current_twist_ptr_;
  std::unique_ptr<geometry_msgs::msg::Pose> prev_ego_pose_ptr_;
  std::unique_ptr<Trajectories> prev_optimal_trajs_ptr_;
  std::unique_ptr<std::vector<autoware_auto_planning_msgs::msg::PathPoint>> prev_path_points_ptr_;
  std::unique_ptr<autoware_auto_perception_msgs::msg::PredictedObjects> objects_ptr_;

  std::unique_ptr<rclcpp::Time> latest_replanned_time_ptr_;
  tier4_autoware_utils::SelfPoseListener self_pose_listener_{this};

  // ROS
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr
    debug_extended_fixed_traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr
    debug_extended_non_fixed_traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr debug_eb_traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr
    debug_mpt_fixed_traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr
    debug_mpt_ref_traj_pub_;
  tilde::TildePublisher<autoware_auto_planning_msgs::msg::Trajectory>::SharedPtr debug_mpt_traj_pub_;

  tilde::TildePublisher<visualization_msgs::msg::MarkerArray>::SharedPtr debug_markers_pub_;
  tilde::TildePublisher<visualization_msgs::msg::MarkerArray>::SharedPtr debug_wall_markers_pub_;
  tilde::TildePublisher<nav_msgs::msg::OccupancyGrid>::SharedPtr debug_clearance_map_pub_;
  tilde::TildePublisher<nav_msgs::msg::OccupancyGrid>::SharedPtr debug_object_clearance_map_pub_;
  tilde::TildePublisher<nav_msgs::msg::OccupancyGrid>::SharedPtr debug_area_with_objects_pub_;
  tilde::TildePublisher<tier4_debug_msgs::msg::StringStamped>::SharedPtr debug_msg_pub_;

  rclcpp::Subscription<autoware_auto_planning_msgs::msg::Path>::SharedPtr path_sub_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_sub_;
  rclcpp::Subscription<autoware_auto_perception_msgs::msg::PredictedObjects>::SharedPtr
    objects_sub_;
  rclcpp::Subscription<tier4_planning_msgs::msg::EnableAvoidance>::SharedPtr is_avoidance_sub_;

  // param callback function
  rcl_interfaces::msg::SetParametersResult paramCallback(
    const std::vector<rclcpp::Parameter> & parameters);

  // subscriber callback functions
  void odomCallback(const nav_msgs::msg::Odometry::SharedPtr);
  void objectsCallback(const autoware_auto_perception_msgs::msg::PredictedObjects::SharedPtr);
  void enableAvoidanceCallback(const tier4_planning_msgs::msg::EnableAvoidance::SharedPtr);
  void pathCallback(const autoware_auto_planning_msgs::msg::Path::SharedPtr);

  // functions
  void resetPlanning();
  void resetPrevOptimization();

  std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> generateOptimizedTrajectory(
    const autoware_auto_planning_msgs::msg::Path & input_path);

  bool checkReplan(const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points);

  autoware_auto_planning_msgs::msg::Trajectory generateTrajectory(
    const autoware_auto_planning_msgs::msg::Path & path);

  Trajectories optimizeTrajectory(
    const autoware_auto_planning_msgs::msg::Path & path, const CVMaps & cv_maps);

  Trajectories getPrevTrajs(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points) const;

  void calcVelocity(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & traj_points) const;

  void insertZeroVelocityOutsideDrivableArea(
    std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & traj_points,
    const CVMaps & cv_maps);

  void publishDebugDataInOptimization(
    const autoware_auto_planning_msgs::msg::Path & path,
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & traj_points);

  Trajectories makePrevTrajectories(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    const Trajectories & trajs);

  std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> generatePostProcessedTrajectory(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & merged_optimized_points);

  std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> getExtendedTrajectory(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & optimized_points);

  std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> generateFineTrajectoryPoints(
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & traj_points) const;

  std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> alignVelocity(
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & fine_traj_points,
    const std::vector<autoware_auto_planning_msgs::msg::PathPoint> & path_points,
    const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & traj_points) const;

  void publishDebugDataInMain(const autoware_auto_planning_msgs::msg::Path & path) const;

public:
  explicit ObstacleAvoidancePlanner(const rclcpp::NodeOptions & node_options);
};

#endif  // OBSTACLE_AVOIDANCE_PLANNER__NODE_HPP_
