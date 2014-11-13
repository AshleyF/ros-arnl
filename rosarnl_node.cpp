
#include "Aria/Aria.h"
#include "Arnl.h"
#include "ArPathPlanningInterface.h"
#include "ArLocalizationTask.h"
#include "ArDocking.h"

#include "ArnlSystem.h"

#include "ros/ros.h"
#include "geometry_msgs/Pose.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PoseWithCovariance.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include "nav_msgs/Odometry.h"
#include "tf/tf.h"
#include "tf/transform_listener.h"  //for tf::getPrefixParam
#include <tf/transform_broadcaster.h>
#include "tf/transform_datatypes.h"
#include <dynamic_reconfigure/server.h>
#include "std_msgs/Float64.h"
#include "std_msgs/Float32.h"
#include "std_msgs/Int8.h"
#include "std_msgs/Bool.h"
#include "std_srvs/Empty.h"

#include <sstream>


class RosArnlNode
{
  public:
    RosArnlNode(ros::NodeHandle n, ArnlSystem& arnlsys);
    virtual ~RosArnlNode();
    
  public:
    int Setup();
    void spin();
    void publish();

  protected:
    ros::NodeHandle n;

    ArFunctorC<RosArnlNode> myPublishCB;

    ros::ServiceServer enable_srv;
    ros::ServiceServer disable_srv;
    bool enable_motors_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response);
    bool disable_motors_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response);

    ros::Publisher motors_state_pub;
    std_msgs::Bool motors_state;
    bool published_motors_state;

    geometry_msgs::PoseWithCovarianceStamped pose;
    ros::Publisher pose_pub;

    ros::Subscriber initialpose_sub;
    void initialpose_sub_cb(const geometry_msgs::PoseWithCovarianceStampedConstPtr &msg);
    
    ros::ServiceServer global_localization_srv;
    bool global_localization_srv_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response);

    //std::string serial_port;

    ArnlSystem &arnl;

};


RosArnlNode::RosArnlNode(ros::NodeHandle nh, ArnlSystem& arnlsys)  :
  myPublishCB(this, &RosArnlNode::publish), 
  arnl(arnlsys)
{
  // read in config options
  n = nh;

  //n.param( "port", serial_port, std::string("/dev/ttyUSB0") );
  //ROS_INFO( "rosarnl: using port: [%s]", serial_port.c_str() );


  // Figure out what frame_id's to use. if a tf_prefix param is specified,
  // it will be added to the beginning of the frame_ids.
  //
  // e.g. rosrun ... _tf_prefix:=MyRobot (or equivalently using <param>s in
  // roslaunch files)
  // will result in the frame_ids being set to /MyRobot/odom etc,
  // rather than /odom. This is useful for Multi Robot Systems.
  // See ROS Wiki for further details.
/*
  tf_prefix = tf::getPrefixParam(n);
  frame_id_odom = tf::resolve(tf_prefix, "odom");
  frame_id_base_link = tf::resolve(tf_prefix, "base_link");
  frame_id_bumper = tf::resolve(tf_prefix, "bumpers_frame");
  frame_id_sonar = tf::resolve(tf_prefix, "sonar_frame");
*/

  motors_state_pub = n.advertise<std_msgs::Bool>("motors_state", 5, true /*latch*/ );
  motors_state.data = false;
  published_motors_state = false;

  pose_pub = n.advertise<geometry_msgs::PoseWithCovarianceStamped>("amcl_pose", 30);

  enable_srv = n.advertiseService("enable_motors", &RosArnlNode::enable_motors_cb, this);
  disable_srv = n.advertiseService("disable_motors", &RosArnlNode::disable_motors_cb, this);
  
  global_localization_srv = n.advertiseService("global_localization", &RosArnlNode::global_localization_srv_cb, this);

  initialpose_sub = n.subscribe("initialpose", 1, (boost::function <void(const
geometry_msgs::PoseWithCovarianceStampedConstPtr&)>)
boost::bind(&RosArnlNode::initialpose_sub_cb, this, _1));

  // Publish data triggered by ARIA sensor interpretation task
  arnl.robot->lock();
  arnl.robot->addSensorInterpTask("ROSPublishingTask", 100, &myPublishCB);
  arnl.robot->unlock();
}

RosArnlNode::~RosArnlNode()
{
}

int RosArnlNode::Setup()
{
  return 0;
}

void RosArnlNode::spin()
{
  ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: Running ROS node...");
  ros::spin();
}

