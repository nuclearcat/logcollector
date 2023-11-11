# logcollector
UDP/syslog collector

## Description

C++ Receive UDP raw/syslog messages and store to sqlite3 database
Database file is rotated each hour
Database files is compressed after X days (default 7 days) by xz

## Usage

```shell
logcollector [-h] [-p PORT] [-d DBDIR] [-v]
```
