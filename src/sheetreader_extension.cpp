#include "duckdb.h"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader/src/XlsxFile.h"
#include "sheetreader/src/XlsxSheet.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "sheetreader_extension.hpp"

#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

// TODO: Fix default constructor
SRScanData::SRScanData(string file_name)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(1))) {
}

SRScanData::SRScanData(string file_name, string sheet_name)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_name))) {
}

SRScanData::SRScanData(string file_name, int sheet_index)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_index))) {
}

// TODO: Is this still needed?
// SRScanData::SRScanData(ClientContext &context, vector<string> file_names, string sheet_name)
//     : file_names(std::move(file_names)), xlsx_file(file_names[0]),
//       xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(1))) {
// 	//, xlsx_file(make_uniq<XlsxFile>(file_names[0]))
// 	// InitializeReaders(context);
// 	// InitializeFormats();
// }

SRScanGlobalState::SRScanGlobalState(ClientContext &context, const SRScanData &bind_data)
    : bind_data(bind_data), chunk_count(0) {
}

SRScanLocalState::SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate)
    : scan_count(0), bind_data(gstate.bind_data) {
}

idx_t SRScanLocalState::ReadNextBatch(SRScanGlobalState &gstate) {
	// TODO: Reset allocated memory from last batch

	// Reset scan_count
	scan_count = 0;

	// TODO: Get next batch of data from SheetReader
	GetNextBatchFromSR(gstate);

	return scan_count;
}

void SRScanLocalState::GetNextBatchFromSR(SRScanGlobalState &gstate) {
	if (gstate.chunk_count > 1) {
		scan_count = 0;
	}
	scan_count = 2048;
}

SRGlobalTableFunctionState::SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input)
    : state(context, input.bind_data->Cast<SRScanData>()) {
}

unique_ptr<GlobalTableFunctionState> SRGlobalTableFunctionState::Init(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SRScanData>();
	auto result = make_uniq<SRGlobalTableFunctionState>(context, input);
	auto &gstate = result->state;

	return std::move(result);
}

SRLocalTableFunctionState::SRLocalTableFunctionState(ClientContext &context, SRScanGlobalState &gstate)
    : state(context, gstate) {
}

unique_ptr<LocalTableFunctionState> SRLocalTableFunctionState::Init(ExecutionContext &context,
                                                                    TableFunctionInitInput &input,
                                                                    GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<SRGlobalTableFunctionState>();
	auto result = make_uniq<SRLocalTableFunctionState>(context.client, gstate.state);

	return std::move(result);
}

void FillChunk(DataChunk &output, idx_t scan_count) {
	// TODO: Do speed test -- mayb SetValue is faster here?

	// Fill the output chunk with data
	// For now, we just fill it with dummy data
	for (idx_t col = 0; col < output.ColumnCount(); col++) {
		Vector &vec = output.data[col];
		string_t *data = FlatVector::GetData<string_t>(vec);
		// auto &validity = FlatVector::Validity(vec);
		for (idx_t i = 0; i < scan_count; i++) {
			string_t &tmp = data[i];
			tmp = "Row: " + std::to_string(i);
		}
	}
}

union DataPtr {
	string_t *string_data;
	double *double_data;
	bool *bool_data;
	date_t *date_data;
};

// TODO: import chrono
inline void FinishChunk(DataChunk &output, idx_t cardinality, SRScanGlobalState &gstate,
                        std::chrono::time_point<std::chrono::system_clock> start_time_copy_chunk,
                        bool print_time = false) {
	// Set the validity of the output chunk
	// TODO: What do we get with this?
	// for (idx_t col = 0; col < output.ColumnCount(); col++) {
	// 	Vector &vec = output.data[col];
	//
	// 	vec.Verify(scan_count);
	// }
	output.SetCardinality(cardinality);

	std::chrono::time_point<std::chrono::system_clock> finish_time_copy_chunk = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds_chunk = finish_time_copy_chunk - start_time_copy_chunk;
	gstate.times_copy.push_back(elapsed_seconds_chunk.count());

	if (cardinality == 0 && print_time) {
		gstate.finish_time_copy = std::chrono::system_clock::now();
		std::chrono::duration<double> elapsed_seconds = gstate.finish_time_copy - gstate.start_time_copy;
		std::cout << "Copy time: " << elapsed_seconds.count() << "s" << std::endl;

		double sum = 0;
		for (auto &time : gstate.times_copy) {
			sum += time;
		}
		std::cout << "Pure Copy time: " << sum << "s" << std::endl;
	}
	gstate.chunk_count++;
	return;
}

