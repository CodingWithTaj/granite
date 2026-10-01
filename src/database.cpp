#include "database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace granite {

namespace {
constexpr uint32_t CATALOG_ROOT = 1;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ------------------------------------------------------------------ row encoding
// [tag][payload] per column: 0 = null, 1 = int64, 2 = text (u16 length + bytes)

void encodeRow(const std::vector<Value>& row, Bytes& out) {
    out.clear();
    for (const Value& v : row) {
        if (v.kind == Value::Kind::Int) {
            out.push_back(1);
            for (int b = 0; b < 8; b++) out.push_back(uint8_t(uint64_t(v.i) >> (8 * b)));
        } else if (v.kind == Value::Kind::Text) {
            out.push_back(2);
            size_t n = std::min<size_t>(v.s.size(), 65535);
            out.push_back(uint8_t(n));
            out.push_back(uint8_t(n >> 8));
            out.insert(out.end(), v.s.begin(), v.s.begin() + long(n));
        } else {
            out.push_back(0);
        }
    }
}

bool decodeRow(const Bytes& in, size_t columns, std::vector<Value>& row) {
    row.assign(columns, Value{});
    size_t off = 0;
    for (size_t c = 0; c < columns && off < in.size(); c++) {
        uint8_t tag = in[off++];
        if (tag == 1) {
            if (off + 8 > in.size()) return false;
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v |= uint64_t(in[off + size_t(b)]) << (8 * b);
            row[c] = Value::integer(int64_t(v));
            off += 8;
        } else if (tag == 2) {
            if (off + 2 > in.size()) return false;
            size_t n = in[off] | (size_t(in[off + 1]) << 8);
            off += 2;
            if (off + n > in.size()) return false;
            row[c] = Value::text(std::string(in.begin() + long(off), in.begin() + long(off + n)));
            off += n;
        } else if (tag != 0) {
            return false;
        }
    }
    return true;
}

// catalog entry: name, root, next rowid, then columns
void encodeTable(const TableDef& t, Bytes& out) {
    std::vector<Value> v{Value::text(t.name), Value::integer(t.root), Value::integer(t.nextRowid),
                         Value::integer(int64_t(t.cols.size()))};
    for (auto& c : t.cols) {
        v.push_back(Value::text(c.name));
        v.push_back(Value::integer((c.integer ? 1 : 0) | (c.primaryKey ? 2 : 0)));
    }
    encodeRow(v, out);
}

bool decodeTable(int64_t id, const Bytes& in, TableDef& t) {
    std::vector<Value> head;
    if (!decodeRow(in, 4, head) || head[3].kind != Value::Kind::Int) return false;
    size_t n = size_t(head[3].i);
    std::vector<Value> all;
    if (n > 200 || !decodeRow(in, 4 + 2 * n, all)) return false;
    t.id = uint32_t(id);
    t.name = all[0].s;
    t.root = uint32_t(all[1].i);
    t.nextRowid = all[2].i;
    t.cols.clear();
    for (size_t c = 0; c < n; c++) {
        Column col;
        col.name = all[4 + 2 * c].s;
        col.integer = all[5 + 2 * c].i & 1;
        col.primaryKey = all[5 + 2 * c].i & 2;
        t.cols.push_back(col);
    }
    return true;
}

// ------------------------------------------------------------------ comparing values

/// SQL-style ordering: NULL < numbers < text.
int compare(const Value& a, const Value& b) {
    auto rank = [](const Value& v) { return v.isNull() ? 0 : v.numeric() ? 1 : 2; };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (ra == 1) {
        if (a.kind == Value::Kind::Int && b.kind == Value::Kind::Int) return a.i < b.i ? -1 : a.i > b.i;
        return a.num() < b.num() ? -1 : a.num() > b.num();
    }
    if (ra == 2) return a.s < b.s ? -1 : a.s > b.s;
    return 0;
}

bool truthy(const Value& v) {
    if (v.kind == Value::Kind::Int) return v.i != 0;
    if (v.kind == Value::Kind::Real) return v.r != 0;
    return false;
}

// ------------------------------------------------------------------ tokens

enum class T { Ident, Int, Str, Sym, End };
struct Tok {
    T t;
    std::string v;  // identifiers lower-cased; symbols as written
    int64_t n = 0;
    int pos = 0;
};

bool lex(const std::string& s, std::vector<Tok>& out, std::string& err, int& errPos) {
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') { while (i < s.size() && s[i] != '\n') i++; continue; }
        int pos = int(i);
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) j++;
            out.push_back({T::Ident, lower(s.substr(i, j - i)), 0, pos});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
            if (j - i > 18) { err = "number too large"; errPos = pos; return false; }
            out.push_back({T::Int, s.substr(i, j - i), std::stoll(s.substr(i, j - i)), pos});
            i = j;
        } else if (c == '\'') {
            std::string v;
            size_t j = i + 1;
            for (;;) {
                if (j >= s.size()) { err = "unterminated string (missing closing ')"; errPos = pos; return false; }
                if (s[j] == '\'') {
                    if (j + 1 < s.size() && s[j + 1] == '\'') { v += '\''; j += 2; continue; }
                    break;
                }
                v += s[j++];
            }
            out.push_back({T::Str, v, 0, pos});
            i = j + 1;
        } else {
            static const char* two[] = {"<=", ">=", "!=", "<>"};
            std::string sym(1, c);
            for (auto t : two) if (s.compare(i, 2, t) == 0) sym = t;
            if (sym.size() == 1 && std::string("(),;*=<>+-/.%").find(c) == std::string::npos) {
                err = std::string("unexpected character '") + c + "'";
                errPos = pos;
                return false;
            }
            out.push_back({T::Sym, sym == "<>" ? "!=" : sym, 0, pos});
            i += sym.size();
        }
    }
    out.push_back({T::End, "", 0, int(s.size())});
    return true;
}

// ------------------------------------------------------------------ syntax tree

struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
    enum K { Lit, Col, Bin, Not, Neg, IsNull, Func } k = Lit;
    Value lit;
    std::string table, name, op, text;
    ExprP a, b;
    bool negate = false;  // IS NOT NULL
    bool star = false;    // COUNT(*)
};

struct SelectItem { ExprP e; std::string alias; bool star = false; };
struct TableRef { std::string name, alias; };

