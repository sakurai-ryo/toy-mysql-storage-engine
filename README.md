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

# Initialize data directory and my.cnf
# This shows the initial password for the root user
mise run mysql:run-init

# Run mysqld
mise run mysql:run
```

After running the above commands, you can connect to the MySQL server using the following command:

```sh
mysql -u root -p
```

To reset the root password, you can execute the following SQL command:

```sql
ALTER USER root@'localhost' IDENTIFIED BY 'sample-password';
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
