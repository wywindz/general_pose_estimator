cmake_minimum_required(VERSION 2.8.3)
project(general_pose_estimator)

find_package(
 PCL 1.7 REQUIRED 
)
find_package(OPENMP)

include_directories(
 ${EIGEN_INCLUDE_DIRS}
 ${PCL_INCLUDE_DIRS}
)
link_directories(${PCL_LIBRARY_DIRS})
add_definitions(${PCL_DEFINITIONS})
add_definitions(${EIGEN_DEFINITIONS})

if(OPENMP_FOUND)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
endif()


add_executable(general_pose_estimator src/general_pose_estimation.cpp)
target_link_libraries(general_pose_estimator ${PCL_LIBRARIES} ${Boost_LIBRARIES} ${EIGEN_INCLUDE_DIRS})
