# Use same base image for both stages
FROM oraclelinux:9 AS builder

ENV MYSQL_VERSION=8.4.3

WORKDIR /usr/src

RUN dnf update -y

RUN dnf install -y git
RUN git clone --depth 1 --branch 8.4 \
    https://github.com/mysql/mysql-server.git mysql-src

RUN dnf install -y oracle-epel-release-el9
RUN dnf config-manager --set-enabled ol9_codeready_builder
RUN dnf install -y \
    gcc-toolset-12-gcc \
    gcc-toolset-12-gcc-c++ \
    gcc-toolset-12-binutils \
    gcc-toolset-12-annobin-annocheck \
    gcc-toolset-12-annobin-plugin-gcc \
    cmake \
    openssl-devel \
    zlib-devel \
    ncurses-devel \
    libtirpc-devel \
    rpcgen \
    elfutils \
    bison

COPY src/CMakeLists.txt src/ha_toydb.cc src/ha_toydb.h /usr/src/mysql-src/storage/toydb/

WORKDIR /usr/src/mysql-src/build
RUN cmake .. \
    -DDOWNLOAD_BOOST=1 \
    -DWITH_BOOST=/usr/src/boost \
    -DWITH_UNIT_TESTS=OFF

RUN cmake --build . --target toydb -- -j$(nproc)


FROM mysql:8.4.8

COPY --from=builder /usr/src/mysql-src/build/plugin_output_directory/ha_toydb.so \
    /usr/lib64/mysql/plugin/

RUN echo 'INSTALL PLUGIN toydb SONAME "ha_toydb.so";' \
    > /docker-entrypoint-initdb.d/install_toydb.sql

CMD ["mysqld", "--skip-log-bin"]
