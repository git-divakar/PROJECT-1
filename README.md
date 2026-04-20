#  Multi-container parent supervisor – Task 1

##  Overview

This project implements a **multi-container runtime with a parent supervisor** in C using Linux system programming primitives.

It demonstrates how basic containerization works internally using:

* `clone()` (namespaces)
* `chroot()` (filesystem isolation)
* Unix domain sockets (IPC)
* Process lifecycle management

---

##  Features

###  Supervisor Process

* Runs as a **long-lived daemon**
* Accepts commands via Unix socket (`/tmp/engine.sock`)
* Manages multiple containers concurrently

###  Multi-Container Support

* Multiple containers can run at the same time
* Each container is independently tracked and managed

###  Namespace Isolation

Each container runs with:

* **PID namespace** → isolated process tree
* **UTS namespace** → isolated hostname
* **Mount namespace** → isolated filesystem mounts

---

##  Filesystem Isolation

* `rootfs-base/` is used as a template

* Each container gets its own copy:

  ```
  rootfs-c1/
  rootfs-c2/
  ```

* Containers are isolated using:

  ```c
  chroot(rootfs);
  ```

* `/proc` is mounted inside each container:

  ```c
  mount("proc", "/proc", "proc", 0, NULL);
  ```

---

##  Container Metadata

Each container tracks:

* Container ID
* Host PID
* Start time
* Current state (STARTING, RUNNING, STOPPED)
* Exit status
* Root filesystem path
* Log file path

---

##  Process Lifecycle Management

* Supervisor **reaps child processes** using:

  ```c
  waitpid(-1, &status, WNOHANG);
  ```

* Prevents **zombie processes**

---

##  Logging

* Each container writes output to:

  ```
  <container-id>.log
  ```

---

##  IPC (Client–Server Model)

* CLI communicates with supervisor using:

  * Unix domain sockets
* Commands:

  * `start`
  * `stop`
  * `ps`

---

##  Build Instructions

```bash
gcc engine.c -o engine -lpthread
```

---

##  Usage

### Start Supervisor

```bash
sudo ./engine supervisor
```

### Start Container

```bash
sudo ./engine start c1 /bin/sh
```

### List Containers

```bash
sudo ./engine ps
```

### Stop Container

```bash
sudo ./engine stop c1
```

---

##  Root Filesystem Setup

Create a minimal rootfs:

```bash
mkdir -p rootfs-base/bin
cp /bin/sh rootfs-base/bin/
```

(Optional: add more binaries as needed)

---

##  Example Output

```bash
ID    PID    STATE     START
c1    1234   RUNNING   1710000000
c2    1235   RUNNING   1710000005
```

---

##  Limitations

This is an educational implementation and does NOT include:

* cgroups (CPU/memory limits)
* user namespaces (security)
* seccomp filtering
* container networking
* image management

---

##  Future Improvements

* Add **cgroups** for resource control
* Use **pivot_root** instead of `chroot`
* Implement **network namespaces**
* Add support for command arguments
* Improve security isolation

---

##  Summary

This project demonstrates the **core building blocks of container runtimes** and provides a simplified view of how systems like Docker work internally.

---
