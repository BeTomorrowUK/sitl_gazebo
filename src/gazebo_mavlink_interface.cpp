/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 * Copyright 2015-2018 PX4 Pro Development Team
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gazebo_mavlink_interface.h>

namespace gazebo {
GZ_REGISTER_MODEL_PLUGIN(GazeboMavlinkInterface);

GazeboMavlinkInterface::~GazeboMavlinkInterface() {
  event::Events::DisconnectWorldUpdateBegin(updateConnection_);
}

void GazeboMavlinkInterface::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  // Store the pointer to the model.
  model_ = _model;

  world_ = model_->GetWorld();

  const char *env_alt = std::getenv("PX4_HOME_ALT");
  if (env_alt) {
    gzmsg << "Home altitude is set to " << env_alt << ".\n";
    alt_home = std::stod(env_alt);
  }

  namespace_.clear();
  if (_sdf->HasElement("robotNamespace")) {
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  } else {
    gzerr << "[gazebo_mavlink_interface] Please specify a robotNamespace.\n";
  }

  node_handle_ = transport::NodePtr(new transport::Node());
  node_handle_->Init(namespace_);

  getSdfParam<std::string>(_sdf, "motorSpeedCommandPubTopic", motor_velocity_reference_pub_topic_,
      motor_velocity_reference_pub_topic_);
  getSdfParam<std::string>(_sdf, "imuSubTopic", imu_sub_topic_, imu_sub_topic_);
  getSdfParam<std::string>(_sdf, "gpsSubTopic", gps_sub_topic_, gps_sub_topic_);
  getSdfParam<std::string>(_sdf, "lidarSubTopic", lidar_sub_topic_, lidar_sub_topic_);
  getSdfParam<std::string>(_sdf, "opticalFlowSubTopic",
      opticalFlow_sub_topic_, opticalFlow_sub_topic_);
  getSdfParam<std::string>(_sdf, "sonarSubTopic", sonar_sub_topic_, sonar_sub_topic_);
  getSdfParam<std::string>(_sdf, "irlockSubTopic", irlock_sub_topic_, irlock_sub_topic_);
  groundtruth_sub_topic_ = "/groundtruth";

  // set input_reference_ from inputs.control
  input_reference_.resize(n_out_max);
  joints_.resize(n_out_max);
  pids_.resize(n_out_max);
  for (int i = 0; i < n_out_max; ++i)
  {
    pids_[i].Init(0, 0, 0, 0, 0, 0, 0);
    input_reference_[i] = 0;
  }

