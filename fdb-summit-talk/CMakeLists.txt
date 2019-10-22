set(SRCS simulation.actor.cpp)

add_flow_target(EXECUTABLE NAME simulation SRCS ${SRCS})
target_link_libraries(simulation flow boost_target)
