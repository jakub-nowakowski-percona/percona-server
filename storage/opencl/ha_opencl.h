/* Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface /* gcc class implementation */
#endif

#include "sql/handler.h"
#include "sql/sql_const.h"
#include "sql/table.h"
#include "thr_lock.h"

#include <CL/opencl.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

typedef std::vector<uchar> OpenCLRow;

struct OpenCLTable {
  std::string name;
  std::size_t rows_count = 0;
  cl::Buffer rows_buffer;
};

struct OpenCLDatabase {
  std::vector<std::shared_ptr<OpenCLTable>> tables;
  cl::Platform openclPlatform;
  cl::Device openclDevice;
  cl::Context openclContext;
  cl::CommandQueue openclQueue;
};

struct OpenCLCursor {
  std::shared_ptr<OpenCLTable> table;
  std::size_t position = 0;
};

class ha_opencl final : public handler {
 public:
  ha_opencl(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_opencl() = default;

  const char *index_type([[maybe_unused]] uint key_number) { return ""; }

  ulong index_flags([[maybe_unused]] uint inx, [[maybe_unused]] uint part,
                    [[maybe_unused]] bool all_parts) const override {
    return 0;
  }

  const char *table_type() const override { return "OPENCL"; }

  ulonglong table_flags() const override { return 0; }

  /* The following defines can be increased if necessary */
#define OPENCL_MAX_KEY MAX_KEY     /* Max allowed keys */
#define OPENCL_MAX_KEY_SEG 16      /* Max segments for key */
#define OPENCL_MAX_KEY_LENGTH 3500 /* Like in InnoDB */
  uint max_supported_keys() const override { return OPENCL_MAX_KEY; }

  uint max_supported_key_length() const override {
    return OPENCL_MAX_KEY_LENGTH;
  }

  uint max_supported_key_part_length(
      [[maybe_unused]] HA_CREATE_INFO *create_info) const override {
    return OPENCL_MAX_KEY_LENGTH;
  }

  int open([[maybe_unused]] const char *name, [[maybe_unused]] int mode,
           [[maybe_unused]] uint test_if_locked,
           [[maybe_unused]] const dd::Table *table_def) override {
    return 0;
  }

  int close() override { return 0; }

  int truncate([[maybe_unused]] dd::Table *table_def) override { return 0; }

  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;

  int rnd_pos([[maybe_unused]] uchar *buf,
              [[maybe_unused]] uchar *pos) override {
    return 0;
  }

  int index_read_map([[maybe_unused]] uchar *buf,
                     [[maybe_unused]] const uchar *key,
                     [[maybe_unused]] key_part_map keypart_map,
                     [[maybe_unused]] ha_rkey_function find_flag) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_read_idx_map([[maybe_unused]] uchar *buf, [[maybe_unused]] uint idx,
                         [[maybe_unused]] const uchar *key,
                         [[maybe_unused]] key_part_map keypart_map,
                         [[maybe_unused]] ha_rkey_function find_flag) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_read_last_map([[maybe_unused]] uchar *buf,
                          [[maybe_unused]] const uchar *key,
                          [[maybe_unused]] key_part_map keypart_map) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_next([[maybe_unused]] uchar *buf) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_prev([[maybe_unused]] uchar *buf) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_first([[maybe_unused]] uchar *buf) override {
    return HA_ERR_END_OF_FILE;
  }

  int index_last([[maybe_unused]] uchar *buf) override {
    return HA_ERR_END_OF_FILE;
  }
  void position([[maybe_unused]] const uchar *record) override { return; }

  int info([[maybe_unused]] uint flag) override { return 0; }

  int external_lock([[maybe_unused]] THD *thd,
                    [[maybe_unused]] int lock_type) override {
    return 0;
  }

  int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info,
             [[maybe_unused]] dd::Table *table_def) override;

  THR_LOCK_DATA **store_lock(
      [[maybe_unused]] THD *thd, THR_LOCK_DATA **to,
      [[maybe_unused]] thr_lock_type lock_type) override {
    return to;
  }

  int delete_table(const char *name, const dd::Table *table_def) override;

 private:
  void reset_cursor();

  int write_row(uchar *buf) override;

  int update_row([[maybe_unused]] const uchar *old_data,
                 [[maybe_unused]] uchar *new_data) override {
    return HA_ERR_WRONG_COMMAND;
  }

  int delete_row([[maybe_unused]] const uchar *buf) override {
    return HA_ERR_WRONG_COMMAND;
  }

  OpenCLCursor curr_cursor;
};
