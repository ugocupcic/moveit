cmake_minimum_required(VERSION 2.8.12)
project(moveit_ros_move_group)

add_compile_options(-std=c++11)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Boost REQUIRED system filesystem date_time program_options signals thread)
find_package(catkin REQUIRED COMPONENTS
  moveit_core
  moveit_ros_planning
  actionlib
  roscpp
  pluginlib
  std_srvs
  tf
)

catkin_package(
  LIBRARIES
    moveit_move_group_capabilities_base
  INCLUDE_DIRS
    include
  CATKIN_DEPENDS
    moveit_core
    moveit_ros_planning
)

include_directories(include)
include_directories(${catkin_INCLUDE_DIRS})
include_directories(SYSTEM
                    ${Boost_INCLUDE_DIRS}
                    )

link_directories(${Boost_LIBRARY_DIRS})
link_directories(${catkin_LIBRARY_DIRS})

add_library(moveit_move_group_capabilities_base
  src/move_group_context.cpp
  src/move_group_capability.cpp
  )
add_dependencies(moveit_move_group_capabilities_base ${catkin_EXPORTED_TARGETS}) # wait until all *_msgs packages are finished being built

add_executable(move_group src/move_group.cpp)

add_executable(list_move_group_capabilities src/list_capabilities.cpp)

add_library(moveit_move_group_default_capabilities
  src/default_capabilities/move_action_capability.cpp
  src/default_capabilities/plan_service_capability.cpp
  src/default_capabilities/execute_trajectory_service_capability.cpp
  src/default_capabilities/execute_trajectory_action_capability.cpp
  src/default_capabilities/query_planners_service_capability.cpp
  src/default_capabilities/kinematics_service_capability.cpp
  src/default_capabilities/state_validation_service_capability.cpp
  src/default_capabilities/cartesian_path_service_capability.cpp
  src/default_capabilities/get_planning_scene_service_capability.cpp
  src/default_capabilities/apply_planning_scene_service_capability.cpp
  src/default_capabilities/clear_octomap_service_capability.cpp
  )
add_dependencies(moveit_move_group_default_capabilities ${catkin_EXPORTED_TARGETS})

target_link_libraries(moveit_move_group_capabilities_base ${catkin_LIBRARIES} ${Boost_LIBRARIES})
target_link_libraries(move_group moveit_move_group_capabilities_base ${catkin_LIBRARIES} ${Boost_LIBRARIES})
target_link_libraries(moveit_move_group_default_capabilities moveit_move_group_capabilities_base ${catkin_LIBRARIES} ${Boost_LIBRARIES})
target_link_libraries(list_move_group_capabilities ${catkin_LIBRARIES} ${Boost_LIBRARIES})

install(TARGETS move_group list_move_group_capabilities moveit_move_group_capabilities_base moveit_move_group_default_capabilities
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})

install(DIRECTORY include/ DESTINATION include)

install(PROGRAMS
  scripts/load_map scripts/save_map DESTINATION
  ${CATKIN_PACKAGE_BIN_DESTINATION})

install(FILES
  default_capabilities_plugin_description.xml
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})