  if (_sdf->HasElement("control_channels")) {
    sdf::ElementPtr control_channels = _sdf->GetElement("control_channels");
    sdf::ElementPtr channel = control_channels->GetElement("channel");
    while (channel)
    {
      if (channel->HasElement("input_index"))
      {
        int index = channel->Get<int>("input_index");
        if (index < n_out_max)
        {
          input_offset_[index] = channel->Get<double>("input_offset");
          input_scaling_[index] = channel->Get<double>("input_scaling");
          zero_position_disarmed_[index] = channel->Get<double>("zero_position_disarmed");
          zero_position_armed_[index] = channel->Get<double>("zero_position_armed");
          if (channel->HasElement("joint_control_type"))
          {
            joint_control_type_[index] = channel->Get<std::string>("joint_control_type");
          }
          else
          {
            gzwarn << "joint_control_type[" << index << "] not specified, using velocity.\n";
            joint_control_type_[index] = "velocity";
          }

          // start gz transport node handle
          if (joint_control_type_[index] == "position_gztopic")
          {
            // setup publisher handle to topic
            if (channel->HasElement("gztopic"))
              gztopic_[index] = "~/" + model_->GetName() + channel->Get<std::string>("gztopic");
            else
              gztopic_[index] = "control_position_gztopic_" + std::to_string(index);
      #if GAZEBO_MAJOR_VERSION >= 7 && GAZEBO_MINOR_VERSION >= 4
            /// only gazebo 7.4 and above support Any
            joint_control_pub_[index] = node_handle_->Advertise<gazebo::msgs::Any>(
                gztopic_[index]);
      #else
            joint_control_pub_[index] = node_handle_->Advertise<gazebo::msgs::GzString>(
                gztopic_[index]);
      #endif
          }

          if (channel->HasElement("joint_name"))
          {
            std::string joint_name = channel->Get<std::string>("joint_name");
            joints_[index] = model_->GetJoint(joint_name);
            if (joints_[index] == nullptr)
            {
              gzwarn << "joint [" << joint_name << "] not found for channel["
                     << index << "] no joint control for this channel.\n";
            }
            else
            {
              gzdbg << "joint [" << joint_name << "] found for channel["
                    << index << "] joint control active for this channel.\n";
            }
          }
          else
          {
            gzdbg << "<joint_name> not found for channel[" << index
                  << "] no joint control will be performed for this channel.\n";
          }

          // setup joint control pid to control joint
          if (channel->HasElement("joint_control_pid"))
          {
            sdf::ElementPtr pid = channel->GetElement("joint_control_pid");
            double p = 0;
            if (pid->HasElement("p"))
              p = pid->Get<double>("p");
            double i = 0;
            if (pid->HasElement("i"))
              i = pid->Get<double>("i");
            double d = 0;
            if (pid->HasElement("d"))
              d = pid->Get<double>("d");
            double iMax = 0;
            if (pid->HasElement("iMax"))
              iMax = pid->Get<double>("iMax");
            double iMin = 0;
            if (pid->HasElement("iMin"))
              iMin = pid->Get<double>("iMin");
            double cmdMax = 0;
            if (pid->HasElement("cmdMax"))
              cmdMax = pid->Get<double>("cmdMax");
            double cmdMin = 0;
            if (pid->HasElement("cmdMin"))
              cmdMin = pid->Get<double>("cmdMin");
            pids_[index].Init(p, i, d, iMax, iMin, cmdMax, cmdMin);
          }
        }
        else
        {
          gzerr << "input_index[" << index << "] out of range, not parsing.\n";
        }
      }
      else
      {
        gzerr << "no input_index, not parsing.\n";
        break;
      }
      channel = channel->GetNextElement("channel");
    }
  }

  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  updateConnection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&GazeboMavlinkInterface::OnUpdate, this, _1));

  // Subscriber to IMU sensor_msgs::Imu Message and SITL message
  imu_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + imu_sub_topic_, &GazeboMavlinkInterface::ImuCallback, this);
  lidar_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + lidar_sub_topic_, &GazeboMavlinkInterface::LidarCallback, this);
  opticalFlow_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + opticalFlow_sub_topic_, &GazeboMavlinkInterface::OpticalFlowCallback, this);
  sonar_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + sonar_sub_topic_, &GazeboMavlinkInterface::SonarCallback, this);
  irlock_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + irlock_sub_topic_, &GazeboMavlinkInterface::IRLockCallback, this);
  gps_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + gps_sub_topic_, &GazeboMavlinkInterface::GpsCallback, this);
  groundtruth_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + groundtruth_sub_topic_, &GazeboMavlinkInterface::GroundtruthCallback, this);
  vision_sub_ = node_handle_->Subscribe("~/" + model_->GetName() + vision_sub_topic_, &GazeboMavlinkInterface::VisionCallback, this);

  // Publish gazebo's motor_speed message
  motor_velocity_reference_pub_ = node_handle_->Advertise<mav_msgs::msgs::CommandMotorSpeed>("~/" + model_->GetName() + motor_velocity_reference_pub_topic_, 1);

  _rotor_count = 5;
  last_time_ = world_->GetSimTime();
  last_imu_time_ = world_->GetSimTime();
  gravity_W_ = world_->GetPhysicsEngine()->GetGravity();

  if (_sdf->HasElement("imu_rate")) {
    imu_update_interval_ = 1 / _sdf->GetElement("imu_rate")->Get<int>();
  }

  // Magnetic field data for Zurich from WMM2015 (10^5xnanoTesla (N, E D) n-frame )
  // mag_n_ = {0.21523, 0.00771, -0.42741};
  // We set the world Y component to zero because we apply
  // the declination based on the global position,
  // and so we need to start without any offsets.
  // The real value for Zurich would be 0.00771
  // frame d is the magnetic north frame
  mag_d_.x = 0.21523;
  mag_d_.y = 0;
  mag_d_.z = -0.42741;

  if(_sdf->HasElement("hil_state_level"))
  {
    hil_mode_ = _sdf->GetElement("hil_mode")->Get<bool>();
  }

  if(_sdf->HasElement("hil_state_level"))
  {
    hil_state_level_ = _sdf->GetElement("hil_state_level")->Get<bool>();
  }

  // Get serial params
  if(_sdf->HasElement("serialEnabled"))
  {
    serial_enabled_ = _sdf->GetElement("serialEnabled")->Get<bool>();
  }

  if(serial_enabled_) {
    // Set up serial interface
    if(_sdf->HasElement("serialDevice"))
    {
      device_ = _sdf->GetElement("serialDevice")->Get<std::string>();
    }

    if (_sdf->HasElement("baudRate")) {
      baudrate_ = _sdf->GetElement("baudRate")->Get<int>();
    }
    io_service.post(std::bind(&GazeboMavlinkInterface::do_read, this));

    // run io_service for async io
    io_thread = std::thread([this] () {
    io_service.run();
  });
    open();
  }

  //Create socket
  // udp socket data
  mavlink_addr_ = htonl(INADDR_ANY);
  if (_sdf->HasElement("mavlink_addr")) {
    std::string mavlink_addr = _sdf->GetElement("mavlink_addr")->Get<std::string>();
    if (mavlink_addr != "INADDR_ANY") {
      mavlink_addr_ = inet_addr(mavlink_addr.c_str());
      if (mavlink_addr_ == INADDR_NONE) {
        fprintf(stderr, "invalid mavlink_addr \"%s\"\n", mavlink_addr.c_str());
        return;
      }
    }
  }
  if (_sdf->HasElement("mavlink_udp_port")) {
    mavlink_udp_port_ = _sdf->GetElement("mavlink_udp_port")->Get<int>();
  }

  qgc_addr_ = htonl(INADDR_ANY);
  if (_sdf->HasElement("qgc_addr")) {
    std::string qgc_addr = _sdf->GetElement("qgc_addr")->Get<std::string>();
    if (qgc_addr != "INADDR_ANY") {
      qgc_addr_ = inet_addr(qgc_addr.c_str());
      if (qgc_addr_ == INADDR_NONE) {
        fprintf(stderr, "invalid qgc_addr \"%s\"\n", qgc_addr.c_str());
        return;
      }
    }
  }
  if (_sdf->HasElement("qgc_udp_port")) {
    qgc_udp_port_ = _sdf->GetElement("qgc_udp_port")->Get<int>();
  }

  // try to setup udp socket for communcation
  if ((_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    printf("create socket failed\n");
    return;
  }

  if(_sdf->HasElement("vehicle_is_tailsitter"))
  {
    vehicle_is_tailsitter_ = _sdf->GetElement("vehicle_is_tailsitter")->Get<bool>();
  }

  memset((char *)&_myaddr, 0, sizeof(_myaddr));
  _myaddr.sin_family = AF_INET;
  _srcaddr.sin_family = AF_INET;

  if (serial_enabled_) {
    // gcs link
    _myaddr.sin_addr.s_addr = mavlink_addr_;
    _myaddr.sin_port = htons(mavlink_udp_port_);
    _srcaddr.sin_addr.s_addr = qgc_addr_;
    _srcaddr.sin_port = htons(qgc_udp_port_);
  }

  else {
    _myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    // Let the OS pick the port
    _myaddr.sin_port = htons(0);
    _srcaddr.sin_addr.s_addr = mavlink_addr_;
    _srcaddr.sin_port = htons(mavlink_udp_port_);
  }

  _addrlen = sizeof(_srcaddr);

  if (bind(_fd, (struct sockaddr *)&_myaddr, sizeof(_myaddr)) < 0) {
    printf("bind failed\n");
    return;
  }

  fds[0].fd = _fd;
  fds[0].events = POLLIN;

  mavlink_status_t* chan_state = mavlink_get_channel_status(MAVLINK_COMM_0);
  chan_state->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
}