struct Stmt {
    enum K { Create, Drop, Insert, Select, Update, Delete, Begin, Commit, Rollback } k = Select;
    bool explain = false;
    std::string table;
    std::vector<Column> cols;
    std::vector<std::string> insertCols;
    std::vector<std::vector<ExprP>> values;
    std::vector<SelectItem> items;
    TableRef from;
    bool noFrom = false;  // SELECT 1 + 1
    std::optional<TableRef> join;
    ExprP on, where, having;
    std::vector<ExprP> groupBy;
    std::vector<std::pair<ExprP, bool>> orderBy;  // bool = descending
    int64_t limit = -1;
    std::vector<std::pair<std::string, ExprP>> sets;
};

bool isAggregate(const std::string& f) { return f == "count" || f == "sum" || f == "min" || f == "max" || f == "avg"; }

// ------------------------------------------------------------------ parser

class Parser {
public:
    Parser(const std::vector<Tok>& toks, const std::string& src) : t_(toks), src_(src) {}
    std::string err;
    int errPos = -1;
    bool atEnd() { skipSemis(); return peek().t == T::End; }
    bool statement(Stmt& s);

private:
    const Tok& peek(int k = 0) const { return t_[std::min(i_ + size_t(k), t_.size() - 1)]; }
    bool is(const char* kw, int k = 0) const { return peek(k).t == T::Ident && peek(k).v == kw; }
    bool sym(const char* s, int k = 0) const { return peek(k).t == T::Sym && peek(k).v == s; }
    bool eat(const char* kw) { if (is(kw)) { i_++; return true; } return false; }
    bool eatSym(const char* s) { if (sym(s)) { i_++; return true; } return false; }
    void skipSemis() { while (sym(";")) i_++; }
    bool fail(const std::string& m) {
        if (err.empty()) {
            const Tok& tk = peek();
            err = m + (tk.t == T::End ? ", but the statement ended" : ", found '" + src_.substr(size_t(tk.pos), tk.v.empty() ? 1 : std::min<size_t>(tk.v.size() + (tk.t == T::Str ? 2 : 0), 20)) + "'");
            errPos = tk.pos;
        }
        return false;
    }
    bool expectKw(const char* kw) { return eat(kw) || fail(std::string("expected ") + kw); }
    bool expectSym(const char* s) { return eatSym(s) || fail(std::string("expected '") + s + "'"); }
    bool ident(std::string& out, const char* what) {
        static const std::set<std::string> reserved = {"select", "from", "where", "insert", "into", "values", "update", "set",
            "delete", "create", "table", "drop", "join", "on", "group", "order", "by", "limit", "and", "or", "not", "null",
            "is", "as", "begin", "commit", "rollback", "inner", "asc", "desc", "primary", "key", "explain", "having"};
        if (peek().t != T::Ident || reserved.count(peek().v)) return fail(std::string("expected ") + what);
        out = peek().v;
        i_++;
        return true;
    }
    ExprP expr() { return orExpr(); }
    ExprP binary(ExprP a, const std::string& op, ExprP b) {
        if (!a || !b) return nullptr;
        auto e = std::make_shared<Expr>();
        e->k = Expr::Bin; e->op = op; e->a = a; e->b = b;
        return e;
    }
    ExprP orExpr() { auto a = andExpr(); while (a && eat("or")) a = binary(a, "or", andExpr()); return a; }
    ExprP andExpr() { auto a = notExpr(); while (a && eat("and")) a = binary(a, "and", notExpr()); return a; }
    ExprP notExpr() {
        if (eat("not")) {
            auto a = notExpr();
            if (!a) return nullptr;
            auto e = std::make_shared<Expr>(); e->k = Expr::Not; e->a = a; return e;
        }
        return cmpExpr();
    }
    ExprP cmpExpr() {
        auto a = addExpr();
        if (!a) return nullptr;
        if (is("is")) {
            i_++;
            bool neg = eat("not");
            if (!expectKw("null")) return nullptr;
            auto e = std::make_shared<Expr>(); e->k = Expr::IsNull; e->a = a; e->negate = neg; return e;
        }
        for (const char* op : {"=", "!=", "<", "<=", ">", ">="}) {
            if (eatSym(op)) return binary(a, op, addExpr());
        }
        return a;
    }
    ExprP addExpr() {
        auto a = mulExpr();
        while (a && (sym("+") || sym("-"))) { std::string op = peek().v; i_++; a = binary(a, op, mulExpr()); }
        return a;
    }
    ExprP mulExpr() {
        auto a = unary();
        while (a && (sym("*") || sym("/") || sym("%"))) { std::string op = peek().v; i_++; a = binary(a, op, unary()); }
        return a;
    }
    ExprP unary() {
        if (eatSym("-")) {
            auto a = unary();
            if (!a) return nullptr;
            auto e = std::make_shared<Expr>(); e->k = Expr::Neg; e->a = a; return e;
        }
        return primary();
    }
    ExprP primary() {
        auto e = std::make_shared<Expr>();
        int start = peek().pos;
        const Tok& tk = peek();
        if (tk.t == T::Int) { e->lit = Value::integer(tk.n); i_++; }
        else if (tk.t == T::Str) { e->lit = Value::text(tk.v); i_++; }
        else if (is("null")) { i_++; }
        else if (eatSym("(")) {
            auto inner = expr();
            if (!inner || !expectSym(")")) return nullptr;
            return inner;
        } else if (tk.t == T::Ident && sym("(", 1)) {
            e->k = Expr::Func;
            e->name = tk.v;
            if (!isAggregate(e->name)) { fail("unknown function " + tk.v + "()"); return nullptr; }
            i_ += 2;
            if (eatSym("*")) {
                if (e->name != "count") { fail("only COUNT accepts *"); return nullptr; }
                e->star = true;
            } else {
                e->a = expr();
                if (!e->a) return nullptr;
            }
            if (!expectSym(")")) return nullptr;
        } else if (tk.t == T::Ident) {
            e->k = Expr::Col;
            std::string first;
            if (!ident(first, "a column name")) return nullptr;
            if (eatSym(".")) {
                e->table = first;
                if (!ident(e->name, "a column name after '.'")) return nullptr;
            } else e->name = first;
        } else {
            fail("expected a value or column");
            return nullptr;
        }
        e->text = src_.substr(size_t(start), size_t(peek().pos - start));
        while (!e->text.empty() && std::isspace(static_cast<unsigned char>(e->text.back()))) e->text.pop_back();
        return e;
    }
    bool tableRef(TableRef& r) {
        if (!ident(r.name, "a table name")) return false;
        eat("as");
        if (peek().t == T::Ident && !is("join") && !is("inner") && !is("where") && !is("on") && !is("group") && !is("order") && !is("limit") && !is("having"))
            return ident(r.alias, "an alias");
        return true;
    }
    bool select(Stmt& s);
    const std::vector<Tok>& t_;
    const std::string& src_;
    size_t i_ = 0;
};

