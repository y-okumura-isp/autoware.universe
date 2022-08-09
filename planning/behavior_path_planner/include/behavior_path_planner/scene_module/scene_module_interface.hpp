// Copyright 2021 Tier IV, Inc.
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

#ifndef BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_
#define BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_

#include "behavior_path_planner/data_manager.hpp"
#include "behavior_path_planner/utilities.hpp"

#include <rclcpp/rclcpp.hpp>
#include <route_handler/route_handler.hpp>
#include <rtc_interface/rtc_interface.hpp>

#include <autoware_auto_planning_msgs/msg/path_with_lane_id.hpp>
#include <autoware_auto_vehicle_msgs/msg/hazard_lights_command.hpp>
#include <autoware_auto_vehicle_msgs/msg/turn_indicators_command.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_factor.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_msg.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_msg_array.hpp>
#include <unique_identifier_msgs/msg/uuid.hpp>

#include <boost/optional.hpp>

#include <behaviortree_cpp_v3/basic_types.h>

#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>

#include "tilde/tilde_publisher.hpp"
#include "tilde/tilde_node.hpp"

namespace behavior_path_planner
{
using autoware_auto_planning_msgs::msg::PathWithLaneId;
using autoware_auto_vehicle_msgs::msg::HazardLightsCommand;
using autoware_auto_vehicle_msgs::msg::TurnIndicatorsCommand;
using route_handler::LaneChangeDirection;
using route_handler::PullOutDirection;
using route_handler::PullOverDirection;
using rtc_interface::RTCInterface;
using tier4_planning_msgs::msg::AvoidanceDebugMsgArray;
using unique_identifier_msgs::msg::UUID;
using visualization_msgs::msg::MarkerArray;
using PlanResult = PathWithLaneId::SharedPtr;

struct TurnSignalInfo
{
  TurnSignalInfo()
  {
    turn_signal.command = TurnIndicatorsCommand::NO_COMMAND;
    hazard_signal.command = HazardLightsCommand::NO_COMMAND;
  }

  // desired turn signal
  TurnIndicatorsCommand turn_signal;
  HazardLightsCommand hazard_signal;

  // TODO(Horibe) replace with point. Distance should be calculates in turn_signal_decider.
  // distance to the turn signal trigger point (to choose nearest signal for multiple requests)
  double signal_distance{std::numeric_limits<double>::max()};
};

struct BehaviorModuleOutput
{
  BehaviorModuleOutput() {}

  // path planed by module
  PlanResult path{};

  // path candidate planed by module
  PlanResult path_candidate{};

  TurnSignalInfo turn_signal_info{};
};

struct CandidateOutput
{
  CandidateOutput() {}
  explicit CandidateOutput(const PathWithLaneId & path) : path_candidate{path} {}
  PathWithLaneId path_candidate{};
  double lateral_shift{0.0};
  double distance_to_path_change{std::numeric_limits<double>::lowest()};
};

class SceneModuleInterface
{
public:
  SceneModuleInterface(const std::string & name, rclcpp::Node & node)
  : name_{name},
    logger_{node.get_logger().get_child(name)},
    clock_{node.get_clock()},
    uuid_(generateUUID()),
    is_waiting_approval_{false},
    current_state_{BT::NodeStatus::IDLE}
  {
  }

  virtual ~SceneModuleInterface() = default;

  /**
   * @brief Return SUCCESS if plan is not needed or plan is successfully finished,
   *        FAILURE if plan has failed, RUNNING if plan is on going.
   *        These condition is to be implemented in each modules.
   */
  virtual BT::NodeStatus updateState() = 0;

  /**
   * @brief Return true if the module has request for execution (not necessarily feasible)
   */
  virtual bool isExecutionRequested() const = 0;

  /**
   * @brief Return true if the execution is available (e.g. safety check for lane change)
   */
  virtual bool isExecutionReady() const = 0;

  /**
   * @brief Calculate path. This function is called with the plan is approved.
   */
  virtual BehaviorModuleOutput plan() = 0;