// This gets called by the world update start event.
void GazeboMavlinkInterface::OnUpdate(const common::UpdateInfo&  /*_info*/) {
  common::Time current_time = world_->GetSimTime();
  double dt = (current_time - last_time_).Double();

  pollForMAVLinkMessages(dt, 1000);

  handle_control(dt);

  if (received_first_referenc_) {
    mav_msgs::msgs::CommandMotorSpeed turning_velocities_msg;

    for (int i = 0; i < input_reference_.size(); i++) {
      if (last_actuator_time_ == 0 || (current_time - last_actuator_time_).Double() > 0.2) {
        turning_velocities_msg.add_motor_speed(0);
      } else {
        turning_velocities_msg.add_motor_speed(input_reference_[i]);
      }
    }
    // TODO Add timestamp and Header
    // turning_velocities_msg->header.stamp.sec = current_time.sec;
    // turning_velocities_msg->header.stamp.nsec = current_time.nsec;

    motor_velocity_reference_pub_->Publish(turning_velocities_msg);
  }

  last_time_ = current_time;
}

void GazeboMavlinkInterface::send_mavlink_message(const mavlink_message_t *message, const int destination_port)
{

  if(serial_enabled_ && destination_port == 0) {
    assert(message != nullptr);
    if (!is_open()) {
      gzerr << "Serial port closed! \n";
      return;
    }

    {
      lock_guard lock(mutex);

      if (tx_q.size() >= MAX_TXQ_SIZE) {
//         gzwarn << "TX queue overflow. \n";
      }
      tx_q.emplace_back(message);
    }
    io_service.post(std::bind(&GazeboMavlinkInterface::do_write, this, true));
  }

  else {
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    int packetlen = mavlink_msg_to_send_buffer(buffer, message);

    struct sockaddr_in dest_addr;
    memcpy(&dest_addr, &_srcaddr, sizeof(_srcaddr));

    if (destination_port != 0) {
      dest_addr.sin_port = htons(destination_port);
    }

    ssize_t len = sendto(_fd, buffer, packetlen, 0, (struct sockaddr *)&_srcaddr, sizeof(_srcaddr));

    if (len <= 0) {
      printf("Failed sending mavlink message\n");
    }
  }

}

