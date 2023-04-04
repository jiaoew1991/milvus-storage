
#include <arrow/type.h>
#include <parquet/exception.h>

#include <cassert>
#include <memory>
#include <numeric>
#include <utility>

#include "arrow/array/builder_primitive.h"
#include "arrow/array/util.h"
#include "arrow/filesystem/localfs.h"
#include "arrow/filesystem/mockfs.h"
#include "arrow/record_batch.h"
#include "common/exception.h"
#include "common/fs_util.h"
#include "format/parquet/file_writer.h"
#include "reader/record_reader.h"
#include "storage/default_space.h"
#include "storage/deleteset.h"
#include "arrow/util/uri.h"

DefaultSpace::DefaultSpace(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<SpaceOption>& options)
    : Space(options) {

  delete_set_ = std::make_unique<DeleteSet>(*this);
  fs_ = BuildFileSystem(options->uri);
}

void
DefaultSpace::Write(arrow::RecordBatchReader* reader, WriteOption* option) {
  if (!reader->schema()->Equals(*this->manifest_->get_schema())) {
    throw StorageException("schema not match");
  }

  // remove duplicated codes
  auto scalar_schema = this->manifest_->get_scalar_schema(), vector_schema = this->manifest_->get_vector_schema();

  std::vector<std::shared_ptr<arrow::Array>> scalar_cols;
  std::vector<std::shared_ptr<arrow::Array>> vector_cols;

  FileWriter* scalar_writer = nullptr;
  FileWriter* vector_writer = nullptr;

  std::vector<std::string> scalar_files;
  std::vector<std::string> vector_files;

  for (auto rec = reader->Next(); rec.ok(); rec = reader->Next()) {
    auto batch = rec.ValueOrDie();
    if (batch == nullptr) {
      break;
    }
    if (batch->num_rows() == 0) {
      continue;
    }
    auto cols = batch->columns();
    for (int i = 0; i < cols.size(); ++i) {
      if (scalar_schema->GetFieldByName(batch->column_name(i))) {
        scalar_cols.emplace_back(cols[i]);
      }
      if (vector_schema->GetFieldByName(batch->column_name(i))) {
        vector_cols.emplace_back(cols[i]);
      }
    }

    // add offset column
    std::vector<int64_t> offset_values(batch->num_rows());
    std::iota(offset_values.begin(), offset_values.end(), 0);
    arrow::NumericBuilder<arrow::Int64Type> builder;
    auto offset_col = builder.AppendValues(offset_values);
    scalar_cols.emplace_back(builder.Finish().ValueOrDie());

    auto scalar_record = arrow::RecordBatch::Make(scalar_schema, batch->num_rows(), scalar_cols);
    auto vector_record = arrow::RecordBatch::Make(vector_schema, batch->num_rows(), vector_cols);

    if (!scalar_writer) {
      auto scalar_file_path = GetNewParquetFile(manifest_->get_option()->uri);
      scalar_writer = new ParquetFileWriter(scalar_schema.get(), fs_.get(), scalar_file_path);

      auto vector_file_path = GetNewParquetFile(manifest_->get_option()->uri);
      vector_writer = new ParquetFileWriter(vector_schema.get(), fs_.get(), vector_file_path);

      scalar_files.emplace_back(scalar_file_path);
      vector_files.emplace_back(vector_file_path);
    }

    scalar_writer->Write(scalar_record.get());
    vector_writer->Write(vector_record.get());

    if (scalar_writer->count() >= option->max_record_per_file) {
      scalar_writer->Close();
      vector_writer->Close();
      scalar_writer = nullptr;
      vector_writer = nullptr;
    }
  }

  if (scalar_writer) {
    scalar_writer->Close();
    vector_writer->Close();
    scalar_writer = nullptr;
    vector_writer = nullptr;
  }

  manifest_->AddDataFiles(scalar_files, vector_files);
  SafeSaveManifest();
}

void
DefaultSpace::Delete(arrow::RecordBatchReader* reader) {
  FileWriter* writer = nullptr;
  std::string delete_file;
  for (auto rec = reader->Next(); rec.ok(); rec = reader->Next()) {
    auto batch = rec.ValueOrDie();
    if (batch == nullptr) {
      break;
    }

    if (!writer) {
      delete_file = GetNewParquetFile(manifest_->get_option()->uri);
      auto schema = manifest_->get_delete_schema().get();
      writer = new ParquetFileWriter(schema, fs_.get(), delete_file);
    }

    if (batch->num_rows() == 0) {
      continue;
    }

    writer->Write(batch.get());
    delete_set_->Add(batch);
  }

  if (!writer) {
    writer->Close();
    manifest_->AddDeleteFile(delete_file);
    SafeSaveManifest();
  }
}

std::unique_ptr<arrow::RecordBatchReader>
DefaultSpace::Read(std::shared_ptr<ReadOption> option) {
  return RecordReader::GetRecordReader(*this, option);
}

void
DefaultSpace::SafeSaveManifest() {
  auto tmp_manifest_file_path = GetManifestTmpFile(manifest_->get_option()->uri);
  auto manifest_file_path = GetManifestFile(manifest_->get_option()->uri);

  PARQUET_ASSIGN_OR_THROW(auto output, fs_->OpenOutputStream(tmp_manifest_file_path));
  Manifest::WriteManifestFile(manifest_.get(), output.get());
  PARQUET_THROW_NOT_OK(output->Flush());
  PARQUET_THROW_NOT_OK(output->Close());

  PARQUET_THROW_NOT_OK(fs_->Move(tmp_manifest_file_path, manifest_file_path));
  PARQUET_THROW_NOT_OK(fs_->DeleteFile(tmp_manifest_file_path));
}