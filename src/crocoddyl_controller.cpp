#include "cpcc2_tiago/crocoddyl_controller.hpp"

namespace cpcc2_tiago {

void CrocoddylController::init_shared_memory() {
  boost::interprocess::shared_memory_object::remove("crcoddyl_shm");

  crocoddyl_shm_ = boost::interprocess::managed_shared_memory(
      boost::interprocess::create_only,
      "crcoddyl_shm",  // segment name
      3 * sizeof(double) *
          (x_meas_.size() + us_.size() + xs_.size() + Ks_.size() +
           3));  // segment size in bytes

  // Initialize shared memory STL-compatible allocator
  const shm_allocator alloc_inst(crocoddyl_shm_.get_segment_manager());

  // Construct a shared memory
  x_meas_shm_ =
      crocoddyl_shm_.construct<shared_vector>("x_meas_shm")  // object name
      (alloc_inst);  // first ctor parameter
  us_shm_ = crocoddyl_shm_.construct<shared_vector>("us_shm")(alloc_inst);
  xs_shm_ = crocoddyl_shm_.construct<shared_vector>("xs_shm")(alloc_inst);
  Ks_shm_ = crocoddyl_shm_.construct<shared_vector>("Ks_shm")(alloc_inst);
  target_shm_ =
      crocoddyl_shm_.construct<shared_vector>("target_shm")(alloc_inst);
  solver_started = crocoddyl_shm_.construct<bool>("solver_started")(false);

  x_meas_shm_->resize(x_meas_.size());
  us_shm_->resize(us_.size());
  xs_shm_->resize(xs_.size());
  Ks_shm_->resize(Ks_.size());
  target_shm_->resize(3);
}

void CrocoddylController::send_solver_x(Eigen::VectorXd x) {
  mutex_.lock();
  x_meas_shm_->assign(x.data(), x.data() + x.size());
  mutex_.unlock();
}

void CrocoddylController::read_solver_results() {
  mutex_.lock();
  us_ = Eigen::Map<Eigen::VectorXd>(us_shm_->data(), us_shm_->size());
  xs_ = Eigen::Map<Eigen::VectorXd>(xs_shm_->data(), xs_shm_->size());
  Ks_ = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      Ks_shm_->data(), Ks_.rows(), Ks_.cols());
  mutex_.unlock();
}

void CrocoddylController::update_target_from_subscriber(
    const std_msgs::msg::Float64MultiArray::SharedPtr msg) {
  if (msg->data.size() != 3) {
    RCLCPP_ERROR(get_node()->get_logger(),
                 "Target message has wrong size, should be 3");
    return;
  }
  Vector3d new_hand_target;
  new_hand_target << msg->data[0], msg->data[1], msg->data[2];
  mutex_.lock();
  target_shm_->assign(new_hand_target.data(),
                      new_hand_target.data() + new_hand_target.size());
  mutex_.unlock();
}

void CrocoddylController::declare_parameters() {
  param_listener_ = std::make_shared<ParamListener>(get_node());
}

