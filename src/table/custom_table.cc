#include "custom_table.h"
#include <cstring>
#include <assert.h>
#include <iostream>
#include <cstdio>

namespace bytedance_db_project {
CustomTable::CustomTable() {
  // Since ncols must larger than 3
  // 10 * 4, align to 8 -> = 5 Bytes per row
  nbytespr_part_[0] = (FIXED_BITS_FIELD * PART_ONE_NCOLS + 7) >> 3;
  // nbytespr_part_[0] = (nbytespr_part_[0] + 1) & (~0x1);
  
}

CustomTable::~CustomTable() {
  for (size_t i = 0; i < TABLE_NPARTS; ++i)
    if (storage_part_[i] != nullptr) {
      delete storage_part_[i];
      storage_part_[i] = nullptr;
    }

  if (storage_sum_row_ != nullptr) {
    delete storage_sum_row_;
    storage_sum_row_ = nullptr;
  }
}

CustomTable::BitPacker::BitPacker() : bit_offset_(0), 
  ptr_cur_(nullptr) {}

CustomTable::BitPacker::~BitPacker() {}

void CustomTable::BitPacker::set_ptr_cur(uint16_t* ptr_cur) {
  reset();
  ptr_cur_ = ptr_cur;
  *ptr_cur_ = 0;
}


int16_t CustomTable::BitPacker::swap_bytes(uint16_t val) {
  union {
    uint16_t integer;
    uint8_t  bytes[2];
  } a, b;
  a.integer = val;
  for (int i = 0; i < 2; ++i)
    b.bytes[i] = a.bytes[1-i];
  return b.integer;
}

int CustomTable::BitPacker::write(uint16_t val, size_t nbits) {
  size_t available = MAXB - bit_offset_;
  int len0 = (available >= nbits) ? nbits : available;
  int len1 = nbits - len0;
  assert(nbits <= MAXB);

  // Only in case val is not clean
  if (nbits != MAXB)
		val = val & ((1ULL << nbits) - 1ULL);
  if (len0) {
    *ptr_cur_ |= ((val >> len1) << (available - len0));
    bit_offset_ += len0;
  }
  // assuming space is enough, Otherwise should return -1;
  if (len1) {
    // swap Bytes (le to be)
    *ptr_cur_ = swap_bytes(*ptr_cur_);
    ptr_cur_++;
    *ptr_cur_ = val << (MAXB - len1);
    bit_offset_ = len1;
  }

  return nbits;
}

void CustomTable::BitPacker::flush() {
  if (!is_flushed_ && bit_offset_ != 0) {
    *ptr_cur_ = swap_bytes(*ptr_cur_);
  }
  is_flushed_ = 1;
  bit_offset_ = 0;
}

void CustomTable::BitPacker::reset() {
  bit_offset_ = 0;
  ptr_cur_ = nullptr;
  is_flushed_ = 0;
}

void CustomTable::Load(BaseDataLoader *loader) {
  // TODO: Implement this!
  BitPacker bit_packer;
  num_cols_ = loader->GetNumCols();
  auto rows = loader->GetRows();
  num_rows_ = rows.size();
  // since some queries requires col3
  assert(num_cols_ > 3);
  nbytespr_part_[1] = ((num_cols_ - PART_ONE_NCOLS) * FIXED_BITS_FIELD + 7) >> 3;
  nbytespr_part_[1] = (nbytespr_part_[1] + 1) & (~0x1);

  for (size_t i = 0; i < TABLE_NPARTS; ++i)
    storage_part_[i] = new char[(nbytespr_part_[i] * num_rows_ + 1)];

  for (size_t row_id = 0; row_id < num_rows_; row_id++) {
    auto cur_row = rows.at(row_id);
    int64_t sum = 0;
    int32_t val;
    bit_packer.set_ptr_cur((uint16_t*) (storage_part_[0] + nbytespr_part_[0] * row_id + 
                                            (FIXED_BITS_SUM_FIELD >> 3)) );
    for (size_t col_id = 0; col_id < num_cols_; ++col_id) {
      val = *(int32_t*) (cur_row + FIXED_FIELD_LEN * col_id);
      // assert(val < 1024 && val >= 0);
      sum += val;
      if (col_id == 0)
        sum_col0_ += val;
      // start of part II in this row
      if (col_id == PART_ONE_NCOLS) {
        bit_packer.flush();
        bit_packer.set_ptr_cur((uint16_t*) (storage_part_[1] + nbytespr_part_[1] * row_id) );
      }
      bit_packer.write((uint16_t) val, FIXED_BITS_FIELD);
    }
    bit_packer.flush();
    *(int64_t*) (storage_part_[0] + nbytespr_part_[0] * row_id) = sum;
  }
  is_col0_sumed_ = 1;
}

inline int64_t CustomTable::GetRowSum(int32_t row_id) {

  return *(int64_t*) (storage_part_[0] + nbytespr_part_[0] * row_id);
}

inline void CustomTable::UpdateRowSum(int32_t row_id, int64_t val_diff) {
  *(int64_t*) (storage_part_[0] + nbytespr_part_[0] * row_id) += val_diff;
}

int32_t CustomTable::GetIntField(int32_t row_id, int32_t col_id) {
  // TODO: Implement this!
  // Note that: ONE val need TWO Bytes data
  int byte_id, bit_offset, len0, len1;
  uint8_t v1, v2;
  int32_t res = 0;
  int tb_id; // table idx

  tb_id = col_id >= PART_ONE_NCOLS;
  if (tb_id == 1)
    col_id -= PART_ONE_NCOLS;

  byte_id = (col_id * FIXED_BITS_FIELD >> 3); 
  bit_offset = col_id * FIXED_BITS_FIELD - (byte_id << 3);
  byte_id += nbytespr_part_[tb_id] * row_id;
  if (tb_id == 0)
    byte_id += FIXED_BITS_SUM_FIELD >> 3;

  v1 = *(uint8_t*) (storage_part_[tb_id] + byte_id);
  v2 = *(uint8_t*) (storage_part_[tb_id] + byte_id + 1);

  len0 = 8 - bit_offset; // bits in first Byte
  len1 = FIXED_BITS_FIELD - len0;
  res |= ((int32_t) v1 & ((1 << len0) - 1)) << len1;
  res |= (v2 >> (8 - len1));

  return res;
}

void CustomTable::PutIntField(int32_t row_id, int32_t col_id, int32_t field) {
  // TODO: Implement this!
  // assert(field < 1024 && field >= 0);
  int byte_id, bit_offset, len0, len1;
  uint8_t v1, v2;
  int32_t ori_val = 0;
  int tb_id; // table idx
  int64_t sum_diff;

  tb_id = col_id >= PART_ONE_NCOLS;
  if (tb_id == 1)
    col_id -= PART_ONE_NCOLS;

  byte_id = (col_id * FIXED_BITS_FIELD >> 3); 
  bit_offset = col_id * FIXED_BITS_FIELD - (byte_id << 3);
  byte_id += nbytespr_part_[tb_id] * row_id;
  if (tb_id == 0)
    byte_id += FIXED_BITS_SUM_FIELD >> 3;

  v1 = *(uint8_t*) (storage_part_[tb_id] + byte_id);
  v2 = *(uint8_t*) (storage_part_[tb_id] + byte_id + 1);

  len0 = 8 - bit_offset; // bits in first Byte
  len1 = FIXED_BITS_FIELD - len0;
  ori_val |= ((int32_t) v1 & ((1 << len0) - 1)) << len1;
  ori_val |= (v2 >> (8 - len1));
  sum_diff = field - ori_val;

  // update value and put value in storage
  v1 = ((v1 >> len0) << len0) | (uint8_t) (field >> len1);
  // caution, v2 will auto extend to more bits if v2 << len1 >> len1
  v2 = (v2 & ((1 << (8 - len1)) - 1)) | ((uint8_t) field << (8 - len1));
  *(uint8_t*) (storage_part_[tb_id] + byte_id) = v1;
  *(uint8_t*) (storage_part_[tb_id] + byte_id + 1) = v2;

  // update cached sum
  UpdateRowSum(row_id, sum_diff);
  if (col_id == 0)
    sum_col0_ += sum_diff;
}

int64_t CustomTable::ColumnSum() {
  // TODO: Implement this!
  int64_t res = 0;

  if (is_col0_sumed_)
    return sum_col0_;

  for (size_t i = 0; i < num_rows_; ++i)
    res += GetIntField(i, 0);
  is_col0_sumed_ = 1;
  return res;
}

int64_t CustomTable::PredicatedColumnSum(int32_t threshold1,
                                         int32_t threshold2) {
  // TODO: Implement this!
  int64_t res = 0;

  for (size_t row_id = 0; row_id < num_rows_; ++row_id)
    if (GetIntField(row_id, 1) > threshold1 && GetIntField(row_id, 2) < threshold2)
      res += GetIntField(row_id, 0);
  return res;
}

int64_t CustomTable::PredicatedAllColumnsSum(int32_t threshold) {
  // TODO: Implement this!
  int64_t res = 0;
  for (size_t row_id = 0; row_id < num_rows_; ++row_id)
    if (GetIntField(row_id, 0) > threshold)
        res += GetRowSum(row_id);
  return res;
}

int64_t CustomTable::PredicatedUpdate(int32_t threshold) {
  // TODO: Implement this!
  int64_t cnt = 0;
  for (size_t row_id  = 0; row_id < num_rows_; ++row_id)
    if (GetIntField(row_id, 0) < threshold) {
      PutIntField(row_id, 3, GetIntField(row_id, 3) + GetIntField(row_id, 2));
      cnt++;
    }
  return cnt;
}
} // namespace bytedance_db_project