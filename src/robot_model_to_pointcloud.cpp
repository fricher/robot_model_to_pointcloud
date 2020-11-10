// Author : Antoine Hoarau <hoarau.robotics@gmail.com>
#include <iostream>
// ROS
#include <ros/ros.h>
#include <sensor_msgs/JointState.h>
#include <sensor_msgs/ChannelFloat32.h>
#include <geometry_msgs/Point32.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>
#include <sensor_msgs/PointCloud2.h>
// MoveIt!

#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/planning_interface/planning_interface.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/planning_scene_monitor/current_state_monitor.h>
#include <moveit/planning_scene_monitor/planning_scene_monitor.h>


#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shape_operations.h>

typedef std::pair<const moveit::core::LinkModel*, shapes::Mesh*> linkMap;
typedef std::map<std::string, linkMap > meshesMap;

int main(int argc, char** argv)
{
    ros::Time::init();
    ros::init(argc, argv, "robot_model_to_pointcloud");
    
    ros::AsyncSpinner spinner(1);
    spinner.start();
    ros::NodeHandle nh("~");
    
    ros::Publisher cloud_pub = nh.advertise<sensor_msgs::PointCloud2>("robot_cloud2", 1);
    
    ROS_INFO("Loading robot from the parameter server");
    
    
    std::string robot_description("");
    if(! ros::param::get("robot_description",robot_description))
    {
        ROS_ERROR("robot_description not found");
        return 0;
    }
    
    std::string joint_states_topic("joint_states");
    if(! nh.getParam("joint_states",joint_states_topic))
    {
        ROS_WARN("joint_states_topic will be set to default : %s",joint_states_topic.c_str());
    }
    
    int publish_frequency(50.0);
    if(! nh.getParam("publish_frequency",publish_frequency))
    {
        ROS_WARN("publish_frequency will be set to 50Hz");
    }
    
    bool use_visual_mesh(false);
    if(! nh.getParam("use_visual_mesh",use_visual_mesh))
    {
        ROS_INFO("Using collision mesh");
    }
    
    planning_scene_monitor::PlanningSceneMonitor psm("robot_description");
    psm.startStateMonitor(joint_states_topic);
    
    auto link_models = psm.getStateMonitor()->getCurrentState()->getRobotModel()->getLinkModelsWithCollisionGeometry();
    std::vector<std::string> mesh_filenames(link_models.size());
    
    
    meshesMap meshes;
    
    // Loading the visual meshes
    for (unsigned i=0;i<mesh_filenames.size() ; ++i)
    {
        mesh_filenames[i] = link_models[i]->getVisualMeshFilename();
        ROS_INFO_STREAM("- "<<mesh_filenames[i]);
        if(!mesh_filenames[i].empty())
        meshes[link_models[i]->getName()] = (
            std::make_pair(link_models[i],
                shapes::createMeshFromResource(mesh_filenames[i]))
            );
        }
        
        
        sensor_msgs::PointCloud cloud;
        cloud.points.reserve(5000);
        sensor_msgs::PointCloud2 cloud2;
        geometry_msgs::Point32 pt;
        bool initialized = false;
        Eigen::Vector3d vertice;
        Eigen::Vector3d vertice_transformed;
        ros::Duration elapsed;
        ros::Duration sleep_time(1./publish_frequency);
        
        sensor_msgs::ChannelFloat32 link_info_channel;
        link_info_channel.name = "intensity";
        link_info_channel.values.reserve(5000);
        
        ROS_INFO("Starting");
        while(ros::ok())
        {
            unsigned j = 0;
            unsigned link_nb = 0;
            
            if(psm.getStateMonitor()->waitForCurrentState(ros::Time::now(),1.0))
            {
                
                const ros::Time start = ros::Time::now();
                
                cloud.header.frame_id = psm.getStateMonitor()->getRobotModel()->getRootLinkName();
                ROS_DEBUG_STREAM("Frame id : "<<cloud.header.frame_id);
                
                // Get the link transform (base to link_i )
                //Might be a moveit bug : getCollisionBodyTransform
                //and getGlobalLinkTransform does not return the same thing
                //even if the visual and collisions are defined the same way.
                //const Eigen::Affine3d& Transform = psm.getStateMonitor()->getCurrentState()->getGlobalLinkTransform(link_p.first);
                const std::pair<robot_model::RobotStatePtr,ros::Time> robot_state = psm.getStateMonitor()->getCurrentStateAndTime();
                cloud.header.stamp = robot_state.second;
                
                
                for (auto& it : meshes)
                {
                    auto name = it.first;
                    auto link_p = it.second;
                    
                    
                    ROS_DEBUG_STREAM(""<<name);
                    
                    
                    auto Transform = robot_state.first->getCollisionBodyTransform(link_p.first,0);
                    
                    ROS_DEBUG_STREAM("CollisionTransform : "<<std::endl<<Transform.matrix()
                    <<"GlobaLinkTransform : "<<std::endl<<robot_state.first->getGlobalLinkTransform(link_p.first).matrix());
                    
                    if(use_visual_mesh == false)
                    {
                        // ######################## Using Collision Meshes (default) ################################### //                        
                        for(auto& shape : link_p.first->getShapes())
                        {
                            // Meshes to pointcloud
                            ROS_DEBUG_STREAM("Type : "<<shape->type);
                            
                            if(shape->type != shapes::MESH)
                            {
                                //TODO: support other types
                                continue;
                            }
                            
                            auto mesh = static_cast<const shapes::Mesh*>(shape.get());
                            
                            ROS_DEBUG_STREAM("mesh->vertex_count : "<<mesh->vertex_count);
                            
                            for(auto i=0;i<3*mesh->vertex_count;i=i+3)
                            {
                                vertice[0] = mesh->vertices[i];
                                vertice[1] = mesh->vertices[i+1];
                                vertice[2] = mesh->vertices[i+2];
                                // Get the point location with respect to the base
                                vertice_transformed =  Transform*vertice;
                                
                                pt.x = vertice_transformed[0];
                                pt.y = vertice_transformed[1];
                                pt.z = vertice_transformed[2];
                                
                                if(!initialized)
                                {
                                    cloud.points.push_back(pt);
                                    link_info_channel.values.push_back(link_nb);
                                }
                                else
                                {
                                    cloud.points[j] = pt;
                                    link_info_channel.values[j] = link_nb;
                                }
                                
                                j++;
                            }
                        }
                        // ######################## END Using Collision Meshes (default) ############################## //
                    }
                    else
                    {
                        // ######################## Using Visual Meshes ################################### //
                        for(auto i=0;i<3*link_p.second->vertex_count;i=i+3)
                        {
                            vertice[0] = link_p.second->vertices[i];
                            vertice[1] = link_p.second->vertices[i+1];
                            vertice[2] = link_p.second->vertices[i+2];
                            // Get the point location with respect to the base
                            vertice_transformed =  Transform*vertice;
                            
                            pt.x = vertice_transformed[0];
                            pt.y = vertice_transformed[1];
                            pt.z = vertice_transformed[2];
                            
                            if(!initialized)
                            {
                                cloud.points.push_back(pt);
                                link_info_channel.values.push_back(link_nb);
                            }
                            else
                            {
                                cloud.points[j] = pt;
                                link_info_channel.values[j] = link_nb;
                            }
                            
                            j++;
                        }
                    }
                    // ######################## END Using Visual Meshes  ################################### //
                    link_nb++;
                }
                // PointCloud to PointCloud2
                if(!initialized)
                {
                    cloud.channels.push_back(link_info_channel);
                }
                else
                {
                    cloud.channels[0] = link_info_channel;
                }
                
                sensor_msgs::convertPointCloudToPointCloud2(cloud,cloud2);
                cloud_pub.publish(cloud2);
                
                if(!initialized)
                {
                    initialized = true;
                }
                
                elapsed = ros::Time::now() - start;
                ROS_DEBUG_STREAM("Computation time : "<<elapsed.toSec());
                
                if(elapsed < sleep_time)
                {
                    (sleep_time - elapsed).sleep();
                }
                else
                {
                    ROS_INFO_THROTTLE(1,"Loop is slower then expected period");
                }
            }
            else
            {
                ROS_WARN("Waiting for complete state !");
            }
            
        }
        return 0;
    }
