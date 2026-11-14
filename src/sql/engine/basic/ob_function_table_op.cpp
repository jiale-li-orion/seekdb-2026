/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_,
                    file_column_indices_, has_correlated_expr_, file_sql_format_, file_sql_header_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.file_sql_format_ != 0) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    ObDatum *path_datum = nullptr;
    ObString root;
    bool enabled = false;
    if (OB_ISNULL(ctx_.get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ctx_.get_my_session()->get_sys_variable(
            share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(ctx_.get_my_session()->get_secure_file_priv(root))) {
    } else if (root.empty()) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, path_datum))) {
    } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ObString path = path_datum->get_string();
      try {
        file_reader_ = new (std::nothrow) file_sql::Reader(
            std::string(path.ptr(), path.length()),
            MY_SPEC.file_sql_format_ == 1 ? file_sql::Reader::Format::csv
                : MY_SPEC.file_sql_format_ == 2 ? file_sql::Reader::Format::jsonl
                                                : file_sql::Reader::Format::glob,
            std::string(root.ptr(), root.length()), MY_SPEC.file_sql_header_);
        file_cells_ = new (std::nothrow) std::vector<file_sql::Cell>();
        if (file_reader_ == nullptr || file_cells_ == nullptr) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          const auto schema = file_reader_->infer();
          for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.file_column_indices_.count(); ++i) {
            const int64_t index = MY_SPEC.file_column_indices_.at(i);
            if (index < 0 || static_cast<std::size_t>(index) >= schema.columns.size()) {
              ret = OB_INVALID_ARGUMENT;
            }
          }
        }
      } catch (const std::exception &error) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("file table reader opening failed", K(ret), "reason", error.what());
      }
      if (OB_FAIL(ret)) release_file_reader();
    }
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.file_sql_format_ != 0) {
    if (file_reader_ == nullptr) {
      ret = OB_NOT_INIT;
    } else {
      try { file_reader_->rewind(); }
      catch (const std::exception &error) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("file table rescan failed", K(ret), "reason", error.what());
      }
    }
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  release_file_reader();
  return ret;
}

void ObFunctionTableOp::release_file_reader()
{
  delete file_reader_;
  delete file_cells_;
  file_reader_ = nullptr;
  file_cells_ = nullptr;
}

void ObFunctionTableOp::destroy()
{
  release_file_reader();
  ObOperator::destroy();
}


int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  if (file_reader_ == nullptr || file_cells_ == nullptr) {
    ret = OB_NOT_INIT;
  } else {
    try {
      clear_evaluated_flag();
      if (!file_reader_->next(*file_cells_)) {
        ret = OB_ITER_END;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
          ObExpr *expr = MY_SPEC.column_exprs_.at(i);
          const int64_t index = MY_SPEC.file_column_indices_.at(i);
          if (expr == nullptr || index < 0
              || static_cast<std::size_t>(index) >= file_cells_->size()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const auto &value = file_cells_->at(index);
            ObDatum &out = expr->locate_datum_for_write(eval_ctx_);
            const ObString text(static_cast<int32_t>(value.value.size()), value.value.data());
            if (value.is_null) {
              out.set_null();
            } else {
              switch (expr->datum_meta_.type_) {
                case ObIntType: {
                  int64_t integer = 0;
                  const auto parsed = std::from_chars(value.value.data(),
                      value.value.data() + value.value.size(), integer);
                  if (parsed.ec != std::errc()
                      || parsed.ptr != value.value.data() + value.value.size()) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    out.set_int(integer);
                  }
                  break;
                }
                case ObTinyIntType:
                  out.set_int(value.value == "true" ? 1 : 0);
                  break;
                case ObDoubleType: {
                  char *end = nullptr;
                  const double floating = std::strtod(value.value.c_str(), &end);
                  if (end != value.value.c_str() + value.value.size()) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    out.set_double(floating);
                  }
                  break;
                }
                case ObDateType: {
                  ObDateSqlMode mode;
                  int32_t day = 0;
                  if (OB_FAIL(ObTimeConverter::str_to_date(text, day, mode))) {
                  } else {
                    out.set_date(day);
                  }
                  break;
                }
                case ObDateTimeType: {
                  ObTimeConvertCtx convert(nullptr, false);
                  ObDateSqlMode mode;
                  int64_t timestamp = 0;
                  ObScale scale;
                  if (OB_FAIL(ObTimeConverter::str_to_datetime(
                          text, convert, timestamp, &scale, mode))) {
                  } else {
                    out.set_datetime(timestamp);
                  }
                  break;
                }
                default: {
                  if (text.empty()) {
                    out.set_string("", 0);
                  } else {
                    char *buffer = expr->get_str_res_mem(eval_ctx_, text.length());
                    if (buffer == nullptr) {
                      ret = OB_ALLOCATE_MEMORY_FAILED;
                    } else {
                      MEMCPY(buffer, text.ptr(), text.length());
                      out.set_string(buffer, text.length());
                    }
                  }
                  break;
                }
              }
            }
            if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
          }
        }
      }
    } catch (const std::exception &error) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file table scan failed", K(ret), "reason", error.what());
    }
  }
  return ret;
}

int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
