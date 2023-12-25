/*
C++ Receive UDP raw/syslog messages and store to sqlite3 database
Database file is rotated each hour
Database files is compressed after X days (default 7 days) by xz

(c) Denys Fedoryshchenko, 2023
SPDX-License-Identifier: LGPL-2.1-only
*/
#include <queue>
#include <tuple>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>

struct {
    char *dbdir;
    char *ip;
    int port;
    int verbose;
    int compress_age;
} config;

struct {
    int sock;
    sqlite3 *db;
} fd;

#define VERSION "0.1"

// queue will contain timestamp, host, message
std::queue <std::tuple<int, std::string, std::string>> queue;

void init_new_db() {
    const char *sql = "CREATE TABLE IF NOT EXISTS log (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER, host TEXT, message TEXT);";
    char *err_msg = 0;
    int rc = sqlite3_exec(fd.db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
}

int open_listener(int port) {
    int sock;
    struct sockaddr_in name;

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("opening datagram socket");
        exit(EXIT_FAILURE);
    }

    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&name, sizeof(name)) < 0) {
        perror("binding datagram socket");
        exit(EXIT_FAILURE);
    }

    // increase buffer size
    int optval = 262144;
    socklen_t optlen = sizeof(optval);
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &optval, optlen) < 0) {
        perror("setsockopt"); // soft-failure
    }

    return sock;
}

/*
    * Check if dbfile needs to be updated
    * If yes, close current db and open new one
*/
void dbtimecheck(int *current_hour, char *dbfile) {
    time_t now = time(NULL);
    if (*current_hour != localtime(&now)->tm_hour) {
        *current_hour = localtime(&now)->tm_hour;
        sprintf(dbfile, "%s/%04d%02d%02d%02d.sqlite3", config.dbdir,
                localtime(&now)->tm_year + 1900, localtime(&now)->tm_mon + 1,
                localtime(&now)->tm_mday, localtime(&now)->tm_hour);
        if (config.verbose) {
            printf("dbfile: %s\n", dbfile);
        }
        if (fd.db != 0) {
            sqlite3_close(fd.db);
        }
        if (sqlite3_open(dbfile, &fd.db) != SQLITE_OK) {
            fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(fd.db));
            sqlite3_close(fd.db);
            exit(EXIT_FAILURE);
        }
        init_new_db();
    }
}

/*
    * Insert message into db
    * Return 0 on success, 1 on error
*/
int insert_db(char *remote, char *msg, int ts) {
    const char *sql = "INSERT INTO log (timestamp, host, message) VALUES (?, ?, ?);";
    char *err_msg = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(fd.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }
    sqlite3_bind_int(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, remote, strlen(remote), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, msg, strlen(msg), SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }
    sqlite3_finalize(stmt);
    return 0;

}

/*
    * Compress old db files
*/
void cleanup() {
    // Iterate over files, convert filename to timestamp, xz compress older than 1 day
    DIR *dir;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        // file pattern is YYYYMMDDHH.sqlite3
        if (strlen(ent->d_name) != 14)
            continue;
        // convert to timestamp
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        strptime(ent->d_name, "%Y%m%d%H.sqlite3", &tm);
        time_t filetime = mktime(&tm);
        // check if older than 1 day
        time_t now = time(NULL);
        if (now - filetime < config.compress_age) {
            continue;
        }
        // compress
        {
            char cmd[1024];
            printf("Compressing %s/%s\n", config.dbdir, ent->d_name);
            sprintf(cmd, "xz -1 %s/%s", config.dbdir, ent->d_name);
            system(cmd);
        }
    }
    closedir(dir);
}

void *db_thread(void *arg) {
    printf("db_thread() started\n");
    char dbfile[1024] = {0};
    int current_hour = -1;
    // initial dbfile
    dbtimecheck(&current_hour, dbfile);
    while (1) {
        // check if dbfile needs to be updated
        dbtimecheck(&current_hour, dbfile);
        // check if queue is empty
        if (queue.empty()) {
            usleep(1000);
            continue;
        }
        // get first element from queue
        std::tuple<int, std::string, std::string> element = queue.front();
        queue.pop();
        // insert into db
        {
            int ts = std::get<0>(element);
            char *remote = strdup(std::get<1>(element).c_str());
            char *msg = strdup(std::get<2>(element).c_str());        
            insert_db(remote, msg, ts);
        }
    }
}


int main(int argc, char *argv[]) {
    int c;
    memset(&config, 0, sizeof(config));
    memset(&fd, 0, sizeof(fd));

    while ((c = getopt(argc, argv, "d:p:vh")) != -1) {
        switch (c) {
            case 'd':
                config.dbdir = optarg;
                break;
            case 'p':
                config.port = atoi(optarg);
                break;
            case 'v':
                config.verbose = 1;
                break;
            case 'h':
                fprintf(stderr, "Usage: %s [-d dbdir] [-p port] [-v]\n", argv[0]);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Usage: %s [-d dbdir] [-p port] [-v]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    printf("logcollectd started\n");
    printf("Version: %s\n", VERSION);

    if (config.dbdir == NULL) {
        config.dbdir = "./db";
    }
    if (config.verbose) {
        printf("dbdir: %s\n", config.dbdir);
    }
    // verify if db dir exists
    if (access(config.dbdir, F_OK) == -1) {
        fprintf(stderr, "dbdir %s does not exist\n", config.dbdir);
        exit(EXIT_FAILURE);
    }

    if (config.port == 0) {
        // Check uid
        if (getuid() == 0) {
            config.port = 514; // privileged port
            if (config.verbose)
                printf("Running as root, using privileged port %d\n", config.port);
        } else {
            config.port = 5140; // non-privileged port
            if (config.verbose)
                printf("Running as non-root, using non-privileged port %d\n", config.port);
        }
    }

    // default compress_age is 7 days in seconds
    if (config.compress_age == 0) {
        config.compress_age = 7 * 86400;
        if (config.verbose)
            printf("compress_age: %d\n", config.compress_age);
    }
    // open listener
    fd.sock = open_listener(config.port);

    // create db thread
    pthread_t db_thread_id;
    pthread_create(&db_thread_id, NULL, db_thread, NULL);

    if (config.verbose)
        printf("Listening on port %d\n", config.port);
    // dbfile is updated each hour, named YYYYMMDDHH.db
    while (1) {
        // check for new messages over select()
        {
            char remote[INET_ADDRSTRLEN];
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd.sock, &readfds);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int retval = select(fd.sock + 1, &readfds, NULL, NULL, &tv);
            if (retval == -1) {
                perror("select()");
            } else if (retval) {
                // read message
                char buffer[65536];
                struct sockaddr_in clientname;
                socklen_t clientnamelen = sizeof(clientname);
                int recvlen = recvfrom(fd.sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&clientname, &clientnamelen);
                if (recvlen > 0) {
                    buffer[recvlen] = 0;
                    // fill remote
                    inet_ntop(AF_INET, &(clientname.sin_addr), remote, INET_ADDRSTRLEN);
                    // if queue size is too big, drop message to avoid OOM
                    if (queue.size() > 100000) {
                        printf("Queue is too big, dropping message\n");
                        continue;
                    }
                    // add to queue
                    queue.push(std::make_tuple(time(NULL), remote, buffer));
                }
            } else {
                // no new messages
                usleep(1000);
            }
        }        
    }
}