void GazeboMavlinkInterface::ImuCallback(ImuPtr& imu_message) {
  common::Time current_time = world_->GetSimTime();
  double dt = (current_time - last_imu_time_).Double();

    // frames
    // g - gazebo (ENU), east, north, up
    // r - rotors imu frame (FLU), forward, left, up
    // b - px4 (FRD) forward, right down
    // n - px4 (NED) north, east, down
    math::Quaternion q_gr = math::Quaternion(
      imu_message->orientation().w(),
      imu_message->orientation().x(),
      imu_message->orientation().y(),
      imu_message->orientation().z());

    // q_br
    /*
    tf.euler2quat(*tf.mat2euler([
    #        F  L  U
            [1, 0, 0],  # F
            [0, -1, 0], # R
            [0, 0, -1]  # D
        ]
    )).round(5)
    */
    math::Quaternion q_br(0, 1, 0, 0);


    // q_ng
    /*
    tf.euler2quat(*tf.mat2euler([
    #        N  E  D
            [0, 1, 0],  # E
            [1, 0, 0],  # N
            [0, 0, -1]  # U
        ]
    )).round(5)
    */
    math::Quaternion q_ng(0, 0.70711, 0.70711, 0);

    math::Quaternion q_gb = q_gr*q_br.GetInverse();
    math::Quaternion q_nb = q_ng*q_gb;

    math::Vector3 pos_g = model_->GetWorldPose().pos;
    math::Vector3 pos_n = q_ng.RotateVector(pos_g);

    float declination = get_mag_declination(groundtruth_lat_rad, groundtruth_lon_rad);

    math::Quaternion q_dn(0.0, 0.0, declination);
    math::Vector3 mag_n = q_dn.RotateVector(mag_d_);

    math::Vector3 vel_b = q_br.RotateVector(model_->GetRelativeLinearVel());
    math::Vector3 vel_n = q_ng.RotateVector(model_->GetWorldLinearVel());
    math::Vector3 omega_nb_b = q_br.RotateVector(model_->GetRelativeAngularVel());

    math::Vector3 mag_noise_b(
      0.01 * randn_(rand_),
      0.01 * randn_(rand_),
      0.01 * randn_(rand_));

    math::Vector3 accel_b = q_br.RotateVector(math::Vector3(
      imu_message->linear_acceleration().x(),
      imu_message->linear_acceleration().y(),
      imu_message->linear_acceleration().z()));
    math::Vector3 gyro_b = q_br.RotateVector(math::Vector3(
      imu_message->angular_velocity().x(),
      imu_message->angular_velocity().y(),
      imu_message->angular_velocity().z()));
    math::Vector3 mag_b = q_nb.RotateVectorReverse(mag_n) + mag_noise_b;

  if (imu_update_interval_!=0 && dt >= imu_update_interval_)
  {
    mavlink_hil_sensor_t sensor_msg;
    sensor_msg.time_usec = world_->GetSimTime().Double() * 1e6;
    sensor_msg.xacc = accel_b.x;
    sensor_msg.yacc = accel_b.y;
    sensor_msg.zacc = accel_b.z;
    sensor_msg.xgyro = gyro_b.x;
    sensor_msg.ygyro = gyro_b.y;
    sensor_msg.zgyro = gyro_b.z;
    sensor_msg.xmag = mag_b.x;
    sensor_msg.ymag = mag_b.y;
    sensor_msg.zmag = mag_b.z;

    // calculate abs_pressure using an ISA model for the tropsphere (valid up to 11km above MSL)
    const float lapse_rate = 0.0065f; // reduction in temperature with altitude (Kelvin/m)
    const float temperature_msl = 288.0f; // temperature at MSL (Kelvin)
    float alt_msl = (float)alt_home - pos_n.z;
    float temperature_local = temperature_msl - lapse_rate * alt_msl;
    float pressure_ratio = powf((temperature_msl/temperature_local) , 5.256f);
    const float pressure_msl = 101325.0f; // pressure at MSL
    sensor_msg.abs_pressure = pressure_msl / pressure_ratio;

    // generate Gaussian noise sequence using polar form of Box-Muller transformation
    // http://www.design.caltech.edu/erik/Misc/Gaussian.html
    double x1, x2, w, y1, y2;
    do {
     x1 = 2.0 * (rand() * (1.0 / (double)RAND_MAX)) - 1.0;
     x2 = 2.0 * (rand() * (1.0 / (double)RAND_MAX)) - 1.0;
     w = x1 * x1 + x2 * x2;
    } while ( w >= 1.0 );
    w = sqrt( (-2.0 * log( w ) ) / w );
    y1 = x1 * w;
    y2 = x2 * w;

    // Apply 1 Pa RMS noise
    float abs_pressure_noise = 1.0f * (float)w;
    sensor_msg.abs_pressure += abs_pressure_noise;

    // convert to hPa
    sensor_msg.abs_pressure *= 0.01f;

    // calculate density using an ISA model for the tropsphere (valid up to 11km above MSL)
    const float density_ratio = powf((temperature_msl/temperature_local) , 4.256f);
    float rho = 1.225f / density_ratio;

    // calculate pressure altitude including effect of pressure noise
    sensor_msg.pressure_alt = alt_msl - abs_pressure_noise / (gravity_W_.GetLength() * rho);

    // calculate differential pressure in hPa
    // if vehicle is a tailsitter the airspeed axis is different (z points from nose to tail)
    if (vehicle_is_tailsitter_) {
      sensor_msg.diff_pressure = 0.005f*rho*vel_b.z*vel_b.z;
    } else {
      sensor_msg.diff_pressure = 0.005f*rho*vel_b.x*vel_b.x;
    }

    // calculate temperature in Celsius
    sensor_msg.temperature = temperature_local - 273.0f;

    sensor_msg.fields_updated = 4095;

    //accumulate gyro measurements that are needed for the optical flow message
    static uint32_t last_dt_us = sensor_msg.time_usec;
    uint32_t dt_us = sensor_msg.time_usec - last_dt_us;
    if (dt_us > 1000) {
      optflow_gyro += gyro_b * (dt_us / 1000000.0f);
      last_dt_us = sensor_msg.time_usec;
    }

    mavlink_message_t msg;
    mavlink_msg_hil_sensor_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
    if (hil_mode_) {
      if (!hil_state_level_){
        send_mavlink_message(&msg);
      }
    }

    else {
      send_mavlink_message(&msg);
    }
    last_imu_time_ = current_time;
  }

    // ground truth
    math::Vector3 accel_true_b = q_br.RotateVector(model_->GetRelativeLinearAccel());

    // send ground truth

    mavlink_hil_state_quaternion_t hil_state_quat;
    hil_state_quat.time_usec = world_->GetSimTime().Double() * 1e6;
    hil_state_quat.attitude_quaternion[0] = q_nb.w;
    hil_state_quat.attitude_quaternion[1] = q_nb.x;
    hil_state_quat.attitude_quaternion[2] = q_nb.y;
    hil_state_quat.attitude_quaternion[3] = q_nb.z;

    hil_state_quat.rollspeed = omega_nb_b.x;
    hil_state_quat.pitchspeed = omega_nb_b.y;
    hil_state_quat.yawspeed = omega_nb_b.z;

    hil_state_quat.lat = groundtruth_lat_rad * 180 / M_PI * 1e7;
    hil_state_quat.lon = groundtruth_lon_rad * 180 / M_PI * 1e7;
    hil_state_quat.alt = groundtruth_altitude * 1000;

    hil_state_quat.vx = vel_n.x * 100;
    hil_state_quat.vy = vel_n.y * 100;
    hil_state_quat.vz = vel_n.z * 100;

    // assumed indicated airspeed due to flow aligned with pitot (body x)
    hil_state_quat.ind_airspeed = vel_b.x;
    hil_state_quat.true_airspeed = model_->GetWorldLinearVel().GetLength() * 100;  //no wind simulated

    hil_state_quat.xacc = accel_true_b.x * 1000;
    hil_state_quat.yacc = accel_true_b.y * 1000;
    hil_state_quat.zacc = accel_true_b.z * 1000;

    mavlink_message_t msg;
    mavlink_msg_hil_state_quaternion_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &hil_state_quat);
    if (hil_mode_) {
      if (hil_state_level_){
        send_mavlink_message(&msg);
      }
    }

    else {
      send_mavlink_message(&msg);
    }
}

