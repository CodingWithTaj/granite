// The SQL engine: values, the catalog, parsing, planning and execution.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "btree.h"
#include "storage.h"

namespace granite {

struct Value {
    enum class Kind : uint8_t { Null, Int, Real, Text };
    Kind kind = Kind::Null;
    int64_t i = 0;
    double r = 0;
    std::string s;
    static Value integer(int64_t v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
    static Value real(double v) { Value x; x.kind = Kind::Real; x.r = v; return x; }
    static Value text(std::string v) { Value x; x.kind = Kind::Text; x.s = std::move(v); return x; }
    bool isNull() const { return kind == Kind::Null; }
    bool numeric() const { return kind == Kind::Int || kind == Kind::Real; }
    double num() const { return kind == Kind::Int ? double(i) : r; }
    std::string show() const;
};

struct Column {
    std::string name;
    bool integer = false;
    bool primaryKey = false;
};

struct TableDef {
    uint32_t id = 0;
    std::string name;
    std::vector<Column> cols;
    uint32_t root = 0;
    int64_t nextRowid = 1;
    int pk() const;
};

struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;
    std::vector<std::string> plan;
    std::string error;
    int errorPos = -1;
    int pagesRead = 0;
    bool isQuery = false;
};

class Database {
public:
    Database(File& db, File& wal, const Bugs& bugs = {}, size_t cachePages = 64);
    RecoveryReport open();
    /// Run one or more `;`-separated statements.
    std::vector<Result> execute(const std::string& sql);
    std::vector<TableDef> tables();
    std::optional<TableDef> table(const std::string& name);
    std::string treeJson(const std::string& table);
    /// Check every table's B+ tree. Empty string = sound.
    std::string check();
    void checkpoint() { pager_.checkpoint(); }
    bool inTransaction() const { return explicitTxn_; }
    Pager& pager() { return pager_; }
    /// Checkpoint automatically once the log holds this many frames.
    uint32_t autoCheckpointFrames = 512;

private:
    friend struct Executor;
    void saveTable(const TableDef& t);
    Bugs bugs_;
    Pager pager_;
    bool explicitTxn_ = false;
};

/// Text table for terminals.
std::string formatResult(const Result& r);
/// JSON for the browser.
std::string resultJson(const Result& r);
std::string jsonString(const std::string& s);

}  // namespace granite