bool Parser::select(Stmt& s) {
    s.k = Stmt::Select;
    do {
        SelectItem it;
        if (eatSym("*")) it.star = true;
        else {
            it.e = expr();
            if (!it.e) return false;
            if (eat("as")) { if (!ident(it.alias, "a column alias")) return false; }
        }
        s.items.push_back(it);
    } while (eatSym(","));
    if (!eat("from")) {
        // a SELECT without a table evaluates its expressions once
        s.noFrom = true;
        for (auto& it : s.items) if (it.star) return fail("SELECT * needs a FROM clause");
        if (!sym(";") && peek().t != T::End) return fail("expected FROM");
        return true;
    }
    if (!tableRef(s.from)) return false;
    if (is("join") || (is("inner") && is("join", 1))) {
        eat("inner");
        i_++;
        TableRef j;
        if (!tableRef(j) || !expectKw("on")) return false;
        s.join = j;
        if (!(s.on = expr())) return false;
    }
    if (eat("where") && !(s.where = expr())) return false;
    if (eat("group")) {
        if (!expectKw("by")) return false;
        do { auto g = expr(); if (!g) return false; s.groupBy.push_back(g); } while (eatSym(","));
    }
    if (eat("having") && !(s.having = expr())) return false;
    if (eat("order")) {
        if (!expectKw("by")) return false;
        do {
            auto o = expr();
            if (!o) return false;
            bool desc = eat("desc");
            if (!desc) eat("asc");
            s.orderBy.push_back({o, desc});
        } while (eatSym(","));
    }
    if (eat("limit")) {
        if (peek().t != T::Int) return fail("expected a number after LIMIT");
        s.limit = peek().n;
        i_++;
    }
    return true;
}

bool Parser::statement(Stmt& s) {
    skipSemis();
    if (eat("explain")) {
        s.explain = true;
        if (!is("select")) return fail("EXPLAIN works with SELECT");
    }
    if (eat("select")) { if (!select(s)) return false; }
    else if (eat("create")) {
        s.k = Stmt::Create;
        if (!expectKw("table") || !ident(s.table, "a table name") || !expectSym("(")) return false;
        do {
            Column c;
            if (!ident(c.name, "a column name")) return false;
            if (peek().t != T::Ident) return fail("expected a column type (INTEGER or TEXT)");
            std::string type = peek().v;
            i_++;
            if (type == "integer" || type == "int") c.integer = true;
            else if (type == "text" || type == "varchar" || type == "string") {
                if (eatSym("(")) { if (peek().t == T::Int) i_++; if (!expectSym(")")) return false; }
            } else { i_--; return fail("unknown type (use INTEGER or TEXT)"); }
            if (eat("primary")) {
                if (!expectKw("key")) return false;
                if (!c.integer) return fail("only an INTEGER column can be the PRIMARY KEY");
                c.primaryKey = true;
            }
            for (auto& o : s.cols) if (o.name == c.name) return fail("duplicate column " + c.name);
            s.cols.push_back(c);
        } while (eatSym(","));
        int pks = 0;
        for (auto& c : s.cols) pks += c.primaryKey;
        if (pks > 1) return fail("a table can have only one PRIMARY KEY");
        if (!expectSym(")")) return false;
    } else if (eat("drop")) {
        s.k = Stmt::Drop;
        if (!expectKw("table") || !ident(s.table, "a table name")) return false;
    } else if (eat("insert")) {
        s.k = Stmt::Insert;
        if (!expectKw("into") || !ident(s.table, "a table name")) return false;
        if (eatSym("(")) {
            do { std::string c; if (!ident(c, "a column name")) return false; s.insertCols.push_back(c); } while (eatSym(","));
            if (!expectSym(")")) return false;
        }
        if (!expectKw("values")) return false;
        do {
            if (!expectSym("(")) return false;
            std::vector<ExprP> row;
            do { auto e = expr(); if (!e) return false; row.push_back(e); } while (eatSym(","));
            if (!expectSym(")")) return false;
            s.values.push_back(row);
        } while (eatSym(","));
    } else if (eat("update")) {
        s.k = Stmt::Update;
        if (!ident(s.table, "a table name") || !expectKw("set")) return false;
        do {
            std::string c;
            if (!ident(c, "a column name") || !expectSym("=")) return false;
            auto e = expr();
            if (!e) return false;
            s.sets.push_back({c, e});
        } while (eatSym(","));
        if (eat("where") && !(s.where = expr())) return false;
    } else if (eat("delete")) {
        s.k = Stmt::Delete;
        if (!expectKw("from") || !ident(s.table, "a table name")) return false;
        if (eat("where") && !(s.where = expr())) return false;
    } else if (eat("begin")) { s.k = Stmt::Begin; eat("transaction"); }
    else if (eat("commit")) { s.k = Stmt::Commit; }
    else if (eat("rollback")) { s.k = Stmt::Rollback; }
    else return fail("expected a statement (SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, DROP TABLE, BEGIN, COMMIT or ROLLBACK)");
    if (!sym(";") && peek().t != T::End) return fail("expected ';' or the end of the statement");
    return true;
}

}  // namespace

// ------------------------------------------------------------------ values

std::string Value::show() const {
    switch (kind) {
        case Kind::Null: return "NULL";
        case Kind::Int: return std::to_string(i);
        case Kind::Text: return s;
        case Kind::Real: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", r);
            return buf;
        }
    }
    return "";
}

int TableDef::pk() const {
    for (size_t c = 0; c < cols.size(); c++) if (cols[c].primaryKey) return int(c);
    return -1;
}

