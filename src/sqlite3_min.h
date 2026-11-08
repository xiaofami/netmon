/*
 * sqlite3_min.h - SQLite3 最小接口声明
 *
 * 为什么不用官方 sqlite3.h：
 *   本机没有装 sqlite dev 包（apk 源里只有 sqlite3-cli），
 *   而 SQLite 的 C ABI 是出了名的稳定——从 3.0 到 3.53 这些函数签名没变过。
 *   所以手工声明所需的子集完全安全，链接 /usr/lib/libsqlite3.so.0 即可。
 *
 * 只声明用到的函数，避免把整个 6000 行的头文件搬进来。
 * 一旦 SQLite 主版本变到 4.x，这里需要复核。
 */

#ifndef SQLITE3_MIN_H
#define SQLITE3_MIN_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明类型 ---- */
typedef struct sqlite3        sqlite3;
typedef struct sqlite3_stmt   sqlite3_stmt;

/* ---- 返回码 ---- */
#define SQLITE_OK         0
#define SQLITE_ERROR      1
#define SQLITE_BUSY       5
#define SQLITE_LOCKED     6
#define SQLITE_NOMEM      7
#define SQLITE_READONLY   8
#define SQLITE_IOERR      10
#define SQLITE_CORRUPT    11
#define SQLITE_FULL       13
#define SQLITE_CANTOPEN   14
#define SQLITE_CONSTRAINT 19
#define SQLITE_MISUSE     21
#define SQLITE_ROW        100
#define SQLITE_DONE       101

/* ---- 打开标志 ---- */
#define SQLITE_OPEN_READONLY   0x00000001
#define SQLITE_OPEN_READWRITE  0x00000002
#define SQLITE_OPEN_CREATE     0x00000004
#define SQLITE_OPEN_URI        0x00000040
#define SQLITE_OPEN_NOMUTEX    0x00008000
#define SQLITE_OPEN_FULLMUTEX  0x00010000

/* ---- 数据类型 ---- */
#define SQLITE_INTEGER 1
#define SQLITE_FLOAT   2
#define SQLITE_TEXT    3
#define SQLITE_BLOB    4
#define SQLITE_NULL    5

/* ---- 特殊析构符 ---- */
#define SQLITE_STATIC    ((void (*)(void *))0)
#define SQLITE_TRANSIENT ((void (*)(void *))-1)

/* ---- 生命周期 ---- */
int  sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int  sqlite3_close_v2(sqlite3 *db);
int  sqlite3_extended_result_codes(sqlite3 *db, int onoff);
const char *sqlite3_errmsg(sqlite3 *db);
const char *sqlite3_errstr(int rc);
int  sqlite3_errcode(sqlite3 *db);

/* ---- 执行 ---- */
int  sqlite3_exec(sqlite3 *db, const char *sql,
                  int (*cb)(void *, int, char **, char **),
                  void *arg, char **errmsg);
int  sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte,
                        sqlite3_stmt **ppStmt, const char **pzTail);
int  sqlite3_step(sqlite3_stmt *pStmt);
int  sqlite3_reset(sqlite3_stmt *pStmt);
int  sqlite3_finalize(sqlite3_stmt *pStmt);
int  sqlite3_clear_bindings(sqlite3_stmt *pStmt);

/* ---- 绑定 ---- */
int  sqlite3_bind_int(sqlite3_stmt *p, int i, int v);
int  sqlite3_bind_int64(sqlite3_stmt *p, int i, long long v);
int  sqlite3_bind_double(sqlite3_stmt *p, int i, double v);
int  sqlite3_bind_text(sqlite3_stmt *p, int i, const char *v, int n,
                       void (*destructor)(void *));
int  sqlite3_bind_blob(sqlite3_stmt *p, int i, const void *v, int n,
                       void (*destructor)(void *));
int  sqlite3_bind_null(sqlite3_stmt *p, int i);

/* ---- 取列 ---- */
int         sqlite3_column_count(sqlite3_stmt *p);
int         sqlite3_column_type(sqlite3_stmt *p, int i);
long long   sqlite3_column_int64(sqlite3_stmt *p, int i);
int         sqlite3_column_int(sqlite3_stmt *p, int i);
double      sqlite3_column_double(sqlite3_stmt *p, int i);
const char *sqlite3_column_text(sqlite3_stmt *p, int i);
const void *sqlite3_column_blob(sqlite3_stmt *p, int i);
int         sqlite3_column_bytes(sqlite3_stmt *p, int i);

/* ---- 变更统计 ---- */
int sqlite3_changes(sqlite3 *db);
long long sqlite3_last_insert_rowid(sqlite3 *db);

/* ---- 内存 ---- */
void sqlite3_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* SQLITE3_MIN_H */
