/* test_sqlite.c - 验证手写 sqlite3 声明可用 */
#include <stdio.h>
#include <string.h>
#include "sqlite3_min.h"

int main(void)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(":memory:", &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_NOMUTEX, NULL);
    printf("open: rc=%d\n", rc);
    if (rc != SQLITE_OK) return 1;

    rc = sqlite3_exec(db,
        "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, v INTEGER);", 0, 0, 0);
    printf("create: rc=%d\n", rc);

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT INTO t(name,v) VALUES(?,?)", -1, &st, NULL);
    printf("prepare: rc=%d\n", rc);
    if (rc != SQLITE_OK) { printf("err: %s\n", sqlite3_errmsg(db)); return 1; }

    for (int i = 0; i < 1000; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "row%d", i);
        sqlite3_bind_text(st, 1, nm, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (long long)i * 1000);
        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { printf("step fail rc=%d\n", rc); return 1; }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    printf("inserted 1000, last_rowid=%lld\n", sqlite3_last_insert_rowid(db));

    /* 读回来 */
    sqlite3_prepare_v2(db, "SELECT count(*), sum(v) FROM t", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        printf("count=%lld sum=%lld\n",
               sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
    }
    sqlite3_finalize(st);

    /* 测 last_insert_rowid 的性能（关键路径要用） */
    sqlite3_prepare_v2(db, "INSERT INTO t(name,v) VALUES('x',1)", -1, &st, NULL);
    sqlite3_step(st);
    printf("plain insert rowid=%lld\n", sqlite3_last_insert_rowid(db));
    sqlite3_reset(st);
    sqlite3_step(st);
    printf("second insert rowid=%lld\n", sqlite3_last_insert_rowid(db));
    sqlite3_finalize(st);

    sqlite3_close_v2(db);
    printf("ALL OK\n");
    return 0;
}