// ------------------------------------------------------------------ executor

/// Column names visible to an expression, and where each lives in a row.
struct Scope {
    std::vector<std::string> tables, names;
    int find(const Expr& e, std::string& err) const {
        int hit = -1;
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] != e.name || (!e.table.empty() && tables[i] != e.table)) continue;
            if (hit >= 0) { err = "column '" + e.name + "' is ambiguous: write table.column"; return -1; }
            hit = int(i);
        }
        if (hit < 0) err = "no such column: " + (e.table.empty() ? "" : e.table + ".") + e.name;
        return hit;
    }
};

struct Executor {
    Database& db;
    std::string err;
    int pagesRead = 0;

    Value eval(const Expr& e, const Scope& sc, const std::vector<Value>& row) {
        switch (e.k) {
            case Expr::Lit: return e.lit;
            case Expr::Col: { int i = sc.find(e, err); return i < 0 ? Value{} : row[size_t(i)]; }
            case Expr::Not: { Value a = eval(*e.a, sc, row); return a.isNull() ? a : Value::integer(!truthy(a)); }
            case Expr::Neg: {
                Value a = eval(*e.a, sc, row);
                if (a.kind == Value::Kind::Int) return Value::integer(-a.i);
                if (a.kind == Value::Kind::Real) return Value::real(-a.r);
                return Value{};
            }
            case Expr::IsNull: { Value a = eval(*e.a, sc, row); return Value::integer(a.isNull() != e.negate); }
            case Expr::Func: err = "aggregate " + e.name + "() can only be used in SELECT"; return Value{};
            case Expr::Bin: return binary(e.op, eval(*e.a, sc, row), eval(*e.b, sc, row));
        }
        return Value{};
    }

    Value binary(const std::string& op, const Value& a, const Value& b) {
        if (op == "and") {
            if ((!a.isNull() && !truthy(a)) || (!b.isNull() && !truthy(b))) return Value::integer(0);
            if (a.isNull() || b.isNull()) return Value{};
            return Value::integer(1);
        }
        if (op == "or") {
            if (truthy(a) || truthy(b)) return Value::integer(1);
            if (a.isNull() || b.isNull()) return Value{};
            return Value::integer(0);
        }
        if (a.isNull() || b.isNull()) return Value{};
        if (op == "=" ) return Value::integer(compare(a, b) == 0);
        if (op == "!=") return Value::integer(compare(a, b) != 0);
        if (op == "<" ) return Value::integer(compare(a, b) < 0);
        if (op == "<=") return Value::integer(compare(a, b) <= 0);
        if (op == ">" ) return Value::integer(compare(a, b) > 0);
        if (op == ">=") return Value::integer(compare(a, b) >= 0);
        if (op == "+" && a.kind == Value::Kind::Text && b.kind == Value::Kind::Text) return Value::text(a.s + b.s);
        if (!a.numeric() || !b.numeric()) { err = "can't do arithmetic on text"; return Value{}; }
        if (a.kind == Value::Kind::Int && b.kind == Value::Kind::Int) {
            if (op == "+") return Value::integer(a.i + b.i);
            if (op == "-") return Value::integer(a.i - b.i);
            if (op == "*") return Value::integer(a.i * b.i);
            if (b.i == 0) return Value{};  // division by zero is NULL, as in SQLite
            if (op == "/") return Value::integer(a.i / b.i);
            if (op == "%") return Value::integer(a.i % b.i);
        }
        double x = a.num(), y = b.num();
        if (op == "+") return Value::real(x + y);
        if (op == "-") return Value::real(x - y);
        if (op == "*") return Value::real(x * y);
        if (y == 0) return Value{};
        if (op == "/") return Value::real(x / y);
        return Value::real(std::fmod(x, y));
    }

    /// Check every column reference resolves, before touching any rows, so an
    /// error is reported even when a table is empty. `outputs` names columns
    /// of the result, which ORDER BY may also refer to.
    bool validate(const Expr* e, const Scope& sc, const std::vector<std::string>* outputs = nullptr) {
        if (!e || !err.empty()) return err.empty();
        if (e->k == Expr::Col) {
            if (outputs && e->table.empty())
                for (auto& o : *outputs) if (o == e->name) return true;
            sc.find(*e, err);
            return err.empty();
        }
        return validate(e->a.get(), sc, outputs) && validate(e->b.get(), sc, outputs);
    }

    bool hasAggregate(const Expr* e) {
        if (!e) return false;
        if (e->k == Expr::Func) return true;
        return hasAggregate(e->a.get()) || hasAggregate(e->b.get());
    }

    Value aggregate(const Expr& e, const Scope& sc, const std::vector<std::vector<Value>>& rows) {
        if (e.k == Expr::Func) {
            if (e.star) return Value::integer(int64_t(rows.size()));
            int64_t count = 0;
            Value best;
            double sum = 0;
            bool allInt = true;
            int64_t isum = 0;
            for (auto& r : rows) {
                Value v = eval(*e.a, sc, r);
                if (v.isNull()) continue;
                count++;
                if (e.name == "min" && (best.isNull() || compare(v, best) < 0)) best = v;
                if (e.name == "max" && (best.isNull() || compare(v, best) > 0)) best = v;
                if (v.numeric()) { sum += v.num(); if (v.kind == Value::Kind::Int) isum += v.i; else allInt = false; }
            }
            if (e.name == "count") return Value::integer(count);
            if (e.name == "min" || e.name == "max") return best;
            if (count == 0) return Value{};
            if (e.name == "sum") return allInt ? Value::integer(isum) : Value::real(sum);
            return Value::real(sum / double(count));
        }
        if (e.k == Expr::Bin) return binary(e.op, aggregate(*e.a, sc, rows), aggregate(*e.b, sc, rows));
        if (e.k == Expr::Lit) return e.lit;
        return rows.empty() ? Value{} : eval(e, sc, rows.front());
    }

    // ---------------------------------------------------------- scanning with the index