void GazeboMavlinkInterface::GpsCallback(GpsPtr& gps_msg){
  // fill HIL GPS Mavlink msg
  mavlink_hil_gps_t hil_gps_msg;
  hil_gps_msg.time_usec = gps_msg->time() * 1e6;
  hil_gps_msg.fix_type = 3;
  hil_gps_msg.lat = gps_msg->latitude_deg() * 1e7;
  hil_gps_msg.lon = gps_msg->longitude_deg() * 1e7;
  hil_gps_msg.alt = gps_msg->altitude() * 1000.0;
  hil_gps_msg.eph = gps_msg->eph() * 100.0;
  hil_gps_msg.epv = gps_msg->epv() * 100.0;
  hil_gps_msg.vel = gps_msg->velocity() * 100.0;
  hil_gps_msg.vn = gps_msg->velocity_north() * 100.0;
  hil_gps_msg.ve = gps_msg->velocity_east() * 100.0;
  hil_gps_msg.vd = -gps_msg->velocity_up() * 100.0;
  // MAVLINK_HIL_GPS_T CoG is [0, 360]. math::Angle::Normalize() is [-pi, pi].
  math::Angle cog(atan2(gps_msg->velocity_east(), gps_msg->velocity_north()));
  cog.Normalize();
  hil_gps_msg.cog = static_cast<uint16_t>(GetDegrees360(cog) * 100.0);
  hil_gps_msg.satellites_visible = 10;

  // send HIL_GPS Mavlink msg
  mavlink_message_t msg;
  mavlink_msg_hil_gps_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &hil_gps_msg);
  if (hil_mode_) {
    if (!hil_state_level_){
      send_mavlink_message(&msg);
    }
  }

  else {
    send_mavlink_message(&msg);
  }
}

