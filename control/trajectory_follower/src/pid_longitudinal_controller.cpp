// Copyright 2021 Tier IV, Inc. All rights reserved.
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

#include "trajectory_follower/pid_longitudinal_controller.hpp"

#include "motion_common/motion_common.hpp"
#include "motion_common/trajectory_common.hpp"
#include "tier4_autoware_utils/tier4_autoware_utils.hpp"
#include "time_utils/time_utils.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace autoware
{
namespace motion
{
namespace control
{
namespace trajectory_follower
{
PidLongitudinalController::PidLongitudinalController(tilde::TildeNode & node) : node_{&node}
{
  using std::placeholders::_1;

  // parameters timer
  m_longitudinal_ctrl_period = node_->get_parameter("ctrl_period").as_double();

  m_wheel_base = vehicle_info_util::VehicleInfoUtil(*node_).getVehicleInfo().wheel_base_m;

  // parameters for delay compensation
  m_delay_compensation_time =
    node_->declare_parameter<float64_t>("delay_compensation_time");  // [s]

  // parameters to enable functions
  m_enable_smooth_stop = node_->declare_parameter<bool8_t>("enable_smooth_stop");
  m_enable_overshoot_emergency = node_->declare_parameter<bool8_t>("enable_overshoot_emergency");
  m_enable_large_tracking_error_emergency =
    node_->declare_parameter<bool8_t>("enable_large_tracking_error_emergency");
  m_enable_slope_compensation = node_->declare_parameter<bool8_t>("enable_slope_compensation");
  m_enable_keep_stopped_until_steer_convergence =
    node_->declare_parameter<bool8_t>("enable_keep_stopped_until_steer_convergence");

  // parameters for state transition
  {
    auto & p = m_state_transition_params;
    // drive
    p.drive_state_stop_dist = node_->declare_parameter<float64_t>("drive_state_stop_dist");  // [m]
    p.drive_state_offset_stop_dist =
      node_->declare_parameter<float64_t>("drive_state_offset_stop_dist");  // [m]
    // stopping
    p.stopping_state_stop_dist =
      node_->declare_parameter<float64_t>("stopping_state_stop_dist");  // [m]
    p.stopped_state_entry_duration_time =
      node_->declare_parameter<float64_t>("stopped_state_entry_duration_time");  // [s]
    // stop
    p.stopped_state_entry_vel =
      node_->declare_parameter<float64_t>("stopped_state_entry_vel");  // [m/s]
    p.stopped_state_entry_acc =
      node_->declare_parameter<float64_t>("stopped_state_entry_acc");  // [m/s²]

    // emergency
    p.emergency_state_overshoot_stop_dist =
      node_->declare_parameter<float64_t>("emergency_state_overshoot_stop_dist");  // [m]
    p.emergency_state_traj_trans_dev =
      node_->declare_parameter<float64_t>("emergency_state_traj_trans_dev");  // [m]
    p.emergency_state_traj_rot_dev =
      node_->declare_parameter<float64_t>("emergency_state_traj_rot_dev");  // [m]
  }

  // parameters for drive state
  {
    // initialize PID gain
    const float64_t kp{node_->declare_parameter<float64_t>("kp")};
    const float64_t ki{node_->declare_parameter<float64_t>("ki")};
    const float64_t kd{node_->declare_parameter<float64_t>("kd")};
    m_pid_vel.setGains(kp, ki, kd);

    // initialize PID limits
    const float64_t max_pid{node_->declare_parameter<float64_t>("max_out")};     // [m/s^2]
    const float64_t min_pid{node_->declare_parameter<float64_t>("min_out")};     // [m/s^2]
    const float64_t max_p{node_->declare_parameter<float64_t>("max_p_effort")};  // [m/s^2]
    const float64_t min_p{node_->declare_parameter<float64_t>("min_p_effort")};  // [m/s^2]
    const float64_t max_i{node_->declare_parameter<float64_t>("max_i_effort")};  // [m/s^2]
    const float64_t min_i{node_->declare_parameter<float64_t>("min_i_effort")};  // [m/s^2]
    const float64_t max_d{node_->declare_parameter<float64_t>("max_d_effort")};  // [m/s^2]
    const float64_t min_d{node_->declare_parameter<float64_t>("min_d_effort")};  // [m/s^2]
    m_pid_vel.setLimits(max_pid, min_pid, max_p, min_p, max_i, min_i, max_d, min_d);

    // set lowpass filter for vel error and pitch
    const float64_t lpf_vel_error_gain{node_->declare_parameter<float64_t>("lpf_vel_error_gain")};
    m_lpf_vel_error =
      std::make_shared<trajectory_follower::LowpassFilter1d>(0.0, lpf_vel_error_gain);

    m_current_vel_threshold_pid_integrate =
      node_->declare_parameter<float64_t>("current_vel_threshold_pid_integration");  // [m/s]

    m_enable_brake_keeping_before_stop =
      node_->declare_parameter<bool8_t>("enable_brake_keeping_before_stop");         // [-]
    m_brake_keeping_acc = node_->declare_parameter<float64_t>("brake_keeping_acc");  // [m/s^2]
  }

  // parameters for smooth stop state
  {
    const float64_t max_strong_acc{
      node_->declare_parameter<float64_t>("smooth_stop_max_strong_acc")};  // [m/s^2]
    const float64_t min_strong_acc{
      node_->declare_parameter<float64_t>("smooth_stop_min_strong_acc")};  // [m/s^2]
    const float64_t weak_acc{
      node_->declare_parameter<float64_t>("smooth_stop_weak_acc")};  // [m/s^2]
    const float64_t weak_stop_acc{
      node_->declare_parameter<float64_t>("smooth_stop_weak_stop_acc")};  // [m/s^2]
    const float64_t strong_stop_acc{
      node_->declare_parameter<float64_t>("smooth_stop_strong_stop_acc")};  // [m/s^2]

    const float64_t max_fast_vel{
      node_->declare_parameter<float64_t>("smooth_stop_max_fast_vel")};  // [m/s]
    const float64_t min_running_vel{
      node_->declare_parameter<float64_t>("smooth_stop_min_running_vel")};  // [m/s]
    const float64_t min_running_acc{
      node_->declare_parameter<float64_t>("smooth_stop_min_running_acc")};  // [m/s^2]
    const float64_t weak_stop_time{
      node_->declare_parameter<float64_t>("smooth_stop_weak_stop_time")};  // [s]

    const float64_t weak_stop_dist{
      node_->declare_parameter<float64_t>("smooth_stop_weak_stop_dist")};  // [m]
    const float64_t strong_stop_dist{
      node_->declare_parameter<float64_t>("smooth_stop_strong_stop_dist")};  // [m]

    m_smooth_stop.setParams(
      max_strong_acc, min_strong_acc, weak_acc, weak_stop_acc, strong_stop_acc, max_fast_vel,
      min_running_vel, min_running_acc, weak_stop_time, weak_stop_dist, strong_stop_dist);
  }

  // parameters for stop state
  {
    auto & p = m_stopped_state_params;
    p.vel = node_->declare_parameter<float64_t>("stopped_vel");    // [m/s]
    p.acc = node_->declare_parameter<float64_t>("stopped_acc");    // [m/s^2]
    p.jerk = node_->declare_parameter<float64_t>("stopped_jerk");  // [m/s^3]
  }

  // parameters for emergency state
  {
    auto & p = m_emergency_state_params;
    p.vel = node_->declare_parameter<float64_t>("emergency_vel");    // [m/s]
    p.acc = node_->declare_parameter<float64_t>("emergency_acc");    // [m/s^2]
    p.jerk = node_->declare_parameter<float64_t>("emergency_jerk");  // [m/s^3]
  }

  // parameters for acceleration limit
  m_max_acc = node_->declare_parameter<float64_t>("max_acc");  // [m/s^2]
  m_min_acc = node_->declare_parameter<float64_t>("min_acc");  // [m/s^2]

  // parameters for jerk limit
  m_max_jerk = node_->declare_parameter<float64_t>("max_jerk");  // [m/s^3]
  m_min_jerk = node_->declare_parameter<float64_t>("min_jerk");  // [m/s^3]

  // parameters for slope compensation
  m_use_traj_for_pitch = node_->declare_parameter<bool8_t>("use_trajectory_for_pitch_calculation");
  const float64_t lpf_pitch_gain{node_->declare_parameter<float64_t>("lpf_pitch_gain")};
  m_lpf_pitch = std::make_shared<trajectory_follower::LowpassFilter1d>(0.0, lpf_pitch_gain);
  m_max_pitch_rad = node_->declare_parameter<float64_t>("max_pitch_rad");  // [rad]
  m_min_pitch_rad = node_->declare_parameter<float64_t>("min_pitch_rad");  // [rad]

  // subscriber, publisher
  m_pub_slope =
    node_->create_tilde_publisher<autoware_auto_system_msgs::msg::Float32MultiArrayDiagnostic>(
      "~/output/slope_angle", rclcpp::QoS{1});
  m_pub_debug =
    node_->create_tilde_publisher<autoware_auto_system_msgs::msg::Float32MultiArrayDiagnostic>(
      "~/output/longitudinal_diagnostic", rclcpp::QoS{1});

  // set parameter callback
  m_set_param_res = node_->add_on_set_parameters_callback(
    std::bind(&PidLongitudinalController::paramCallback, this, _1));

  // set lowpass filter for acc
  m_lpf_acc = std::make_shared<trajectory_follower::LowpassFilter1d>(0.0, 0.2);
}
void PidLongitudinalController::setInputData(InputData const & input_data)
{
  setTrajectory(input_data.current_trajectory_ptr);
  setCurrentVelocity(input_data.current_odometry_ptr);
}

void PidLongitudinalController::setCurrentVelocity(
  const nav_msgs::msg::Odometry::ConstSharedPtr msg)
{
  if (!msg) return;

  if (m_current_velocity_ptr) {
    m_prev_velocity_ptr = m_current_velocity_ptr;
  }
  m_current_velocity_ptr = std::make_shared<nav_msgs::msg::Odometry>(*msg);
}

void PidLongitudinalController::setTrajectory(
  const autoware_auto_planning_msgs::msg::Trajectory::ConstSharedPtr msg)
{
  if (!msg) return;

  if (!trajectory_follower::longitudinal_utils::isValidTrajectory(*msg)) {
    RCLCPP_ERROR_THROTTLE(
      node_->get_logger(), *node_->get_clock(), 3000, "received invalid trajectory. ignore.");
    return;
  }

  if (msg->points.size() < 2) {
    RCLCPP_WARN_THROTTLE(
      node_->get_logger(), *node_->get_clock(), 3000, "Unexpected trajectory size < 2. Ignored.");
    return;
  }

  m_trajectory_ptr = std::make_shared<autoware_auto_planning_msgs::msg::Trajectory>(*msg);
}

rcl_interfaces::msg::SetParametersResult PidLongitudinalController::paramCallback(
  const std::vector<rclcpp::Parameter> & parameters)
{
  auto update_param = [&](const std::string & name, float64_t & v) {
    auto it = std::find_if(
      parameters.cbegin(), parameters.cend(),
      [&name](const rclcpp::Parameter & parameter) { return parameter.get_name() == name; });
    if (it != parameters.cend()) {
      v = it->as_double();
      return true;
    }
    return false;
  };

  // delay compensation
  update_param("delay_compensation_time", m_delay_compensation_time);

  // state transition
  {
    auto & p = m_state_transition_params;
    update_param("drive_state_stop_dist", p.drive_state_stop_dist);
    update_param("stopping_state_stop_dist", p.stopping_state_stop_dist);
    update_param("stopped_state_entry_duration_time", p.stopped_state_entry_duration_time);
    update_param("stopped_state_entry_vel", p.stopped_state_entry_vel);
    update_param("stopped_state_entry_acc", p.stopped_state_entry_acc);
    update_param("emergency_state_overshoot_stop_dist", p.emergency_state_overshoot_stop_dist);
    update_param("emergency_state_traj_trans_dev", p.emergency_state_traj_trans_dev);
    update_param("emergency_state_traj_rot_dev", p.emergency_state_traj_rot_dev);
  }

  // drive state
  {
    float64_t kp{node_->get_parameter("kp").as_double()};
    float64_t ki{node_->get_parameter("ki").as_double()};
    float64_t kd{node_->get_parameter("kd").as_double()};
    update_param("kp", kp);
    update_param("ki", ki);
    update_param("kd", kd);
    m_pid_vel.setGains(kp, ki, kd);

    float64_t max_pid{node_->get_parameter("max_out").as_double()};
    float64_t min_pid{node_->get_parameter("min_out").as_double()};
    float64_t max_p{node_->get_parameter("max_p_effort").as_double()};
    float64_t min_p{node_->get_parameter("min_p_effort").as_double()};
    float64_t max_i{node_->get_parameter("max_i_effort").as_double()};
    float64_t min_i{node_->get_parameter("min_i_effort").as_double()};
    float64_t max_d{node_->get_parameter("max_d_effort").as_double()};
    float64_t min_d{node_->get_parameter("min_d_effort").as_double()};
    update_param("max_out", max_pid);
    update_param("min_out", min_pid);
    update_param("max_p_effort", max_p);
    update_param("min_p_effort", min_p);
    update_param("max_i_effort", max_i);
    update_param("min_i_effort", min_i);
    update_param("max_d_effort", max_d);
    update_param("min_d_effort", min_d);
    m_pid_vel.setLimits(max_pid, min_pid, max_p, min_p, max_i, min_i, max_d, min_d);

    update_param("current_vel_threshold_pid_integration", m_current_vel_threshold_pid_integrate);
  }

  // stopping state
  {
    float64_t max_strong_acc{node_->get_parameter("smooth_stop_max_strong_acc").as_double()};
    float64_t min_strong_acc{node_->get_parameter("smooth_stop_min_strong_acc").as_double()};
    float64_t weak_acc{node_->get_parameter("smooth_stop_weak_acc").as_double()};
    float64_t weak_stop_acc{node_->get_parameter("smooth_stop_weak_stop_acc").as_double()};
    float64_t strong_stop_acc{node_->get_parameter("smooth_stop_strong_stop_acc").as_double()};
    float64_t max_fast_vel{node_->get_parameter("smooth_stop_max_fast_vel").as_double()};
    float64_t min_running_vel{node_->get_parameter("smooth_stop_min_running_vel").as_double()};
    float64_t min_running_acc{node_->get_parameter("smooth_stop_min_running_acc").as_double()};
    float64_t weak_stop_time{node_->get_parameter("smooth_stop_weak_stop_time").as_double()};
    float64_t weak_stop_dist{node_->get_parameter("smooth_stop_weak_stop_dist").as_double()};
    float64_t strong_stop_dist{node_->get_parameter("smooth_stop_strong_stop_dist").as_double()};
    update_param("smooth_stop_max_strong_acc", max_strong_acc);
    update_param("smooth_stop_min_strong_acc", min_strong_acc);
    update_param("smooth_stop_weak_acc", weak_acc);
    update_param("smooth_stop_weak_stop_acc", weak_stop_acc);
    update_param("smooth_stop_strong_stop_acc", strong_stop_acc);
    update_param("smooth_stop_max_fast_vel", max_fast_vel);
    update_param("smooth_stop_min_running_vel", min_running_vel);
    update_param("smooth_stop_min_running_acc", min_running_acc);
    update_param("smooth_stop_weak_stop_time", weak_stop_time);
    update_param("smooth_stop_weak_stop_dist", weak_stop_dist);
    update_param("smooth_stop_strong_stop_dist", strong_stop_dist);
    m_smooth_stop.setParams(
      max_strong_acc, min_strong_acc, weak_acc, weak_stop_acc, strong_stop_acc, max_fast_vel,
      min_running_vel, min_running_acc, weak_stop_time, weak_stop_dist, strong_stop_dist);
  }

  // stop state
  {
    auto & p = m_stopped_state_params;
    update_param("stopped_vel", p.vel);
    update_param("stopped_acc", p.acc);
    update_param("stopped_jerk", p.jerk);
  }

  // emergency state
  {
    auto & p = m_emergency_state_params;
    update_param("emergency_vel", p.vel);
    update_param("emergency_acc", p.acc);
    update_param("emergency_jerk", p.jerk);
  }

  // acceleration limit
  update_param("min_acc", m_min_acc);

  // jerk limit
  update_param("max_jerk", m_max_jerk);
  update_param("min_jerk", m_min_jerk);

  // slope compensation
  update_param("max_pitch_rad", m_max_pitch_rad);
  update_param("min_pitch_rad", m_min_pitch_rad);

  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";
  return result;
}

boost::optional<LongitudinalOutput> PidLongitudinalController::run()
{
  // wait for initial pointers
  if (
    !m_current_velocity_ptr || !m_prev_velocity_ptr || !m_trajectory_ptr ||
    !m_tf_buffer.canTransform(m_trajectory_ptr->header.frame_id, "base_link", tf2::TimePointZero)) {
    return boost::none;
  }

  // get current ego pose
  geometry_msgs::msg::TransformStamped tf =
    m_tf_buffer.lookupTransform(m_trajectory_ptr->header.frame_id, "base_link", tf2::TimePointZero);

  // calculate current pose and control data
  geometry_msgs::msg::Pose current_pose;
  current_pose.position.x = tf.transform.translation.x;
  current_pose.position.y = tf.transform.translation.y;
  current_pose.position.z = tf.transform.translation.z;
  current_pose.orientation = tf.transform.rotation;

  const auto control_data = getControlData(current_pose);

  // self pose is far from trajectory
  if (control_data.is_far_from_trajectory) {
    if (m_enable_large_tracking_error_emergency) {
      m_control_state = ControlState::EMERGENCY;  // update control state
    }
    const Motion raw_ctrl_cmd = calcEmergencyCtrlCmd(control_data.dt);  // calculate control command
    m_prev_raw_ctrl_cmd = raw_ctrl_cmd;
    const auto cmd_msg =
      createCtrlCmdMsg(raw_ctrl_cmd, control_data.current_motion.vel);  // create control command
    publishDebugData(raw_ctrl_cmd, control_data);                       // publish debug data
    LongitudinalOutput output;
    output.control_cmd = cmd_msg;
    return output;
  }

  // update control state
  m_control_state = updateControlState(m_control_state, control_data);

  // calculate control command
  const Motion ctrl_cmd = calcCtrlCmd(m_control_state, current_pose, control_data);

  // publish control command
  const auto cmd_msg = createCtrlCmdMsg(ctrl_cmd, control_data.current_motion.vel);
  LongitudinalOutput output;
  output.control_cmd = cmd_msg;

  // publish debug data
  publishDebugData(ctrl_cmd, control_data);

  return output;
}

PidLongitudinalController::ControlData PidLongitudinalController::getControlData(
  const geometry_msgs::msg::Pose & current_pose)
{
  ControlData control_data{};

  // dt
  control_data.dt = getDt();

  // current velocity and acceleration
  control_data.current_motion = getCurrentMotion();

  // nearest idx
  const float64_t max_dist = m_state_transition_params.emergency_state_traj_trans_dev;
  const float64_t max_yaw = m_state_transition_params.emergency_state_traj_rot_dev;
  const auto nearest_idx_opt =
    motion_common::findNearestIndex(m_trajectory_ptr->points, current_pose, max_dist, max_yaw);

  // return here if nearest index is not found
  if (!nearest_idx_opt) {
    control_data.is_far_from_trajectory = true;
    return control_data;
  }
  control_data.nearest_idx = *nearest_idx_opt;

  // shift
  control_data.shift = getCurrentShift(control_data.nearest_idx);
  if (control_data.shift != m_prev_shift) {
    m_pid_vel.reset();
  }
  m_prev_shift = control_data.shift;

  // distance to stopline
  control_data.stop_dist = trajectory_follower::longitudinal_utils::calcStopDistance(
    current_pose, *m_trajectory_ptr, max_dist, max_yaw);

  // pitch
  const float64_t raw_pitch =
    trajectory_follower::longitudinal_utils::getPitchByPose(current_pose.orientation);
  const float64_t traj_pitch = trajectory_follower::longitudinal_utils::getPitchByTraj(
    *m_trajectory_ptr, control_data.nearest_idx, m_wheel_base);
  control_data.slope_angle = m_use_traj_for_pitch ? traj_pitch : m_lpf_pitch->filter(raw_pitch);
  updatePitchDebugValues(control_data.slope_angle, traj_pitch, raw_pitch);

  return control_data;
}

PidLongitudinalController::Motion PidLongitudinalController::calcEmergencyCtrlCmd(
  const float64_t dt) const
{
  // These accelerations are without slope compensation
  const auto & p = m_emergency_state_params;
  const float64_t vel = trajectory_follower::longitudinal_utils::applyDiffLimitFilter(
    p.vel, m_prev_raw_ctrl_cmd.vel, dt, p.acc);
  const float64_t acc = trajectory_follower::longitudinal_utils::applyDiffLimitFilter(
    p.acc, m_prev_raw_ctrl_cmd.acc, dt, p.jerk);

  RCLCPP_ERROR_THROTTLE(
    node_->get_logger(), *node_->get_clock(), 3000, "[Emergency stop] vel: %3.3f, acc: %3.3f", vel,
    acc);

  return Motion{vel, acc};
}

PidLongitudinalController::ControlState PidLongitudinalController::updateControlState(
  const ControlState current_control_state, const ControlData & control_data)
{
  const float64_t current_vel = control_data.current_motion.vel;
  const float64_t current_acc = control_data.current_motion.acc;
  const float64_t stop_dist = control_data.stop_dist;

  // flags for state transition
  const auto & p = m_state_transition_params;

  const bool8_t departure_condition_from_stopping =
    stop_dist > p.drive_state_stop_dist + p.drive_state_offset_stop_dist;
  const bool8_t departure_condition_from_stopped = stop_dist > p.drive_state_stop_dist;

  const bool8_t keep_stopped_condition =
    m_enable_keep_stopped_until_steer_convergence && !lateral_sync_data_.is_steer_converged;

  const bool8_t stopping_condition = stop_dist < p.stopping_state_stop_dist;
  if (
    std::fabs(current_vel) > p.stopped_state_entry_vel ||
    std::fabs(current_acc) > p.stopped_state_entry_acc) {
    m_last_running_time = std::make_shared<rclcpp::Time>(node_->now());
  }
  const bool8_t stopped_condition =
    m_last_running_time
      ? (node_->now() - *m_last_running_time).seconds() > p.stopped_state_entry_duration_time
      : false;

  const bool8_t emergency_condition =
    m_enable_overshoot_emergency && stop_dist < -p.emergency_state_overshoot_stop_dist;

  // transit state
  if (current_control_state == ControlState::DRIVE) {
    if (emergency_condition) {
      return ControlState::EMERGENCY;
    }

    if (m_enable_smooth_stop) {
      if (stopping_condition) {
        // predictions after input time delay
        const float64_t pred_vel_in_target =
          predictedVelocityInTargetPoint(control_data.current_motion, m_delay_compensation_time);
        const float64_t pred_stop_dist =
          control_data.stop_dist -
          0.5 * (pred_vel_in_target + current_vel) * m_delay_compensation_time;
        m_smooth_stop.init(pred_vel_in_target, pred_stop_dist);
        return ControlState::STOPPING;
      }
    } else {
      if (stopped_condition && !departure_condition_from_stopped) {
        return ControlState::STOPPED;
      }
    }
  } else if (current_control_state == ControlState::STOPPING) {
    if (emergency_condition) {
      return ControlState::EMERGENCY;
    }

    if (stopped_condition) {
      return ControlState::STOPPED;
    }

    if (departure_condition_from_stopping) {
      m_pid_vel.reset();
      m_lpf_vel_error->reset(0.0);
      // prevent the car from taking a long time to start to move
      m_prev_ctrl_cmd.acc = std::max(0.0, m_prev_ctrl_cmd.acc);
      return ControlState::DRIVE;
    }
  } else if (current_control_state == ControlState::STOPPED) {
    if (keep_stopped_condition) return ControlState::STOPPED;
    if (departure_condition_from_stopped) {
      m_pid_vel.reset();
      m_lpf_vel_error->reset(0.0);
      // prevent the car from taking a long time to start to move
      m_prev_ctrl_cmd.acc = std::max(0.0, m_prev_ctrl_cmd.acc);
      return ControlState::DRIVE;
    }
  } else if (m_control_state == ControlState::EMERGENCY) {
    if (stopped_condition && !emergency_condition) {
      return ControlState::STOPPED;
    }
  }

  return current_control_state;
}

PidLongitudinalController::Motion PidLongitudinalController::calcCtrlCmd(
  const ControlState & current_control_state, const geometry_msgs::msg::Pose & current_pose,
  const ControlData & control_data)
{
  const size_t nearest_idx = control_data.nearest_idx;
  const float64_t current_vel = control_data.current_motion.vel;
  const float64_t current_acc = control_data.current_motion.acc;

  // velocity and acceleration command
  Motion raw_ctrl_cmd{};
  Motion target_motion{};
  if (current_control_state == ControlState::DRIVE) {
    const auto target_pose = trajectory_follower::longitudinal_utils::calcPoseAfterTimeDelay(
      current_pose, m_delay_compensation_time, current_vel);
    const auto target_interpolated_point =
      calcInterpolatedTargetValue(*m_trajectory_ptr, target_pose, nearest_idx);
    target_motion = Motion{
      target_interpolated_point.longitudinal_velocity_mps,
      target_interpolated_point.acceleration_mps2};

    target_motion = keepBrakeBeforeStop(*m_trajectory_ptr, target_motion, nearest_idx);

    const float64_t pred_vel_in_target =
      predictedVelocityInTargetPoint(control_data.current_motion, m_delay_compensation_time);
    m_debug_values.setValues(
      trajectory_follower::DebugValues::TYPE::PREDICTED_VEL, pred_vel_in_target);

    raw_ctrl_cmd.vel = target_motion.vel;
    raw_ctrl_cmd.acc = applyVelocityFeedback(target_motion, control_data.dt, pred_vel_in_target);
    RCLCPP_DEBUG(
      node_->get_logger(),
      "[feedback control]  vel: %3.3f, acc: %3.3f, dt: %3.3f, v_curr: %3.3f, v_ref: %3.3f "
      "feedback_ctrl_cmd.ac: %3.3f",
      raw_ctrl_cmd.vel, raw_ctrl_cmd.acc, control_data.dt, current_vel, target_motion.vel,
      raw_ctrl_cmd.acc);
  } else if (current_control_state == ControlState::STOPPING) {
    raw_ctrl_cmd.acc = m_smooth_stop.calculate(
      control_data.stop_dist, current_vel, current_acc, m_vel_hist, m_delay_compensation_time);
    raw_ctrl_cmd.vel = m_stopped_state_params.vel;

    RCLCPP_DEBUG(
      node_->get_logger(), "[smooth stop]: Smooth stopping. vel: %3.3f, acc: %3.3f",
      raw_ctrl_cmd.vel, raw_ctrl_cmd.acc);
  } else if (current_control_state == ControlState::STOPPED) {
    // This acceleration is without slope compensation
    const auto & p = m_stopped_state_params;
    raw_ctrl_cmd.vel = p.vel;
    raw_ctrl_cmd.acc = trajectory_follower::longitudinal_utils::applyDiffLimitFilter(
      p.acc, m_prev_raw_ctrl_cmd.acc, control_data.dt, p.jerk);

    RCLCPP_DEBUG(
      node_->get_logger(), "[Stopped]. vel: %3.3f, acc: %3.3f", raw_ctrl_cmd.vel, raw_ctrl_cmd.acc);
  } else if (current_control_state == ControlState::EMERGENCY) {
    raw_ctrl_cmd = calcEmergencyCtrlCmd(control_data.dt);
  }

  // store acceleration without slope compensation
  m_prev_raw_ctrl_cmd = raw_ctrl_cmd;

  // apply slope compensation and filter acceleration and jerk
  const float64_t filtered_acc_cmd = calcFilteredAcc(raw_ctrl_cmd.acc, control_data);
  const Motion filtered_ctrl_cmd{raw_ctrl_cmd.vel, filtered_acc_cmd};

  // update debug visualization
  updateDebugVelAcc(target_motion, current_pose, control_data);

  return filtered_ctrl_cmd;
}

// Do not use nearest_idx here
autoware_auto_control_msgs::msg::LongitudinalCommand PidLongitudinalController::createCtrlCmdMsg(
  const Motion & ctrl_cmd, const float64_t & current_vel)
{
  // publish control command
  autoware_auto_control_msgs::msg::LongitudinalCommand cmd{};
  cmd.stamp = node_->now();
  cmd.speed = static_cast<decltype(cmd.speed)>(ctrl_cmd.vel);
  cmd.acceleration = static_cast<decltype(cmd.acceleration)>(ctrl_cmd.acc);

  // store current velocity history
  m_vel_hist.push_back({node_->now(), current_vel});
  while (m_vel_hist.size() > static_cast<size_t>(0.5 / m_longitudinal_ctrl_period)) {
    m_vel_hist.erase(m_vel_hist.begin());
  }

  m_prev_ctrl_cmd = ctrl_cmd;

  return cmd;
}

void PidLongitudinalController::publishDebugData(
  const Motion & ctrl_cmd, const ControlData & control_data)
{
  using trajectory_follower::DebugValues;
  // set debug values
  m_debug_values.setValues(DebugValues::TYPE::DT, control_data.dt);
  m_debug_values.setValues(DebugValues::TYPE::CALCULATED_ACC, control_data.current_motion.acc);
  m_debug_values.setValues(DebugValues::TYPE::SHIFT, static_cast<float64_t>(control_data.shift));
  m_debug_values.setValues(DebugValues::TYPE::STOP_DIST, control_data.stop_dist);
  m_debug_values.setValues(
    DebugValues::TYPE::CONTROL_STATE, static_cast<float64_t>(m_control_state));
  m_debug_values.setValues(DebugValues::TYPE::ACC_CMD_PUBLISHED, ctrl_cmd.acc);

  // publish debug values
  autoware_auto_system_msgs::msg::Float32MultiArrayDiagnostic debug_msg{};
  debug_msg.diag_header.data_stamp = node_->now();
  for (const auto & v : m_debug_values.getValues()) {
    debug_msg.diag_array.data.push_back(
      static_cast<decltype(debug_msg.diag_array.data)::value_type>(v));
  }
  m_pub_debug->publish(debug_msg);

  // slope angle
  autoware_auto_system_msgs::msg::Float32MultiArrayDiagnostic slope_msg{};
  slope_msg.diag_header.data_stamp = node_->now();
  slope_msg.diag_array.data.push_back(
    static_cast<decltype(slope_msg.diag_array.data)::value_type>(control_data.slope_angle));
  m_pub_slope->publish(slope_msg);
}

float64_t PidLongitudinalController::getDt()
{
  float64_t dt;
  if (!m_prev_control_time) {
    dt = m_longitudinal_ctrl_period;
    m_prev_control_time = std::make_shared<rclcpp::Time>(node_->now());
  } else {
    dt = (node_->now() - *m_prev_control_time).seconds();
    *m_prev_control_time = node_->now();
  }
  const float64_t max_dt = m_longitudinal_ctrl_period * 2.0;
  const float64_t min_dt = m_longitudinal_ctrl_period * 0.5;
  return std::max(std::min(dt, max_dt), min_dt);
}

PidLongitudinalController::Motion PidLongitudinalController::getCurrentMotion() const
{
  const float64_t dv =
    m_current_velocity_ptr->twist.twist.linear.x - m_prev_velocity_ptr->twist.twist.linear.x;
  const float64_t dt = std::max(
    (rclcpp::Time(m_current_velocity_ptr->header.stamp) -
     rclcpp::Time(m_prev_velocity_ptr->header.stamp))
      .seconds(),
    1e-03);
  const float64_t accel = dv / dt;

  const float64_t current_vel = m_current_velocity_ptr->twist.twist.linear.x;
  const float64_t current_acc = m_lpf_acc->filter(accel);

  return Motion{current_vel, current_acc};
}

enum PidLongitudinalController::Shift PidLongitudinalController::getCurrentShift(
  const size_t nearest_idx) const
{
  constexpr float64_t epsilon = 1e-5;

  const float64_t target_vel = m_trajectory_ptr->points.at(nearest_idx).longitudinal_velocity_mps;

  if (target_vel > epsilon) {
    return Shift::Forward;
  } else if (target_vel < -epsilon) {
    return Shift::Reverse;
  }

  return m_prev_shift;
}

float64_t PidLongitudinalController::calcFilteredAcc(
  const float64_t raw_acc, const ControlData & control_data)
{
  using trajectory_follower::DebugValues;
  const float64_t acc_max_filtered = ::motion::motion_common::clamp(raw_acc, m_min_acc, m_max_acc);
  m_debug_values.setValues(DebugValues::TYPE::ACC_CMD_ACC_LIMITED, acc_max_filtered);

  // store ctrl cmd without slope filter
  storeAccelCmd(acc_max_filtered);

  const float64_t acc_slope_filtered =
    applySlopeCompensation(acc_max_filtered, control_data.slope_angle, control_data.shift);
  m_debug_values.setValues(DebugValues::TYPE::ACC_CMD_SLOPE_APPLIED, acc_slope_filtered);

  // This jerk filter must be applied after slope compensation
  const float64_t acc_jerk_filtered = trajectory_follower::longitudinal_utils::applyDiffLimitFilter(
    acc_slope_filtered, m_prev_ctrl_cmd.acc, control_data.dt, m_max_jerk, m_min_jerk);
  m_debug_values.setValues(DebugValues::TYPE::ACC_CMD_JERK_LIMITED, acc_jerk_filtered);

  return acc_jerk_filtered;
}

void PidLongitudinalController::storeAccelCmd(const float64_t accel)
{
  if (m_control_state == ControlState::DRIVE) {
    // convert format
    autoware_auto_control_msgs::msg::LongitudinalCommand cmd;
    cmd.stamp = node_->now();
    cmd.acceleration = static_cast<decltype(cmd.acceleration)>(accel);

    // store published ctrl cmd
    m_ctrl_cmd_vec.emplace_back(cmd);
  } else {
    // reset command
    m_ctrl_cmd_vec.clear();
  }

  // remove unused ctrl cmd
  if (m_ctrl_cmd_vec.size() <= 2) {
    return;
  }
  if ((node_->now() - m_ctrl_cmd_vec.at(1).stamp).seconds() > m_delay_compensation_time) {
    m_ctrl_cmd_vec.erase(m_ctrl_cmd_vec.begin());
  }
}

float64_t PidLongitudinalController::applySlopeCompensation(
  const float64_t input_acc, const float64_t pitch, const Shift shift) const
{
  if (!m_enable_slope_compensation) {
    return input_acc;
  }
  const float64_t pitch_limited = std::min(std::max(pitch, m_min_pitch_rad), m_max_pitch_rad);

  // Acceleration command is always positive independent of direction (= shift) when car is running
  float64_t sign = (shift == Shift::Forward) ? -1 : (shift == Shift::Reverse ? 1 : 0);
  float64_t compensated_acc = input_acc + sign * 9.81 * std::sin(pitch_limited);
  return compensated_acc;
}

PidLongitudinalController::Motion PidLongitudinalController::keepBrakeBeforeStop(
  const autoware_auto_planning_msgs::msg::Trajectory & traj, const Motion & target_motion,
  const size_t nearest_idx) const
{
  Motion output_motion = target_motion;

  if (m_enable_brake_keeping_before_stop == false) {
    return output_motion;
  }
  // const auto stop_idx = motion_utils::searchZeroVelocityIndex(traj.points);
  const auto stop_idx = motion_common::searchZeroVelocityIndex(traj.points);
  if (!stop_idx) {
    return output_motion;
  }

  float64_t min_acc_before_stop = std::numeric_limits<float64_t>::max();
  size_t min_acc_idx = std::numeric_limits<size_t>::max();
  for (int i = static_cast<int>(*stop_idx); i >= 0; --i) {
    const auto ui = static_cast<size_t>(i);
    if (traj.points.at(ui).acceleration_mps2 > static_cast<float>(min_acc_before_stop)) {
      break;
    }
    min_acc_before_stop = traj.points.at(ui).acceleration_mps2;
    min_acc_idx = ui;
  }

  const float64_t brake_keeping_acc = std::max(m_brake_keeping_acc, min_acc_before_stop);
  if (nearest_idx >= min_acc_idx && target_motion.acc > brake_keeping_acc) {
    output_motion.acc = brake_keeping_acc;
  }

  return output_motion;
}

autoware_auto_planning_msgs::msg::TrajectoryPoint
PidLongitudinalController::calcInterpolatedTargetValue(
  const autoware_auto_planning_msgs::msg::Trajectory & traj, const geometry_msgs::msg::Pose & pose,
  const size_t nearest_idx) const
{
  if (traj.points.size() == 1) {
    return traj.points.at(0);
  }

  // If the current position is not within the reference trajectory, enable the edge value.
  // Else, apply linear interpolation
  if (nearest_idx == 0) {
    if (motion_common::calcSignedArcLength(traj.points, pose.position, 0) > 0) {
      return traj.points.at(0);
    }
  }
  if (nearest_idx == traj.points.size() - 1) {
    if (
      motion_common::calcSignedArcLength(traj.points, pose.position, traj.points.size() - 1) < 0) {
      return traj.points.at(traj.points.size() - 1);
    }
  }

  // apply linear interpolation
  return trajectory_follower::longitudinal_utils::lerpTrajectoryPoint(
    traj.points, pose, m_state_transition_params.emergency_state_traj_trans_dev,
    m_state_transition_params.emergency_state_traj_rot_dev);
}

float64_t PidLongitudinalController::predictedVelocityInTargetPoint(
  const Motion current_motion, const float64_t delay_compensation_time) const
{
  const float64_t current_vel = current_motion.vel;
  const float64_t current_acc = current_motion.acc;

  if (std::fabs(current_vel) < 1e-01) {  // when velocity is low, no prediction
    return current_vel;
  }

  const float64_t current_vel_abs = std::fabs(current_vel);
  if (m_ctrl_cmd_vec.size() == 0) {
    const float64_t pred_vel = current_vel + current_acc * delay_compensation_time;
    // avoid to change sign of current_vel and pred_vel
    return pred_vel > 0 ? std::copysign(pred_vel, current_vel) : 0.0;
  }

  float64_t pred_vel = current_vel_abs;

  const auto past_delay_time =
    node_->now() - rclcpp::Duration::from_seconds(delay_compensation_time);
  for (std::size_t i = 0; i < m_ctrl_cmd_vec.size(); ++i) {
    if ((node_->now() - m_ctrl_cmd_vec.at(i).stamp).seconds() < m_delay_compensation_time) {
      if (i == 0) {
        // size of m_ctrl_cmd_vec is less than m_delay_compensation_time
        pred_vel = current_vel_abs + static_cast<float64_t>(m_ctrl_cmd_vec.at(i).acceleration) *
                                       delay_compensation_time;
        return pred_vel > 0 ? std::copysign(pred_vel, current_vel) : 0.0;
      }
      // add velocity to accel * dt
      const float64_t acc = m_ctrl_cmd_vec.at(i - 1).acceleration;
      const auto curr_time_i = rclcpp::Time(m_ctrl_cmd_vec.at(i).stamp);
      const float64_t time_to_next_acc = std::min(
        (curr_time_i - rclcpp::Time(m_ctrl_cmd_vec.at(i - 1).stamp)).seconds(),
        (curr_time_i - past_delay_time).seconds());
      pred_vel += acc * time_to_next_acc;
    }
  }

  const float64_t last_acc = m_ctrl_cmd_vec.at(m_ctrl_cmd_vec.size() - 1).acceleration;
  const float64_t time_to_current =
    (node_->now() - m_ctrl_cmd_vec.at(m_ctrl_cmd_vec.size() - 1).stamp).seconds();
  pred_vel += last_acc * time_to_current;

  // avoid to change sign of current_vel and pred_vel
  return pred_vel > 0 ? std::copysign(pred_vel, current_vel) : 0.0;
}

float64_t PidLongitudinalController::applyVelocityFeedback(
  const Motion target_motion, const float64_t dt, const float64_t current_vel)
{
  using trajectory_follower::DebugValues;
  const float64_t current_vel_abs = std::fabs(current_vel);
  const float64_t target_vel_abs = std::fabs(target_motion.vel);
  const bool8_t enable_integration = (current_vel_abs > m_current_vel_threshold_pid_integrate);
  const float64_t error_vel_filtered = m_lpf_vel_error->filter(target_vel_abs - current_vel_abs);

  std::vector<float64_t> pid_contributions(3);
  const float64_t pid_acc =
    m_pid_vel.calculate(error_vel_filtered, dt, enable_integration, pid_contributions);
  const float64_t feedback_acc = target_motion.acc + pid_acc;

  m_debug_values.setValues(DebugValues::TYPE::ACC_CMD_PID_APPLIED, feedback_acc);
  m_debug_values.setValues(DebugValues::TYPE::ERROR_VEL_FILTERED, error_vel_filtered);
  m_debug_values.setValues(
    DebugValues::TYPE::ACC_CMD_FB_P_CONTRIBUTION, pid_contributions.at(0));  // P
  m_debug_values.setValues(
    DebugValues::TYPE::ACC_CMD_FB_I_CONTRIBUTION, pid_contributions.at(1));  // I
  m_debug_values.setValues(
    DebugValues::TYPE::ACC_CMD_FB_D_CONTRIBUTION, pid_contributions.at(2));  // D

  return feedback_acc;
}

void PidLongitudinalController::updatePitchDebugValues(
  const float64_t pitch, const float64_t traj_pitch, const float64_t raw_pitch)
{
  using trajectory_follower::DebugValues;
  const float64_t to_degrees = (180.0 / static_cast<float64_t>(autoware::common::types::PI));
  m_debug_values.setValues(DebugValues::TYPE::PITCH_LPF_RAD, pitch);
  m_debug_values.setValues(DebugValues::TYPE::PITCH_LPF_DEG, pitch * to_degrees);
  m_debug_values.setValues(DebugValues::TYPE::PITCH_RAW_RAD, raw_pitch);
  m_debug_values.setValues(DebugValues::TYPE::PITCH_RAW_DEG, raw_pitch * to_degrees);
  m_debug_values.setValues(DebugValues::TYPE::PITCH_RAW_TRAJ_RAD, traj_pitch);
  m_debug_values.setValues(DebugValues::TYPE::PITCH_RAW_TRAJ_DEG, traj_pitch * to_degrees);
}

void PidLongitudinalController::updateDebugVelAcc(
  const Motion & target_motion, const geometry_msgs::msg::Pose & current_pose,
  const ControlData & control_data)
{
  using trajectory_follower::DebugValues;
  const float64_t current_vel = control_data.current_motion.vel;
  const size_t nearest_idx = control_data.nearest_idx;

  const auto interpolated_point =
    calcInterpolatedTargetValue(*m_trajectory_ptr, current_pose, nearest_idx);

  m_debug_values.setValues(DebugValues::TYPE::CURRENT_VEL, current_vel);
  m_debug_values.setValues(DebugValues::TYPE::TARGET_VEL, target_motion.vel);
  m_debug_values.setValues(DebugValues::TYPE::TARGET_ACC, target_motion.acc);
  m_debug_values.setValues(
    DebugValues::TYPE::NEAREST_VEL, interpolated_point.longitudinal_velocity_mps);
  m_debug_values.setValues(DebugValues::TYPE::NEAREST_ACC, interpolated_point.acceleration_mps2);
  m_debug_values.setValues(DebugValues::TYPE::ERROR_VEL, target_motion.vel - current_vel);
}

}  // namespace trajectory_follower
}  // namespace control
}  // namespace motion
}  // namespace autoware