  /**
   * @brief Calculate path under waiting_approval condition.
   *        The default implementation is just to return the reference path.
   */
  virtual BehaviorModuleOutput planWaitingApproval()
  {
    BehaviorModuleOutput out;
    out.path = util::generateCenterLinePath(planner_data_);
    const auto candidate = planCandidate();
    out.path_candidate = std::make_shared<PathWithLaneId>(candidate.path_candidate);
    return out;
  }

  /**
   * @brief Get candidate path. This information is used for external judgement.
   */
  virtual CandidateOutput planCandidate() const = 0;

  /**
   * @brief update data for planning. Note that the call of this function does not mean
   *        that the module executed. It should only updates the data necessary for
   *        planCandidate (e.g., resampling of path).
   */
  virtual void updateData() {}

  /**
   * @brief Execute module. Once this function is executed,
   *        it will continue to run as long as it is in the RUNNING state.
   */
  virtual BehaviorModuleOutput run()
  {
    current_state_ = BT::NodeStatus::RUNNING;

    updateData();

    if (!isWaitingApproval()) {
      return plan();
    }

    // module is waiting approval. Check it.
    if (isActivated()) {
      RCLCPP_DEBUG(logger_, "Was waiting approval, and now approved. Do plan().");
      return plan();
    } else {
      RCLCPP_DEBUG(logger_, "keep waiting approval... Do planCandidate().");
      return planWaitingApproval();
    }
  }

  /**
   * @brief Called on the first time when the module goes into RUNNING.
   */
  virtual void onEntry() = 0;

  /**
   * @brief Called when the module exit from RUNNING.
   */
  virtual void onExit() = 0;

  /**
   * @brief Publish status if the module is requested to run
   */
  virtual void publishRTCStatus()
  {
    if (!rtc_interface_ptr_) {
      return;
    }
    rtc_interface_ptr_->publishCooperateStatus(clock_->now());
  }

  /**
   * @brief Return true if the activation command is received
   */
  virtual bool isActivated()
  {
    if (!rtc_interface_ptr_) {
      return true;
    }
    if (rtc_interface_ptr_->isRegistered(uuid_)) {
      return rtc_interface_ptr_->isActivated(uuid_);
    }
    return false;
  }

  /**
   * @brief set planner data
   */
  void setData(const std::shared_ptr<const PlannerData> & data) { planner_data_ = data; }

  std::string name() const { return name_; }

  rclcpp::Logger getLogger() const { return logger_; }

  std::shared_ptr<const PlannerData> planner_data_;

  MarkerArray getDebugMarker() { return debug_marker_; }

  AvoidanceDebugMsgArray::SharedPtr getAvoidanceDebugMsgArray()
  {
    if (debug_avoidance_msg_array_ptr_) {
      debug_avoidance_msg_array_ptr_->header.stamp = clock_->now();
    }
    return debug_avoidance_msg_array_ptr_;
  }
  bool isWaitingApproval() const { return is_waiting_approval_; }

private:
  std::string name_;
  rclcpp::Logger logger_;

protected:
  MarkerArray debug_marker_;
  rclcpp::Clock::SharedPtr clock_;
  mutable AvoidanceDebugMsgArray::SharedPtr debug_avoidance_msg_array_ptr_{};

  std::shared_ptr<RTCInterface> rtc_interface_ptr_;
  UUID uuid_;
  bool is_waiting_approval_;

  void updateRTCStatus(const double distance)
  {
    if (!rtc_interface_ptr_) {
      return;
    }
    rtc_interface_ptr_->updateCooperateStatus(uuid_, isExecutionReady(), distance, clock_->now());
  }

  virtual void removeRTCStatus()
  {
    if (!rtc_interface_ptr_) {
      return;
    }
    rtc_interface_ptr_->clearCooperateStatus();
  }

  void waitApproval() { is_waiting_approval_ = true; }

  void clearWaitingApproval() { is_waiting_approval_ = false; }

  static UUID generateUUID()
  {
    // Generate random number
    UUID uuid;
    std::mt19937 gen(std::random_device{}());
    std::independent_bits_engine<std::mt19937, 8, uint8_t> bit_eng(gen);
    std::generate(uuid.uuid.begin(), uuid.uuid.end(), bit_eng);

    return uuid;
  }

public:
  BT::NodeStatus current_state_;
};

}  // namespace behavior_path_planner

#endif  // BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_
