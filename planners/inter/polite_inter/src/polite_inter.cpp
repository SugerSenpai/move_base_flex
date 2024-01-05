#include "../include/polite_inter.h"
#include <std_msgs/Int32.h>
#include <pluginlib/class_list_macros.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2/LinearMath/Quaternion.h>
#include <ros/ros.h>
#include <ros/master.h>
#include <geometry_msgs/Point32.h>
#include <dynamic_reconfigure/Reconfigure.h>
#include <dynamic_reconfigure/Config.h>

PLUGINLIB_EXPORT_CLASS(polite_inter::PoliteInter, mbf_costmap_core::CostmapInter)

namespace polite_inter
{
    ros::Subscriber subscriber_;
    ros::ServiceClient setParametersClient_;
    const uint32_t SUCCESS = 0;
    const uint32_t INTERNAL_ERROR = 1;
    geometry_msgs::PoseStamped temp_goal;
    bool new_goal_set_ = false;
    double max_vel_x_param_;
    double changed_max_vel_x_param_;
    dynamic_reconfigure::Reconfigure reconfig_;
    dynamic_reconfigure::DoubleParameter double_param_;
    dynamic_reconfigure::Config conf_;
    double final_value;
    std::vector<geometry_msgs::Point32> semanticPoints;

    uint32_t PoliteInter::makePlan(const geometry_msgs::PoseStamped &start, const geometry_msgs::PoseStamped &goal,
                                   std::vector<geometry_msgs::PoseStamped> &plan, double &cost, std::string &message)
    {
        boost::unique_lock<boost::mutex> lock(plan_mtx_);

        double robot_x = start.pose.position.x;
        double robot_y = start.pose.position.y;
        double robot_z = start.pose.position.z;
        for (const auto &point : semanticPoints)
        {
            double distance = std::sqrt(std::pow(point.x - robot_x, 2) + std::pow(point.y - robot_y, 2)) + std::pow(point.z - robot_z, 2);
            // Check if the pedestrian is in range to set temporary goal and move back
            if ((distance <= ped_minimum_distance_) && !new_goal_set_)
            {
                ROS_INFO("Pedestrian detected. Distance: %f", distance);
                if (!new_goal_set_)
                {
                    ROS_INFO("Setting new temp_goal");
                    temp_goal = start;
                    // calculating position for temporary goal
                    double theta = tf::getYaw(temp_goal.pose.orientation);
                    temp_goal.pose.position.x -= temp_goal_distance_ * cos(theta);
                    temp_goal.pose.position.y -= temp_goal_distance_ * sin(theta);
                    temp_goal.pose.orientation = tf::createQuaternionMsgFromYaw(tf::getYaw(temp_goal.pose.orientation));
                    temp_goal.header.frame_id = start.header.frame_id;
                    new_goal_set_ = true;
                }
            }
            // reset max_vel_x to normal value
            double_param_.name = "max_vel_x";
            final_value = max_vel_x_param_;
            if (distance <= caution_detection_range_)
            {
                // set max_vel_x to reduced value if peds are in range
                final_value = changed_max_vel_x_param_;
            }
            // set max_vel_x parameter
            double_param_.value = final_value;
            conf_.doubles.clear();
            conf_.doubles.push_back(double_param_);
            reconfig_.request.config = conf_;
            setParametersClient_.call(reconfig_);
        }
        if (new_goal_set_)
        {
            //calculate distance to temporary goal
            double distance_to_temp_goal = std::sqrt(std::pow(temp_goal.pose.position.x - robot_x, 2) + std::pow(temp_goal.pose.position.y - robot_y, 2));

            if (distance_to_temp_goal <= temp_goal_tolerance_)
            {
                ROS_INFO("Reached temp_goal. Resetting goal.");
                new_goal_set_ = false;
            }
            // Clear the existing plan and add temp_goal
            plan.clear();
            plan.push_back(temp_goal);
            return 0;
        }
        plan.insert(plan.end(), plan_.begin(), plan_.end());
        return 0;
    }

    bool PoliteInter::setPlan(const std::vector<geometry_msgs::PoseStamped> &plan)
    {
        boost::unique_lock<boost::mutex> lock(plan_mtx_);
        plan_ = plan;
        return true;
    }

    std::string PoliteInter::get_local_planner(){

        std::string keyword;
        std::string local_planner_name;

        if (!nh_.getParam(node_namespace_+"/local_planner", keyword))
        {
            ROS_ERROR("Failed to get parameter %s/local_planner", node_namespace_.c_str());
      
        }
        if(keyword=="teb"){
            local_planner_name= "TebLocalPlannerROS";
        }
        if(keyword=="mpc"){
            local_planner_name= "MpcLocalPlannerROS";
        }
        if(keyword=="dwa"){
            local_planner_name= "DwaLocalPlannerROS";
        }
        if(keyword=="cohan"){
            local_planner_name= "HAtebLocalPlannerROS";
        }        

        return local_planner_name;
    }

    void PoliteInter::semanticCallback(const pedsim_msgs::SemanticData::ConstPtr &message)
    {
        semanticPoints.clear();
        for (const auto &point : message->points)
        {
            geometry_msgs::Point32 pedestrianPoint;
            pedestrianPoint.x = point.location.x;
            pedestrianPoint.y = point.location.y;
            pedestrianPoint.z = point.location.z;

            semanticPoints.push_back(pedestrianPoint);
        }
    }

    void PoliteInter::initialize(std::string name, costmap_2d::Costmap2DROS *global_costmap_ros, costmap_2d::Costmap2DROS *local_costmap_ros)
    {
        std::string local_planner_name = get_local_planner();
        this->name = name;
        std::string node_namespace_ = ros::this_node::getNamespace();
        nh_ = ros::NodeHandle("~");
        std::string semanticLayer = "/pedsim_agents/semantic/pedestrian";
        // get the starting parameter for max_vel_x from our planner
        subscriber_ = nh_.subscribe(semanticLayer, 1, &PoliteInter::semanticCallback, this);
        if (!nh_.getParam(node_namespace_+"/move_base_flex/"+ local_planner_name+"/max_vel_x", max_vel_x_param_))
        {
            ROS_ERROR("Failed to get parameter %s/move_base_flex/TebLocalPlannerROS/max_vel_x", node_namespace_.c_str());
            return;
        }
        // Create service clients for the GetDump and Reconfigure services
        setParametersClient_ = nh_.serviceClient<dynamic_reconfigure::Reconfigure>(node_namespace_+"/move_base_flex/"+ local_planner_name+"/set_parameters");

        dynamic_reconfigure::Server<polite_inter::PoliteInterConfig> server;
        server.setCallback(boost::bind(&PoliteInter::reconfigure, this, _1, _2));
        // needs to be declared here because cautious_speed gets declared with reconfigure
        changed_max_vel_x_param_ = (cautious_speed_ * max_vel_x_param_);
    }

    void PoliteInter::reconfigure(polite_inter::PoliteInterConfig &config, uint32_t level)
    {
        ped_minimum_distance_ = config.ped_minimum_distance;
        temp_goal_distance_ = config.temp_goal_distance;
        caution_detection_range_ = config.caution_detection_range;
        cautious_speed_ = config.cautious_speed;
        temp_goal_tolerance_ = config.temp_goal_tolerance;
    }

}