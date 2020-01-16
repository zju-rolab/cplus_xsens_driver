#ifndef _MTDEVICE_H__
#define _MTDEVICE_H__ 1

#include <linux/serial.h>
#include <math.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sys/ioctl.h>
#include <time.h>

#include <boost/asio/placeholders.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/bind.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>

#include <cplus_xsens_driver/MtType.hpp>
#include <cplus_xsens_driver/PacketCounter.h>
#include <cplus_xsens_driver/mt_message.h>

#define MAX_MESSAGE_LENGTH 128
#define BAG_ABANDON 800

typedef unsigned char uchar;

namespace xsens_device {

struct MTMessage {
  uchar bid;
  uchar mid;
  uchar len;
  uchar data[MAX_MESSAGE_LENGTH];
  uchar checksum;
};

struct MTPacket {
  uchar id[2];
  uchar len;
  uchar data[MAX_MESSAGE_LENGTH];
};

struct MTData2Data {
  uint32_t packet_counter;
  uint32_t sample_time_fine;
  uint32_t sample_time_coarse;
  double sample_time;
  uint32_t sec;
  uint32_t nsec;
  double realTime;
  float quatemion[3];
  float acceleration[3];
  float turn_rate[3];
};

struct Utctime
{
  uint32_t nanosec;
  uint16_t year;
  uint8_t mon;
  uint8_t day;
  uint8_t hour;
  uint8_t min;
  uint8_t sec;
  uint8_t flag;
};

class MTDevice
{
public:
  MTDevice();
  ~MTDevice();
  void parse();
  MTMessage message_;
  MTData2Data mtdata2_;
  Utctime utctime_;

  // ros pub
  ros::NodeHandle nh;
  ros::Publisher imu_device_pub;
  ros::Publisher imu_data_pub;
  ros::Publisher imu_packet_counter_pub;

  sensor_msgs::Imu imu_message;
  cplus_xsens_driver::mt_message device;
  cplus_xsens_driver::PacketCounter packet_counter_;

private:
  void read();
  void parseMTPacket(const MTPacket &packet);
  void receiveCallback(const boost::system::error_code &ec,
                       size_t bytes_transferred);
  float getData2Float(const uchar *src);
  boost::asio::io_service io_srv_;
  boost::shared_ptr<boost::asio::serial_port> port_;
  boost::shared_ptr<boost::thread> thrd_io_srv_;

  boost::circular_buffer<uchar> buf_;
  uchar raw_buf_[MAX_MESSAGE_LENGTH];

  ros::Time begin_;
  double diff_tosec;
  int step_;
  int diff_cal_flag;
  int diff_cal_flag2;
  int pub_flag;
  int packet_counter_begin;

  uint32_t loop;
  uint32_t short_counter;
  struct  tm time_;