    /// If WHERE pins down the primary key, use the B+ tree to visit only that range.
    struct Range { std::optional<int64_t> lo, hi; std::string text; };
    Range keyRange(const TableDef& t, const std::string& alias, const Expr* where) {
        Range r;
        int pk = t.pk();
        if (!where || pk < 0) return r;
        std::vector<const Expr*> conj;
        std::function<void(const Expr*)> split = [&](const Expr* e) {
            if (e->k == Expr::Bin && e->op == "and") { split(e->a.get()); split(e->b.get()); }
            else conj.push_back(e);
        };
        split(where);
        auto isPk = [&](const Expr* e) {
            return e->k == Expr::Col && e->name == t.cols[size_t(pk)].name && (e->table.empty() || e->table == alias);
        };
        for (const Expr* e : conj) {
            if (e->k != Expr::Bin) continue;
            const Expr *col = e->a.get(), *lit = e->b.get();
            std::string op = e->op;
            if (!isPk(col)) {
                std::swap(col, lit);
                if (op == "<") op = ">"; else if (op == ">") op = "<"; else if (op == "<=") op = ">="; else if (op == ">=") op = "<=";
            }
            if (!isPk(col) || lit->k != Expr::Lit || lit->lit.kind != Value::Kind::Int) continue;
            int64_t v = lit->lit.i;
            auto tighten = [&](std::optional<int64_t>& b, int64_t x, bool low) { b = !b ? x : low ? std::max(*b, x) : std::min(*b, x); };
            if (op == "=") { tighten(r.lo, v, true); tighten(r.hi, v, false); }
            else if (op == ">") tighten(r.lo, v + 1, true);
            else if (op == ">=") tighten(r.lo, v, true);
            else if (op == "<") tighten(r.hi, v - 1, false);
            else if (op == "<=") tighten(r.hi, v, false);
        }
        if (r.lo || r.hi) {
            std::string k = t.cols[size_t(pk)].name;
            if (r.lo && r.hi && *r.lo == *r.hi) r.text = k + " = " + std::to_string(*r.lo);
            else r.text = (r.lo ? std::to_string(*r.lo) + " <= " : "") + k + (r.hi ? " <= " + std::to_string(*r.hi) : "");
        }
        return r;
    }

    /// Visit rows of a table, optionally limited to a key range. Return false to stop.
    bool scan(const TableDef& t, const Range& r, const std::function<bool(int64_t, std::vector<Value>&)>& visit) {
        BTree tree(db.pager_, t.root);
        auto c = r.lo ? tree.seek(*r.lo) : tree.first();
        std::vector<Value> row;
        bool ok = true;
        for (; c.valid(); c.next()) {
            if (r.hi && c.key() > *r.hi) break;
            if (!decodeRow(c.value(), t.cols.size(), row)) { err = "corrupt row in " + t.name; ok = false; break; }
            if (!visit(c.key(), row)) break;
        }
        pagesRead += tree.pagesRead() + c.pagesLoaded() - 1;  // the first leaf was counted in the descent
        return ok && err.empty();
    }

    std::optional<TableDef> need(const std::string& name) {
        auto t = db.table(name);
        if (!t) err = "no such table: " + name;
        return t;
    }

    // ---------------------------------------------------------- statements

    void create(const Stmt& s, Result& res) {
        if (db.table(s.table)) { err = "table " + s.table + " already exists"; return; }
        TableDef t;
        t.name = s.table;
        t.cols = s.cols;
        uint32_t maxId = 0;
        for (auto& o : db.tables()) maxId = std::max(maxId, o.id);
        t.id = maxId + 1;
        t.root = db.pager_.allocate();
        BTree::create(db.pager_, t.root);
        db.saveTable(t);
        res.message = "Created table " + t.name + ".";
    }

    void drop(const Stmt& s, Result& res) {
        auto t = need(s.table);
        if (!t) return;
        BTree catalog(db.pager_, CATALOG_ROOT);
        catalog.remove(t->id);
        res.message = "Dropped table " + t->name + ".";
    }

    bool checkType(const TableDef& t, size_t c, Value& v) {
        if (v.isNull()) return true;
        if (t.cols[c].integer) {
            if (v.kind == Value::Kind::Int) return true;
            err = "column " + t.cols[c].name + " needs an INTEGER, got " + (v.kind == Value::Kind::Text ? "'" + v.s + "'" : v.show());
            return false;
        }
        if (v.kind != Value::Kind::Text) v = Value::text(v.show());
        return true;
    }

    bool writeRow(const TableDef& t, int64_t key, const std::vector<Value>& row) {
        Bytes bytes;
        encodeRow(row, bytes);
        if (bytes.size() > MAX_VALUE) { err = "row too large (the limit is " + std::to_string(MAX_VALUE) + " bytes)"; return false; }
        BTree tree(db.pager_, t.root);
        tree.upsert(key, bytes);
        return true;
    }

    void insert(const Stmt& s, Result& res) {
        auto t = need(s.table);
        if (!t) return;
        std::vector<int> map;
        if (s.insertCols.empty()) for (size_t c = 0; c < t->cols.size(); c++) map.push_back(int(c));
        else for (auto& name : s.insertCols) {
            int found = -1;
            for (size_t c = 0; c < t->cols.size(); c++) if (t->cols[c].name == name) found = int(c);
            if (found < 0) { err = "table " + t->name + " has no column " + name; return; }
            map.push_back(found);
        }
        int pk = t->pk();
        BTree tree(db.pager_, t->root);
        Scope none;
        int count = 0;
        for (auto& exprs : s.values) {
            if (exprs.size() != map.size()) { err = "expected " + std::to_string(map.size()) + " values, got " + std::to_string(exprs.size()); return; }
            std::vector<Value> row(t->cols.size());
            for (size_t k = 0; k < exprs.size(); k++) {
                row[size_t(map[k])] = eval(*exprs[k], none, {});
                if (!err.empty()) return;
            }
            for (size_t c = 0; c < row.size(); c++) if (!checkType(*t, c, row[c])) return;
            int64_t key;
            if (pk >= 0 && !row[size_t(pk)].isNull()) {
                key = row[size_t(pk)].i;
                Bytes existing;
                if (tree.find(key, existing)) { err = "duplicate primary key " + std::to_string(key) + " in " + t->name; return; }
            } else key = t->nextRowid;
            if (pk >= 0) row[size_t(pk)] = Value::integer(key);
            if (!writeRow(*t, key, row)) return;
            t->nextRowid = std::max(t->nextRowid, key + 1);
            count++;
        }
        db.saveTable(*t);
        res.message = "Inserted " + std::to_string(count) + " row" + (count == 1 ? "" : "s") + ".";
    }