void GazeboMavlinkInterface::GroundtruthCallback(GtPtr& groundtruth_msg){
  // update groundtruth lat_rad, lon_rad and altitude
  groundtruth_lat_rad = groundtruth_msg->latitude_rad();
  groundtruth_lon_rad = groundtruth_msg->longitude_rad();
  groundtruth_altitude = groundtruth_msg->altitude();
  // the rest of the data is obtained directly on this interface and sent to
  // the FCU
}

void GazeboMavlinkInterface::LidarCallback(LidarPtr& lidar_message) {
  mavlink_distance_sensor_t sensor_msg;
  sensor_msg.time_boot_ms = lidar_message->time_msec();
  sensor_msg.min_distance = lidar_message->min_distance() * 100.0;
  sensor_msg.max_distance = lidar_message->max_distance() * 100.0;
  sensor_msg.current_distance = lidar_message->current_distance() * 100.0;
  sensor_msg.type = 0;
  sensor_msg.id = 0;
  sensor_msg.orientation = 25;//downward facing
  sensor_msg.covariance = 0;

  //distance needed for optical flow message
  optflow_distance = lidar_message->current_distance();  //[m]

  mavlink_message_t msg;
  mavlink_msg_distance_sensor_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
  send_mavlink_message(&msg);
}

void GazeboMavlinkInterface::OpticalFlowCallback(OpticalFlowPtr& opticalFlow_message) {
  mavlink_hil_optical_flow_t sensor_msg;
  sensor_msg.time_usec = world_->GetSimTime().Double() * 1e6;
  sensor_msg.sensor_id = opticalFlow_message->sensor_id();
  sensor_msg.integration_time_us = opticalFlow_message->integration_time_us();
  sensor_msg.integrated_x = opticalFlow_message->integrated_x();
  sensor_msg.integrated_y = opticalFlow_message->integrated_y();
  sensor_msg.integrated_xgyro = opticalFlow_message->quality() ? -optflow_gyro.y : 0.0f;//xy switched
  sensor_msg.integrated_ygyro = opticalFlow_message->quality() ? optflow_gyro.x : 0.0f;  //xy switched
  sensor_msg.integrated_zgyro = opticalFlow_message->quality() ? -optflow_gyro.z : 0.0f;//change direction
  sensor_msg.temperature = opticalFlow_message->temperature();
  sensor_msg.quality = opticalFlow_message->quality();
  sensor_msg.time_delta_distance_us = opticalFlow_message->time_delta_distance_us();
  sensor_msg.distance = optflow_distance;

  //reset gyro integral
  optflow_gyro.Set();

  mavlink_message_t msg;
  mavlink_msg_hil_optical_flow_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
  send_mavlink_message(&msg);
}

void GazeboMavlinkInterface::SonarCallback(SonarSensPtr& sonar_message) {
  mavlink_distance_sensor_t sensor_msg;
  sensor_msg.time_boot_ms = world_->GetSimTime().Double() * 1e3;
  sensor_msg.min_distance = sonar_message->min_distance() * 100.0;
  sensor_msg.max_distance = sonar_message->max_distance() * 100.0;
  sensor_msg.current_distance = sonar_message->current_distance() * 100.0;
  sensor_msg.type = 1;
  sensor_msg.id = 1;
  sensor_msg.orientation = 0;  // forward facing
  sensor_msg.covariance = 0;

  mavlink_message_t msg;
  mavlink_msg_distance_sensor_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
  send_mavlink_message(&msg);
}