  bool gps_enable;
};

MTDevice::MTDevice() : buf_(8 * MAX_MESSAGE_LENGTH, 0), step_(0)
{
  buf_.clear();

  loop = 0;
  short_counter = 0;

  imu_data_pub = nh.advertise<sensor_msgs::Imu>("xsens/imu_data", 100);
  imu_device_pub =
      nh.advertise<cplus_xsens_driver::mt_message>("xsens/device_info", 100);
  imu_packet_counter_pub = nh.advertise<cplus_xsens_driver::PacketCounter>(
      "xsens/packet_counter", 100);

  ros::param::get("~gps_enable",gps_enable);
  ROS_WARN("GPS_STATUS IS %d",gps_enable);
  
  port_.reset(new boost::asio::serial_port(io_srv_, "/dev/ttyUSB0"));
  port_->set_option(boost::asio::serial_port_base::baud_rate(460800));
  port_->set_option(boost::asio::serial_port_base::flow_control(
      boost::asio::serial_port_base::flow_control::none));
  boost::asio::basic_serial_port<boost::asio::serial_port_service>::native_type
      native = port_->native();
  struct serial_struct serial;
  ioctl(native, TIOCGSERIAL, &serial);
  serial.flags |= ASYNC_LOW_LATENCY;
  ioctl(native, TIOCSSERIAL, &serial);

  io_srv_.post(boost::bind(&MTDevice::read, this));
  thrd_io_srv_.reset(
      new boost::thread(boost::bind(&boost::asio::io_service::run, &io_srv_)));
  diff_cal_flag = 1;
  diff_cal_flag2 = 1;
  pub_flag = 0;
  port_->write_some(boost::asio::buffer("\xFA\xFF\x10\x00\xF1",5));
}

MTDevice::~MTDevice() { port_->close(); }

float MTDevice::getData2Float(const uchar *src)
{
  float ret;
  uint8_t *dest = (uint8_t *)&ret;
  dest[0] = src[3];
  dest[1] = src[2];
  dest[2] = src[1];
  dest[3] = src[0];

  return ret;
}

void MTDevice::read()
{
  if (port_.get() == NULL || !port_->is_open()) {
    std::cout << "Port not opened\n" << std::endl;
    return;
  }
  port_->async_read_some(
      boost::asio::buffer(raw_buf_, sizeof(raw_buf_)),
      boost::bind(&MTDevice::receiveCallback, this,
                  boost::asio::placeholders::error,
                  boost::asio::placeholders::bytes_transferred));
}

void MTDevice::parse()
{
  size_t old_size = 0;

  /*
  * step:
  * 0: Preamble, BID, MID & LEN
  * 1: DATA & Checksum
  */

  //将整个读到的msg的数据输出检验
  if (buf_.empty()) {
    ROS_INFO("EMPTY");
  }
  while (!buf_.empty() && old_size != buf_.size()) {
    old_size = buf_.size();
    switch (step_) {
    case 0:
      while (buf_.size() >= 4 && buf_.front() != 0xfa) {
        buf_.pop_front();
      }
      if (buf_.front() == 0xfa && buf_.size() >= 4) {
        buf_.pop_front();
        message_.bid = buf_.front();
        buf_.pop_front();
        message_.mid = buf_.front();
        buf_.pop_front();
        message_.len = buf_.front();
        buf_.pop_front();

        if (message_.len == 0xff) {
          printf("Extened format not supported\n");
          step_ = 0;
        }

        step_ = 1;
      }
      break;
    case 1:
      if (buf_.size() > message_.len) {
        for (int i = 0; i < message_.len; ++i) {
          message_.data[i] = buf_.front();
          buf_.pop_front();
        }

        message_.checksum = buf_.front();
        buf_.pop_front();

        int checksum = 0;
        checksum += message_.bid;
        checksum += message_.mid;
        checksum += message_.len;
        for (int i = 0; i < message_.len; ++i)
          checksum += message_.data[i];
        checksum += message_.checksum;
        ROS_WARN("RIGHT MESSAGE BUFFER SIZE IS %lu",buf_.size());
        if ((checksum & 0xff) != 0x00) {
          ROS_ERROR("/** Wrong checksum:%02x **/\n", checksum);
          ROS_ERROR("BID=%02x, MID=%02x, len=%02x(%d), checksum=%02x\n",
                    message_.bid, message_.mid, message_.len, message_.len,
                    message_.checksum);
          ROS_ERROR("WRONG BUFFER SIZE IS %lu",buf_.size());
          for (int i = 0; i < message_.len; ++i) {
            ROS_ERROR("%02x ", message_.data[i]);
          }
          printf("\n");
          printf("/** END **/\n");
        } else {

          for (int i = 0; i < message_.len;) {
            MTPacket packet;
            if (message_.len - i < 2) {
              printf("not enough id\n");
              break;
            }
            packet.id[0] = message_.data[i++];
            packet.id[1] = message_.data[i++];

            if (message_.len - i < 1) {
              printf("not eough len\n");
              break;
            }
            packet.len = message_.data[i++];

            if (message_.len - i < packet.len) {
              printf("not enough data: %d - 1 - %d, %d\n", message_.len, i,
                     packet.len);
              break;
            }
            memcpy(packet.data, &message_.data[i], packet.len);
            i += packet.len;

            parseMTPacket(packet);
          }
        }

        step_ = 0;
      }
      break;
    default:
      printf("\n");
      break;
    }
  }
}

void MTDevice::receiveCallback(const boost::system::error_code &ec,
                               size_t bytes_transferred)
{
  if (ec) {
    std::cout << "read error: " << ec.message() << std::endl;
    read();
    return;
  } else {
    ROS_DEBUG("bytes_transferred=%lu", bytes_transferred);
  }
  // ros::Time t1 = ros::Time::now();
  for (unsigned int i = 0; i < bytes_transferred; ++i) {
    buf_.push_back(raw_buf_[i]);
  }
  parse();
  // ros::Time t2 = ros::Time::now();
  // ROS_INFO("parse takes %lf ms", (t2-t1).toSec()*1000.0f);
  read();
}

void MTDevice::parseMTPacket(const MTPacket &packet)
{
  enum xsens_imu::GroupId group;

  if (message_.mid == 0x36) {
    group = xsens_imu::GroupId(packet.id[0]);
    if (group == xsens_imu::GroupId::kTimestamp) {
      if (packet.id[1] == 0x20) {
        mtdata2_.packet_counter = packet.data[0] * 256 + packet.data[1];
        // if (mtdata2_.packet_counter < short_counter)
          // loop++;
        // short_counter = mtdata2_.packet_counter;

        // packet_counter_.counter = (loop << 16) | mtdata2_.packet_counter;

        // ROS_DEBUG("packet_counter=%d", packet_counter_.counter);
        if (diff_cal_flag) {
          packet_counter_begin = packet_counter_.counter;
          diff_cal_flag = false;
        }
        if ((packet_counter_.counter - packet_counter_begin) > BAG_ABANDON)
          pub_flag = true;
      }

      if (packet.id[1] == 0x10){
          utctime_.nanosec =((uint32_t)packet.data[0] << 24) + 
                                    ((uint32_t)packet.data[1] << 16) + 
                                    ((uint32_t)packet.data[2] <<  8) + 
                                    ((uint32_t)packet.data[3] <<  0);
          utctime_.year = ((uint16_t)packet.data[4] << 8) + ((uint16_t)packet.data[5] << 0);
          utctime_.mon = packet.data[6];
          utctime_.day = packet.data[7];
          utctime_.hour = packet.data[8];
          utctime_.min = packet.data[9];
          utctime_.sec = packet.data[10];
          utctime_.flag = packet.data[11];

          ROS_INFO("nano %lu ,year %u ,mon %u,day %u,hour %u",utctime_.nanosec,utctime_.year,utctime_.mon,utctime_.day,utctime_.hour);

          time_ = {0};
          time_.tm_year = utctime_.year-1900;
          time_.tm_mon = utctime_.mon-1;
          time_.tm_mday = utctime_.day ;
          time_.tm_hour = utctime_.hour + 8;
          time_.tm_min = utctime_.min;
          time_.tm_sec = utctime_.sec;
          
      }

      if (packet.id[1] == 0x60) {
        // mtdata2_.sample_time_fine =
        // packet.data[0]<<24+packet.data[1]<<16+packet.data[2]<<8+packet.data[3];
        // mtdata2_.sample_time_fine = packet.data[0] * pow(2, 24) +
                            //  packet.data[1] * pow(2, 16) +
                            //  packet.data[2] * pow(2, 8) + packet.data[3];
        mtdata2_.sample_time_fine = ((uint32_t)packet.data[0] << 24) + 
                                    ((uint32_t)packet.data[1] << 16) + 
                                    ((uint32_t)packet.data[2] <<  8) + 
                                    ((uint32_t)packet.data[3] <<  0);
      }
      if (packet.id[1] == 0x70) {
        mtdata2_.sample_time_coarse = ((uint32_t)packet.data[0] << 24) + 
                                      ((uint32_t)packet.data[1] << 16) + 
                                      ((uint32_t)packet.data[2] <<  8) + 
                                      ((uint32_t)packet.data[3] <<  0);
        mtdata2_.sample_time =
            (mtdata2_.sample_time_coarse + (mtdata2_.sample_time_fine % 10000) / 10000.0);
        if (pub_flag && diff_cal_flag2) {
          begin_ = ros::Time::now();
          diff_tosec = begin_.toSec() - mtdata2_.sample_time;
          diff_cal_flag2 = false;
        }
        mtdata2_.realTime = mtdata2_.sample_time + diff_tosec;
        mtdata2_.sec = (uint32_t)floor(mtdata2_.realTime);
        mtdata2_.nsec = (mtdata2_.realTime - mtdata2_.sec) * pow(10, 9);

        //Fill packet counter stamp
        if(gps_enable){
          packet_counter_.header.stamp.sec = mktime( &time_);
          packet_counter_.header.stamp.nsec = utctime_.nanosec;
        }
        else{
          packet_counter_.header.stamp.sec = mtdata2_.sec;
          packet_counter_.header.stamp.nsec = mtdata2_.nsec;
          packet_counter_.sample_time = mtdata2_.sample_time;
        }

        // Fill packet counter
        int loop = std::round((400 * packet_counter_.sample_time - mtdata2_.packet_counter) / (1 << 16));
        mtdata2_.packet_counter += loop * (1 << 16);
        packet_counter_.counter = mtdata2_.packet_counter;
        ROS_DEBUG("packet_counter=%d", packet_counter_.counter);
      }
    }
    if (group == xsens_imu::GroupId::kOrientationData) {
      if ((packet.id[1] & 0xF0) == 0x10) {
        mtdata2_.quatemion[0] = getData2Float(packet.data);
        mtdata2_.quatemion[1] = getData2Float(packet.data + 4);
        mtdata2_.quatemion[2] = getData2Float(packet.data + 8);
        mtdata2_.quatemion[3] = getData2Float(packet.data + 12);
      } else {
        ROS_INFO("no orientation data\n");
      }
    }
    if (group == xsens_imu::GroupId::kAcceleration)
    // if (packet.id[0] == 0x40 )
    {
      if ((packet.id[1] & 0xF0) == 0x20) {
        mtdata2_.acceleration[0] = getData2Float(packet.data);
        mtdata2_.acceleration[1] = getData2Float(packet.data + 4);
        mtdata2_.acceleration[2] = getData2Float(packet.data + 8);
      } else {
        ROS_INFO("no acceleration data\n");
      }
    }

    if (group == xsens_imu::GroupId::kAngularVelocity) {
      if ((packet.id[1] & 0xF0) == 0X20) {
        // memcpy(&mtdata2_.turn_rate,packet.data,3*sizeof(float));
        mtdata2_.turn_rate[0] = getData2Float(packet.data);
        mtdata2_.turn_rate[1] = getData2Float(packet.data + 4);
        mtdata2_.turn_rate[2] = getData2Float(packet.data + 8);
        // printf("gry_X is %02f\n",mtdata2_.turn_rate[0]);
        // printf("gry_Y is %02f\n",mtdata2_.turn_rate[1]);
        // printf("gry_Z is %02f\n",mtdata2_.turn_rate[2]);
      } else {
        ROS_INFO("NO GRY DATA\n");
      }
    }

    if (packet.id[0] == 0xe0) {
      if ((packet.id[1] & 0xF0) == 0X20) {
        imu_message.header.stamp = packet_counter_.header.stamp;
        imu_message.header.frame_id = "xsens_link";
        imu_message.orientation.x = mtdata2_.quatemion[0];
        imu_message.orientation.y = mtdata2_.quatemion[1];
        imu_message.orientation.z = mtdata2_.quatemion[2];
        imu_message.orientation.w = mtdata2_.quatemion[3];

        imu_message.linear_acceleration.x = mtdata2_.acceleration[0];
        imu_message.linear_acceleration.y = mtdata2_.acceleration[1];
        imu_message.linear_acceleration.z = mtdata2_.acceleration[2];

        imu_message.angular_velocity.x = mtdata2_.turn_rate[0];
        imu_message.angular_velocity.y = mtdata2_.turn_rate[1];
        imu_message.angular_velocity.z = mtdata2_.turn_rate[2];

        // Device_data
        device.bid = message_.bid;
        device.mid = message_.mid;
        device.len = message_.len;
        device.check = message_.checksum;

        if (pub_flag) {
          imu_device_pub.publish(device);
          imu_data_pub.publish(imu_message);
          imu_packet_counter_pub.publish(packet_counter_);
        }
      }
    }
  }
}

} // namespace xsens_device

#endif // !_MTDEVICE_H__