controller_interface::CallbackReturn CrocoddylController::read_parameters() {
  if (!param_listener_) {
    RCLCPP_ERROR(get_node()->get_logger(), "Error encountered during init");
    return controller_interface::CallbackReturn::ERROR;
  }
  params_ = param_listener_->get_params();

  if (params_.joints.empty()) {
    RCLCPP_ERROR(get_node()->get_logger(), "'joints' parameter was empty");
    return controller_interface::CallbackReturn::ERROR;
  }

  if (params_.state_interfaces_name.empty()) {
    RCLCPP_ERROR(get_node()->get_logger(),
                 "'state_interfaces_name' parameter was empty");
    return controller_interface::CallbackReturn::ERROR;
  }

  for (auto state_inter_ : params_.state_interfaces_name) {
    for (auto joint : params_.joints) {
      state_interface_types_.push_back(joint + "/" + state_inter_);
    }
  }

  n_joints_ = params_.joints.size();
  joints_names_.reserve(n_joints_);

  joints_names_ = params_.joints;

  x_meas_.resize(2 * n_joints_);
  xs_.resize(2 * n_joints_);
  us_.resize(n_joints_);
  Ks_.resize(n_joints_, 2 * n_joints_);

  OCP_horizon_length_ = params_.horizon_length;
  OCP_time_step_ = params_.time_step;

  enable_logging_ = params_.enable_logging;
  logging_frequency_ = params_.logging_frequency;

  // same for the current state
  current_state_.position.resize(n_joints_);
  current_state_.velocity.resize(n_joints_);

  x_meas_.resize(2 * n_joints_);

  RCLCPP_INFO(get_node()->get_logger(),
              "motors parameters loaded successfully");

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn CrocoddylController::on_init() {
  RCLCPP_INFO(get_node()->get_logger(), "Initializing CrocoddylController.");

  try {
    declare_parameters();
  } catch (const std::exception &e) {
    fprintf(stderr, "Exception thrown during init stage with message: %s \n",
            e.what());
    return controller_interface::CallbackReturn::ERROR;
  }
  auto ret = this->read_parameters();
  if (ret != controller_interface::CallbackReturn::SUCCESS) {
    return ret;
  }

  if (n_joints_ == 0) {
    RCLCPP_ERROR_STREAM(get_node()->get_logger(),
                        "List of joint names is empty.");
    return controller_interface::CallbackReturn::ERROR;
  }

  while (true) {
    if (mutex_.try_lock()) {
      break;
    }

    if (!mutex_.timed_lock(boost::get_system_time() +
                           boost::posix_time::milliseconds(10))) {
      mutex_.unlock();
    }
  }

  mutex_.unlock();

  init_shared_memory();

  // Build the model from the urdf
  model_ = model_builder::build_model(joints_names_);

  data_ = Data(model_);

  Vector3d hand_target = Eigen::Vector3d(0.8, 0, 0.8);  // random target

  target_shm_->assign(hand_target.data(),
                      hand_target.data() + hand_target.size());

  std::unordered_map<std::string, int> columnNames{
      {"error", 3}  // name of column + their size

  };
  if (false) {
    logger_ = logger_OCP::logger(params_.log_file_path, columnNames);

    logger_.data_to_log_.reserve(columnNames.size());
  }

  target_subscriber_ =
      get_node()->create_subscription<std_msgs::msg::Float64MultiArray>(
          "~/target", 10,
          std::bind(&CrocoddylController::update_target_from_subscriber, this,
                    std::placeholders::_1));

  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::InterfaceConfiguration
CrocoddylController::command_interface_configuration() const {
  RCLCPP_INFO(get_node()->get_logger(),
              "Command Interface CrocoddylController");

  controller_interface::InterfaceConfiguration command_interfaces_config;
  command_interfaces_config.type =
      controller_interface::interface_configuration_type::INDIVIDUAL;
  // Claiming Command interface exported as reference interface by
  // PvegContrller
  for (int i = 0; i < n_joints_; i++) {
    command_interfaces_config.names.push_back("pveg_chained_controller/" +
                                              joints_names_[i] + "/" +
                                              hardware_interface::HW_IF_EFFORT);
  }
  for (int i = 0; i < n_joints_; i++) {
    command_interfaces_config.names.push_back(
        "pveg_chained_controller/" + joints_names_[i] + "/" +
        hardware_interface::HW_IF_POSITION);
  }
  for (int i = 0; i < n_joints_; i++) {
    command_interfaces_config.names.push_back(
        "pveg_chained_controller/" + joints_names_[i] + "/" +
        hardware_interface::HW_IF_VELOCITY);
  }

  for (int i = 0; i < n_joints_; i++) {  // all the gains
    for (int j = 0; j < 2 * n_joints_; j++) {
      command_interfaces_config.names.push_back(
          "pveg_chained_controller/" + joints_names_[i] + "/" + "gain" +
          std::to_string(i).c_str() + "_" + std::to_string(j).c_str());
    }
  }

  return command_interfaces_config;
}

controller_interface::InterfaceConfiguration
CrocoddylController::state_interface_configuration() const {
  RCLCPP_INFO(get_node()->get_logger(), "State Interface CrocoddylController.");

  controller_interface::InterfaceConfiguration state_interfaces_config;
  state_interfaces_config.type =
      controller_interface::interface_configuration_type::INDIVIDUAL;
  state_interfaces_config.names = state_interface_types_;

  state_interfaces_config.names = state_interface_types_;
  return state_interfaces_config;
}

controller_interface::return_type CrocoddylController::update(
    const rclcpp::Time & /*time*/, const rclcpp::Duration & /*period*/) {
  if (!start_updating) {
    mutex_.lock();
    start_updating = *solver_started;
    mutex_.unlock();
    return controller_interface::return_type::OK;
  }

  start_update_time_ = rclcpp::Clock(RCL_ROS_TIME).now();

  diff_ = (start_update_time_ - prev_update_time_)
              .to_chrono<std::chrono::microseconds>();

  read_state_from_hardware();

  x_meas_ << current_state_.position, current_state_.velocity;

  send_solver_x(x_meas_);

  if ((diff_.count()) * 1e-6 > OCP_time_step_) {
    prev_update_time_ = start_update_time_;

    read_solver_results();

    set_u_command(us_);
    set_x_command(xs_);
    set_K_command(Ks_);

    // Log the current state
    std::cout << "Update frequency: " << 1 / (diff_.count() * 1e-6) << "Hz"
              << std::endl;
  }

  model_builder::updateReducedModel(
      x_meas_, model_,
      data_);  // set the model pos to the measured
               // value to get the end effector pos
  end_effector_pos_ = model_builder::get_end_effector_SE3(data_, lh_id_)
                          .translation();  // get the end
                                           // effector pos

  // pos_error_ = (OCP_tiago_.get_target() - end_effector_pos_);

  if (false) {
    // start_logging_time_ = rclcpp::Clock(RCL_ROS_TIME).now();
    // if ((start_logging_time_ - prev_log_time_)
    //             .to_chrono<std::chrono::microseconds>()
    //             .count() *
    //         1e-6 >=
    //     1 / logging_frequency_) {
    //   logger_.data_to_log_ = {pos_error_};
    //   logger_.log(); // log what is in data_to_log_
    //   // Update the last log time
    //   prev_log_time_ = start_logging_time_;
    // }

    logger_.data_to_log_ = {pos_error_};
    logger_.log();  // log what is in data_to_log_
  }

  return controller_interface::return_type::OK;
}

void CrocoddylController::read_state_from_hardware() {
  for (int i = 0; i < n_joints_; ++i) {
    current_state_.position[i] = state_interfaces_[i].get_value();
    current_state_.velocity[i] = state_interfaces_[n_joints_ + i].get_value();
  }
}

void CrocoddylController::set_u_command(VectorXd command_u) {
  for (int i = 0; i < n_joints_; i++) {
    command_interfaces_[i].set_value(command_u[i]);
  }
}
void CrocoddylController::set_x_command(VectorXd command_x) {
  for (int i = 0; i < 2 * n_joints_; i++) {
    command_interfaces_[n_joints_ + i].set_value(command_x[i]);
    command_interfaces_[2 * n_joints_ + i].set_value(command_x[n_joints_ + i]);
  }
}
void CrocoddylController::set_K_command(MatrixXd command_K) {
  for (int i = 0; i < n_joints_; ++i) {
    for (int j = 0; j < 2 * n_joints_; j++) {
      command_interfaces_[3 * n_joints_ + i * 2 * n_joints_ + j].set_value(
          command_K(i, j));
    }
  }
}

}  // namespace cpcc2_tiago

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(cpcc2_tiago::CrocoddylController,
                       controller_interface::ControllerInterface)