void RosArnlNode::publish()
{
  // Note, this is called via SensorInterpTask callback (myPublishCB, named "ROSPublishingTask"). ArRobot object 'robot' sholud not be locked or unlocked.
  ArPose pos = arnl.robot->getPose();

  // convert mm and degrees to position meters and quaternion angle in ros pose
  tf::poseTFToMsg(tf::Transform(tf::createQuaternionFromYaw(pos.getTh()*M_PI/180), tf::Vector3(pos.getX()/1000,
    pos.getY()/1000, 0)), pose.pose.pose); 
  
  pose.header.frame_id = 1; // TODO check this
  pose.header.stamp = ros::Time::now();

  // TODO add covariance to position
  
  pose_pub.publish(pose);

  ROS_DEBUG_NAMED("rosarnl_node", "rosarnl_node: publish: (time %f) pose x: %f, y: %f, angle: %f", 
    pose.header.stamp.toSec(), 
    (double)pose.pose.pose.position.x,
    (double)pose.pose.pose.position.y,
    (double)pose.pose.pose.orientation.w
  );


/*
  // publishing transform odom->base_link
  odom_trans.header.stamp = ros::Time::now();
  odom_trans.header.frame_id = frame_id_odom;
  odom_trans.child_frame_id = frame_id_base_link;
  
  odom_trans.transform.translation.x = pos.getX()/1000;
  odom_trans.transform.translation.y = pos.getY()/1000;
  odom_trans.transform.translation.z = 0.0;
  odom_trans.transform.rotation = tf::createQuaternionMsgFromYaw(pos.getTh()*M_PI/180);
  
  odom_broadcaster.sendTransform(odom_trans);
*/
  
  // publish motors state if changed
  bool e = arnl.robot->areMotorsEnabled();
  if(e != motors_state.data || !published_motors_state)
  {
    ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: publishing new motors state %d.", e);
    motors_state.data = e;
    motors_state_pub.publish(motors_state);
    published_motors_state = true;
  }

}

bool RosArnlNode::enable_motors_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response)
{
    ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: Enable motors request.");
    arnl.robot->lock();
    if(arnl.robot->isEStopPressed())
        ROS_WARN_NAMED("rosarnl_node", "rosarnl_node: Warning: Enable motors requested, but robot also has E-Stop button pressed. Motors will not enable.");
    arnl.robot->enableMotors();
    arnl.robot->unlock();
	// todo could wait and see if motors do become enabled, and send a response with an error flag if not
    return true;
}

bool RosArnlNode::disable_motors_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response)
{
    ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: Disable motors request.");
    arnl.robot->lock();
    arnl.robot->disableMotors();
    arnl.robot->unlock();
	// todo could wait and see if motors do become disabled, and send a response with an error flag if not
    return true;
}

bool RosArnlNode::global_localization_srv_cb(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response)
{
  ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: Localize init (global_localization service) request...");
//  arnl.locTask->localizeRobotInMapInit();
  if(! arnl.locTask->localizeRobotAtHomeBlocking() )
    ROS_WARN_NAMED("rosarnl_node", "rosarnl_node: Error in initial localization.");
  return true;
}

void RosArnlNode::initialpose_sub_cb(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg)
{
  ROS_INFO_NAMED("rosarnl_node", "rosarnl_node: Init localization pose received...");
  double x = msg->pose.pose.position.x * 1000.0;
  double y = msg->pose.pose.position.y * 1000.0;
  double th = tf::getYaw(msg->pose.pose.orientation) / (M_PI/180.0);
  arnl.locTask->forceUpdatePose(ArPose(x, y, th));
}

void ariaLogHandler(const char *msg, ArLog::LogLevel level)
{
  // node that ARIA logging is normally limited at Normal and Terse only. Set
  // ARLOG_LEVEL environment variable to override.
  switch(level)
  {
    case ArLog::Normal:
      ROS_INFO_NAMED("ARNL", "ARNL: %s", msg);
      return;
    case ArLog::Terse:
      ROS_WARN_NAMED("ARNL", "ARNL: %s", msg);
      return;
    case ArLog::Verbose:
      ROS_DEBUG_NAMED("ARNL", "ARNL: %s", msg);
      return;
  }
}

  

int main( int argc, char** argv )
{
  ros::init(argc,argv, "rosarnl_node");

  Aria::init();
  Arnl::init();
 /* set log type to None to only use
    ariaLogHandler to redirect ARNL log messages to rosconsole by deufault. This can
    be changed in the ARNL parameter file however.
  */
  ArLog::init(ArLog::None, ArLog::Normal); 
  ArLog::setFunctor(new ArGlobalFunctor2<const char *, ArLog::LogLevel>(&ariaLogHandler));

  ArnlSystem arnl;
  if( arnl.setup() != ArnlSystem::OK)
  {
    ROS_FATAL_NAMED("rosarnl_node", "rosarnl_node: ARNL and ARIA setup failed... \n" );
    return -2;
  }


  ArGlobalFunctor1<int> *ariaExitF = new ArGlobalFunctor1<int>(&Aria::exit, 9);
  arnl.robot->addDisconnectOnErrorCB(ariaExitF);
  for(std::map<int, ArLaser*>::iterator i = arnl.robot->getLaserMap()->begin();
      i != arnl.robot->getLaserMap()->end();
      ++i)
  {
    (*i).second->addDisconnectOnErrorCB(ariaExitF);
  }

  ros::NodeHandle n(std::string("~"));
  RosArnlNode *node = new RosArnlNode(n, arnl);
  if( node->Setup() != 0 )
  {
    ROS_FATAL_NAMED("rosarnl_node", "rosarnl_node: ROS node setup failed... \n" );
    return -1;
  }

  node->spin();

  delete node;

  ROS_INFO_NAMED("rosarnl_node",  "rosarnl_node: Quitting... \n" );
  Aria::exit(0);
  return 0;

}

