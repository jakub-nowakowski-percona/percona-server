#INCLUDE_DIRECTORIES(AFTER /opt/intel/oneapi/2025.3/include)

SET(OPENCL_SOURCES  ha_opencl.cc ha_opencl.h)
MYSQL_ADD_PLUGIN(opencl ${OPENCL_SOURCES} STORAGE_ENGINE
  LINK_LIBRARIES extra::rapidjson /opt/intel/opencl/lib/libOpenCL.so)