void GazeboMavlinkInterface::IRLockCallback(IRLockPtr& irlock_message) {
  mavlink_landing_target_t sensor_msg;

  sensor_msg.time_usec = world_->GetSimTime().Double() * 1e6;
  sensor_msg.target_num = irlock_message->signature();
  sensor_msg.angle_x = irlock_message->pos_x();
  sensor_msg.angle_y = irlock_message->pos_y();
  sensor_msg.size_x = irlock_message->size_x();
  sensor_msg.size_y = irlock_message->size_y();
  sensor_msg.position_valid = false;
  sensor_msg.type = LANDING_TARGET_TYPE_LIGHT_BEACON;

  mavlink_message_t msg;
  mavlink_msg_landing_target_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
  send_mavlink_message(&msg);
}

void GazeboMavlinkInterface::VisionCallback(OdomPtr& odom_message) {
  mavlink_vision_position_estimate_t sensor_msg;
  sensor_msg.usec = odom_message->usec();
  // convert from ENU to NED
  sensor_msg.x = odom_message->y();
  sensor_msg.y = -odom_message->x();
  sensor_msg.z = -odom_message->z();
  sensor_msg.roll = odom_message->pitch();
  sensor_msg.pitch = -odom_message->roll();
  sensor_msg.yaw = -odom_message->yaw();

  // send VISION_POSITION_ESTIMATE Mavlink msg
  mavlink_message_t msg;
  mavlink_msg_vision_position_estimate_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
  send_mavlink_message(&msg);
}

/*ssize_t GazeboMavlinkInterface::receive(void *_buf, const size_t _size, uint32_t _timeoutMs)
   {
   fd_set fds;
   struct timeval tv;

   FD_ZERO(&fds);
   FD_SET(this->handle, &fds);

   tv.tv_sec = _timeoutMs / 1000;
   tv.tv_usec = (_timeoutMs % 1000) * 1000UL;

   if (select(this->handle+1, &fds, NULL, NULL, &tv) != 1)
   {
      return -1;
   }

   return recv(this->handle, _buf, _size, 0);
   }*/

void GazeboMavlinkInterface::pollForMAVLinkMessages(double _dt, uint32_t _timeoutMs)
{
  // convert timeout in ms to timeval
  struct timeval tv;
  tv.tv_sec = _timeoutMs / 1000;
  tv.tv_usec = (_timeoutMs % 1000) * 1000UL;

  // poll
  ::poll(&fds[0], (sizeof(fds[0]) / sizeof(fds[0])), 0);

  if (fds[0].revents & POLLIN) {
    int len = recvfrom(_fd, _buf, sizeof(_buf), 0, (struct sockaddr *)&_srcaddr, &_addrlen);
    if (len > 0) {
      mavlink_message_t msg;
      mavlink_status_t status;
      for (unsigned i = 0; i < len; ++i)
      {
        if (mavlink_parse_char(MAVLINK_COMM_0, _buf[i], &msg, &status))
        {
          if (serial_enabled_) {
            // forward message from qgc to serial
            send_mavlink_message(&msg);
          }
          // have a message, handle it
          handle_message(&msg);
        }
      }
    }
  }
}

void GazeboMavlinkInterface::handle_message(mavlink_message_t *msg)
{
  switch (msg->msgid) {
  case MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS:
    mavlink_hil_actuator_controls_t controls;
    mavlink_msg_hil_actuator_controls_decode(msg, &controls);
    bool armed = false;

    if ((controls.mode & MAV_MODE_FLAG_SAFETY_ARMED) > 0) {
      armed = true;
    }

    last_actuator_time_ = world_->GetSimTime();

    for (unsigned i = 0; i < n_out_max; i++) {
      input_index_[i] = i;
    }

    // set rotor speeds, controller targets
    input_reference_.resize(n_out_max);
    for (int i = 0; i < input_reference_.size(); i++) {
      if (armed) {
        input_reference_[i] = (controls.controls[input_index_[i]] + input_offset_[i])
            * input_scaling_[i] + zero_position_armed_[i];
      } else {
        input_reference_[i] = zero_position_disarmed_[i];
      }
    }

    received_first_referenc_ = true;
    break;
  }
}

