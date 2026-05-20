# Instrument Data Library

A high-performance, cross-process shared memory data exchange library written in C99.

## Overview

The Instrument Data Library provides a lightweight framework for creating, sharing, and manipulating numerical data buffers across multiple processes. It is designed for high-throughput, low-latency applications such as scientific instrumentation, data acquisition systems, and real-time analytics pipelines.

Key features include:

- Zero-copy shared memory buffers
- Cross-process data access
- Reference-counted buffer lifecycle management
- Built-in metadata tracking
- Thread-safe and process-safe operations
- Deterministic and stress-tested concurrency behavior

---

## Language & Standards

This library exposes a pure C99 API and is intended to be:

- Portable across C and C++ projects
- Compatible with POSIX and windows systems
- ABI-stable when compiled as a shared library

All external interfaces are provided via extern "C" for C++ compatibility.

---

## Core Concepts

### Data Buffers

Data is stored in shared memory as typed arrays:

- float32, float64
- signed/unsigned integers

Each buffer has:

- A unique ID
- A data region (shared memory)
- A metadata region (also shared memory)

---

### Shared Metadata

Each buffer includes metadata describing:

- Buffer ID
- Instrument + command identifiers
- Data type
- Element count and total size
- Timestamp
- Ownership (multi-process tracking)
- Global reference count

This metadata enables safe sharing and lifecycle control across processes.

---

### Registry

The registry acts as a global lookup table that allows processes to:

- Discover existing buffers
- Attach to buffers via ID
- Retrieve shared memory handles

---

### Zero-Copy Design

By default, the library supports zero-copy buffer creation:

Example:

double *ptr = NULL;

gchar *id = data_manager_create_buffer_zero_copy(
    "instrument",
    "command",
    INST_DATA_FLOAT64,
    1024,
    (void **)&ptr
);

/*Write directly into shared memory*/
ptr[0] = 42.0;

No intermediate copying is required, enabling maximum performance.

---

## Basic Usage

### Create a Buffer

double data[10] = {1,2,3};

gchar *id = data_manager_create_buffer(
    "instrument",
    "cmd",
    INST_DATA_FLOAT64,
    10,
    data
);

---

### Access a Buffer

DataBuffer *buffer = data_manager_get_buffer(id);

double *values = data_buffer_data(buffer);

---

### Modify Data

data_manager_add_offset(id, 5.0);
data_manager_multiply_gain(id, 2.0);

---

### Release a Buffer

data_manager_release_buffer(id);
g_free(id);

Buffers are automatically cleaned up when no processes are using them.

---

## Building the Library

This project uses CMake.

### Requirements

- C compiler with C99 support (clang or gcc)
- CMake (3.16+ recommended)
- GLib

---

### Build Steps

mkdir build
cd build
cmake ..
cmake --build .

---

## Running Tests

The project includes:

- Unit tests
- Multiprocess IPC tests
- Deterministic stress tests
- Chaos stress tests

---

### Run All Tests

ctest --output-on-failure

---

### Test Categories

Basic: API correctness  
Registry: buffer discovery & metadata  
Multiprocess: IPC validation  
Deterministic stress: concurrent correctness  
Chaos stress: randomized concurrency + failure injection  

---

### Notes for Developers

- Tests rely on spawning the test binary as a subprocess
- The binary path is injected via test_config.h
- Shared memory and semaphore artifacts are cleaned before test runs

If tests hang or fail unexpectedly, you can manually clear IPC artifacts:

rm -f /dev/shm/sem.mtx_*
rm -f /dev/shm/inst_*

---

## Concurrency Model

The library provides:

- Process-safe shared memory
- Mutex-protected buffer operations
- Ownership tracking across processes

Concurrency is validated using:

- Parallel process execution
- Thread-triggered race conditions
- Chaos testing with simulated crashes

---

## Design Goals

- High performance (zero-copy)
- Cross-process safety
- Minimal dependencies
- Predictable lifecycle behavior
- Robustness under failure

---

## Known Limitations

- No built-in garbage collection daemon (cleanup is lazy/on access)
- Requires careful synchronization for user-defined writes

---

## License

This project is licensed under the Mozilla Public License 2.0 (MPL-2.0).

See the LICENSE file for details.

---

## Summary

The Instrument Data Library provides a fast, safe, and well-tested foundation for sharing structured numerical data across processes in C99 environments.

It is suitable for:

- Scientific computing
- Embedded instrumentation systems
- Real-time data pipelines
- High-performance computing contexts
