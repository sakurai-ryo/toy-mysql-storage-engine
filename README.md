## Getting Started

This repository uses Nix for managing the development environment.

To set up the development environment, run the following command:

```sh
nix develop
# or 
direnv allow
```

To generate mysql header files, run the following command:

```sh
mise run mysql:gen-header
```

Doing this enables auto-completion for MySQL server's internal header files in your editor.

### Run mysqld in local

Below are the steps to build and run the MySQL server:

```sh
# Build mysql-server
mise run mysql:build

mise run mysql:run-init

# Run mysqld
mise run mysql:run
```

After running the above commands, you can connect to the MySQL server using the following command:

```sh
# you can login to mysql with password "root"
mysql -u root -p
```

To stop the MySQL server, you can run:

```sh
mise run mysql:stop
```

### Run mysql in Docker

```sh
docker build -t mysql-toydb-engine .

docker run -d --name mysql-toydb \
  -e MYSQL_ROOT_PASSWORD=root \
  -p 3306:3306 \
  mysql-toydb-engine
```

### Sample SQL

```sql
CREATE DATABASE toydb;
use toydb;

CREATE TABLE t1 (id INT) ENGINE=TOYDB;
INSERT INTO t1 VALUES (1);
SELECT * FROM t1;

CREATE TABLE t2 (id INT, name VARCHAR(255)) ENGINE=TOYDB;
INSERT INTO t2 VALUES (1, 'Alice');
SELECT * FROM t2;

CREATE TABLE t3 (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, name VARCHAR(255), PRIMARY KEY (id)) ENGINE=TOYDB;
INSERT INTO t3 (name) VALUES ('Alice');
SELECT * FROM t3;

CREATE TABLE t4 (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL,
    age INT NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY idx_name (name),
    KEY idx_age (age)
) ENGINE = TOYDB;
INSERT INTO t4 (name, age) VALUES ('Alice', 30), ('Bob', 25), ('Charlie', 30);

CREATE TABLE t5 (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  last_name VARCHAR(20) NOT NULL,
  first_name VARCHAR(20) NOT NULL,
  PRIMARY KEY (id),
  INDEX idx_name (last_name, first_name)
) ENGINE=TOYDB;
INSERT INTO t5 (last_name, first_name) VALUES ('Smith', 'John'), ('Doe', 'Jane'), ('Smith', 'Alice');

SET SESSION cte_max_recursion_depth = 10000;
INSERT INTO t3 (name)
WITH RECURSIVE seq AS (
  SELECT 1 AS n
  UNION ALL
  SELECT n + 1 FROM seq WHERE n < 10000
)
SELECT CONCAT('user_', n) FROM seq;
```
