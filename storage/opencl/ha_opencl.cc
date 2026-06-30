/* Copyright (c) 2005, 2012, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "ha_opencl.h"
#include <mysql/components/services/udf_metadata.h>
#include <stdexcept>
#include "sql/field.h"
#include "sql/sql_class.h"

namespace {

SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(mysql_udf_metadata) *udf_metadata_service = nullptr;

}  // namespace

static OpenCLDatabase *database = nullptr;

static int table_index(std::string_view full_name) {
  for (std::size_t idx = 0; idx < database->tables.size(); ++idx) {
    if (database->tables[idx]->name == full_name) {
      return idx;
    }
  }
  return -1;
}

static int table_index(std::string_view db_name, std::string_view table_name) {
  return table_index("./" + std::string(db_name) + "/" +
                     std::string(table_name));
}

static std::size_t opencl_field_size(Field *field) {
  switch (field->type()) {
    case MYSQL_TYPE_FLOAT:
      return field->data_length();
    case MYSQL_TYPE_VECTOR:
      return field->field_length;
    default:
      break;
  }
  throw std::runtime_error("Unknown type");
}

static std::size_t opencl_row_size(TABLE *table) {
  std::size_t row_size = 0;
  for (std::size_t idx = 0; table->field[idx]; ++idx) {
    auto *field = table->field[idx];
    row_size += opencl_field_size(field);
  }
  return row_size;
}

////////////////////////////////////////////////////////////////////////////////
// UDF

static bool set_return_value_charset(UDF_INIT *initid,
                                     const char *charset = "utf8mb4") {
  return !udf_metadata_service->result_set(initid, "charset",
                                           const_cast<char *>(charset));
}

static bool set_args_charset(UDF_ARGS *udf_args,
                             const char *charset = "utf8mb4") {
  for (std::size_t idx = 0; idx < udf_args->arg_count; ++idx) {
    if (udf_args->arg_type[idx] == STRING_RESULT &&
        udf_metadata_service->argument_set(udf_args, "charset", idx,
                                           const_cast<char *>(charset))) {
      return false;
    }
  }

  return true;
}

static bool set_charset(UDF_INIT *initid, UDF_ARGS *udf_args,
                        const char *charset = "utf8mb4") {
  return set_return_value_charset(initid, charset) &&
         set_args_charset(udf_args, charset);
}

extern "C" {

// opencl_devices

bool opencl_devices_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (!set_charset(initid, args)) {
    strcpy(message, "cannot set charset");
    return true;
  }

  if (args->arg_count != 0) {
    strcpy(message, "too many arguments");
    return true;
  }

  return false;
}

char *opencl_devices([[maybe_unused]] UDF_INIT *initid,
                     [[maybe_unused]] UDF_ARGS *args,
                     [[maybe_unused]] char *result, unsigned long *length,
                     char *is_null, char *error) {
  *is_null = 0;
  *error = 0;

  std::ostringstream oss;
  oss << "[\n";
  std::vector<cl::Platform> platforms;
  cl::Platform::get(&platforms);
  for (auto platform = platforms.cbegin(); platform != platforms.cend();
       ++platform) {
    std::vector<cl::Device> devices;
    platform->getDevices(CL_DEVICE_TYPE_ALL, &devices);
    for (auto device = devices.cbegin(); device != devices.cend(); ++device) {
      std::string device_type;
      switch (device->getInfo<CL_DEVICE_TYPE>()) {
        case CL_DEVICE_TYPE_CPU:
          device_type = "CPU";
          break;
        case CL_DEVICE_TYPE_GPU:
          device_type = "GPU";
          break;
        default:
          device_type = "unknown";
      }
      oss << "  {\n"
          << "    \"platform\": \"" << platform->getInfo<CL_PLATFORM_NAME>()
          << "\",\n"
          << "    \"device\": \"" << device->getInfo<CL_DEVICE_NAME>()
          << "\",\n"
          << "    \"type\": \"" << device_type << "\"\n"
          << "  }";
      if (platform != platforms.cend() - 1 || device != devices.cend() - 1) {
        oss << ",";
      }
      oss << "\n";
    }
  }
  oss << "]";

  auto result_string = new std::string(oss.str());
  initid->ptr = reinterpret_cast<char *>(result_string);

  *length = result_string->size();
  return result_string->data();
}

void opencl_devices_deinit(UDF_INIT *initid) {
  delete reinterpret_cast<std::string *>(initid->ptr);
}

// opencl_exec

bool opencl_exec_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (!set_charset(initid, args)) {
    strcpy(message, "cannot set charset");
    return true;
  }

  if (args->arg_count != 4 || args->arg_type[0] != STRING_RESULT ||
      args->arg_type[1] != STRING_RESULT ||
      args->arg_type[2] != STRING_RESULT ||
      args->arg_type[3] != STRING_RESULT) {
    strcpy(message, "opencl_exec() requires three string arguments");
    return true;
  }

  return false;
}

char *opencl_exec([[maybe_unused]] UDF_INIT *initid,
                  [[maybe_unused]] UDF_ARGS *args,
                  [[maybe_unused]] char *result, unsigned long *length,
                  char *is_null, char *error) {
  *is_null = 0;
  *error = 0;

  auto db_name = std::string_view(args->args[0], args->lengths[0]);
  auto table_name = std::string_view(args->args[1], args->lengths[1]);
  auto kernel_func = std::string_view(args->args[2], args->lengths[2]);
  auto kernel_code = std::string_view(args->args[3], args->lengths[3]);

  auto table_idx = table_index(db_name, table_name);
  if (table_idx < 0) {
    *error = 1;
    return nullptr;
  }

  auto &table = database->tables[table_idx];

  cl::Program::Sources sources;
  sources.push_back({kernel_code.data(), kernel_code.size()});

  cl::Program program(database->openclContext, sources);
  if (program.build({database->openclDevice}) != CL_SUCCESS) {
    *error = 1;
    auto error_msg =
        program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(database->openclDevice);
    return nullptr;
  }

  auto result_buffer = cl::Buffer(database->openclContext, CL_MEM_READ_WRITE,
                                  table->rows_count * sizeof(float));

  cl::Kernel user_kernel(program, kernel_func.data());
  user_kernel.setArg(0, table->rows_buffer);
  user_kernel.setArg(1, result_buffer);
  database->openclQueue.enqueueNDRangeKernel(user_kernel, cl::NullRange,
                                             cl::NDRange(table->rows_count),
                                             cl::NullRange);
  database->openclQueue.finish();

  std::vector<float> result_vector(table->rows_count);
  database->openclQueue.enqueueReadBuffer(result_buffer, CL_TRUE, 0,
                                          table->rows_count * sizeof(float),
                                          result_vector.data());

  std::ostringstream oss;
  oss << std::scientific << "[";
  for (std::size_t idx = 0; idx < result_vector.size(); ++idx) {
    if (idx != 0) {
      oss << ",";
    }
    oss << result_vector[idx];
  }
  oss << "]";

  auto result_string = new std::string(oss.str());
  initid->ptr = reinterpret_cast<char *>(result_string);

  *length = result_string->size();
  return result_string->data();
}

void opencl_exec_deinit(UDF_INIT *initid) {
  delete reinterpret_cast<std::string *>(initid->ptr);
}

}  // extern "C"

////////////////////////////////////////////////////////////////////////////////
// Storage

static handler *create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                               MEM_ROOT *mem_root) {
  return new (mem_root) ha_opencl(hton, table);
}

static int init(void *p) {
  // Services
  my_h_service h_udf_metadata_service = nullptr;
  reg_srv = mysql_plugin_registry_acquire();
  if (reg_srv->acquire("mysql_udf_metadata", &h_udf_metadata_service)) {
    assert(false);
  }

  udf_metadata_service = reinterpret_cast<SERVICE_TYPE(mysql_udf_metadata) *>(
      h_udf_metadata_service);

  // Storage
  auto opencl_hton = static_cast<handlerton *>(p);
  opencl_hton->db_type = DB_TYPE_OPENCL;
  opencl_hton->create = create_handler;
  opencl_hton->flags = HTON_CAN_RECREATE;

  database = new OpenCLDatabase;

  // UDFs

  return 0;
}

static int deinit([[maybe_unused]] void *p) {
  // Storage
  delete database;

  // Services
  using udf_metadata_t = SERVICE_TYPE_NO_CONST(mysql_udf_metadata);

  if (udf_metadata_service) {
    reg_srv->release(reinterpret_cast<my_h_service>(
        const_cast<udf_metadata_t *>(udf_metadata_service)));
  }

  return 0;
}

ha_opencl::ha_opencl(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {
  std::vector<cl::Platform> platforms;
  cl::Platform::get(&platforms);
  database->openclPlatform = platforms[1];
  DBUG_PRINT("ha_opencl",
             ("OpenCL platform: %s",
              database->openclPlatform.getInfo<CL_PLATFORM_NAME>().c_str()));
  auto pn = database->openclPlatform.getInfo<CL_PLATFORM_NAME>();

  std::vector<cl::Device> devices;
  database->openclPlatform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
  database->openclDevice = devices[0];
  DBUG_PRINT("ha_opencl",
             ("OpenCL device: %s",
              database->openclDevice.getInfo<CL_DEVICE_NAME>().c_str()));
  auto dn = database->openclDevice.getInfo<CL_DEVICE_NAME>();

  database->openclContext = cl::Context(database->openclDevice);
  database->openclQueue =
      cl::CommandQueue(database->openclContext, database->openclDevice);
}

int ha_opencl::create(const char *name, TABLE *table_arg,
                      [[maybe_unused]] HA_CREATE_INFO *create_info,
                      [[maybe_unused]] dd::Table *table_def) {
  if (table_index(name) != -1) {
    DBUG_PRINT("ha_opencl", ("Table '%s' already exists.", name));
    return HA_ERR_TABLE_EXIST;
  }

  std::size_t idx = 0;
  while (table_arg->field[idx]) {
    auto field = table_arg->field[idx];

    if (field->type() != MYSQL_TYPE_FLOAT &&
        field->type() != MYSQL_TYPE_VECTOR) {
      DBUG_PRINT("ha_opencl", ("Unsupported field type."));
      return 1;
    }

    if (field->is_nullable()) {
      DBUG_PRINT("ha_opencl", ("Nullable fields are not supported."));
      return 1;
    }

    idx++;
  }

  auto table = std::make_shared<OpenCLTable>();
  table->name = name;
  table->rows_buffer =
      cl::Buffer(database->openclContext, CL_MEM_READ_WRITE,
                 opencl_row_size(table_arg) * table_arg->s->max_rows);
  database->tables.push_back(table);
  DBUG_PRINT("ha_opencl", ("Created table '%s'.", name));

  return 0;
}

int ha_opencl::delete_table(const char *name,
                            [[maybe_unused]] const dd::Table *table_def) {
  auto table_idx = table_index(name);
  if (table_idx < 0) {
    return HA_ERR_NO_SUCH_TABLE;
  }

  database->tables.erase(database->tables.begin() + table_idx);

  return 0;
}

void ha_opencl::reset_cursor() {
  if (database->tables.empty()) {
    return;
  }

  auto index = table_index(table->s->db.str, table->s->table_name.str);
  if (index < 0) {
    return;
  }

  curr_cursor = {database->tables[index], 0};
}

int ha_opencl::write_row([[maybe_unused]] uchar *buf) {
  if (!curr_cursor.table) {
    reset_cursor();
  }

  auto out_buffer = std::vector<uchar>(opencl_row_size(table), 0);
  auto out_ptr = out_buffer.data();

  for (std::size_t idx = 0; table->field[idx]; ++idx) {
    auto field = table->field[idx];
    switch (field->type()) {
      case (MYSQL_TYPE_FLOAT): {
        float value = field->val_real();
        std::memcpy(out_ptr, &value, sizeof(value));
        break;
      }
      case MYSQL_TYPE_VECTOR: {
        auto vector_field = down_cast<const Field_vector *>(field);
        auto blob_data = vector_field->get_blob_data();
        auto dl = vector_field->data_length();
        std::memcpy(out_ptr, blob_data, dl);
        break;
      }
      default: {
        assert(false);
      }
    }
    out_ptr += opencl_field_size(field);
  }

  database->openclQueue.enqueueWriteBuffer(
      curr_cursor.table->rows_buffer, CL_TRUE,
      curr_cursor.table->rows_count * out_buffer.size(), out_buffer.size(),
      out_buffer.data());
  ++curr_cursor.table->rows_count;

  return 0;
}

int ha_opencl::rnd_init([[maybe_unused]] bool scan) {
  reset_cursor();

  return 0;
}

int ha_opencl::rnd_next(uchar *buf) {
  if (curr_cursor.position == curr_cursor.table->rows_count) {
    curr_cursor = {};
    return HA_ERR_END_OF_FILE;
  }

  auto in_buffer = std::vector<uchar>(opencl_row_size(table));
  auto in_ptr = in_buffer.data();

  database->openclQueue.enqueueReadBuffer(
      curr_cursor.table->rows_buffer, CL_TRUE,
      curr_cursor.position * in_buffer.size(), in_buffer.size(),
      in_buffer.data());

  for (std::size_t idx = 0; table->field[idx]; ++idx) {
    auto field = table->field[idx];
    auto out_ptr = buf + field->offset(table->record[0]);
    switch (field->type()) {
      case (MYSQL_TYPE_FLOAT): {
        std::memcpy(out_ptr, in_ptr, sizeof(float));
        break;
      }
      case MYSQL_TYPE_VECTOR: {
        auto vector_field = down_cast<Field_vector *>(field);
        bitmap_set_bit(table->write_set, vector_field->field_index());
        vector_field->store((char *)in_ptr, vector_field->field_length,
                            &my_charset_bin);
        break;
      }
      default: {
        assert(false);
      }
    }
    in_ptr += opencl_field_size(field);
  }

  ++curr_cursor.position;

  return 0;
}

struct st_mysql_storage_engine opencl_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(opencl){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &opencl_storage_engine,
    "OPENCL",
    "Jakub Nowakowski",
    "OpenCL storage engine",
    PLUGIN_LICENSE_GPL,
    init,    /* Plugin Init */
    nullptr, /* Plugin check uninstall */
    deinit,  /* Plugin Deinit */
    0x0100 /* 1.0 */,
    nullptr, /* status variables                */
    nullptr, /* system variables                */
    nullptr, /* config options                  */
    0,       /* flags                           */
} mysql_declare_plugin_end;
