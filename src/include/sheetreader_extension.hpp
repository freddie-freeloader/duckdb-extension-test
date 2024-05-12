#pragma once

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/function/function.hpp"

#include "sheetreader/XlsxFile.h"

namespace duckdb {

class SheetreaderExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
};

//! We call this ScanData analog to the JSONScanData -- BindData would be a better name
// TODO: Or should this renamed to SRReadData as in ReadCSVData?
struct SRScanData : public TableFunctionData {
public:
	SRScanData();

	// void Bind(ClientContext &context, TableFunctionBindInput &input);

	// void InitializeReaders(ClientContext &context);
	// void InitializeFormats();
	// void InitializeFormats(bool auto_detect);
	// void SetCompression(const string &compression);

	// static unique_ptr<SheetreaderScanData> Deserialize(Deserializer &deserializer);

public:
	//! The paths of the files we're reading
	vector<string> file_names;

	//! Name of the sheet to read
	string sheet_name;

	//! For testing purposes
	idx_t iterations;
	
	//! All column names (in order)
	vector<string> names;

	XlsxFile xlsx_file;

private:
	SRScanData(ClientContext &context, vector<string> file_names, string sheet_name);

};
struct SRScanGlobalState {
public:
	SRScanGlobalState(ClientContext &context, const SRScanData &bind_data);

public:
	//! Bound data
	const SRScanData &bind_data;

	//! Number of reads so far
	idx_t chunk_count;
};

struct SRScanLocalState {
public:
	SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate);

public:
	//! Get next batch of data and return number of rows gathered
  idx_t ReadNextBatch(SRScanGlobalState &gstate);
	void GetNextBatchFromSR(SRScanGlobalState &gstate);

public:
	//! Current scan data
	idx_t scan_count;

private:
	const SRScanData &bind_data;
};

struct SRGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input);
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input);

public:
	SRScanGlobalState state;
};

struct SRLocalTableFunctionState : public LocalTableFunctionState {
public:
	SRLocalTableFunctionState(ClientContext &context, SRScanGlobalState &gstate);
	static unique_ptr<LocalTableFunctionState> Init(ExecutionContext &context, TableFunctionInitInput &input,
	                                                GlobalTableFunctionState *global_state);

public:
	SRScanLocalState state;
};
} // namespace duckdb
