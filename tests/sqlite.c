#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    sqlite3 *db;
    sqlite3_stmt *st;
    const char *name = "/tmp/myos-sqlite.db";
    int rc;
    unlink(name);
    rc = sqlite3_open(name, &db);
    if (rc != SQLITE_OK) { printf("open %d\n", rc); return 1; }
    rc = sqlite3_exec(db, "create table t(id integer, name text);", 0, 0, 0);
    if (rc != SQLITE_OK) { printf("create %s\n", sqlite3_errmsg(db)); return 2; }
    rc = sqlite3_exec(db, "insert into t values(1, 'hi');", 0, 0, 0);
    if (rc != SQLITE_OK) { printf("insert %s\n", sqlite3_errmsg(db)); return 3; }
    sqlite3_close(db);
    rc = sqlite3_open(name, &db);
    if (rc != SQLITE_OK) { printf("reopen %d\n", rc); return 4; }
    rc = sqlite3_prepare_v2(db, "select name from t where id=1", -1, &st, 0);
    if (rc != SQLITE_OK) { printf("prep %s\n", sqlite3_errmsg(db)); return 5; }
    if (sqlite3_step(st) != SQLITE_ROW) { printf("step\n"); return 6; }
    if (strcmp((const char *)sqlite3_column_text(st, 0), "hi") != 0) return 7;
    sqlite3_finalize(st);
    sqlite3_close(db);
    unlink(name);
    printf("sqlite: ok\n");
    return 0;
}