    void modify(const Stmt& s, Result& res) {
        auto t = need(s.table);
        if (!t) return;
        Scope sc;
        for (auto& c : t->cols) { sc.tables.push_back(t->name); sc.names.push_back(c.name); }
        std::vector<std::pair<size_t, ExprP>> sets;
        for (auto& [name, e] : s.sets) {
            int found = -1;
            for (size_t c = 0; c < t->cols.size(); c++) if (t->cols[c].name == name) found = int(c);
            if (found < 0) { err = "table " + t->name + " has no column " + name; return; }
            if (found == t->pk()) { err = "changing the primary key isn't supported"; return; }
            sets.push_back({size_t(found), e});
        }
        if (!validate(s.where.get(), sc)) return;
        for (auto& [c, e] : sets) if (!validate(e.get(), sc)) return;
        std::vector<std::pair<int64_t, std::vector<Value>>> hits;
        Range r = keyRange(*t, t->name, s.where.get());
        scan(*t, r, [&](int64_t key, std::vector<Value>& row) {
            if (s.where && !truthy(eval(*s.where, sc, row))) return err.empty();
            hits.push_back({key, row});
            return err.empty();
        });
        if (!err.empty()) return;
        BTree tree(db.pager_, t->root);
        for (auto& [key, row] : hits) {
            if (s.k == Stmt::Delete) { tree.remove(key); continue; }
            std::vector<Value> updated = row;
            for (auto& [c, e] : sets) {
                updated[c] = eval(*e, sc, row);
                if (!err.empty() || !checkType(*t, c, updated[c])) return;
            }
            if (!writeRow(*t, key, updated)) return;
        }
        res.message = std::string(s.k == Stmt::Delete ? "Deleted " : "Updated ") + std::to_string(hits.size()) + " row" + (hits.size() == 1 ? "" : "s") + ".";
    }

    /// Evaluate a HAVING condition on one group: aggregates over its rows, and
    /// output-column aliases (SELECT COUNT(*) AS n ... HAVING n > 2) by value.
    Value havingValue(const Expr& e, const Scope& sc, const std::vector<std::vector<Value>>& rows,
                      const std::vector<std::string>& names, const std::vector<Value>& outVals) {
        if (e.k == Expr::Col && e.table.empty()) {
            for (size_t c = 0; c < names.size(); c++) if (names[c] == e.name) {
                std::string ignore;
                if (sc.find(e, ignore) < 0) return outVals[c];
            }
        }
        if (e.k == Expr::Not) { Value a = havingValue(*e.a, sc, rows, names, outVals); return a.isNull() ? a : Value::integer(!truthy(a)); }
        if (e.k == Expr::IsNull) return Value::integer(havingValue(*e.a, sc, rows, names, outVals).isNull() != e.negate);
        if (e.k == Expr::Neg) { Value a = havingValue(*e.a, sc, rows, names, outVals); return a.kind == Value::Kind::Int ? Value::integer(-a.i) : a.kind == Value::Kind::Real ? Value::real(-a.r) : Value{}; }
        if (e.k == Expr::Bin) return binary(e.op, havingValue(*e.a, sc, rows, names, outVals), havingValue(*e.b, sc, rows, names, outVals));
        return aggregate(e, sc, rows);
    }