inline void SetRow(const SRScanData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors,
                   vector<XlsxCell> &row_values, idx_t row_id) {

	auto &xlsx_file = bind_data.xlsx_file;

	for (idx_t j = 0; j < bind_data.types.size(); j++) {
		switch (bind_data.types[j].id()) {
		case LogicalTypeId::VARCHAR: {
			auto value = xlsx_file.getString(row_values[j].data.integer);
			// string_t creates values that fail the UTF-8 check, so we use the unperformant technique
			// flat_vectors[j].string_data[i] = string_t(value);
			output.data[j].SetValue(row_id, Value(value));
			break;
		}
		case LogicalTypeId::DOUBLE: {
			auto value = row_values[j].data.real;
			flat_vectors[j].double_data[row_id] = value;
			break;
		}
		case LogicalTypeId::BOOLEAN: {
			auto value = row_values[j].data.boolean;
			flat_vectors[j].bool_data[row_id] = value;
			break;
		}
		case LogicalTypeId::DATE: {
			date_t value = date_t((int)(row_values[j].data.real / 86400.0));
			flat_vectors[j].date_data[row_id] = value;
			break;
		}
		default:
			throw InternalException("This shouldn't happen. Unsupported Logical type");
		}
	}
}

inline void SetNull(const SRScanData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {
	// Set cell to NULL
	LogicalType expected_type = bind_data.types[column_id];
	output.data[column_id].SetValue(row_id, Value(expected_type));
}
inline void SetCell(const SRScanData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {

	auto &xlsx_file = bind_data.xlsx_file;

	// TODO: [IMPORTANT] Check whether cell is null and maybe if compatible with column type

	// TODO: Maybe set validity mask

	switch (bind_data.types[column_id].id()) {
	case LogicalTypeId::VARCHAR: {
		auto value = xlsx_file.getString(cell.data.integer);
		// string_t creates values that fail the UTF-8 check, so we use the unperformant technique
		// flat_vectors[j].string_data[i] = string_t(value);
		output.data[column_id].SetValue(row_id, Value(value));
		break;
	}
	case LogicalTypeId::DOUBLE: {
		auto value = cell.data.real;
		flat_vectors[column_id].double_data[row_id] = value;
		break;
	}
	case LogicalTypeId::BOOLEAN: {
		auto value = cell.data.boolean;
		flat_vectors[column_id].bool_data[row_id] = value;
		break;
	}
	case LogicalTypeId::DATE: {
		date_t value = date_t((int)(cell.data.real / 86400.0));
		flat_vectors[column_id].date_data[row_id] = value;
		break;
	}
	default:
		throw InternalException("This shouldn't happen. Unsupported Logical type");
	}
}

bool CheckRowLimitReached(SRScanGlobalState &gstate) {
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	long long limit = row_offset + STANDARD_VECTOR_SIZE;
	long long skipRows = gstate.bind_data.xlsx_sheet->mSkipRows;
	bool limit_reached = gstate.currentRow - skipRows >= limit;
	return limit_reached;
}

idx_t GetCardinality(SRScanGlobalState &gstate) {
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	long long skipRows = gstate.bind_data.xlsx_sheet->mSkipRows;
	// This is the case when no new rows are copied and last chunk was not full (last iteration)
	if (gstate.currentRow + 1 < skipRows + row_offset) {
		return 0;
	}
	return gstate.currentRow - skipRows - row_offset + 1;
}

//! Assume that types of XlsxCells align with the types of the columns
//! Returns Cardinality (number of rows copied)
size_t UnsafeCopy(SRScanGlobalState &gstate, const SRScanData &bind_data, DataChunk &output,
                  vector<DataPtr> &flat_vectors) {

	auto &fsheet = bind_data.xlsx_sheet;

	D_ASSERT(bind_data.number_threads == fsheet->mCells.size());
	idx_t number_threads = bind_data.number_threads;

	if (number_threads == 0) {
		return 0;
	}

	size_t row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	auto calcAdjustedRow = [row_offset](long long currentRow, unsigned long skip_rows) {
		return currentRow - skip_rows - row_offset;
	};

	// Initialize state
	if (gstate.currentLocs.size() == 0) {
		// Get number of buffers from first thread (is always the maximum)
		gstate.maxBuffers = fsheet->mCells[0].size();
		gstate.currentBuffer = 0;
		gstate.currentThread = 0;
		gstate.currentCell = 0;
		gstate.currentColumn = 0;
		gstate.currentRow = -1;
		gstate.currentLocs = std::vector<size_t>(number_threads, 0);
	}

	//! To get the correct order of rows we iterate for(buffer_index) { for(thread_index) { ... } }
	//! This is due to how sheetreader-core writes the data to the buffers (stored in mCells)
	for (; gstate.currentBuffer < gstate.maxBuffers; ++gstate.currentBuffer) {
		for (; gstate.currentThread < fsheet->mCells.size(); ++gstate.currentThread) {

			// If there are no more buffers to read, return last row and prepare for finishing copy
			if (fsheet->mCells[gstate.currentThread].size() == 0) {
				// Set to maxBuffers, so this is the last iteration
				gstate.currentBuffer = gstate.maxBuffers;

				return GetCardinality(gstate);
			}

			//! Get current cell buffer
			const std::vector<XlsxCell> cells = fsheet->mCells[gstate.currentThread].front();
			//! Location info for current thread
			const std::vector<LocationInfo> &locs_infos = fsheet->mLocationInfos[gstate.currentThread];
			//! Current location index
			size_t &currentLoc = gstate.currentLocs[gstate.currentThread];

			// TODO: Check when this is the case
			// currentCell <= cells.size() because there might be location info after last cell
			for (; gstate.currentCell <= cells.size(); ++gstate.currentCell) {

				// if (get_next_row) {
				// 	break;
				// }

				// Update currentRow & currentColumn when location info is available for current cell at currentLoc
				// After setting those values: Advance to next location info
				//
				// This means that the values won't be updated if there is no location info for the current cell
				// (e.g. not first cell in row)
				//
				// Loop is executed n+1 times for first location info, where n is the number of skip_rows (specified as
				// parameter for interleaved) This is because, sheetreader creates location infoes for the skipped lines
				// with cell == column == buffer == 0 sames as for the first "real" row
				while (currentLoc < locs_infos.size() && locs_infos[currentLoc].buffer == gstate.currentBuffer &&
				       locs_infos[currentLoc].cell == gstate.currentCell) {

					gstate.currentColumn = locs_infos[currentLoc].column;
					if (locs_infos[currentLoc].row == -1ul) {
						++gstate.currentRow;
					} else {
						gstate.currentRow = locs_infos[currentLoc].row;
					}

					// TODO: Check how expensive lambda call is
					long long adjustedRow = calcAdjustedRow(gstate.currentRow, fsheet->mSkipRows);
					// This only happens for header rows -- we want to skip them
					if (adjustedRow < 0) {
						++currentLoc;
						// Skip to next row
						if (currentLoc < locs_infos.size()) {
							gstate.currentCell = locs_infos[currentLoc].cell;
						} else {
							throw InternalException("Skipped more rows than available in first buffer -- consider "
							                        "decreasing number of threads");
							// No more location infos for this buffer
							// TODO: Advance to next thread
							// fsheet->mCells[gstate.currentThread].pop_front();
						}
						continue;
					}

					// Increment for next iteration
					++currentLoc;

					if (CheckRowLimitReached(gstate)) {
						return (GetCardinality(gstate) - 1);
					}
				}
				// TODO: I think this can never be the case, since currentCell is not modified after loop check
				if (gstate.currentCell >= cells.size())
					break;
				const auto currentColumn = gstate.currentColumn;
				// If this cell is in a column that was not present in the first row, we throw an error
				// TODO: Can we resize the columns after bind?
				// TODO: make columns user definable via named parameter
				if (currentColumn >= bind_data.types.size()) {
					// throw InvalidInputException("Row " + std::to_string(gstate.currentRow) +
					//                             "has more columns than the first row: " +
					//                             std::to_string(currentColumn + 1));
					// currentRowData.resize(fsheet->mDimension.first - fsheet->mSkipColumns, XlsxCell());
					std::cout << "Row " << gstate.currentRow
					          << " has more columns than the first row: " << currentColumn + 1 << std::endl;
				}
				const XlsxCell &cell = cells[gstate.currentCell];
				long long mSkipRows = fsheet->mSkipRows;
				long long adjustedRow = calcAdjustedRow(gstate.currentRow, mSkipRows);

				bool types_align = cell.type == bind_data.SR_types[currentColumn];
				// TODO: For some reason sheetreader-core doesn't determine empty cells to be T_NONE
				if (cell.type == CellType::T_NONE || cell.type == CellType::T_ERROR || !types_align) {
					SetNull(bind_data, output, flat_vectors, cell, adjustedRow, currentColumn);
				} else {
					// std::cout << "Row: " << adjustedRow << " Adjusted Column: " << currentColumn << std::endl;
					SetCell(bind_data, output, flat_vectors, cell, adjustedRow, currentColumn);
				}
				++gstate.currentColumn;
			}
			fsheet->mCells[gstate.currentThread].pop_front();
			gstate.currentCell = 0;
		}
		gstate.currentThread = 0;
		// }

		// TODO: is there a case where currentRowData is not empty?
		// const auto ret = std::pair<size_t, std::vector<XlsxCell>>(gstate.currentRow, currentRowData);
		// gstate.currentRow = 0;
		// return ret;
	}
	// TODO: +1 here?
	return GetCardinality(gstate);
}

inline void SheetreaderTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	// Get the bind dataclass TARGET
	const SRScanData &bind_data = data_p.bind_data->Cast<SRScanData>();
	auto &gstate = data_p.global_state->Cast<SRGlobalTableFunctionState>().state;
	auto &lstate = data_p.local_state->Cast<SRLocalTableFunctionState>().state;

	if (gstate.chunk_count == 0) {
		gstate.start_time_copy = std::chrono::system_clock::now();
	}

	auto start_time_copy_chunk = std::chrono::system_clock::now();

	auto &xlsx_file = bind_data.xlsx_file;
	auto &fsheet = bind_data.xlsx_sheet;

	const idx_t column_count = output.ColumnCount();

	// Store FlatVectors for all columns (they have different data types)
	vector<DataPtr> flat_vectors;

	// Is this useful?
	// data_ptr_t dataptr = output.data[0].GetData();

	for (idx_t col = 0; col < column_count; col++) {
		switch (bind_data.types[col].id()) {
		case LogicalTypeId::VARCHAR: {
			Vector &vec = output.data[col];
			string_t *data_vec = FlatVector::GetData<string_t>(vec);
			DataPtr data;
			data.string_data = data_vec;
			// Store pointer to data
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::DOUBLE: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<double>(vec);
			DataPtr data;
			data.double_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::BOOLEAN: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<bool>(vec);
			DataPtr data;
			data.bool_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::DATE: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<date_t>(vec);
			DataPtr data;
			data.date_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		default:
			throw InternalException("This shouldn't happen. Unsupported Logical type");
		}
	}

	if (bind_data.version == 0) {

		// Get the next batch of data from sheetreader
		idx_t i = 0;
		for (; i < STANDARD_VECTOR_SIZE; i++) {

			auto row = fsheet->nextRow();
			if (row.first == 0) {
				break;
			}
			auto &row_values = row.second;

			// TODO: Check if types align
			for (idx_t j = 0; j < column_count; j++) {
				switch (bind_data.types[j].id()) {
				case LogicalTypeId::VARCHAR: {
					auto value = xlsx_file.getString(row_values[j].data.integer);
					// string_t creates values that fail the UTF-8 check, so we use the unperformant technique
					// flat_vectors[j].string_data[i] = string_t(value);
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DOUBLE: {
					auto value = row_values[j].data.real;
					flat_vectors[j].double_data[i] = value;
					break;
				}
				case LogicalTypeId::BOOLEAN: {
					auto value = row_values[j].data.boolean;
					flat_vectors[j].bool_data[i] = value;
					break;
				}
				case LogicalTypeId::DATE: {
					date_t value = date_t((int)(row_values[j].data.real / 86400.0));
					flat_vectors[j].date_data[i] = value;
					break;
				}
				default:
					throw InternalException("This shouldn't happen. Unsupported Logical type");
				}
			}
		}

		FinishChunk(output, i, gstate, start_time_copy_chunk, bind_data.flag == 1);

		return;

	} else if (bind_data.version == 1) {

		// Get the next batch of data from sheetreader
		idx_t i = 0;
		for (; i < STANDARD_VECTOR_SIZE; i++) {
			// TODO: fsheet is const due to bind_data being const. Maybe we should store the sheet in the local/global
			// state?
			auto row = fsheet->nextRow();
			if (row.first == 0) {
				break;
			}
			auto &row_values = row.second;

			for (idx_t j = 0; j < column_count; j++) {
				switch (bind_data.types[j].id()) {
				case LogicalTypeId::VARCHAR: {
					// TODO: Check if types align
					auto value = xlsx_file.getString(row_values[j].data.integer);
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DOUBLE: {
					auto value = row_values[j].data.real;
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::BOOLEAN: {
					auto value = row_values[j].data.boolean;
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DATE: {
					date_t value = date_t((int)(row_values[j].data.real / 86400.0));
					output.data[j].SetValue(i, Value::DATE(value));
					break;
				}
				default:
					throw InternalException("This shouldn't happen. Unsupported Logical type");
				}
			}
		}

		FinishChunk(output, i, gstate, start_time_copy_chunk);

		return;
	} else if (bind_data.version == 2) {

		// D_ASSERT(bind_data.number_threads == fsheet->mCells.size());
		// idx_t number_threads = bind_data.number_threads;

		// //! Row index in the current chunk
		// size_t current_chunk_row = 0;

		// if (number_threads == 0) {
		// 	// TODO: Set Cardinality 0 etc.
		// 	return std::make_pair(0, std::vector<XlsxCell>());
		// }

		// // Initialize state
		// if (gstate.currentLocs.size() == 0) {
		// 	// Get number of buffers from first thread (is always the maximum)
		// 	gstate.maxBuffers = fsheet->mCells[0].size();
		// 	gstate.currentBuffer = 0;
		// 	gstate.currentThread = 0;
		// 	gstate.currentCell = 0;
		// 	gstate.currentColumn = 0;
		// 	gstate.currentRow = -1;
		// 	gstate.currentLocs = std::vector<size_t>(number_threads, 0);
		// }

		// std::vector<XlsxCell> currentValues;
		// currentValues.resize(fsheet->mDimension.first - fsheet->mSkipColumns, XlsxCell());

		// //! To get the correct order of rows we iterate for(buffer_index) { for(thread_index) { ... } }
		// //! This is due to how sheetreader-core writes the data to the buffers (stored in mCells)
		// for (; gstate.currentBuffer < gstate.maxBuffers; ++gstate.currentBuffer) {
		// 	for (; gstate.currentThread < fsheet->mCells.size(); ++gstate.currentThread) {

		// 		// If there are no more buffers to read, return last row and prepare for finishing copy
		// 		if (fsheet->mCells[gstate.currentThread].size() == 0) {
		// 			// Set to maxBuffers, so this is the last iteration
		// 			gstate.currentBuffer = gstate.maxBuffers;
		// 			// TODO: What's happening here?
		// 			return std::pair<size_t, std::vector<XlsxCell>>(gstate.currentRow - 1, currentValues);
		// 		}
		// 		const std::vector<XlsxCell> cells = fsheet->mCells[gstate.currentThread].front();
		// 		const std::vector<LocationInfo> &locs = fsheet->mLocationInfos[gstate.currentThread];
		// 		size_t &currentLoc = gstate.currentLocs[gstate.currentThread];

		// 		// currentCell <= cells.size() because there might be location info after last cell
		// 		for (; gstate.currentCell <= cells.size(); ++gstate.currentCell) {
		// 			while (currentLoc < locs.size() && locs[currentLoc].buffer == gstate.currentBuffer &&
		// 			       locs[currentLoc].cell == gstate.currentCell) {
		// 				gstate.currentColumn = locs[currentLoc].column;
		// 				if (locs[currentLoc].row == -1ul) {
		// 					++gstate.currentRow;
		// 					++currentLoc;
		// 					if (gstate.currentRow > 0)
		// 						return std::pair<size_t, std::vector<XlsxCell>>(gstate.currentRow - 1, currentValues);
		// 				} else if (static_cast<long long>(locs[currentLoc].row) > gstate.currentRow) {
		// 					const size_t nextRow = locs[currentLoc].row;
		// 					if (nextRow > static_cast<size_t>(gstate.currentRow + 1)) {
		// 						++gstate.currentRow;
		// 					} else {
		// 						gstate.currentRow = locs[currentLoc].row;
		// 						++currentLoc;
		// 					}
		// 					if (gstate.currentRow > 0)
		// 						return std::pair<size_t, std::vector<XlsxCell>>(gstate.currentRow - 1, currentValues);
		// 				} else {
		// 					++currentLoc;
		// 				}
		// 			}
		// 			if (gstate.currentCell >= cells.size())
		// 				break;
		// 			const auto adjustedColumn = gstate.currentColumn;
		// 			++gstate.currentColumn;
		// 			const XlsxCell &cell = cells[gstate.currentCell];
		// 			currentValues[adjustedColumn] = cell;
		// 		}
		// 		fsheet->mCells[gstate.currentThread].pop_front();
		// 		gstate.currentCell = 0;
		// 	}
		// 	gstate.currentThread = 0;
		// }

		// return std::pair<size_t, std::vector<XlsxCell>>(0, {});

		// // Get the next batch of data from sheetreader
		// idx_t i = 0;
		// for (; i < STANDARD_VECTOR_SIZE; i++) {
		// }
	} else if (bind_data.version == 3) {

		auto cardinality = UnsafeCopy(gstate, bind_data, output, flat_vectors);

		FinishChunk(output, cardinality, gstate, start_time_copy_chunk, bind_data.flag == 1);

		return;
	}
}

inline bool ConvertCellTypes(vector<LogicalType> &column_types, vector<string> &column_names,
                             vector<CellType> &colTypesByIndex) {
	idx_t column_index = 0;
	bool first_row_all_string = true;
	for (auto &colType : colTypesByIndex) {
		switch (colType) {
		case CellType::T_STRING_REF:
			column_types.push_back(LogicalType::VARCHAR);
			column_names.push_back("String" + std::to_string(column_index));
			break;
		case CellType::T_STRING:
		case CellType::T_STRING_INLINE:
			// TODO
			throw BinderException("Inline & dynamic String types not supported yet");
			break;
		case CellType::T_NUMERIC:
			column_types.push_back(LogicalType::DOUBLE);
			column_names.push_back("Numeric" + std::to_string(column_index));
			first_row_all_string = false;
			break;
		case CellType::T_BOOLEAN:
			column_types.push_back(LogicalType::BOOLEAN);
			column_names.push_back("Boolean" + std::to_string(column_index));
			first_row_all_string = false;
			break;
		case CellType::T_DATE:
			// TODO: Fix date type
			column_types.push_back(LogicalType::DATE);
			column_names.push_back("Date" + std::to_string(column_index));
			first_row_all_string = false;
			break;
		default:
			// TODO: Add specific column or data type
			throw BinderException("Unknown cell type in column in column " + std::to_string(column_index));
		}
		column_index++;
	}
	return first_row_all_string;
}

inline vector<string> GetHeaderNames(vector<XlsxCell> &row, SRScanData &bind_data) {

	vector<string> column_names;

	for (idx_t j = 0; j < row.size(); j++) {
		switch (row[j].type) {
		case CellType::T_STRING_REF: {
			auto value = bind_data.xlsx_file.getString(row[j].data.integer);
			column_names.push_back(value);
			break;
		}
		case CellType::T_STRING:
		case CellType::T_STRING_INLINE: {
			// TODO
			throw BinderException("Inline & dynamic String types not supported yet");
			break;
		}
		default:
			throw BinderException("Header row contains non-string values");
		}
	}

	return column_names;
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	// Get the file names from the first parameter
	// Note: GetFileList also checks if the files exist

	// Old -- this was nice
	// auto file_names = MultiFileReader::GetFileList(context, input.inputs[0], ".XLSX (Excel)");
	// New (0.10.3) -- TODO: Is there a way to specify file type?
	auto filereader = MultiFileReader::Create(input.table_function);
	auto file_list = filereader->CreateFileList(context, input.inputs[0]);
	auto file_names = file_list->GetAllFiles();

	if (file_names.size() == 0) {
		throw BinderException("No files found in path");
	} else if (file_names.size() > 1) {
		// TODO: Support multiple files
		throw BinderException("Only one file can be read at a time");
	}

	string sheet_name;
	int sheet_index;
	bool sheet_index_set = false;

	bool use_header = false;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "sheet_name") {
			sheet_name = StringValue::Get(kv.second);
		} else if (loption == "sheet_index") {
			sheet_index = IntegerValue::Get(kv.second);
			sheet_index_set = true;
		} else if (loption == "has_header") {
			use_header = BooleanValue::Get(kv.second);
		} else {
			continue;
		}
	}

	if (!sheet_name.empty() && sheet_index_set) {
		throw BinderException("Sheet index & sheet name cannot be set at the same time.");
	}

	// Create the bind data object and return it
	unique_ptr<duckdb::SRScanData> bind_data;

	try {
		if (!sheet_name.empty()) {
			bind_data = make_uniq<SRScanData>(file_names[0], sheet_name);
		} else if (sheet_index_set) {
			bind_data = make_uniq<SRScanData>(file_names[0], sheet_index);
		} else {
			bind_data = make_uniq<SRScanData>(file_names[0]);
		}
	} catch (std::exception &e) {
		throw BinderException(e.what());
	}

	// TODO: Do we need any information from info?
	// TableFunctionInfo *info = input.info.get();

	// Here we could handle any named parameters
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "version") {
			bind_data->version = IntegerValue::Get(kv.second);
		} else if (loption == "threads") {
			// TODO: Verfiy > 0
			bind_data->number_threads = IntegerValue::Get(kv.second);
		} else if (loption == "flag") {
			bind_data->flag = IntegerValue::Get(kv.second);
		} else if (loption == "skip_rows") {
			// TODO: Verfiy > 0
			// Default: 0
			bind_data->skip_rows = IntegerValue::Get(kv.second);
		} else if (loption == "sheet_name" || loption == "sheet_index" || loption == "has_header") {
			continue;
		} else {
			throw BinderException("Unknown named parameter");
		}
	}

	// Doesn't change the parsing (only when combined with specifyTypes) -- we simply store it, to read it later while
	// copying
	bind_data->xlsx_sheet->mHeaders = use_header;

	// If number threads > 1, we set parallel true
	if (bind_data->number_threads > 1) {
		bind_data->xlsx_file.mParallelStrings = true;
	} else {
		bind_data->xlsx_file.mParallelStrings = false;
	}

	auto start = std::chrono::system_clock::now();

	bind_data->xlsx_file.parseSharedStrings();

	auto &fsheet = bind_data->xlsx_sheet;

	// TODO: Check in Clion what happens when mHeaders is set true

	bool success = fsheet->interleaved(bind_data->skip_rows, 0, bind_data->number_threads);
	if (!success) {
		throw BinderException("Failed to read sheet");
	}

	bind_data->xlsx_file.finalize();

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	if (bind_data->flag == 1) {
		std::cout << "Parsing time time: " << elapsed_seconds.count() << "s" << std::endl;
	}

	auto number_columns = fsheet->mDimension.first;
	auto number_rows = fsheet->mDimension.second;

	if (number_columns == 0 || number_rows == 0) {
		throw BinderException("Sheet appears to be empty");
	}

	vector<CellType> colTypesByIndex_first_row;
	vector<CellType> colTypesByIndex_second_row;
	// Might be used if header is present
	vector<XlsxCell> cells_first_row;

	// Get types of columns
	// TODO: This can fail if
	// 1. The first row is empty
	// 2. The first fow contains column names
	// 3. number_columns < threads
	auto first_buffer = &fsheet->mCells[0].front();

	// Probing the first two rows to get the types
	if (first_buffer->size() < number_columns * 2) {
		throw BinderException("Internal SheetReader extension error: Need minimum of two rows in first buffer to determine column types");
	}

	for (idx_t i = 0; i < number_columns; i++) {
		colTypesByIndex_first_row.push_back(fsheet->mCells[0].front()[i].type);
		cells_first_row.push_back(fsheet->mCells[0].front()[i]);
	}

	for (idx_t i = number_columns; i < number_columns * 2; i++) {
		colTypesByIndex_second_row.push_back(fsheet->mCells[0].front()[i].type);
	}

	// Convert CellType to LogicalType
	vector<LogicalType> column_types_first_row;
	vector<string> column_names_first_row;
	idx_t column_index = 0;

	bool first_row_all_string =
	    ConvertCellTypes(column_types_first_row, column_names_first_row, colTypesByIndex_first_row);

	if (use_header && !first_row_all_string) {
		throw BinderException("First row must contain only strings when has_header is set to true");
	}

	vector<LogicalType> column_types_second_row;
	vector<string> column_names_second_row;
	bool has_header = false;

	if (number_rows > 1) {
		bool second_row_all_string =
		    ConvertCellTypes(column_types_second_row, column_names_second_row, colTypesByIndex_second_row);

		if (use_header || (first_row_all_string && !second_row_all_string)) {
			has_header = true;

			return_types = column_types_second_row;
			bind_data->types = column_types_second_row;
			bind_data->SR_types = colTypesByIndex_second_row;

			vector<string> header_names;

			// Get header names from cell values of first row
			for (idx_t j = 0; j < cells_first_row.size(); j++) {
				switch (cells_first_row[j].type) {
				case CellType::T_STRING_REF: {
					auto value = bind_data->xlsx_file.getString(cells_first_row[j].data.integer);
					header_names.push_back(value);
					break;
				}
				case CellType::T_STRING:
				case CellType::T_STRING_INLINE: {
					// TODO
					throw BinderException("Inline & dynamic String types not supported yet");
					break;
				}
				default:
					throw BinderException("Header row contains non-string values");
				}
			}
			names = header_names;
			bind_data->names = header_names;
		} else {
			return_types = column_types_first_row;
			bind_data->types = column_types_first_row;
			bind_data->SR_types = colTypesByIndex_first_row;

			names = column_names_first_row;
			bind_data->names = column_names_first_row;
		}
	}

	if (has_header) {
		bind_data->skip_rows++;
		// TODO: Check whether this is correct
		bind_data->xlsx_sheet->mSkipRows++;
	}

	// First row is discarded
	// TODO: Remove when not using nextRow() anymore
	for (idx_t i = 0; i < bind_data->skip_rows; i++) {
		fsheet->nextRow();
	}

	return std::move(bind_data);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun,
	                                         SheetreaderBindFun, SRGlobalTableFunctionState::Init,
	                                         SRLocalTableFunctionState::Init);

	sheetreader_table_function.named_parameters["sheet_name"] = LogicalType::VARCHAR;
	sheetreader_table_function.named_parameters["sheet_index"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["version"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["threads"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["flag"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["skip_rows"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["has_header"] = LogicalType::BOOLEAN;

	ExtensionUtil::RegisterFunction(instance, sheetreader_table_function);
}

void SheetreaderExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string SheetreaderExtension::Name() {
	return "sheetreader";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void sheetreader_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::SheetreaderExtension>();
}

DUCKDB_EXTENSION_API const char *sheetreader_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
