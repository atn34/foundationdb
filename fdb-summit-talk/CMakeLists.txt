set(SRCS simulation.actor.cpp)

add_flow_target(EXECUTABLE NAME simulation_fair SRCS ${SRCS})
target_compile_options(simulation_fair PRIVATE -fsanitize=address,undefined)
target_link_options(simulation_fair PRIVATE -fsanitize=address,undefined)
target_link_libraries(simulation_fair PRIVATE flow boost_target)

if (CLANG)
  add_flow_target(EXECUTABLE NAME simulation_fuzz SRCS ${SRCS})
  target_compile_options(simulation_fuzz PRIVATE -fsanitize=address,undefined,fuzzer)
  target_compile_definitions(simulation_fuzz PRIVATE -DUSE_LIBFUZZER=1)
  target_link_options(simulation_fuzz PRIVATE -fsanitize=address,undefined,fuzzer)
  target_link_libraries(simulation_fuzz PRIVATE flow boost_target)
endif()