    void select(const Stmt& s, Result& res) {
        if (s.noFrom) {
            Scope none;
            res.isQuery = true;
            std::vector<Value> row;
            for (auto& it : s.items) {
                if (hasAggregate(it.e.get())) { err = "aggregate functions need a FROM clause"; return; }
                row.push_back(eval(*it.e, none, {}));
                res.columns.push_back(!it.alias.empty() ? it.alias : it.e->text);
            }
            if (!err.empty()) return;
            res.rows.push_back(row);
            res.plan.push_back("CONSTANT (no table to read)");
            if (s.explain) { res.columns = {"plan"}; res.rows = {{Value::text(res.plan[0])}}; }
            return;
        }
        auto left = need(s.from.name);
        if (!left) return;
        std::string la = s.from.alias.empty() ? left->name : s.from.alias;
        Scope sc;
        for (auto& c : left->cols) { sc.tables.push_back(la); sc.names.push_back(c.name); }
        std::optional<TableDef> right;
        std::string ra;
        if (s.join) {
            right = need(s.join->name);
            if (!right) return;
            ra = s.join->alias.empty() ? right->name : s.join->alias;
            for (auto& c : right->cols) { sc.tables.push_back(ra); sc.names.push_back(c.name); }
        }

        {
            std::vector<std::string> outNames;
            for (auto& it : s.items) {
                if (!it.star && !validate(it.e.get(), sc)) return;
                if (!it.alias.empty()) outNames.push_back(it.alias);
            }
            if (!validate(s.on.get(), sc) || !validate(s.where.get(), sc) || !validate(s.having.get(), sc, &outNames)) return;
            for (auto& g : s.groupBy) if (!validate(g.get(), sc)) return;
            for (auto& [o, d] : s.orderBy) if (!validate(o.get(), sc, &outNames)) return;
        }
        // plan: narrow the outer table with its primary key when WHERE allows
        Range r = right ? Range{} : keyRange(*left, la, s.where.get());
        res.plan.push_back(r.text.empty() ? "SCAN " + left->name + " (reads every row)"
                                          : "SEARCH " + left->name + " USING PRIMARY KEY (" + r.text + ")");
        // for a join, look rows up by primary key if ON compares it with the outer table
        int rpk = right ? right->pk() : -1;
        int lookupFrom = -1;
        if (right && rpk >= 0 && s.on && s.on->k == Expr::Bin && s.on->op == "=") {
            for (int side = 0; side < 2 && lookupFrom < 0; side++) {
                const Expr* key = side ? s.on->b.get() : s.on->a.get();
                const Expr* other = side ? s.on->a.get() : s.on->b.get();
                if (key->k == Expr::Col && key->name == right->cols[size_t(rpk)].name && (key->table.empty() || key->table == ra) &&
                    other->k == Expr::Col && (other->table.empty() || other->table == la)) {
                    Scope ls;
                    for (auto& c : left->cols) { ls.tables.push_back(la); ls.names.push_back(c.name); }
                    std::string ignore;
                    lookupFrom = ls.find(*other, ignore);
                }
            }
        }
        if (right) res.plan.push_back(lookupFrom >= 0 ? "SEARCH " + right->name + " USING PRIMARY KEY for each row (index nested loop)"
                                                      : "SCAN " + right->name + " for each row (nested loop)");

        std::vector<std::vector<Value>> rows;
        scan(*left, r, [&](int64_t, std::vector<Value>& lrow) {
            if (!right) {
                if (!s.where || truthy(eval(*s.where, sc, lrow))) rows.push_back(lrow);
                return err.empty();
            }
            auto consider = [&](std::vector<Value>& rrow) {
                std::vector<Value> both = lrow;
                both.insert(both.end(), rrow.begin(), rrow.end());
                if (truthy(eval(*s.on, sc, both)) && (!s.where || truthy(eval(*s.where, sc, both)))) rows.push_back(both);
                return err.empty();
            };
            if (lookupFrom >= 0) {
                const Value& k = lrow[size_t(lookupFrom)];
                if (k.kind == Value::Kind::Int) scan(*right, Range{k.i, k.i, ""}, [&](int64_t, std::vector<Value>& rr) { return consider(rr); });
            } else {
                scan(*right, Range{}, [&](int64_t, std::vector<Value>& rr) { return consider(rr); });
            }
            return err.empty();
        });
        if (!err.empty()) return;

        // output columns
        std::vector<ExprP> outs;
        for (auto& it : s.items) {
            if (it.star) {
                for (size_t c = 0; c < sc.names.size(); c++) {
                    auto e = std::make_shared<Expr>();
                    e->k = Expr::Col; e->table = sc.tables[c]; e->name = sc.names[c];
                    outs.push_back(e);
                    res.columns.push_back(sc.names[c]);
                }
            } else {
                outs.push_back(it.e);
                res.columns.push_back(!it.alias.empty() ? it.alias : it.e->k == Expr::Col ? it.e->name : it.e->text);
            }
        }
        bool grouped = !s.groupBy.empty() || s.having;
        for (auto& o : outs) grouped = grouped || hasAggregate(o.get());

        // each output row keeps its source rows, so ORDER BY can refer to either
        struct Out { std::vector<Value> vals; std::vector<std::vector<Value>> src; };
        std::vector<Out> out;
        if (grouped) {
            std::map<std::string, std::vector<std::vector<Value>>> groups;
            std::vector<std::string> order;
            for (auto& row : rows) {
                std::string key;
                for (auto& g : s.groupBy) { Value v = eval(*g, sc, row); key += char(v.kind) + v.show() + '\x1f'; }
                if (!groups.count(key)) order.push_back(key);
                groups[key].push_back(row);
            }
            if (s.groupBy.empty() && order.empty()) { order.push_back(""); groups[""]; }
            for (auto& key : order) {
                Out o;
                o.src = groups[key];
                for (auto& e : outs) o.vals.push_back(aggregate(*e, sc, o.src));
                if (s.having && !truthy(havingValue(*s.having, sc, o.src, res.columns, o.vals))) continue;
                out.push_back(std::move(o));
            }
            if (s.having) res.plan.push_back("FILTER groups by HAVING");
        } else {
            for (auto& row : rows) {
                Out o;
                o.src = {row};
                for (auto& e : outs) o.vals.push_back(eval(*e, sc, row));
                out.push_back(std::move(o));
            }
        }
        if (!err.empty()) return;

        if (!s.orderBy.empty()) {
            // ORDER BY may name an output column (or alias); otherwise it's evaluated on the rows
            std::vector<std::vector<Value>> keys(out.size());
            for (size_t i = 0; i < out.size(); i++) {
                for (auto& [e, desc] : s.orderBy) {
                    int col = -1;
                    if (e->k == Expr::Lit && e->lit.kind == Value::Kind::Int) {
                        // ORDER BY 2 means the second output column
                        if (e->lit.i < 1 || size_t(e->lit.i) > res.columns.size()) {
                            err = "ORDER BY position " + std::to_string(e->lit.i) + " is out of range (there are " + std::to_string(res.columns.size()) + " columns)";
                            return;
                        }
                        col = int(e->lit.i - 1);
                    }
                    for (size_t c = 0; c < res.columns.size() && col < 0; c++) {
                        if ((e->k == Expr::Col && e->table.empty() && res.columns[c] == e->name) || res.columns[c] == e->text) col = int(c);
                    }
                    keys[i].push_back(col >= 0 ? out[i].vals[size_t(col)] : grouped ? aggregate(*e, sc, out[i].src) : eval(*e, sc, out[i].src.front()));
                }
            }
            std::vector<size_t> idx(out.size());
            for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                for (size_t k = 0; k < s.orderBy.size(); k++) {
                    int c = compare(keys[a][k], keys[b][k]);
                    if (c) return s.orderBy[k].second ? c > 0 : c < 0;
                }
                return false;
            });
            std::vector<Out> sorted;
            for (size_t i : idx) sorted.push_back(std::move(out[i]));
            out = std::move(sorted);
            res.plan.push_back("SORT the results");
        }
        if (s.limit >= 0 && size_t(s.limit) < out.size()) out.resize(size_t(s.limit));
        if (!err.empty()) return;
        for (auto& o : out) res.rows.push_back(std::move(o.vals));
        res.isQuery = true;
        if (s.explain) {
            res.columns = {"plan"};
            res.rows.clear();
            for (auto& p : res.plan) res.rows.push_back({Value::text(p)});
        }
    }
};

// ------------------------------------------------------------------ database

Database::Database(File& db, File& wal, const Bugs& bugs, size_t cachePages)
    : bugs_(bugs), pager_(db, wal, bugs_, cachePages) {}

RecoveryReport Database::open() {
    RecoveryReport r = pager_.open();
    explicitTxn_ = false;
    if (pager_.pageCount() == 0) {
        // a brand-new database: page 0 is the header, page 1 the catalog
        pager_.begin();
        uint32_t header = pager_.allocate(), catalog = pager_.allocate();
        Page pg{};
        std::memcpy(pg.data(), "GRANITE format 1", 16);
        pager_.put(header, pg);
        BTree::create(pager_, catalog);
        pager_.commit();
    }
    return r;
}