void GazeboMavlinkInterface::handle_control(double _dt)
{
  // set joint positions
  for (int i = 0; i < input_reference_.size(); i++) {
    if (joints_[i]) {
      double target = input_reference_[i];
      if (joint_control_type_[i] == "velocity")
      {
        double current = joints_[i]->GetVelocity(0);
        double err = current - target;
        double force = pids_[i].Update(err, _dt);
        joints_[i]->SetForce(0, force);
      }
      else if (joint_control_type_[i] == "position")
      {
        double current = joints_[i]->GetAngle(0).Radian();

        double err = current - target;
        double force = pids_[i].Update(err, _dt);
        joints_[i]->SetForce(0, force);
      }
      else if (joint_control_type_[i] == "position_gztopic")
      {
     #if GAZEBO_MAJOR_VERSION >= 7 && GAZEBO_MINOR_VERSION >= 4
        /// only gazebo 7.4 and above support Any
        gazebo::msgs::Any m;
        m.set_type(gazebo::msgs::Any_ValueType_DOUBLE);
        m.set_double_value(target);
     #else
        std::stringstream ss;
        gazebo::msgs::GzString m;
        ss << target;
        m.set_data(ss.str());
     #endif
        joint_control_pub_[i]->Publish(m);
      }
      else if (joint_control_type_[i] == "position_kinematic")
      {
        /// really not ideal if your drone is moving at all,
        /// mixing kinematic updates with dynamics calculation is
        /// non-physical.
     #if GAZEBO_MAJOR_VERSION >= 6
        joints_[i]->SetPosition(0, input_reference_[i]);
     #else
        joints_[i]->SetAngle(0, input_reference_[i]);
     #endif
      }
      else
      {
        gzerr << "joint_control_type[" << joint_control_type_[i] << "] undefined.\n";
      }
    }
  }
}

void GazeboMavlinkInterface::open() {
  try{
    serial_dev.open(device_);
    serial_dev.set_option(boost::asio::serial_port_base::baud_rate(baudrate_));
    serial_dev.set_option(boost::asio::serial_port_base::character_size(8));
    serial_dev.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
    serial_dev.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
    serial_dev.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));
    gzdbg << "Opened serial device " << device_ << "\n";
  }
  catch (boost::system::system_error &err) {
    gzerr <<"Error opening serial device: " << err.what() << "\n";
  }
}

void GazeboMavlinkInterface::close()
{
  lock_guard lock(mutex);
  if (!is_open())
    return;

  io_service.stop();
  serial_dev.close();

  if (io_thread.joinable())
    io_thread.join();
}

void GazeboMavlinkInterface::do_read(void)
{
  serial_dev.async_read_some(boost::asio::buffer(rx_buf), boost::bind(
      &GazeboMavlinkInterface::parse_buffer, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred
      )
  );
}

// Based on MAVConnInterface::parse_buffer in MAVROS
void GazeboMavlinkInterface::parse_buffer(const boost::system::error_code& err, std::size_t bytes_t){
  mavlink_status_t status;
  mavlink_message_t message;
  uint8_t *buf = this->rx_buf.data();

  assert(rx_buf.size() >= bytes_t);

  for(; bytes_t > 0; bytes_t--)
  {
    auto c = *buf++;

    auto msg_received = static_cast<Framing>(mavlink_frame_char_buffer(&m_buffer, &m_status, c, &message, &status));
    if (msg_received == Framing::bad_crc || msg_received == Framing::bad_signature) {
      _mav_parse_error(&m_status);
      m_status.msg_received = MAVLINK_FRAMING_INCOMPLETE;
      m_status.parse_state = MAVLINK_PARSE_STATE_IDLE;
      if (c == MAVLINK_STX) {
        m_status.parse_state = MAVLINK_PARSE_STATE_GOT_STX;
        m_buffer.len = 0;
        mavlink_start_checksum(&m_buffer);
      }
    }

    if(msg_received != Framing::incomplete){
      // send to gcs
      send_mavlink_message(&message, qgc_udp_port_);
      handle_message(&message);
    }
  }
  do_read();
}

void GazeboMavlinkInterface::do_write(bool check_tx_state){
  if (check_tx_state && tx_in_progress)
    return;

  lock_guard lock(mutex);
  if (tx_q.empty())
    return;

  tx_in_progress = true;
  auto &buf_ref = tx_q.front();

  serial_dev.async_write_some(
    boost::asio::buffer(buf_ref.dpos(), buf_ref.nbytes()), [this, &buf_ref] (boost::system::error_code error,   size_t bytes_transferred)
    {
      assert(bytes_transferred <= buf_ref.len);
      if(error) {
        gzerr << "Serial error: " << error.message() << "\n";
      return;
      }

    lock_guard lock(mutex);

    if (tx_q.empty()) {
      tx_in_progress = false;
      return;
    }

    buf_ref.pos += bytes_transferred;
    if (buf_ref.nbytes() == 0) {
      tx_q.pop_front();
    }

    if (!tx_q.empty()) {
      do_write(false);
    }
    else {
      tx_in_progress = false;
    }
  });
}

}
