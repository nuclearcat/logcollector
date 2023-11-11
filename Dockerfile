# compile C++ cmake project with libsqlite3-dev
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/app

COPY . .

RUN cmake -B build -S . && cmake --build build

# create volume for sqlite3 database
VOLUME /db

WORKDIR /usr/src/app/build
# launch logcollectd with directory /db as volume -d /db
CMD ["./logcollectd", "-d", "/db", "-v"]