std::vector<TableDef> Database::tables() {
    std::vector<TableDef> out;
    BTree catalog(pager_, CATALOG_ROOT);
    for (auto c = catalog.first(); c.valid(); c.next()) {
        TableDef t;
        if (decodeTable(c.key(), c.value(), t)) out.push_back(t);
    }
    return out;
}

std::optional<TableDef> Database::table(const std::string& name) {
    for (auto& t : tables()) if (t.name == lower(name)) return t;
    return std::nullopt;
}

void Database::saveTable(const TableDef& t) {
    Bytes b;
    encodeTable(t, b);
    BTree catalog(pager_, CATALOG_ROOT);
    catalog.upsert(t.id, b);
}

std::string Database::treeJson(const std::string& name) {
    auto t = table(name);
    if (!t) return "null";
    BTree tree(pager_, t->root);
    return tree.json();
}

std::string Database::check() {
    BTree catalog(pager_, CATALOG_ROOT);
    std::string err = catalog.check();
    if (!err.empty()) return "catalog: " + err;
    for (auto& t : tables()) {
        BTree tree(pager_, t.root);
        err = tree.check();
        if (!err.empty()) return t.name + ": " + err;
    }
    return "";
}

std::vector<Result> Database::execute(const std::string& sql) {
    std::vector<Result> results;
    std::vector<Tok> toks;
    Result lexErr;
    if (!lex(sql, toks, lexErr.error, lexErr.errorPos)) { results.push_back(lexErr); return results; }
    Parser parser(toks, sql);
    while (!parser.atEnd()) {
        Stmt s;
        Result res;
        if (!parser.statement(s)) {
            res.error = parser.err;
            res.errorPos = parser.errPos;
            results.push_back(res);
            break;
        }
        if (s.k == Stmt::Begin) {
            if (explicitTxn_) res.error = "a transaction is already open: COMMIT or ROLLBACK it first";
            else { pager_.begin(); explicitTxn_ = true; res.message = "Transaction started. Changes stay private until COMMIT."; }
        } else if (s.k == Stmt::Commit) {
            if (!explicitTxn_) res.error = "no transaction to commit";
            else {
                size_t dirty = pager_.dirtyPages();
                pager_.commit();
                explicitTxn_ = false;
                res.message = "Committed: " + std::to_string(dirty) + " changed page" + (dirty == 1 ? "" : "s") + " written to the log and synced to disk.";
            }
        } else if (s.k == Stmt::Rollback) {
            if (!explicitTxn_) res.error = "no transaction to roll back";
            else { pager_.rollback(); explicitTxn_ = false; res.message = "Rolled back. Every change since BEGIN was discarded."; }
        } else {
            bool autocommit = !explicitTxn_;
            if (autocommit) pager_.begin();
            Executor ex{*this, "", 0};
            switch (s.k) {
                case Stmt::Create: ex.create(s, res); break;
                case Stmt::Drop: ex.drop(s, res); break;
                case Stmt::Insert: ex.insert(s, res); break;
                case Stmt::Update: case Stmt::Delete: ex.modify(s, res); break;
                default: ex.select(s, res); break;
            }
            res.pagesRead = ex.pagesRead;
            if (!ex.err.empty()) {
                res.error = ex.err;
                res.rows.clear();
                pager_.rollback();
                if (!autocommit) { explicitTxn_ = false; res.error += " (the transaction was rolled back)"; }
            } else if (autocommit) {
                pager_.commit();
            }
        }
        if (!explicitTxn_ && pager_.walFrames() >= autoCheckpointFrames) pager_.checkpoint();
        results.push_back(std::move(res));
        if (!results.back().error.empty()) break;
    }
    return results;
}

// ------------------------------------------------------------------ output

std::string formatResult(const Result& r) {
    if (!r.error.empty()) return "error: " + r.error + "\n";
    if (!r.isQuery) return r.message + "\n";
    std::vector<size_t> w(r.columns.size());
    for (size_t c = 0; c < w.size(); c++) w[c] = r.columns[c].size();
    for (auto& row : r.rows) for (size_t c = 0; c < w.size(); c++) w[c] = std::max(w[c], row[c].show().size());
    std::string out, line = "+";
    for (size_t x : w) line += std::string(x + 2, '-') + "+";
    out += line + "\n|";
    for (size_t c = 0; c < w.size(); c++) out += " " + r.columns[c] + std::string(w[c] - r.columns[c].size(), ' ') + " |";
    out += "\n" + line + "\n";
    for (auto& row : r.rows) {
        out += "|";
        for (size_t c = 0; c < w.size(); c++) {
            std::string v = row[c].show();
            bool num = row[c].numeric();
            out += " " + (num ? std::string(w[c] - v.size(), ' ') + v : v + std::string(w[c] - v.size(), ' ')) + " |";
        }
        out += "\n";
    }
    out += line + "\n" + std::to_string(r.rows.size()) + " row" + (r.rows.size() == 1 ? "" : "s") + "\n";
    return out;
}

std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

std::string resultJson(const Result& r) {
    std::string o = "{\"columns\":[";
    for (size_t c = 0; c < r.columns.size(); c++) o += (c ? "," : "") + jsonString(r.columns[c]);
    o += "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); i++) {
        o += i ? ",[" : "[";
        for (size_t c = 0; c < r.rows[i].size(); c++) {
            const Value& v = r.rows[i][c];
            o += c ? "," : "";
            o += v.isNull() ? "null" : v.numeric() ? v.show() : jsonString(v.s);
        }
        o += "]";
    }
    o += "],\"plan\":[";
    for (size_t i = 0; i < r.plan.size(); i++) o += (i ? "," : "") + jsonString(r.plan[i]);
    o += "],\"message\":" + jsonString(r.message) + ",\"error\":" + jsonString(r.error) +
         ",\"errorPos\":" + std::to_string(r.errorPos) + ",\"pagesRead\":" + std::to_string(r.pagesRead) +
         ",\"isQuery\":" + (r.isQuery ? "true" : "false") + "}";
    return o;
}

}  // namespace granite
